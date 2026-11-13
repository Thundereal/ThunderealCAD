// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

// The flexible bodies (docs/AssemblyDynamics.md, D4b), as ChronoFlexible.h
// says. Three things of our own beside Chrono's modal assembly:
//
// - The 10-node tetrahedron with its consistent mass, as CalculiX's C3D10
//   has it (D4a's modes). Chrono's lumps the element's mass equally on its
//   ten nodes, where the consistent mass's rows sum to a negative mass at
//   each corner: on the test's bar, two elements across, its first two
//   bending frequencies were 0.06% and 0.25% lower; more on a coarser
//   mesh. The integrals of N_a N_b are exact for a straight-sided element,
//   by 6 a! b! c! d! / (a + b + c + d + 3)! over the barycentric monomials
//   (MotionLoads._tet10Mass(), D4a).
// - The damping in proportion to the stiffness, R = (2 z / w) K: the ratio
//   z at the frequency w, more above it. Modal damping is the part's modes'
//   as some boundary holds them, and the run's holds them otherwise: each of
//   the reduced system's modes damped at 0.5 -- the free part's, its rigid
//   motions not -- left a cantilever clamped by its joint ringing at 0.023,
//   its mode mostly a rigid turn about the clamp. Chrono's
//   ChModalDampingFactorRmm damps the modes held at every interface and
//   leaves their static modes undamped, and its
//   ChModalDampingFactorAssembly says it is not validated. The stiffness
//   damps deformation alone, whatever holds the part.
// - The interfaces: a boundary ChBody each, of negligible mass, its nodes
//   tied to it by ChLinkNodeFrame, internal (an RBE2 spider).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <Eigen/Dense>

#include <chrono/fea/ChContinuumMaterial.h>
#include <chrono/fea/ChElementTetraCorot_10.h>
#include <chrono/fea/ChLinkNodeFrame.h>
#include <chrono/fea/ChNodeFEAxyz.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChDirectSolverLS.h>
#include <chrono_modal/ChModalDamping.h>
#include <chrono_modal/ChModalSolverUndamped.h>
#include <chrono_modal/ChSymGenEigenvalueSolver.h>

#include "ChronoBackend.h"
#include "ChronoFlexible.h"
#include "Units.h"

using namespace Assembly::Solver;
namespace ch = chrono;

namespace
{

// The integral of N_a N_b over the element, over its volume
const Eigen::Matrix<double, 10, 10>& tet10Mass()
{
    static const Eigen::Matrix<double, 10, 10> mass = [] {
        using Exponents = std::array<int, 4>;
        struct Term
        {
            Exponents e;
            double c;
        };
        auto mono = [](std::initializer_list<int> at) {
            Exponents e {0, 0, 0, 0};
            for (int i : at) {
                ++e[i];
            }
            return e;
        };
        const int edges[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};
        std::vector<std::vector<Term>> shapes;
        for (int i = 0; i < 4; ++i) {
            shapes.push_back({{mono({i, i}), 2.0}, {mono({i}), -1.0}});
        }
        for (const auto& edge : edges) {
            shapes.push_back({{mono({edge[0], edge[1]}), 4.0}});
        }
        auto factorial = [](int n) {
            double f = 1.0;
            for (int i = 2; i <= n; ++i) {
                f *= i;
            }
            return f;
        };
        Eigen::Matrix<double, 10, 10> m = Eigen::Matrix<double, 10, 10>::Zero();
        for (int a = 0; a < 10; ++a) {
            for (int b = 0; b < 10; ++b) {
                for (const Term& p : shapes[a]) {
                    for (const Term& q : shapes[b]) {
                        int sum = 0;
                        double num = 6.0;
                        for (int k = 0; k < 4; ++k) {
                            num *= factorial(p.e[k] + q.e[k]);
                            sum += p.e[k] + q.e[k];
                        }
                        m(a, b) += p.c * q.c * num / factorial(sum + 3);
                    }
                }
            }
        }
        return m;
    }();
    return mass;
}

class ConsistentTet10: public ch::fea::ChElementTetraCorot_10
{
public:
    void ComputeKRMmatricesGlobal(ch::ChMatrixRef H,
                                  double Kfactor,
                                  double Rfactor = 0,
                                  double Mfactor = 0) override
    {
        // The mass alone (the reduction's M) without the corotated K made to
        // be scaled by 0: the strip's mass pass 0.61 s to 0.04
        if (Kfactor == 0.0 && Rfactor == 0.0) {
            H.setZero();
        }
        else {
            ChElementTetraCorot_10::ComputeKRMmatricesGlobal(H, Kfactor, Rfactor, 0.0);
        }
        if (Mfactor == 0.0) {
            return;
        }
        const double m = Mfactor * GetVolume() * GetMaterial()->GetDensity();
        const auto& unit = tet10Mass();
        for (int a = 0; a < 10; ++a) {
            for (int b = 0; b < 10; ++b) {
                for (int k = 0; k < 3; ++k) {
                    H(3 * a + k, 3 * b + k) += m * unit(a, b);
                }
            }
        }
    }
};

// R = (2 z / w) K: the ratio z at the frequency w, Flexible::dampingFrequency
// or the first of the modes the reduction found. Also keeps those modes'
// frequencies, which the reduction forgets once done.
class StiffnessDamping: public ch::modal::ChModalDamping
{
public:
    StiffnessDamping(double ratio, double frequency, std::vector<double>& hz)
        : ratio(ratio)
        , frequency(frequency)
        , hz(hz)
    {}

    void ComputeR(ch::modal::ChModalAssembly& assembly,
                  const ch::ChMatrixDynamic<>&,
                  const ch::ChMatrixDynamic<>& modalK,
                  const ch::ChMatrixDynamic<>&,
                  ch::ChMatrixDynamic<>& modalR) const override
    {
        const auto& f = assembly.GetUndampedFrequencies();
        hz.assign(f.data(), f.data() + f.size());
        modalR.setZero(modalK.rows(), modalK.cols());
        const double at = frequency > 0.0 ? frequency : (hz.empty() ? 0.0 : hz.front());
        if (ratio <= 0.0 || !(at > 0.0)) {
            return;
        }
        modalR = (ratio / (std::numbers::pi * at)) * 0.5 * (modalK + modalK.transpose());
    }

private:
    double ratio;
    double frequency;  // Hz
    std::vector<double>& hz;
};

// The modes a reduction found, kept for the next run of the same part: run
// after run of an unchanged study made them again, 1.9 s of the rod's 6.8 s
// reduction. Not keyed by the record: a kept set of the same size is taken
// when its pairs solve this problem, A v = lambda B v to 1e-6 of its terms,
// so a part changed makes its own. A part started elsewhere takes them too:
// its eigenproblem is in its floating frame's axes, the part's own (build()).
// The last four, the newest first.
class KeptModesSolver: public ch::modal::ChSymGenEigenvalueSolverKrylovSchur
{
public:
    int Solve(const ch::ChSparseMatrix& A,
              const ch::ChSparseMatrix& B,
              ch::ChMatrixDynamic<double>& eigvects,
              ch::ChVectorDynamic<double>& eigvals,
              int num_modes,
              double shift) const override
    {
        {
            std::scoped_lock lock(guard());
            auto& sets = kept();
            for (auto it = sets.begin(); it != sets.end(); ++it) {
                if (it->rows == A.rows() && it->entriesA == A.nonZeros()
                    && it->entriesB == B.nonZeros() && it->modes == num_modes
                    && it->shift == shift && it->sorted == sort_ritz_pairs
                    && solves(A, B, it->vectors, it->values)) {
                    eigvects = it->vectors;
                    eigvals = it->values;
                    std::rotate(sets.begin(), it, std::next(it));
                    ++taken();
                    return num_modes;
                }
            }
        }
        const int found = ChSymGenEigenvalueSolverKrylovSchur::Solve(A, B, eigvects, eigvals,
                                                                      num_modes, shift);
        if (found == num_modes) {
            std::scoped_lock lock(guard());
            auto& sets = kept();
            sets.push_front({A.rows(), A.nonZeros(), B.nonZeros(), num_modes, shift,
                             sort_ritz_pairs, eigvects, eigvals});
            if (sets.size() > 4) {
                sets.pop_back();
            }
        }
        return found;
    }

    // The kept sets forgotten (Chrono::forgetFlexibleReductions())
    static void forget()
    {
        std::scoped_lock lock(guard());
        kept().clear();
    }

    // How many solves took a kept set; the kept sets' and its lock
    static std::size_t& taken()
    {
        static std::size_t count = 0;
        return count;
    }
    static std::mutex& guard()
    {
        static std::mutex m;
        return m;
    }

private:
    struct Set
    {
        Eigen::Index rows;
        Eigen::Index entriesA;
        Eigen::Index entriesB;
        int modes;
        double shift;
        bool sorted;
        ch::ChMatrixDynamic<double> vectors;
        ch::ChVectorDynamic<double> values;
    };
    static std::deque<Set>& kept()
    {
        static std::deque<Set> sets;
        return sets;
    }
    static bool solves(const ch::ChSparseMatrix& A,
                       const ch::ChSparseMatrix& B,
                       const ch::ChMatrixDynamic<double>& vectors,
                       const ch::ChVectorDynamic<double>& values)
    {
        for (Eigen::Index i = 0; i < vectors.cols(); ++i) {
            const Eigen::VectorXd Av = A * vectors.col(i);
            const Eigen::VectorXd Bv = B * vectors.col(i);
            const double scale = Av.norm() + std::abs(values(i)) * Bv.norm();
            if (!((Av - values(i) * Bv).norm() <= 1e-6 * scale)) {
                return false;
            }
        }
        return true;
    }
};

ch::ChVector3d toCh(const Base::Vector3d& v)
{
    return {v.x, v.y, v.z};
}

// What a part's reduction is made of, in its own frame: its mesh, its
// material, its modes and damping, its interfaces where they are on it and
// their nodes. Not its pose: the floating frame's axes are the part's
// (build()), so the reduction is the same wherever the part starts.
ChronoFlexible::Built::Key reductionKey(const Flexible& flexible,
                                        const std::vector<ChronoFlexible::Interface>& interfaces,
                                        double size)
{
    ChronoFlexible::Built::Key key;
    auto& exact = key.exact;
    auto& near = key.near;
    exact.reserve(10 * flexible.tetrahedra.size() + interfaces.size() + 6);
    near.reserve(3 * flexible.nodes.size() + 12 * interfaces.size());
    for (const auto& p : flexible.nodes) {
        near.insert(near.end(), {p.x / size, p.y / size, p.z / size});
    }
    for (const auto& tet : flexible.tetrahedra) {
        for (Id n : tet) {
            exact.push_back(static_cast<double>(n));
        }
    }
    exact.insert(exact.end(),
                 {flexible.youngs, flexible.poisson, flexible.density,
                  static_cast<double>(flexible.modes), flexible.damping, flexible.dampingFrequency});
    for (const auto& face : interfaces) {
        exact.push_back(static_cast<double>(face.nodes.size()));
        for (Id n : face.nodes) {
            exact.push_back(static_cast<double>(n));
        }
        const ch::ChVector3d& p = face.local.GetPos();
        const ch::ChMatrix33<> r = face.local.GetRotMat();
        near.insert(near.end(), {p.x() / size, p.y() / size, p.z() / size});
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                near.push_back(r(i, j));
            }
        }
    }
    return key;
}

bool sameReduction(const ChronoFlexible::Built::Key& a, const ChronoFlexible::Built::Key& b)
{
    if (a.exact != b.exact || a.near.size() != b.near.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.near.size(); ++i) {
        if (!(std::abs(a.near[i] - b.near[i]) <= 1e-9)) {
            return false;
        }
    }
    return true;
}

// The reductions made, kept for the next run of the same part (the same
// reductionKey()): its M, K and Cq assembled, turned into the floating frame
// and its static and dynamic modes solved took 3.5 s of the rod's 4.5 s
// reduction, run after run of an unchanged study. The last two, the newest
// first: a fine mesh's are its modes' columns over its every coordinate.
struct KeptReduction
{
    ChronoFlexible::Built::Key key;
    ch::modal::ChModalAssembly::ReducedData data;
    std::vector<double> hz;
};

std::deque<KeptReduction>& keptReductions()
{
    static std::deque<KeptReduction> kept;
    return kept;
}

// A reduction kept in a document: its key's hash, the exact numbers by their
// bits, the near ones at 1e-9, sameReduction()'s tolerance -- one at a
// rounding's edge misses, and the reduction is made again. FNV-1a's.
std::uint64_t keyHash(const ChronoFlexible::Built::Key& key)
{
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    mix(key.exact.size());
    for (double x : key.exact) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof bits);
        mix(bits);
    }
    mix(key.near.size());
    for (double x : key.near) {
        mix(static_cast<std::uint64_t>(std::llround(x * 1e9)));
    }
    return h;
}

// reductionNumbers()'s format
constexpr double reductionFormat = 1.0;

}  // namespace

namespace
{

// The point of triangle abc nearest p, as its weights on the corners (Ericson,
// Real-Time Collision Detection, 5.1.5)
std::array<double, 3> nearestOnTriangle(const ch::ChVector3d& p,
                                        const ch::ChVector3d& a,
                                        const ch::ChVector3d& b,
                                        const ch::ChVector3d& c)
{
    const ch::ChVector3d ab = b - a;
    const ch::ChVector3d ac = c - a;
    const ch::ChVector3d ap = p - a;
    const double d1 = ch::Vdot(ab, ap);
    const double d2 = ch::Vdot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return {1.0, 0.0, 0.0};
    }
    const ch::ChVector3d bp = p - b;
    const double d3 = ch::Vdot(ab, bp);
    const double d4 = ch::Vdot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return {0.0, 1.0, 0.0};
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        return {1.0 - v, v, 0.0};
    }
    const ch::ChVector3d cp = p - c;
    const double d5 = ch::Vdot(ab, cp);
    const double d6 = ch::Vdot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return {0.0, 0.0, 1.0};
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        return {1.0 - w, 0.0, w};
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0.0, 1.0 - w, w};
    }
    const double denominator = 1.0 / (va + vb + vc);
    const double v = vb * denominator;
    const double w = vc * denominator;
    return {1.0 - v - w, v, w};
}

}  // namespace

ChronoFlexible::Surface::Surface(const Flexible& flexible)
{
    points.reserve(flexible.nodes.size());
    for (const auto& p : flexible.nodes) {
        points.push_back(toCh(p));
    }
    // Each tetrahedron's four faces by their corners, sorted: those met once
    // are the surface's, turned to face away from the tetrahedron's fourth
    // corner -- out of the body, a collision triangle being one sided. Each
    // with its edges' nodes, the tetrahedron's (0 1) 4, (1 2) 5, (0 2) 6,
    // (0 3) 7, (1 3) 8, (2 3) 9
    struct Hash
    {
        std::size_t operator()(const std::array<Id, 3>& k) const
        {
            std::size_t h = std::hash<Id>()(k[0]);
            h = h * 1000003u ^ std::hash<Id>()(k[1]);
            return h * 1000003u ^ std::hash<Id>()(k[2]);
        }
    };
    // A face: its corners, the tetrahedron's fourth, and the edges' nodes
    // (0 1), (1 2), (2 0)
    struct Found
    {
        int count = 0;
        std::array<Id, 4> corners {};
        std::array<Id, 3> edges {};
    };
    static const std::array<std::array<int, 2>, 6> edgeOf {
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    std::unordered_map<std::array<Id, 3>, Found, Hash> found;
    found.reserve(4 * flexible.tetrahedra.size());
    for (const auto& tet : flexible.tetrahedra) {
        auto between = [&](int i, int j) {
            for (std::size_t e = 0; e < edgeOf.size(); ++e) {
                if ((edgeOf[e][0] == i && edgeOf[e][1] == j)
                    || (edgeOf[e][0] == j && edgeOf[e][1] == i)) {
                    return tet[4 + e];
                }
            }
            return tet[i];
        };
        const std::array<std::array<int, 4>, 4> four {
            {{0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 3, 1}, {1, 2, 3, 0}}};
        for (const auto& face : four) {
            std::array<Id, 3> key {tet[face[0]], tet[face[1]], tet[face[2]]};
            std::sort(key.begin(), key.end());
            auto& entry = found[key];
            ++entry.count;
            entry.corners = {tet[face[0]], tet[face[1]], tet[face[2]], tet[face[3]]};
            entry.edges = {between(face[0], face[1]),
                           between(face[1], face[2]),
                           between(face[2], face[0])};
        }
    }
    double edges = 0.0;
    for (const auto& [key, entry] : found) {
        if (entry.count != 1 || key[2] >= points.size() || entry.corners[3] >= points.size()) {
            continue;
        }
        std::array<Id, 6> f {entry.corners[0],
                             entry.corners[1],
                             entry.corners[2],
                             entry.edges[0],
                             entry.edges[1],
                             entry.edges[2]};
        if (ch::Vdot(ch::Vcross(points[f[1]] - points[f[0]], points[f[2]] - points[f[0]]),
                     points[entry.corners[3]] - points[f[0]])
            > 0.0) {
            // b and c swapped: the edges (0 1) and (2 0) too
            f = {f[0], f[2], f[1], f[5], f[4], f[3]};
        }
        // Curved: its edges' nodes near their edges' middles, one off it. A
        // flat face's quarters cost the strip between two bars twice its
        // time, four times the points, and changed nothing; and a mesh of
        // the first order, or one whose nodes are not where they should be,
        // is flat
        bool middles = true;
        bool off = false;
        for (int e = 0; e < 3 && middles; ++e) {
            const Id m = f[3 + e];
            const ch::ChVector3d& a = points[f[e]];
            const ch::ChVector3d& b = points[f[(e + 1) % 3]];
            const double along = (b - a).Length();
            const double from = m < points.size() ? (points[m] - (a + b) * 0.5).Length() : 0.0;
            middles =
                m < points.size() && m != f[e] && m != f[(e + 1) % 3] && from <= 0.25 * along;
            off = off || from > 1e-6 * along;
        }
        const bool curved = middles && off;
        const auto face = static_cast<std::uint32_t>(faces.size());
        if (curved) {
            // The four: a ab ca, ab b bc, ca bc c, and ab bc ca in the middle
            static const std::array<std::array<int, 3>, 4> quarters {
                {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}}};
            for (std::uint8_t q = 0; q < 4; ++q) {
                corners.push_back({f[quarters[q][0]], f[quarters[q][1]], f[quarters[q][2]]});
                pieces.emplace_back(face, q);
            }
        }
        else {
            f[3] = f[4] = f[5] = f[0];
            corners.push_back({f[0], f[1], f[2]});
            pieces.emplace_back(face, std::uint8_t(4));
        }
        faces.push_back(f);
        edges += (points[f[1]] - points[f[0]]).Length() + (points[f[2]] - points[f[1]]).Length()
            + (points[f[0]] - points[f[2]]).Length();
    }
    if (corners.empty()) {
        return;
    }
    // How far the faces stand off their triangles: at each one's middle and
    // its edges' middles, the face's point against the triangle's
    for (std::size_t i = 0; i < corners.size(); ++i) {
        if (pieces[i].second > 3) {
            continue;
        }
        static const std::array<std::array<double, 3>, 4> samples {
            {{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, {0.5, 0.5, 0.0}, {0.0, 0.5, 0.5}, {0.5, 0.0, 0.5}}};
        const auto& t = corners[i];
        for (const auto& w : samples) {
            Place place;
            onFace(i, w, place);
            const ch::ChVector3d flat = points[t[0]] * w[0] + points[t[1]] * w[1] + points[t[2]] * w[2];
            most = std::max(most, (place.at - flat).Length());
        }
    }
    // Each face's neighbour across each of its edges, by the edge's corners
    neighbours.assign(faces.size(), {-1, -1, -1});
    std::map<std::pair<Id, Id>, std::int64_t> sharing;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        for (int e = 0; e < 3; ++e) {
            const Id u = faces[f][static_cast<std::size_t>(e)];
            const Id v = faces[f][static_cast<std::size_t>((e + 1) % 3)];
            const auto key = std::make_pair(std::min(u, v), std::max(u, v));
            const auto [it, added] = sharing.emplace(key, static_cast<std::int64_t>(f));
            if (!added && it->second >= 0) {
                const auto g = static_cast<std::size_t>(it->second);
                neighbours[f][static_cast<std::size_t>(e)] = it->second;
                for (int k = 0; k < 3; ++k) {
                    const Id a = faces[g][static_cast<std::size_t>(k)];
                    const Id b = faces[g][static_cast<std::size_t>((k + 1) % 3)];
                    if (std::make_pair(std::min(a, b), std::max(a, b)) == key) {
                        neighbours[g][static_cast<std::size_t>(k)] = static_cast<std::int64_t>(f);
                    }
                }
                it->second = -1;
            }
        }
    }
    // The grid: cells twice the triangles' mean edge, at most 2e6 of them
    low = ch::ChVector3d(std::numeric_limits<double>::max());
    ch::ChVector3d high(-std::numeric_limits<double>::max());
    for (const auto& t : corners) {
        for (Id n : t) {
            low = ch::Vmin(low, points[n]);
            high = ch::Vmax(high, points[n]);
        }
    }
    const ch::ChVector3d extent = high - low;
    cell = std::max(2.0 * edges / (3.0 * static_cast<double>(faces.size())),
                    1e-9 * std::max(extent.Length(), 1e-12));
    auto counts = [&] {
        for (int k = 0; k < 3; ++k) {
            count[k] = std::max(1, static_cast<int>(std::ceil(extent[k] / cell)));
        }
        return static_cast<double>(count[0]) * count[1] * count[2];
    };
    while (counts() > 2e6) {
        cell *= 1.25;
    }
    cells.assign(static_cast<std::size_t>(count[0]) * count[1] * count[2], {});
    auto index = [&](const ch::ChVector3d& p, int k) {
        return std::clamp(static_cast<int>(std::floor((p[k] - low[k]) / cell)), 0, count[k] - 1);
    };
    for (std::uint32_t i = 0; i < corners.size(); ++i) {
        ch::ChVector3d lo(std::numeric_limits<double>::max());
        ch::ChVector3d hi(-std::numeric_limits<double>::max());
        for (Id n : corners[i]) {
            lo = ch::Vmin(lo, points[n]);
            hi = ch::Vmax(hi, points[n]);
        }
        for (int x = index(lo, 0); x <= index(hi, 0); ++x) {
            for (int y = index(lo, 1); y <= index(hi, 1); ++y) {
                for (int z = index(lo, 2); z <= index(hi, 2); ++z) {
                    cells[(static_cast<std::size_t>(z) * count[1] + y) * count[0] + x].push_back(i);
                }
            }
        }
    }
}

std::array<double, 3> ChronoFlexible::Surface::weights(const ch::ChVector3d& p,
                                                       const ch::ChVector3d& a,
                                                       const ch::ChVector3d& b,
                                                       const ch::ChVector3d& c)
{
    return nearestOnTriangle(p, a, b, c);
}

void ChronoFlexible::Surface::onFace(std::size_t i,
                                     const std::array<double, 3>& w,
                                     Place& place) const
{
    const auto [face, piece] = pieces[i];
    const auto& f = faces[face];
    place.nodes = f;
    place.weights = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    if (piece > 3) {
        place.weights[0] = w[0];
        place.weights[1] = w[1];
        place.weights[2] = w[2];
    }
    else {
        place.weights = shape(faceCoordinates(i, w));
    }
    place.at = ch::VNULL;
    for (int k = 0; k < 6; ++k) {
        place.at += points[f[k]] * place.weights[k];
    }
}

std::array<double, 3> ChronoFlexible::Surface::faceCoordinates(std::size_t i,
                                                               const std::array<double, 3>& w) const
{
    const auto piece = pieces[i].second;
    if (piece > 3) {
        return w;
    }
    // The face's area coordinates at the piece's corners, its nodes'
    static const std::array<std::array<double, 3>, 6> at {{{1.0, 0.0, 0.0},
                                                           {0.0, 1.0, 0.0},
                                                           {0.0, 0.0, 1.0},
                                                           {0.5, 0.5, 0.0},
                                                           {0.0, 0.5, 0.5},
                                                           {0.5, 0.0, 0.5}}};
    static const std::array<std::array<int, 3>, 4> quarters {
        {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}}};
    std::array<double, 3> L {0.0, 0.0, 0.0};
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) {
            L[j] += w[k] * at[quarters[piece][k]][j];
        }
    }
    return L;
}

std::array<double, 6> ChronoFlexible::Surface::shape(const std::array<double, 3>& L)
{
    return {L[0] * (2.0 * L[0] - 1.0),
            L[1] * (2.0 * L[1] - 1.0),
            L[2] * (2.0 * L[2] - 1.0),
            4.0 * L[0] * L[1],
            4.0 * L[1] * L[2],
            4.0 * L[2] * L[0]};
}

std::array<double, 3> ChronoFlexible::Surface::farthest(const std::array<ch::ChVector3d, 6>& x,
                                                        const ch::ChVector3d& d,
                                                        bool* curve)
{
    // d . x over the face is the quadratic through the nodes' d . x: in
    // s = L[1], t = L[2], g = c0 + c1 s + c2 t + c3 s^2 + c4 s t + c5 t^2,
    // its most on the triangle at a stationary point inside, on an edge or
    // at a corner
    std::array<double, 6> p {};
    for (int k = 0; k < 6; ++k) {
        p[k] = ch::Vdot(d, x[k]);
    }
    const double c0 = p[0];
    const double c1 = -3.0 * p[0] - p[1] + 4.0 * p[3];
    const double c2 = -3.0 * p[0] - p[2] + 4.0 * p[5];
    const double c3 = 2.0 * (p[0] + p[1]) - 4.0 * p[3];
    const double c5 = 2.0 * (p[0] + p[2]) - 4.0 * p[5];
    const double c4 = 4.0 * (p[4] - c0 - 0.5 * c1 - 0.5 * c2 - 0.25 * c3 - 0.25 * c5);
    auto g = [&](double s, double t) {
        return c0 + c1 * s + c2 * t + c3 * s * s + c4 * s * t + c5 * t * t;
    };
    double bestS = 0.0;
    double bestT = 0.0;
    double best = g(0.0, 0.0);
    bool stationary = false;
    auto consider = [&](double s, double t, bool still) {
        if (!(s >= 0.0 && t >= 0.0 && s + t <= 1.0)) {
            return;
        }
        const double v = g(s, t);
        if (v > best) {
            best = v;
            bestS = s;
            bestT = t;
            stationary = still;
        }
    };
    consider(1.0, 0.0, false);
    consider(0.0, 1.0, false);
    // The edges t = 0, s = 0 and s + t = 1, where curved
    if (c3 < 0.0) {
        consider(-c1 / (2.0 * c3), 0.0, true);
    }
    if (c5 < 0.0) {
        consider(0.0, -c2 / (2.0 * c5), true);
    }
    const double a = c3 - c4 + c5;
    if (a < 0.0) {
        const double s = -(c1 - c2 + c4 - 2.0 * c5) / (2.0 * a);
        consider(s, 1.0 - s, true);
    }
    // Inside: the gradient nought, a most where the face curves to d
    const double det = 4.0 * c3 * c5 - c4 * c4;
    if (det > 0.0 && c3 < 0.0) {
        consider((-c1 * 2.0 * c5 + c2 * c4) / det, (-c2 * 2.0 * c3 + c1 * c4) / det, true);
    }
    if (curve) {
        *curve = stationary;
    }
    return {1.0 - bestS - bestT, bestS, bestT};
}

bool ChronoFlexible::Surface::nearest(const ch::ChVector3d& p, Place& place) const
{
    if (corners.empty()) {
        return false;
    }
    // Ring by ring of cells about p's (clamped to the grid): every point of
    // the cells r + 1 rings out is at least r cells from p's nearest point in
    // the grid, and so from p
    int at[3];
    for (int k = 0; k < 3; ++k) {
        at[k] = std::clamp(static_cast<int>(std::floor((p[k] - low[k]) / cell)), 0, count[k] - 1);
    }
    double best = std::numeric_limits<double>::max();
    bool found = false;
    std::size_t nearestOne = 0;
    std::array<double, 3> nearestWeights {};
    const int most = std::max({count[0], count[1], count[2]});
    for (int r = 0; r <= most; ++r) {
        for (int x = at[0] - r; x <= at[0] + r; ++x) {
            for (int y = at[1] - r; y <= at[1] + r; ++y) {
                for (int z = at[2] - r; z <= at[2] + r; ++z) {
                    if (std::max({std::abs(x - at[0]), std::abs(y - at[1]), std::abs(z - at[2])})
                            != r
                        || x < 0 || y < 0 || z < 0 || x >= count[0] || y >= count[1]
                        || z >= count[2]) {
                        continue;
                    }
                    for (std::uint32_t i :
                         cells[(static_cast<std::size_t>(z) * count[1] + y) * count[0] + x]) {
                        const auto& t = corners[i];
                        const auto w = nearestOnTriangle(p, points[t[0]], points[t[1]], points[t[2]]);
                        const ch::ChVector3d q =
                            points[t[0]] * w[0] + points[t[1]] * w[1] + points[t[2]] * w[2];
                        const double d = (q - p).Length();
                        if (d < best) {
                            best = d;
                            found = true;
                            nearestOne = i;
                            nearestWeights = w;
                        }
                    }
                }
            }
        }
        if (found && best <= r * cell) {
            break;
        }
    }
    if (found) {
        onFace(nearestOne, nearestWeights, place);
    }
    return found;
}

ChronoFlexible::Built ChronoFlexible::build(ch::ChSystem& sys,
                                            const Flexible& flexible,
                                            const ch::ChFramed& pose,
                                            const std::vector<Interface>& interfaces)
{
    if (flexible.nodes.empty() || flexible.tetrahedra.empty()) {
        throw std::runtime_error("the flexible body has no mesh");
    }
    if (!(flexible.youngs > 0.0) || !(flexible.density > 0.0)
        || !(flexible.poisson > -1.0 && flexible.poisson < 0.5)) {
        throw std::runtime_error("the flexible body's material is not one");
    }
    Built built;
    built.assembly = std::make_shared<ch::modal::ChModalAssembly>();
    built.assembly->SetReductionType(ch::modal::ChModalAssembly::ReductionType::CRAIG_BAMPTON);
    // The mesh's nodes not moved with the modes as the run goes: each Newton
    // iteration moved them all, half a flexible pendulum's time. Its
    // deformation is the run's modal coordinates' (D4b step 4).
    built.assembly->SetInternalNodesUpdate(false);
    // And the residual modes of its inertia loads, for its deformation and
    // stresses: what the modes left out take of the gravity and the part's
    // motion, statically (D4b, the stress near a free end)
    built.assembly->SetUseInertialResidual(true);
    // Its K_II inverse, for the static modes: Eigen's SparseLU, 0.4 s for the
    // test's bar where Chrono's default SparseQR took 2.2
    built.assembly->SetModalSolver(std::make_shared<ch::ChSolverSparseLU>());
    built.assembly->SetModalAutomaticGravity(true);
    // Its floating frame in the part's axes, not Chrono's principal axes of
    // inertia, whose order and signs (and for a square section the axes) the
    // rounding of the pose sets: the part started elsewhere has the same local
    // matrices, and so an earlier run's modes (KeptModesSolver)
    built.assembly->SetFloatingFrameAxes(pose.GetRot());
    sys.Add(built.assembly);

    auto material = std::make_shared<ch::fea::ChContinuumElastic>(flexible.youngs,
                                                                   flexible.poisson,
                                                                   flexible.density);
    built.mesh = std::make_shared<ch::fea::ChMesh>();
    built.mesh->SetAutomaticGravity(false);
    std::vector<std::shared_ptr<ch::fea::ChNodeFEAxyz>> nodes;
    nodes.reserve(flexible.nodes.size());
    for (const auto& p : flexible.nodes) {
        auto node =
            std::make_shared<ch::fea::ChNodeFEAxyz>(pose.TransformPointLocalToParent(toCh(p)));
        built.mesh->AddNode(node);
        nodes.push_back(node);
    }
    for (const auto& given : flexible.tetrahedra) {
        for (Id n : given) {
            if (n >= nodes.size()) {
                throw std::runtime_error("the flexible body's mesh names a node it does not have");
            }
        }
        // Chrono's tetrahedron takes its corners turning positively, its
        // stiffness of the Jacobian's sign: Netgen's turn the other way. One
        // turned the other way has corners 1 and 2, and so its edges' nodes
        // (0 1) (2 0) and (1 3) (2 3), swapped.
        std::array<Id, 10> tet = given;
        const ch::ChVector3d a = toCh(flexible.nodes[tet[0]]);
        const ch::ChVector3d b = toCh(flexible.nodes[tet[1]]) - a;
        const ch::ChVector3d c = toCh(flexible.nodes[tet[2]]) - a;
        const ch::ChVector3d d = toCh(flexible.nodes[tet[3]]) - a;
        const double six = ch::Vdot(b, ch::Vcross(c, d));
        if (six < 0.0) {
            tet = {given[0], given[2], given[1], given[3], given[6],
                   given[5], given[4], given[7], given[9], given[8]};
        }
        auto element = std::make_shared<ConsistentTet10>();
        element->SetNodes(nodes[tet[0]], nodes[tet[1]], nodes[tet[2]], nodes[tet[3]],
                          nodes[tet[4]], nodes[tet[5]], nodes[tet[6]], nodes[tet[7]],
                          nodes[tet[8]], nodes[tet[9]]);
        element->SetMaterial(material);
        built.mesh->AddElement(element);
        built.mass += flexible.density * std::abs(six) / 6.0;
    }
    built.assembly->AddInternalMesh(built.mesh);

    // A boundary body: a millionth of the mesh's mass, its inertia a sphere's
    // of the mesh's size, so the boundary's own mass is nothing beside the
    // static modes'
    double size = 0.0;
    {
        ch::ChVector3d lo(std::numeric_limits<double>::max());
        ch::ChVector3d hi(-std::numeric_limits<double>::max());
        for (const auto& p : flexible.nodes) {
            lo = ch::Vmin(lo, toCh(p));
            hi = ch::Vmax(hi, toCh(p));
        }
        size = (hi - lo).Length();
    }
    const double boundaryMass = 1e-6 * built.mass;
    const double boundaryInertia = 0.4 * boundaryMass * 0.25 * size * size;
    for (const auto& face : interfaces) {
        auto body = std::make_shared<ch::ChBody>();
        const ch::ChFramed at = face.local >> pose;
        body->SetPos(at.GetPos());
        body->SetRot(at.GetRot());
        body->SetMass(boundaryMass);
        body->SetInertiaXX(ch::ChVector3d(boundaryInertia));
        built.assembly->Add(body);
        built.interfaces.push_back(body);

        std::vector<Id> tied = face.nodes;
        if (tied.empty()) {
            Id nearest = 0;
            double best = std::numeric_limits<double>::max();
            const ch::ChVector3d o = face.local.GetPos();
            for (Id n = 0; n < flexible.nodes.size(); ++n) {
                const double d = (toCh(flexible.nodes[n]) - o).Length2();
                if (d < best) {
                    best = d;
                    nearest = n;
                }
            }
            tied.push_back(nearest);
        }
        for (Id n : tied) {
            if (n >= nodes.size()) {
                throw std::runtime_error(
                    "a flexible body's interface names a node it does not have");
            }
            auto link = std::make_shared<ch::fea::ChLinkNodeFrame>();
            link->Initialize(nodes[n], body);
            built.assembly->AddInternalLink(link);
        }
    }
    built.key = reductionKey(flexible, interfaces, size);
    return built;
}

std::vector<double> ChronoFlexible::reductionNumbers(const Built& built, const std::vector<double>& hz)
{
    std::vector<double> out;
    if (!built.assembly || !built.assembly->GetModalReductionMatrix().size()) {
        return out;
    }
    const auto data = built.assembly->GetReducedData();
    const std::uint64_t h = keyHash(built.key);
    out = {reductionFormat,
           static_cast<double>(h >> 32),
           static_cast<double>(h & 0xffffffffu),
           static_cast<double>(data.num_modes),
           data.scaling_factor_CqI,
           static_cast<double>(hz.size())};
    out.insert(out.end(), hz.begin(), hz.end());
    // Each matrix its rows, its columns and its terms, as it holds them
    auto put = [&](const ch::ChMatrixDynamic<>& m) {
        out.push_back(static_cast<double>(m.rows()));
        out.push_back(static_cast<double>(m.cols()));
        out.insert(out.end(), m.data(), m.data() + m.size());
    };
    put(data.Psi_S);
    put(data.Psi_D);
    put(data.Psi_S_LambdaI);
    put(data.Psi_D_LambdaI);
    put(data.M_red);
    put(data.K_red);
    put(data.R_red);
    put(data.gravity_M_loc);
    put(data.Psi_R);
    return out;
}

bool ChronoFlexible::keepReduction(const Built& built, const std::vector<double>& numbers)
{
    std::size_t at = 0;
    auto count = [&](double& v) {
        if (at >= numbers.size()) {
            return false;
        }
        v = numbers[at++];
        return v >= 0.0 && v < 1e12 && v == std::floor(v);
    };
    double format = 0.0;
    double high = 0.0;
    double low = 0.0;
    double modes = 0.0;
    double frequencies = 0.0;
    if (!count(format) || format != reductionFormat || !count(high) || !count(low)) {
        return false;
    }
    const std::uint64_t h = keyHash(built.key);
    if (high != static_cast<double>(h >> 32) || low != static_cast<double>(h & 0xffffffffu)) {
        return false;
    }
    KeptReduction kept;
    kept.key = built.key;
    auto& data = kept.data;
    if (!count(modes) || at >= numbers.size()) {
        return false;
    }
    data.num_modes = static_cast<unsigned int>(modes);
    data.scaling_factor_CqI = numbers[at++];
    if (!count(frequencies) || at + static_cast<std::size_t>(frequencies) > numbers.size()) {
        return false;
    }
    kept.hz.assign(numbers.begin() + at, numbers.begin() + at + static_cast<std::size_t>(frequencies));
    at += static_cast<std::size_t>(frequencies);
    auto get = [&](ch::ChMatrixDynamic<>& m) {
        double rows = 0.0;
        double cols = 0.0;
        if (!count(rows) || !count(cols)) {
            return false;
        }
        const std::size_t size = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
        if (at + size > numbers.size()) {
            return false;
        }
        m.resize(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
        std::copy(numbers.begin() + at, numbers.begin() + at + size, m.data());
        at += size;
        return true;
    };
    if (!get(data.Psi_S) || !get(data.Psi_D) || !get(data.Psi_S_LambdaI)
        || !get(data.Psi_D_LambdaI) || !get(data.M_red) || !get(data.K_red) || !get(data.R_red)
        || !get(data.gravity_M_loc) || !get(data.Psi_R) || at != numbers.size()) {
        return false;
    }
    // Psi = [I 0; Psi_S Psi_D; Psi_S_LambdaI Psi_D_LambdaI] (GetModalReductionMatrix())
    const Eigen::Index nB = data.Psi_S.cols();
    const Eigen::Index nI = data.Psi_S.rows();
    const Eigen::Index nL = data.Psi_S_LambdaI.rows();
    const Eigen::Index nMod = data.num_modes;
    if (data.Psi_D.rows() != nI || data.Psi_D.cols() != nMod
        || (nL && (data.Psi_S_LambdaI.cols() != nB || data.Psi_D_LambdaI.rows() != nL
                   || data.Psi_D_LambdaI.cols() != nMod))) {
        return false;
    }
    data.Psi.setZero(nB + nI + nL, nB + nMod);
    data.Psi.topLeftCorner(nB, nB).setIdentity();
    data.Psi.block(nB, 0, nI, nB) = data.Psi_S;
    data.Psi.block(nB, nB, nI, nMod) = data.Psi_D;
    if (nL) {
        data.Psi.block(nB + nI, 0, nL, nB) = data.Psi_S_LambdaI;
        data.Psi.block(nB + nI, nB, nL, nMod) = data.Psi_D_LambdaI;
    }
    std::scoped_lock lock(KeptModesSolver::guard());
    auto& reductions = keptReductions();
    for (const auto& one : reductions) {
        if (sameReduction(one.key, built.key)) {
            return true;
        }
    }
    reductions.push_front(std::move(kept));
    if (reductions.size() > 2) {
        reductions.pop_back();
    }
    return true;
}

std::vector<double>
ChronoFlexible::reduce(ch::ChSystem& sys, Built& built, const Flexible& flexible)
{
    // The document's, for the part as it is now
    if (flexible.reduction && !flexible.reduction->empty()) {
        keepReduction(built, *flexible.reduction);
    }
    sys.Setup();
    sys.Update(ch::UpdateFlags::UPDATE_ALL);
    // The symmetric eigensolver (Spectra's Krylov-Schur, a real LU): the
    // complex one took four times as long (our Chrono takes either); the
    // modes of an earlier run of the part kept (KeptModesSolver)
    // -- or the whole reduction of one, kept (keptReductions())
    {
        std::scoped_lock lock(KeptModesSolver::guard());
        auto& kept = keptReductions();
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (sameReduction(it->key, built.key)) {
                std::rotate(kept.begin(), it, std::next(it));
                built.assembly->DoModalReduction(kept.front().data);
                ++KeptModesSolver::taken();
                return kept.front().hz;
            }
        }
    }
    // The modes found about 0 Hz, the shift at which our Chrono's Craig-Bampton
    // reduction hands the eigensolver its K_IIc factored for the static modes:
    // the strip's 6 s factorization made once, not twice
    auto eigen = std::make_shared<KeptModesSolver>();
    ch::modal::ChModalSolverUndamped<KeptModesSolver> solver(
        std::max(flexible.modes, 1), 0.0, true, false, eigen);
    std::vector<double> hz;
    built.assembly->DoModalReduction(
        solver, StiffnessDamping(flexible.damping, flexible.dampingFrequency, hz));
    std::scoped_lock lock(KeptModesSolver::guard());
    auto& kept = keptReductions();
    kept.push_front({built.key, built.assembly->GetReducedData(), hz});
    if (kept.size() > 2) {
        kept.pop_back();
    }
    return hz;
}

bool Chrono::hasFlexibleBodies()
{
    return true;
}

std::size_t Chrono::flexibleModesKept()
{
    std::scoped_lock lock(KeptModesSolver::guard());
    return KeptModesSolver::taken();
}

void Chrono::forgetFlexibleReductions()
{
    KeptModesSolver::forget();
    std::scoped_lock lock(KeptModesSolver::guard());
    keptReductions().clear();
}

std::vector<double>
Chrono::flexibleFrequencies(const Model& model, std::size_t index, std::string* error)
{
    try {
        if (!model.dynamics || index >= model.dynamics->flexibles.size()) {
            throw std::runtime_error("no such flexible body");
        }
        const Model si = scaled(model, SI::length);
        const Flexible& flexible = si.dynamics->flexibles[index];
        if (flexible.body >= si.bodies.size()) {
            throw std::runtime_error("the flexible body is on no body");
        }
        // Its interfaces as a run takes them (ChronoSession::load()): the
        // frames on it but for its contact shapes', which ride its first
        // interface or, with contact faces named, an interface of those
        // faces' nodes at their centre. With a contact shape's frame an
        // interface of its own, at the node nearest, the modes were not the
        // run's, and the reduction of a hanger on a pin failed to factor
        std::vector<Id> touched;
        for (const auto& shape : si.dynamics->contactShapes) {
            if (shape.frame < si.frames.size() && si.frames[shape.frame].body == flexible.body) {
                touched.push_back(shape.frame);
            }
        }
        std::vector<ChronoFlexible::Interface> interfaces;
        for (const auto& face : flexible.interfaces) {
            if (face.frame >= si.frames.size()) {
                throw std::runtime_error("a flexible body's interface is on no frame");
            }
            if (std::find(touched.begin(), touched.end(), face.frame) != touched.end()) {
                continue;
            }
            const Base::Placement& local = si.frames[face.frame].local;
            const Base::Vector3d p = local.getPosition();
            double x, y, z, w;
            local.getRotation().getValue(x, y, z, w);
            interfaces.push_back({ch::ChFramed(ch::ChVector3d(p.x, p.y, p.z),
                                               ch::ChQuaterniond(w, x, y, z)),
                                  face.nodes});
        }
        // Held by nothing, it has no interface to reduce it about: the
        // reduction read storage that was not its own
        if (interfaces.empty()) {
            throw std::runtime_error("nothing holds the flexible body");
        }
        if (!touched.empty() && !flexible.contactNodes.empty()) {
            ch::ChVector3d centre;
            std::size_t counted = 0;
            for (Id n : flexible.contactNodes) {
                if (n < flexible.nodes.size()) {
                    centre += toCh(flexible.nodes[n]);
                    ++counted;
                }
            }
            ch::ChFramed contactAt;
            if (counted) {
                contactAt.SetPos(centre / static_cast<double>(counted));
            }
            interfaces.push_back({contactAt, flexible.contactNodes});
        }
        const Base::Placement& at = si.bodies[flexible.body].placement;
        double x, y, z, w;
        at.getRotation().getValue(x, y, z, w);
        const ch::ChFramed pose(toCh(at.getPosition()), ch::ChQuaterniond(w, x, y, z));
        ch::ChSystemNSC sys;
        sys.SetGravitationalAcceleration(ch::ChVector3d(0, 0, 0));
        ChronoFlexible::Built built = ChronoFlexible::build(sys, flexible, pose, interfaces);
        return ChronoFlexible::reduce(sys, built, flexible);
    }
    catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return {};
    }
}
