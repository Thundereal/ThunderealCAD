/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <BinTools.hxx>
#include <BinTools_LocationSet.hxx>
#include <BinTools_ShapeSet.hxx>
#include <BRep_PointRepresentation.hxx>
#include <BRep_TVertex.hxx>
#include <TopLoc_Datum3D.hxx>
#include <TopoDS_Builder.hxx>
#include <TopoDS_Iterator.hxx>
#include <BRep_Builder.hxx>
#include <BRep_CurveRepresentation.hxx>
#include <BRep_TEdge.hxx>
#include <BRep_TFace.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_TriangulationParameters.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>

#include <App/Application.h>
#include <App/ServiceWorkers.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include "MeshService.h"
#include "OCCTFork.h"

FC_LOG_LEVEL_INIT("Part", true, true)

using namespace Part;

namespace
{

constexpr std::uint32_t RequestMagic = 0x4d534831;  // "MSH1"
constexpr std::uint32_t ReplyMagic = 0x4d535231;    // "MSR1"
constexpr const char* Kind = "part.mesh";

using CurveReps = std::remove_reference_t<decltype(std::declval<BRep_TEdge&>().ChangeCurves())>;
using PointReps = std::remove_reference_t<decltype(std::declval<BRep_TVertex&>().ChangePoints())>;

double now()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

ParameterGrp::handle workerParams()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Workers");
}

// --- bytes

struct Out
{
    std::string s;
    template<class T>
    void put(T v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        s.append(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    void bytes(const std::string& b)
    {
        put<std::uint64_t>(b.size());
        s.append(b);
    }
};

struct In
{
    const char* p;
    const char* end;
    explicit In(const std::string& s)
        : p(s.data())
        , end(s.data() + s.size())
    {}
    template<class T>
    T get()
    {
        if (static_cast<std::size_t>(end - p) < sizeof(T)) {
            throw std::runtime_error("the mesh message is short");
        }
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string bytes()
    {
        const auto n = get<std::uint64_t>();
        if (static_cast<std::uint64_t>(end - p) < n) {
            throw std::runtime_error("the mesh message is short");
        }
        std::string b(p, static_cast<std::size_t>(n));
        p += n;
        return b;
    }
};

void putParams(Out& o, const IMeshTools_Parameters& m)
{
    o.put<std::int32_t>(static_cast<std::int32_t>(m.MeshAlgo));
    for (double d : {m.Angle, m.Deflection, m.AngleInterior, m.DeflectionInterior, m.MinSize}) {
        o.put(d);
    }
    for (bool b : {m.InParallel, m.Relative, m.InternalVerticesMode, m.ControlSurfaceDeflection,
                   m.EnableControlSurfaceDeflectionAllSurfaces, m.CleanModel, m.AdjustMinSize,
                   m.ForceFaceDeflection, m.AllowQualityDecrease}) {
        o.put<std::uint8_t>(b ? 1 : 0);
    }
}

IMeshTools_Parameters getParams(In& in)
{
    IMeshTools_Parameters m;
    m.MeshAlgo = static_cast<IMeshTools_MeshAlgoType>(in.get<std::int32_t>());
    m.Angle = in.get<double>();
    m.Deflection = in.get<double>();
    m.AngleInterior = in.get<double>();
    m.DeflectionInterior = in.get<double>();
    m.MinSize = in.get<double>();
    bool* flags[] = {&m.InParallel, &m.Relative, &m.InternalVerticesMode,
                     &m.ControlSurfaceDeflection, &m.EnableControlSurfaceDeflectionAllSurfaces,
                     &m.CleanModel, &m.AdjustMinSize, &m.ForceFaceDeflection,
                     &m.AllowQualityDecrease};
    for (bool* f : flags) {
        *f = in.get<std::uint8_t>() != 0;
    }
    return m;
}

/// What the mesher made a triangulation for (Poly_TriangulationParameters):
/// the mesher and a view both judge a resident mesh by it, and BinTools
/// does not keep it
void putMade(Out& o, const Handle(Poly_Triangulation) & t)
{
    const Handle(Poly_TriangulationParameters)& p = t.IsNull() ? nullptr : t->Parameters();
    o.put<std::uint8_t>(p.IsNull() ? 0 : 1);
    if (!p.IsNull()) {
        o.put(p->Deflection());
        o.put(p->Angle());
        o.put(p->MinSize());
    }
}

void getMade(In& in, const Handle(Poly_Triangulation) & t)
{
    if (!in.get<std::uint8_t>()) {
        return;
    }
    const double deflection = in.get<double>();
    const double angle = in.get<double>();
    const double minSize = in.get<double>();
    if (!t.IsNull()) {
        t->Parameters(new Poly_TriangulationParameters(deflection, angle, minSize));
    }
}

void putTriangulation(Out& o, const Handle(Poly_Triangulation) & t)
{
    o.put<std::uint8_t>(t.IsNull() ? 0 : 1);
    if (t.IsNull()) {
        return;
    }
    const int nodes = t->NbNodes();
    const int tris = t->NbTriangles();
    o.put<std::int32_t>(nodes);
    o.put<std::int32_t>(tris);
    putMade(o, t);
    o.put<std::uint8_t>(t->HasUVNodes() ? 1 : 0);
    o.put<std::uint8_t>(t->HasNormals() ? 1 : 0);
    o.put(t->Deflection());
    for (int i = 1; i <= nodes; ++i) {
        const gp_Pnt p = t->Node(i);
        o.put(p.X());
        o.put(p.Y());
        o.put(p.Z());
    }
    if (t->HasUVNodes()) {
        for (int i = 1; i <= nodes; ++i) {
            const gp_Pnt2d uv = t->UVNode(i);
            o.put(uv.X());
            o.put(uv.Y());
        }
    }
    if (t->HasNormals()) {
        for (int i = 1; i <= nodes; ++i) {
            NCollection_Vec3<float> n;
            t->Normal(i, n);
            o.put(n.x());
            o.put(n.y());
            o.put(n.z());
        }
    }
    for (int i = 1; i <= tris; ++i) {
        int a = 0, b = 0, c = 0;
        t->Triangle(i).Get(a, b, c);
        o.put<std::int32_t>(a);
        o.put<std::int32_t>(b);
        o.put<std::int32_t>(c);
    }
}

Handle(Poly_Triangulation) getTriangulation(In& in)
{
    if (!in.get<std::uint8_t>()) {
        return {};
    }
    const int nodes = in.get<std::int32_t>();
    const int tris = in.get<std::int32_t>();
    Handle(Poly_Triangulation) made = new Poly_Triangulation();
    getMade(in, made);
    const bool uv = in.get<std::uint8_t>() != 0;
    const bool normals = in.get<std::uint8_t>() != 0;
    Handle(Poly_Triangulation) t = new Poly_Triangulation(nodes, tris, uv, normals);
    t->Parameters(made->Parameters());
    t->Deflection(in.get<double>());
    for (int i = 1; i <= nodes; ++i) {
        const double x = in.get<double>();
        const double y = in.get<double>();
        const double z = in.get<double>();
        t->SetNode(i, gp_Pnt(x, y, z));
    }
    if (uv) {
        for (int i = 1; i <= nodes; ++i) {
            const double u = in.get<double>();
            const double v = in.get<double>();
            t->SetUVNode(i, gp_Pnt2d(u, v));
        }
    }
    if (normals) {
        for (int i = 1; i <= nodes; ++i) {
            const float x = in.get<float>();
            const float y = in.get<float>();
            const float z = in.get<float>();
            t->SetNormal(i, NCollection_Vec3<float>(x, y, z));
        }
    }
    for (int i = 1; i <= tris; ++i) {
        const int a = in.get<std::int32_t>();
        const int b = in.get<std::int32_t>();
        const int c = in.get<std::int32_t>();
        t->SetTriangle(i, Poly_Triangle(a, b, c));
    }
    return t;
}

void putPolygon(Out& o, const Handle(Poly_PolygonOnTriangulation) & p)
{
    o.put<std::uint8_t>(p.IsNull() ? 0 : 1);
    if (p.IsNull()) {
        return;
    }
    o.put<std::int32_t>(p->NbNodes());
    o.put(p->Deflection());
    o.put<std::uint8_t>(p->HasParameters() ? 1 : 0);
    for (int i = 1; i <= p->NbNodes(); ++i) {
        o.put<std::int32_t>(p->Node(i));
    }
    if (p->HasParameters()) {
        for (int i = 1; i <= p->NbNodes(); ++i) {
            o.put(p->Parameter(i));
        }
    }
}

Handle(Poly_PolygonOnTriangulation) getPolygon(In& in)
{
    if (!in.get<std::uint8_t>()) {
        return {};
    }
    const int n = in.get<std::int32_t>();
    const double deflection = in.get<double>();
    const bool params = in.get<std::uint8_t>() != 0;
    Handle(Poly_PolygonOnTriangulation) p = new Poly_PolygonOnTriangulation(n, params);
    p->Deflection(deflection);
    for (int i = 1; i <= n; ++i) {
        p->SetNode(i, in.get<std::int32_t>());
    }
    if (params) {
        for (int i = 1; i <= n; ++i) {
            p->SetParameter(i, in.get<double>());
        }
    }
    return p;
}

void putPolygon3D(Out& o, const Handle(Poly_Polygon3D) & p)
{
    o.put<std::uint8_t>(p.IsNull() ? 0 : 1);
    if (p.IsNull()) {
        return;
    }
    o.put<std::int32_t>(p->NbNodes());
    o.put(p->Deflection());
    o.put<std::uint8_t>(p->HasParameters() ? 1 : 0);
    for (int i = 1; i <= p->NbNodes(); ++i) {
        const gp_Pnt& q = p->Nodes().Value(i);
        o.put(q.X());
        o.put(q.Y());
        o.put(q.Z());
    }
    if (p->HasParameters()) {
        for (int i = 1; i <= p->NbNodes(); ++i) {
            o.put(p->Parameters().Value(i));
        }
    }
}

Handle(Poly_Polygon3D) getPolygon3D(In& in)
{
    if (!in.get<std::uint8_t>()) {
        return {};
    }
    const int n = in.get<std::int32_t>();
    const double deflection = in.get<double>();
    const bool params = in.get<std::uint8_t>() != 0;
    Handle(Poly_Polygon3D) p = new Poly_Polygon3D(n, params);
    p->Deflection(deflection);
    for (int i = 1; i <= n; ++i) {
        const double x = in.get<double>();
        const double y = in.get<double>();
        const double z = in.get<double>();
        p->ChangeNodes().SetValue(i, gp_Pnt(x, y, z));
    }
    if (params) {
        for (int i = 1; i <= n; ++i) {
            p->ChangeParameters().SetValue(i, in.get<double>());
        }
    }
    return p;
}

// --- the shape's topology, in an order both processes find alike

/// The faces where they are met (TopExp_Explorer's order), the distinct
/// face and edge records in the order they are first met, and the edges of
/// each face where they are met in it. A shape and the one read back from
/// its BinTools form meet them in the same order.
struct Topology
{
    std::vector<TopoDS_Face> faces;
    std::vector<int> faceRecord;  ///< of each face met, its record
    std::vector<Handle(BRep_TFace)> tfaces;
    std::vector<std::vector<int>> metAt;  ///< of each record, where met
    std::vector<TopoDS_Edge> edges;       ///< one place of each edge record
    std::vector<Handle(BRep_TEdge)> tedges;

    explicit Topology(const TopoDS_Shape& shape)
    {
        std::unordered_map<const TopoDS_TShape*, int> faceIndex;
        for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face& f = TopoDS::Face(ex.Current());
            auto [it, fresh] = faceIndex.emplace(f.TShape().get(), static_cast<int>(tfaces.size()));
            if (fresh) {
                tfaces.push_back(Handle(BRep_TFace)::DownCast(f.TShape()));
                metAt.emplace_back();
            }
            metAt[it->second].push_back(static_cast<int>(faces.size()));
            faceRecord.push_back(it->second);
            faces.push_back(f);
        }
        std::unordered_set<const TopoDS_TShape*> seen;
        for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
            if (seen.insert(ex.Current().TShape().get()).second) {
                edges.push_back(TopoDS::Edge(ex.Current()));
                tedges.push_back(Handle(BRep_TEdge)::DownCast(ex.Current().TShape()));
            }
        }
    }

    static std::vector<TopoDS_Edge> edgesOf(const TopoDS_Face& face)
    {
        std::vector<TopoDS_Edge> res;
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
            res.push_back(TopoDS::Edge(ex.Current()));
        }
        return res;
    }
};

Handle(Poly_Polygon3D) ownPolygon3D(const Handle(BRep_TEdge) & edge)
{
    for (CurveReps::Iterator it(edge->Curves()); it.More(); it.Next()) {
        if (it.Value()->IsPolygon3D()) {
            return it.Value()->Polygon3D();
        }
    }
    return {};
}

// --- the shape, its locations exact

static_assert(std::is_trivially_copyable_v<gp_Trsf>);

/** The shape in BinTools' form, and each of its transformations as it is.
 *
 * BinTools writes a location's matrix and reads it back through
 * gp_Trsf::SetValues, which takes the scale out of the determinant and
 * orthogonalizes what is left: a placement's rotation comes back an ulp or
 * so away, and a face meshed under it gets other nodes. So each elementary
 * location of the shape set goes along as the bytes of its gp_Trsf -- this
 * program's on both ends -- and the helper puts every location of the shape
 * it read back together from those.
 */
void writeShape(Out& o, const TopoDS_Shape& shape)
{
    BinTools_ShapeSet set;
    set.SetWithTriangles(true);
    set.SetWithNormals(true);
    set.SetFormatNb(BinTools_FormatVersion_CURRENT);
    set.Add(shape);
    std::ostringstream stream(std::ios::out | std::ios::binary);
    set.Write(stream);
    set.Write(shape, stream);
    o.bytes(stream.str());
    const BinTools_LocationSet& locations = set.Locations();
    o.put<std::uint32_t>(static_cast<std::uint32_t>(locations.NbLocations()));
    for (int i = 1; i <= locations.NbLocations(); ++i) {
        const TopLoc_Location& l = locations.Location(i);
        const bool elementary = !l.IsIdentity() && l.FirstPower() == 1 && l.NextLocation().IsIdentity();
        o.put<std::uint8_t>(elementary ? 1 : 0);
        if (elementary) {
            o.put(l.FirstDatum()->Transformation());
        }
    }
}

class ExactLocations
{
public:
    /// The datums of `set`'s elementary locations made again from `in`
    bool read(In& in, const BinTools_LocationSet& set, std::string& error)
    {
        const auto n = in.get<std::uint32_t>();
        if (static_cast<int>(n) != set.NbLocations()) {
            error = "the locations read here are not the locations sent";
            return false;
        }
        for (int i = 1; i <= set.NbLocations(); ++i) {
            if (!in.get<std::uint8_t>()) {
                continue;
            }
            const gp_Trsf trsf = in.get<gp_Trsf>();
            const TopLoc_Location& l = set.Location(i);
            if (l.IsIdentity() || l.FirstPower() != 1 || !l.NextLocation().IsIdentity()) {
                error = "a location read here is not the elementary one sent";
                return false;
            }
            datums[l.FirstDatum().get()] = new TopLoc_Datum3D(trsf);
        }
        return true;
    }

    /// `l` with every datum the one sent: rebuilt the way TopLoc chains
    /// them, the datum at the head multiplied on the right
    TopLoc_Location exact(const TopLoc_Location& l)
    {
        if (l.IsIdentity()) {
            return l;
        }
        auto done = memo.find(l);
        if (done != memo.end()) {
            return done->second;
        }
        auto d = datums.find(l.FirstDatum().get());
        if (d == datums.end()) {
            throw std::runtime_error("a location of a datum not sent");
        }
        TopLoc_Location res =
            exact(l.NextLocation()) * TopLoc_Location(d->second).Powered(l.FirstPower());
        // The same chain, datum for datum
        TopLoc_Location a = l;
        TopLoc_Location b = res;
        while (!a.IsIdentity() && !b.IsIdentity()) {
            auto m = datums.find(a.FirstDatum().get());
            if (m == datums.end() || m->second != b.FirstDatum()
                || a.FirstPower() != b.FirstPower()) {
                throw std::runtime_error("a location not made again as it was");
            }
            a = a.NextLocation();
            b = b.NextLocation();
        }
        if (!a.IsIdentity() || !b.IsIdentity()) {
            throw std::runtime_error("a location not made again as it was");
        }
        memo.emplace(l, res);
        return res;
    }

    /// Every location of `shape`, and of what its records hold, made exact
    void apply(TopoDS_Shape& shape)
    {
        std::unordered_set<const TopoDS_TShape*> seen;
        relocate(shape, seen);
        shape.Location(exact(shape.Location()), false);
    }

private:
    void relocate(const TopoDS_Shape& s, std::unordered_set<const TopoDS_TShape*>& seen)
    {
        const Handle(TopoDS_TShape)& ts = s.TShape();
        if (!seen.insert(ts.get()).second) {
            return;
        }
        if (auto face = Handle(BRep_TFace)::DownCast(ts)) {
            face->Location(exact(face->Location()));
        }
        else if (auto edge = Handle(BRep_TEdge)::DownCast(ts)) {
            for (CurveReps::Iterator it(edge->ChangeCurves()); it.More(); it.Next()) {
                it.ChangeValue()->Location(exact(it.Value()->Location()));
            }
        }
        else if (auto vertex = Handle(BRep_TVertex)::DownCast(ts)) {
            for (PointReps::Iterator it(vertex->ChangePoints()); it.More(); it.Next()) {
                it.ChangeValue()->Location(exact(it.Value()->Location()));
            }
        }
        TopoDS_Shape parent;
        parent.TShape(ts);
        std::vector<TopoDS_Shape> children;
        bool located = false;
        for (TopoDS_Iterator it(parent, false, false); it.More(); it.Next()) {
            children.push_back(it.Value());
            located = located || !it.Value().Location().IsIdentity();
        }
        for (auto& c : children) {
            relocate(c, seen);
        }
        if (!located) {
            return;
        }
        const bool wasFree = ts->Free();
        ts->Free(true);
        TopoDS_Builder builder;
        for (auto& c : children) {
            builder.Remove(parent, c);
        }
        for (auto& c : children) {
            builder.Add(parent, c.Located(exact(c.Location()), false));
        }
        ts->Free(wasFree);
    }

    std::unordered_map<const TopLoc_Datum3D*, Handle(TopLoc_Datum3D)> datums;
    std::unordered_map<TopLoc_Location, TopLoc_Location> memo;
};

/// The shape writeShape() wrote, its locations exact
bool readShape(In& in, TopoDS_Shape& shape, std::string& error)
{
    const std::string bytes = in.bytes();
    std::istringstream stream(bytes, std::ios::in | std::ios::binary);
    BinTools_ShapeSet set;
    set.SetWithTriangles(true);
    set.Read(stream);
    set.ReadSubs(shape, stream, set.NbShapes());
    ExactLocations exact;
    if (!exact.read(in, set.Locations(), error)) {
        return false;
    }
    exact.apply(shape);
    return true;
}

// --- the helper's side

bool meshHandler(const std::string& request, std::string& reply, std::string& error)
{
    In in(request);
    if (in.get<std::uint32_t>() != RequestMagic) {
        error = "not a mesh request";
        return false;
    }
    const IMeshTools_Parameters params = getParams(in);
    const bool crash = in.get<std::uint8_t>() != 0;
    if (crash) {
        // As a mesher's own crash would: a fault, not an exception
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    TopoDS_Shape shape;
    if (!readShape(in, shape, error)) {
        return false;
    }
    Topology topo(shape);
    // What each face's resident mesh was made for, as it is there
    if (in.get<std::uint32_t>() != topo.tfaces.size()) {
        error = "the shape read here is not the shape sent";
        return false;
    }
    for (auto& f : topo.tfaces) {
        getMade(in, f->ActiveTriangulation());
    }
    std::vector<Handle(Poly_Triangulation)> before;
    for (auto& f : topo.tfaces) {
        before.push_back(f->ActiveTriangulation());
    }
    std::vector<Handle(Poly_Polygon3D)> before3D;
    for (auto& e : topo.tedges) {
        before3D.push_back(ownPolygon3D(e));
    }

    BRepMesh_IncrementalMesh(shape, params);

    Out o;
    o.put(ReplyMagic);
    o.put<std::uint32_t>(static_cast<std::uint32_t>(topo.faces.size()));
    o.put<std::uint32_t>(static_cast<std::uint32_t>(topo.tfaces.size()));
    o.put<std::uint32_t>(static_cast<std::uint32_t>(topo.tedges.size()));
    std::vector<std::uint32_t> changed;
    std::unordered_map<const Poly_Triangulation*, int> installed;
    for (std::size_t i = 0; i < topo.tfaces.size(); ++i) {
        const Handle(Poly_Triangulation)& t = topo.tfaces[i]->ActiveTriangulation();
        if (t != before[i]) {
            changed.push_back(static_cast<std::uint32_t>(i));
            if (!t.IsNull()) {
                installed.emplace(t.get(), static_cast<int>(i));
            }
        }
    }
    o.put<std::uint32_t>(static_cast<std::uint32_t>(changed.size()));
    for (auto i : changed) {
        o.put(i);
        putTriangulation(o, topo.tfaces[i]->ActiveTriangulation());
    }
    // The polygons on the new triangulations, each at the place it was put
    // for: the face met at `k` and its edge met at `j` in it, whose
    // locations give the polygon's (BRep_Builder::UpdateEdge)
    Out polygons;
    std::uint32_t count = 0;
    std::unordered_map<int, std::vector<TopoDS_Edge>> edgesAt;
    for (std::size_t e = 0; e < topo.tedges.size(); ++e) {
        for (CurveReps::Iterator it(topo.tedges[e]->Curves()); it.More(); it.Next()) {
            const Handle(BRep_CurveRepresentation)& rep = it.Value();
            if (!rep->IsPolygonOnTriangulation()) {
                continue;
            }
            auto found = installed.find(rep->Triangulation().get());
            if (found == installed.end()) {
                continue;
            }
            int at = -1;
            int nth = -1;
            for (int k : topo.metAt[found->second]) {
                auto& list = edgesAt[k];
                if (list.empty()) {
                    list = Topology::edgesOf(topo.faces[k]);
                }
                for (std::size_t j = 0; j < list.size() && at < 0; ++j) {
                    if (list[j].TShape() == topo.tedges[e]
                        && rep->Location().IsEqual(
                            topo.faces[k].Location().Predivided(list[j].Location()))) {
                        at = k;
                        nth = static_cast<int>(j);
                    }
                }
                if (at >= 0) {
                    break;
                }
            }
            if (at < 0) {
                error = "a polygon of edge " + std::to_string(e) + " is at no place of its face";
                return false;
            }
            polygons.put<std::uint32_t>(static_cast<std::uint32_t>(at));
            polygons.put<std::uint32_t>(static_cast<std::uint32_t>(nth));
            const bool closed = rep->IsPolygonOnClosedTriangulation();
            polygons.put<std::uint8_t>(closed ? 1 : 0);
            putPolygon(polygons, rep->PolygonOnTriangulation());
            if (closed) {
                putPolygon(polygons, rep->PolygonOnTriangulation2());
            }
            ++count;
        }
    }
    o.put(count);
    o.s.append(polygons.s);
    // An edge on no face: its own polygon
    Out own;
    count = 0;
    for (std::size_t e = 0; e < topo.tedges.size(); ++e) {
        const Handle(Poly_Polygon3D) p = ownPolygon3D(topo.tedges[e]);
        if (p == before3D[e]) {
            continue;
        }
        for (CurveReps::Iterator it(topo.tedges[e]->Curves()); it.More(); it.Next()) {
            if (it.Value()->IsPolygon3D() && !it.Value()->Location().IsIdentity()) {
                error = "the polygon of edge " + std::to_string(e) + " is moved";
                return false;
            }
        }
        own.put<std::uint32_t>(static_cast<std::uint32_t>(e));
        putPolygon3D(own, p);
        ++count;
    }
    o.put(count);
    o.s.append(own.s);
    reply = std::move(o.s);
    return true;
}

[[maybe_unused]] const bool registered = (App::ServiceWorkers::registerKind(Kind, meshHandler), true);

// --- the client's side

/// What the client keeps: never destroyed, as a pre-mesh thread can still
/// be meshing while the process exits
struct ClientState
{
    std::mutex lock;
    MeshServiceInfo stats;
    /// Shapes that killed a helper: never sent again, never meshed here
    std::unordered_set<const TopoDS_TShape*> killers;
    std::vector<Handle(TopoDS_TShape)> killersHeld;
    bool saidFallBack = false;
};

ClientState& client()
{
    static ClientState* state = new ClientState;
    return *state;
}

/// What a helper made, written onto `shape` as the mesher writes it
bool apply(const TopoDS_Shape& shape, const std::string& body, std::string& why, long& faces)
{
    In in(body);
    if (in.get<std::uint32_t>() != ReplyMagic) {
        why = "not a mesh reply";
        return false;
    }
    Topology topo(shape);
    const auto met = in.get<std::uint32_t>();
    const auto nfaces = in.get<std::uint32_t>();
    const auto nedges = in.get<std::uint32_t>();
    if (met != topo.faces.size() || nfaces != topo.tfaces.size()
        || nedges != topo.tedges.size()) {
        why = "the shape read there is not the shape here";
        return false;
    }
    BRep_Builder builder;
    std::unordered_set<const Poly_Triangulation*> dropped;
    std::vector<Handle(Poly_Triangulation)> installed(topo.tfaces.size());
    const auto changed = in.get<std::uint32_t>();
    for (std::uint32_t c = 0; c < changed; ++c) {
        const auto i = in.get<std::uint32_t>();
        if (i >= topo.tfaces.size()) {
            why = "a face out of range";
            return false;
        }
        Handle(Poly_Triangulation) t = getTriangulation(in);
        const Handle(BRep_TFace)& tf = topo.tfaces[i];
        {
            OCCTRepresentationLock guard(tf.get());
            for (NCollection_List<Handle(Poly_Triangulation)>::Iterator it(tf->Triangulations());
                 it.More(); it.Next()) {
                dropped.insert(it.Value().get());
            }
        }
        builder.UpdateFace(topo.faces[topo.metAt[i].front()], t);
        installed[i] = t;
        ++faces;
    }
    if (!dropped.empty()) {
        for (auto& e : topo.tedges) {
            OCCTRepresentationLock guard(e.get());
            CurveReps& reps = e->ChangeCurves();
            bool removed = false;
            for (CurveReps::Iterator it(reps); it.More();) {
                if (it.Value()->IsPolygonOnTriangulation()
                    && dropped.count(it.Value()->Triangulation().get())) {
                    reps.Remove(it);
                    removed = true;
                }
                else {
                    it.Next();
                }
            }
            if (removed) {
                e->Modified(true);
            }
        }
    }
    std::unordered_map<std::uint32_t, std::vector<TopoDS_Edge>> edgesAt;
    const auto polygons = in.get<std::uint32_t>();
    for (std::uint32_t c = 0; c < polygons; ++c) {
        const auto k = in.get<std::uint32_t>();
        const auto j = in.get<std::uint32_t>();
        const bool closed = in.get<std::uint8_t>() != 0;
        Handle(Poly_PolygonOnTriangulation) p1 = getPolygon(in);
        Handle(Poly_PolygonOnTriangulation) p2;
        if (closed) {
            p2 = getPolygon(in);
        }
        if (k >= topo.faces.size()) {
            why = "a face out of range";
            return false;
        }
        auto& list = edgesAt[k];
        if (list.empty()) {
            list = Topology::edgesOf(topo.faces[k]);
        }
        const Handle(Poly_Triangulation)& t = installed[topo.faceRecord[k]];
        if (j >= list.size() || t.IsNull()) {
            why = "a polygon at no place here";
            return false;
        }
        if (closed) {
            builder.UpdateEdge(list[j], p1, p2, t, topo.faces[k].Location());
        }
        else {
            builder.UpdateEdge(list[j], p1, t, topo.faces[k].Location());
        }
    }
    const auto own = in.get<std::uint32_t>();
    for (std::uint32_t c = 0; c < own; ++c) {
        const auto e = in.get<std::uint32_t>();
        Handle(Poly_Polygon3D) p = getPolygon3D(in);
        if (e >= topo.edges.size()) {
            why = "an edge out of range";
            return false;
        }
        builder.UpdateEdge(topo.edges[e], p);
    }
    return true;
}

bool meshHere(const TopoDS_Shape& shape, const IMeshTools_Parameters& params)
{
    const double began = now();
    BRepMesh_IncrementalMesh(shape, params);
    std::lock_guard<std::mutex> lock(client().lock);
    ++client().stats.here;
    client().stats.hereSeconds += now() - began;
    return true;
}

}  // namespace

bool Part::meshInWorker()
{
    // No application (a test's that never made one): no setting, and no
    // program to start a helper with -- meshed here
    if (App::Application::GetARGC() == 0) {
        return false;
    }
    return workerParams()->GetBool("MeshInWorker", false);
}

bool Part::meshShape(const TopoDS_Shape& shape, const IMeshTools_Parameters& params,
                     const char* what, const TopoDS_Shape* standsFor, MeshWhere where,
                     bool crashForTest)
{
    if (shape.IsNull()) {
        return true;
    }
    const bool worker = where == MeshWhere::Worker
        || (where == MeshWhere::Setting && meshInWorker());
    {
        std::lock_guard<std::mutex> lock(client().lock);
        ++client().stats.calls[what ? what : "a shape"];
    }
    if (!worker) {
        return meshHere(shape, params);
    }
    const std::string name = what ? what : "a shape";
    const TopoDS_Shape& key = standsFor && !standsFor->IsNull() ? *standsFor : shape;
    {
        std::lock_guard<std::mutex> lock(client().lock);
        if (client().killers.count(key.TShape().get())) {
            ++client().stats.refused;
            return false;
        }
    }
    const double began = now();
    Out o;
    o.put(RequestMagic);
    putParams(o, params);
    o.put<std::uint8_t>(crashForTest ? 1 : 0);
    {
        writeShape(o, shape);
        const Topology topo(shape);
        o.put<std::uint32_t>(static_cast<std::uint32_t>(topo.tfaces.size()));
        for (auto& f : topo.tfaces) {
            putMade(o, f->ActiveTriangulation());
        }
    }
    const double written = now();
    const double timeout = workerParams()->GetFloat("MeshTimeout", 120.0);
    App::ServiceWorkers::Reply reply = App::ServiceWorkers::call(Kind, "Part", o.s, timeout);
    const double replied = now();
    if (!reply.ok) {
        if (reply.died) {
            FC_ERR("meshing " << name << ": " << reply.error
                              << "; not meshed, and not meshed here either");
            std::lock_guard<std::mutex> lock(client().lock);
            ++client().stats.died;
            if (client().killers.insert(key.TShape().get()).second) {
                client().killersHeld.push_back(key.TShape());
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(client().lock);
            ++client().stats.fellBack;
            if (!client().saidFallBack) {
                client().saidFallBack = true;
                FC_WARN("meshing in a helper: " << reply.error << "; meshed here instead");
            }
        }
        return meshHere(shape, params);
    }
    std::string why;
    long faces = 0;
    bool ok = false;
    try {
        ok = apply(shape, reply.body, why, faces);
    }
    catch (const std::exception& e) {
        why = e.what();
    }
    catch (const Standard_Failure& e) {
        why = e.GetMessageString();
    }
    const double applied = now();
    {
        std::lock_guard<std::mutex> lock(client().lock);
        ++client().stats.worker;
        client().stats.faces += faces;
        client().stats.seconds += applied - began;
        client().stats.write += written - began;
        client().stats.apply += applied - replied;
        client().stats.helper += reply.handler;
        client().stats.started += reply.started;
        client().stats.sent += o.s.size();
        client().stats.received += reply.body.size();
    }
    if (!ok) {
        FC_WARN("meshing " << name << " in a helper: " << why << "; meshed here instead");
        std::lock_guard<std::mutex> lock(client().lock);
        ++client().stats.fellBack;
    }
    return ok ? true : meshHere(shape, params);
}

MeshServiceInfo Part::meshServiceInfo()
{
    std::lock_guard<std::mutex> lock(client().lock);
    return client().stats;
}

// --- the digest

namespace
{

struct Digest
{
    std::uint64_t h = 1469598103934665603ULL;
    void add(const void* p, std::size_t n)
    {
        auto b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 1099511628211ULL;
        }
    }
    template<class T>
    void put(T v)
    {
        add(&v, sizeof(T));
    }
};

}  // namespace

std::string Part::meshDigest(const TopoDS_Shape& shape)
{
    Digest d;
    long faces = 0, triangles = 0, polygons = 0, own = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation)& t = BRep_Tool::Triangulation(face, loc);
        Out o;
        putTriangulation(o, t);
        d.add(o.s.data(), o.s.size());
        if (t.IsNull()) {
            continue;
        }
        ++faces;
        triangles += t->NbTriangles();
        for (TopExp_Explorer ee(face, TopAbs_EDGE); ee.More(); ee.Next()) {
            Out p;
            const Handle(Poly_PolygonOnTriangulation)& poly =
                BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(ee.Current()), t, loc);
            putPolygon(p, poly);
            polygons += poly.IsNull() ? 0 : 1;
            d.add(p.s.data(), p.s.size());
        }
    }
    TopTools_IndexedDataMapOfShapeListOfShape onFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, onFaces);
    for (int i = 1; i <= onFaces.Extent(); ++i) {
        if (!onFaces(i).IsEmpty()) {
            continue;
        }
        TopLoc_Location loc;
        const Handle(Poly_Polygon3D)& p = BRep_Tool::Polygon3D(TopoDS::Edge(onFaces.FindKey(i)), loc);
        Out o;
        putPolygon3D(o, p);
        own += p.IsNull() ? 0 : 1;
        d.add(o.s.data(), o.s.size());
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(d.h));
    return "faces " + std::to_string(faces) + " triangles " + std::to_string(triangles)
        + " polygons " + std::to_string(polygons) + " free " + std::to_string(own) + " " + hex;
}
