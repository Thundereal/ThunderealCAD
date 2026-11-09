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
#include <deque>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>

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
        ChElementTetraCorot_10::ComputeKRMmatricesGlobal(H, Kfactor, Rfactor, 0.0);
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

}  // namespace

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

std::vector<double>
ChronoFlexible::reduce(ch::ChSystem& sys, Built& built, const Flexible& flexible)
{
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
    auto eigen = std::make_shared<KeptModesSolver>();
    ch::modal::ChModalSolverUndamped<KeptModesSolver> solver(
        std::max(flexible.modes, 1), 1e-5, true, false, eigen);
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
        std::vector<ChronoFlexible::Interface> interfaces;
        for (const auto& face : flexible.interfaces) {
            if (face.frame >= si.frames.size()) {
                throw std::runtime_error("a flexible body's interface is on no frame");
            }
            const Base::Placement& local = si.frames[face.frame].local;
            const Base::Vector3d p = local.getPosition();
            double x, y, z, w;
            local.getRotation().getValue(x, y, z, w);
            interfaces.push_back({ch::ChFramed(ch::ChVector3d(p.x, p.y, p.z),
                                               ch::ChQuaterniond(w, x, y, z)),
                                  face.nodes});
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
