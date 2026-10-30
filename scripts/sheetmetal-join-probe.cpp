// What does it cost to join a flange to a sheet metal part that already has
// many? docs/SheetMetal.md sec 4.4 and M0: a feature builds its bend and leg
// directly and joins them to the part along the face they share; whether that
// join stays cheap as the part grows decides how the features are built.
//
// A plate 2 thick with N flanges along one long edge, each a 90 deg bend of
// inner radius 2 (the plate's end face revolved) and a leg 20 high. Joined:
//   chain: one flange at a time onto the part so far, as N features would;
//   fan:   all N in one boolean, as one feature on N edges would;
// each with a plain fuse and with the Boolean's glue option (the flange only
// touches the part, along part of the plate's end face: BOPAlgo_GlueShift).
// Reported: the total, the last join of the chain (the cost of the N-th
// feature), the result's faces and whether it is one valid solid.
//
// Build and run against the dev stack's OCCT:
//   O=~/works/sw/occt/install/conda-relwithdebinfo-801
//   .conda/run.sh bash -c "\$CXX -std=c++17 -O1 -I$O/include/opencascade \
//     scripts/sheetmetal-join-probe.cpp -L$O/lib -Wl,-rpath,$O/lib -lTKBO \
//     -lTKPrim -lTKTopAlgo -lTKBRep -lTKGeomAlgo -lTKGeomBase -lTKG3d -lTKG2d \
//     -lTKMath -lTKernel -o ~/.cache/lv-merge/sheetmetal/join-probe"
//   ~/.cache/lv-merge/sheetmetal/join-probe [N...]
#include <BOPAlgo_GlueEnum.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
const double T = 2, R = 2, W = 50, H = 20, Pitch = 20, Width = 16;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

TopoDS_Shape plate(int n)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), n * Pitch, W, T).Shape();
}

/// Flange i: the bend (the plate's end face over [a, b], revolved up about an
/// axis along X) and its leg, as two solids
std::vector<TopoDS_Shape> flange(int i)
{
    const double a = i * Pitch + (Pitch - Width) / 2, b = a + Width;
    BRepBuilderAPI_MakePolygon end(gp_Pnt(a, W, 0), gp_Pnt(b, W, 0), gp_Pnt(b, W, T),
                                   gp_Pnt(a, W, T), true);
    TopoDS_Face face = BRepBuilderAPI_MakeFace(end.Wire(), true).Face();
    TopoDS_Shape bend =
        BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, W, T + R), gp_Dir(1, 0, 0)), M_PI / 2).Shape();
    TopoDS_Shape leg = BRepPrimAPI_MakeBox(gp_Pnt(a, W + R, T + R), b - a, T, H).Shape();
    return {bend, leg};
}

TopoDS_Shape fuse(const TopoDS_Shape& base, const std::vector<TopoDS_Shape>& tools, bool glue)
{
    TopTools_ListOfShape args, tls;
    args.Append(base);
    for (const auto& t : tools) {
        tls.Append(t);
    }
    BRepAlgoAPI_Fuse op;
    op.SetArguments(args);
    op.SetTools(tls);
    if (glue) {
        op.SetGlue(BOPAlgo_GlueShift);
    }
    op.SetRunParallel(false);
    op.Build();
    if (!op.IsDone()) {
        std::printf("    fuse failed\n");
        std::exit(1);
    }
    return op.Shape();
}

void report(const char* what, int n, double total, double last, const TopoDS_Shape& s)
{
    TopTools_IndexedMapOfShape faces, solids;
    TopExp::MapShapes(s, TopAbs_FACE, faces);
    TopExp::MapShapes(s, TopAbs_SOLID, solids);
    const bool valid = BRepCheck_Analyzer(s).IsValid();
    std::printf("  %-12s N=%-3d total %9.1f ms  last join %7.2f ms  faces %4d  solids %d  %s\n",
                what, n, total * 1e3, last * 1e3, faces.Extent(), solids.Extent(),
                valid ? "valid" : "INVALID");
}

void chain(int n, bool glue)
{
    TopoDS_Shape part = plate(n);
    double total = 0, last = 0;
    for (int i = 0; i < n; ++i) {
        const double t0 = now();
        part = fuse(part, flange(i), glue);
        last = now() - t0;
        total += last;
    }
    report(glue ? "chain glue" : "chain plain", n, total, last, part);
}

void fan(int n, bool glue)
{
    std::vector<TopoDS_Shape> all;
    for (int i = 0; i < n; ++i) {
        for (auto& s : flange(i)) {
            all.push_back(s);
        }
    }
    const double t0 = now();
    TopoDS_Shape part = fuse(plate(n), all, glue);
    const double total = now() - t0;
    report(glue ? "fan glue" : "fan plain", n, total, total, part);
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<int> sizes;
    for (int i = 1; i < argc; ++i) {
        sizes.push_back(std::atoi(argv[i]));
    }
    if (sizes.empty()) {
        sizes = {1, 4, 16, 64};
    }
    for (int n : sizes) {
        std::printf("N = %d\n", n);
        chain(n, false);
        chain(n, true);
        fan(n, false);
        fan(n, true);
    }
    return 0;
}
