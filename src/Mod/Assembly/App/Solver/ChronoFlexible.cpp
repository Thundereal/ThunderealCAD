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
// - Each mode damped at one ratio of critical, the reduced system's own
//   modes -- the boundary's static modes with the dynamic ones, the rigid
//   motions at no frequency undamped: R = M V diag(2 z w) V^T M, V its
//   eigenvectors normalised by the mass. Chrono's ChModalDampingFactorRmm
//   leaves the static modes, what the interfaces do against each other,
//   undamped; its ChModalDampingFactorAssembly says it is not validated
//   and takes six modes.
// - The interfaces: a boundary ChBody each, of negligible mass, its nodes
//   tied to it by ChLinkNodeFrame, internal (an RBE2 spider).

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
#include <chrono_modal/ChUnsymGenEigenvalueSolver.h>

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

// Also keeps the modes' frequencies, which the reduction forgets once done
class ModalRatio: public ch::modal::ChModalDamping
{
public:
    ModalRatio(double ratio, std::vector<double>& hz)
        : ratio(ratio)
        , hz(hz)
    {}

    void ComputeR(ch::modal::ChModalAssembly& assembly,
                  const ch::ChMatrixDynamic<>& modalM,
                  const ch::ChMatrixDynamic<>& modalK,
                  const ch::ChMatrixDynamic<>&,
                  ch::ChMatrixDynamic<>& modalR) const override
    {
        const auto& f = assembly.GetUndampedFrequencies();
        hz.assign(f.data(), f.data() + f.size());
        const Eigen::MatrixXd m = 0.5 * (modalM + modalM.transpose());
        const Eigen::MatrixXd k = 0.5 * (modalK + modalK.transpose());
        modalR.setZero(m.rows(), m.cols());
        if (ratio <= 0.0 || m.rows() == 0) {
            return;
        }
        Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXd> modes(k, m);
        if (modes.info() != Eigen::Success) {
            return;
        }
        const Eigen::MatrixXd mv = m * modes.eigenvectors();
        Eigen::VectorXd d(m.rows());
        for (Eigen::Index i = 0; i < d.size(); ++i) {
            d(i) = 2.0 * ratio * std::sqrt(std::max(modes.eigenvalues()(i), 0.0));
        }
        modalR = mv * d.asDiagonal() * mv.transpose();
    }

private:
    double ratio;
    std::vector<double>& hz;
};

ch::ChVector3d toCh(const Base::Vector3d& v)
{
    return {v.x, v.y, v.z};
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
    built.assembly->SetInternalNodesUpdate(true);
    // Its K_II inverse, for the static modes: Eigen's SparseLU, 0.4 s for the
    // test's bar where Chrono's default SparseQR took 2.2
    built.assembly->SetModalSolver(std::make_shared<ch::ChSolverSparseLU>());
    built.assembly->SetModalAutomaticGravity(true);
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
    for (const auto& tet : flexible.tetrahedra) {
        for (Id n : tet) {
            if (n >= nodes.size()) {
                throw std::runtime_error("the flexible body's mesh names a node it does not have");
            }
        }
        auto element = std::make_shared<ConsistentTet10>();
        element->SetNodes(nodes[tet[0]], nodes[tet[1]], nodes[tet[2]], nodes[tet[3]],
                          nodes[tet[4]], nodes[tet[5]], nodes[tet[6]], nodes[tet[7]],
                          nodes[tet[8]], nodes[tet[9]]);
        element->SetMaterial(material);
        built.mesh->AddElement(element);
        // Its volume, from the corners
        const ch::ChVector3d a = toCh(flexible.nodes[tet[0]]);
        const double six = std::abs(ch::Vdot(toCh(flexible.nodes[tet[1]]) - a,
                                             ch::Vcross(toCh(flexible.nodes[tet[2]]) - a,
                                                        toCh(flexible.nodes[tet[3]]) - a)));
        built.mass += flexible.density * six / 6.0;
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
    return built;
}

std::vector<double>
ChronoFlexible::reduce(ch::ChSystem& sys, Built& built, const Flexible& flexible)
{
    sys.Setup();
    sys.Update(ch::UpdateFlags::UPDATE_ALL);
    auto eigen = std::make_shared<ch::modal::ChUnsymGenEigenvalueSolverKrylovSchur>();
    // The modal assembly takes the complex eigensolver only (its eigenvectors are complex)
    ch::modal::ChModalSolverUndamped<ch::modal::ChUnsymGenEigenvalueSolverKrylovSchur> solver(
        std::max(flexible.modes, 1), 1e-5, true, false, eigen);
    std::vector<double> hz;
    built.assembly->DoModalReduction(solver, ModalRatio(flexible.damping, hz));
    return hz;
}

bool Chrono::hasFlexibleBodies()
{
    return true;
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
