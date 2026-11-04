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

#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>

#include <App/Application.h>
#include <Base/Matrix.h>
#include <Base/Tools.h>

#include <OndselSolver/CREATE.h>
#include <OndselSolver/ASMTAngleJoint.h>
#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTCylSphJoint.h>
#include <OndselSolver/ASMTCylindricalJoint.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTGearJoint.h>
#include <OndselSolver/ASMTGeneralMotion.h>
#include <OndselSolver/ASMTJoint.h>
#include <OndselSolver/ASMTLineInPlaneJoint.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTParallelAxesJoint.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTPerpendicularJoint.h>
#include <OndselSolver/ASMTPlanarJoint.h>
#include <OndselSolver/ASMTPointInLineJoint.h>
#include <OndselSolver/ASMTPointInPlaneJoint.h>
#include <OndselSolver/ASMTPointOnCurveJoint.h>
#include <OndselSolver/ASMTPrincipalMassMarker.h>
#include <OndselSolver/ASMTRackPinionJoint.h>
#include <OndselSolver/ASMTRevCylJoint.h>
#include <OndselSolver/ASMTRevoluteJoint.h>
#include <OndselSolver/ASMTRotationLimit.h>
#include <OndselSolver/ASMTRotationalMotion.h>
#include <OndselSolver/ASMTScrewJoint.h>
#include <OndselSolver/ASMTSimulationParameters.h>
#include <OndselSolver/ASMTSphSphJoint.h>
#include <OndselSolver/ASMTSphericalJoint.h>
#include <OndselSolver/ASMTTranslationLimit.h>
#include <OndselSolver/ASMTTranslationalJoint.h>
#include <OndselSolver/ASMTTranslationalMotion.h>
#include <OndselSolver/Constraint.h>
#include <OndselSolver/ExternalFunction.h>
#include <OndselSolver/DiagonalMatrix.h>
#include <OndselSolver/FullColumn.h>
#include <OndselSolver/Joint.h>
#include <OndselSolver/Part.h>
#include <OndselSolver/RedundantConstraint.h>
#include <OndselSolver/System.h>

#include "OndselBackend.h"

using namespace Assembly::Solver;

namespace
{

// The assembly's name: the root of every marker path.
const std::string assemblyName = "OndselAssembly";

void setRotation(const std::shared_ptr<MbD::ASMTSpatialItem>& item, const Base::Rotation& rot)
{
    // As AssemblyObject::makeMbdPart / makeMbdMarker wrote it: the matrix rows.
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    item->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
}

std::shared_ptr<MbD::ASMTPart> makePart(const std::string& name, const Body& body)
{
    auto mbdPart = MbD::CREATE<MbD::ASMTPart>::With();
    mbdPart->setName(name);

    auto massMarker = MbD::CREATE<MbD::ASMTPrincipalMassMarker>::With();
    massMarker->setMass(body.mass.mass);
    massMarker->setDensity(1.0);
    massMarker->setMomentOfInertias(
        body.mass.principal[0],
        body.mass.principal[1],
        body.mass.principal[2]
    );
    // Today every body has its mass frame on the part frame; written only when
    // a builder gives one, so the default model is what AssemblyObject wrote.
    if (body.mass.com != Base::Vector3d()) {
        massMarker->setPosition3D(body.mass.com.x, body.mass.com.y, body.mass.com.z);
    }
    if (!body.mass.principalAxes.isIdentity()) {
        setRotation(massMarker, body.mass.principalAxes);
    }
    mbdPart->setPrincipalMassMarker(massMarker);

    Base::Vector3d pos = body.placement.getPosition();
    mbdPart->setPosition3D(pos.x, pos.y, pos.z);
    setRotation(mbdPart, body.placement.getRotation());
    return mbdPart;
}

std::shared_ptr<MbD::ASMTMarker> makeMarker(const std::string& name, const Base::Placement& plc)
{
    auto mbdMarker = MbD::CREATE<MbD::ASMTMarker>::With();
    mbdMarker->setName(name);
    Base::Vector3d pos = plc.getPosition();
    mbdMarker->setPosition3D(pos.x, pos.y, pos.z);
    setRotation(mbdMarker, plc.getRotation());
    return mbdMarker;
}

std::shared_ptr<MbD::ASMTJoint> makeJoint(const Joint& joint)
{
    switch (joint.kind) {
        case JointKind::Fixed:
            return MbD::CREATE<MbD::ASMTFixedJoint>::With();
        case JointKind::Revolute:
            return MbD::CREATE<MbD::ASMTRevoluteJoint>::With();
        case JointKind::Cylindrical:
            return MbD::CREATE<MbD::ASMTCylindricalJoint>::With();
        case JointKind::Prismatic:
            return MbD::CREATE<MbD::ASMTTranslationalJoint>::With();
        case JointKind::Spherical:
            return MbD::CREATE<MbD::ASMTSphericalJoint>::With();
        case JointKind::Planar: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTPlanarJoint>::With();
            mbdJoint->offset = joint.value;
            return mbdJoint;
        }
        case JointKind::ParallelZ:
            return MbD::CREATE<MbD::ASMTParallelAxesJoint>::With();
        case JointKind::PerpendicularZ:
            return MbD::CREATE<MbD::ASMTPerpendicularJoint>::With();
        case JointKind::AngleZ: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTAngleJoint>::With();
            mbdJoint->theIzJz = joint.value;
            return mbdJoint;
        }
        case JointKind::PointInPlane: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = joint.value;
            return mbdJoint;
        }
        case JointKind::LineInPlane: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTLineInPlaneJoint>::With();
            mbdJoint->offset = joint.value;
            return mbdJoint;
        }
        case JointKind::PointOnLine:
            return MbD::CREATE<MbD::ASMTPointInLineJoint>::With();
        case JointKind::PointPointDistance: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = joint.value;
            return mbdJoint;
        }
        case JointKind::PointLineDistance: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = joint.value;
            return mbdJoint;
        }
        case JointKind::LineLineDistance: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = joint.value;
            return mbdJoint;
        }
        case JointKind::Gear:
        case JointKind::CoaxialGear: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTGearJoint>::With();
            mbdJoint->radiusI = joint.value;
            mbdJoint->radiusJ = joint.value2;
            return mbdJoint;
        }
        case JointKind::RackPinion: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTRackPinionJoint>::With();
            mbdJoint->pitchRadius = joint.value;
            return mbdJoint;
        }
        case JointKind::Screw: {
            auto mbdJoint = MbD::CREATE<MbD::ASMTScrewJoint>::With();
            mbdJoint->pitch = joint.value;
            return mbdJoint;
        }
        case JointKind::PointOnCurve:  // needs the model's curve: toAsmt()
        case JointKind::Count_:
            break;
    }
    return nullptr;
}

// A PointOnCurve's equations as OndselSolver's host function
// (MbD::PointOnCurveFunction): the neutral projector and rows (Curve.h). The
// closest point is followed from one call to the next -- OndselSolver has no
// notion of an accepted pose, its Newton iterates and its kinematic run's
// steps simply follow each other -- and the first call searches the whole
// curve. The two equations of a joint share the function, so the second
// call at a point projects from where the first ended.
MbD::PointOnCurveFunction curveFunction(std::shared_ptr<const CurveGeometry> curve, double offset)
{
    struct Tracker
    {
        bool started = false;
        double u = 0.0;
    };
    auto tracker = std::make_shared<Tracker>();
    return [curve = std::move(curve), offset, tracker](
               const std::array<double, 3>& x,
               MbD::PointOnCurveValues& values
           ) {
        const Base::Vector3d point(x[0], x[1], x[2]);
        if (!tracker->started) {
            tracker->u = projectOnCurveGlobally(*curve, point).u;
            tracker->started = true;
        }
        const PointOnCurveRows rows = pointOnCurveRows(*curve, offset, point, tracker->u);
        tracker->u = rows.u;
        values.value = rows.value;
        values.gradient = rows.gradient;
        values.hessian = rows.hessian;
    };
}

// A PointOnCurve as OndselSolver's PointOnCurveJoint, the curve evaluated by
// the neutral layer; the ASMT text records the curve's description.
std::shared_ptr<MbD::ASMTJoint> makeCurveJoint(const Model& model, const Joint& joint)
{
    if (joint.curve >= model.curves.size() || !model.curves[joint.curve].geometry) {
        throw std::runtime_error("a point-on-curve joint has no curve");
    }
    const auto& curve = model.curves[joint.curve].geometry;
    auto mbdJoint = MbD::ASMTPointOnCurveJoint::With();
    std::string text = curve->describe();
    if (joint.value != 0.0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " offset %.17g", joint.value);
        text += buf;
    }
    mbdJoint->setCurve(text, curveFunction(curve, joint.value));
    return mbdJoint;
}

std::string toleranceString(double tolerance)
{
    // AssemblyObject wrote the literal "1.0e-9".
    if (tolerance == 1.0e-9) {
        return "1.0e-9";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", tolerance);
    return buf;
}

template<typename LimitT>
void addLimitBound(
    MbD::ASMTAssembly& assembly,
    const std::string& name,
    const std::string& markerI,
    const std::string& markerJ,
    const char* type,
    const std::string& value,
    const std::string& tolerance
)
{
    auto limit = LimitT::With();
    limit->setName(name);
    limit->setMarkerI(markerI);
    limit->setMarkerJ(markerJ);
    limit->settype(type);
    limit->setlimit(value);
    limit->settol(tolerance);
    assembly.addLimit(limit);
}

// A driver's formula as OndselSolver's ExternalFunction callback: the n-th
// time derivative at t, from the neutral evaluator, so that runKINEMATIC
// neither parses nor differentiates the text (it did both before, and got
// some derivatives wrong or had none; docs/AssemblySolverInterface.md sec
// 6.12). It asks for n = 0, 1, 2; NaN beyond. Empty when the formula has no
// evaluator (its text does not parse): the motion then reads the text, as
// before. The text is set either way: it is what the ASMT file records.
MbD::ExternalTimeFunction externalFunction(const Formula& formula)
{
    FormulaFunction evaluate = formula.evaluator();
    if (!evaluate) {
        return {};
    }
    return [evaluate = std::move(evaluate)](double time, int order) {
        const FormulaValue v = evaluate(time);
        switch (order) {
            case 0:
                return v.value;
            case 1:
                return v.rate;
            case 2:
                return v.acceleration;
            default:
                return std::numeric_limits<double>::quiet_NaN();
        }
    };
}

}  // namespace

std::string Ondsel::limitValueString(LimitKind kind, double value)
{
    if (kind == LimitKind::RotationZ) {
        // The model carries radians; ASMT got the degrees as an expression.
        return std::to_string(Base::toDegrees(value)) + "*pi/180.0";
    }
    return std::to_string(value);
}

Base::Placement Ondsel::partPlacement(const std::shared_ptr<MbD::ASMTPart>& part)
{
    if (!part) {
        return {};
    }
    double x, y, z;
    part->getPosition3D(x, y, z);
    double q0, q1, q2, q3;
    part->getQuarternions(q3, q0, q1, q2);
    return Base::Placement(Base::Vector3d(x, y, z), Base::Rotation(q0, q1, q2, q3));
}

Ondsel::AsmtModel Ondsel::toAsmt(const Model& model, const SimulationSpec* simulation, bool debug)
{
    AsmtModel result;
    auto assembly = MbD::CREATE<MbD::ASMTAssembly>::With();
    assembly->setName(assemblyName);
    assembly->setDebug(debug);
    result.assembly = assembly;

    // Bodies in insertion order: the order OndselSolver numbers its columns in.
    result.parts.reserve(model.bodies.size());
    for (const auto& body : model.bodies) {
        auto mbdPart = makePart(model.sourceName(body.source), body);
        assembly->addPart(mbdPart);
        result.parts.push_back(mbdPart);
    }

    // Frames in creation order, so each part lists its markers as before; a
    // frame on the ground is a marker of the assembly itself.
    std::vector<std::string> markerPaths;
    markerPaths.reserve(model.frames.size());
    for (const auto& frame : model.frames) {
        const std::string& name = model.sourceName(frame.label);
        auto mbdMarker = makeMarker(name, frame.local);
        if (frame.body == NoId || frame.body >= result.parts.size()) {
            assembly->addMarker(mbdMarker);
            markerPaths.push_back("/" + assemblyName + "/" + name);
        }
        else {
            const auto& mbdPart = result.parts[frame.body];
            mbdPart->addMarker(mbdMarker);
            markerPaths.push_back("/" + assemblyName + "/" + mbdPart->name + "/" + name);
        }
    }
    auto path = [&](Id frame) -> const std::string& {
        static const std::string none;
        return frame < markerPaths.size() ? markerPaths[frame] : none;
    };

    // Grounds first: AssemblyObject fixed the grounded parts before it added
    // any joint. Each is a fixed joint named after the grounded object.
    for (const auto& ground : model.grounds) {
        auto mbdJoint = MbD::CREATE<MbD::ASMTFixedJoint>::With();
        mbdJoint->setName(model.sourceName(ground.source));
        mbdJoint->setMarkerI(path(ground.frameI));
        mbdJoint->setMarkerJ(path(ground.frameJ));
        assembly->addJoint(mbdJoint);
    }

    result.joints.resize(model.joints.size());
    for (const auto& joint : model.joints) {
        auto mbdJoint = joint.kind == JointKind::PointOnCurve ? makeCurveJoint(model, joint)
                                                               : makeJoint(joint);
        if (!mbdJoint) {
            continue;
        }
        result.joints[joint.id] = mbdJoint;
        if (joint.kind == JointKind::CoaxialGear && joint.frameK != NoId) {
            std::static_pointer_cast<MbD::ASMTGearJoint>(mbdJoint)->setMarkerK(path(joint.frameK));
        }
        mbdJoint->setName(model.sourceName(joint.source));
        mbdJoint->setMarkerI(path(joint.frameI));
        mbdJoint->setMarkerJ(path(joint.frameJ));
        assembly->addJoint(mbdJoint);
    }

    for (const auto& limit : model.limits) {
        const std::string& name = model.sourceName(limit.source);
        const std::string tol = toleranceString(limit.tolerance);
        const bool rotation = limit.kind == LimitKind::RotationZ;
        if (limit.min) {
            const std::string value = limitValueString(limit.kind, *limit.min);
            if (rotation) {
                addLimitBound<MbD::ASMTRotationLimit>(*assembly, name + "-LimitRotMin", path(limit.frameI), path(limit.frameJ), "=>", value, tol);
            }
            else {
                addLimitBound<MbD::ASMTTranslationLimit>(*assembly, name + "-LimitLenMin", path(limit.frameI), path(limit.frameJ), "=>", value, tol);
            }
        }
        if (limit.max) {
            const std::string value = limitValueString(limit.kind, *limit.max);
            if (rotation) {
                addLimitBound<MbD::ASMTRotationLimit>(*assembly, name + "-LimitRotMax", path(limit.frameI), path(limit.frameJ), "=<", value, tol);
            }
            else {
                addLimitBound<MbD::ASMTTranslationLimit>(*assembly, name + "-LimitLenMax", path(limit.frameI), path(limit.frameJ), "=<", value, tol);
            }
        }
    }

    for (const auto& driver : model.drivers) {
        const std::string& name = model.sourceName(driver.source);
        switch (driver.kind) {
            case DriverKind::Both: {
                auto motion = MbD::CREATE<MbD::ASMTGeneralMotion>::With();
                motion->setName(name + "-ScrewMotion");
                motion->setMarkerI(path(driver.frameI));
                motion->setMarkerJ(path(driver.frameJ));
                motion->setrIJI(2, driver.translation.text, externalFunction(driver.translation));
                motion->setangIJJ(2, driver.rotation.text, externalFunction(driver.rotation));
                assembly->addMotion(motion);
                break;
            }
            case DriverKind::RotationZ: {
                auto motion = MbD::CREATE<MbD::ASMTRotationalMotion>::With();
                motion->setName(name + "-AngularMotion");
                motion->setMarkerI(path(driver.frameI));
                motion->setMarkerJ(path(driver.frameJ));
                motion->setRotationZ(driver.rotation.text, externalFunction(driver.rotation));
                assembly->addMotion(motion);
                break;
            }
            case DriverKind::TranslationZ: {
                auto motion = MbD::CREATE<MbD::ASMTTranslationalMotion>::With();
                motion->setName(name + "-LinearMotion");
                motion->setMarkerI(path(driver.frameI));
                motion->setMarkerJ(path(driver.frameJ));
                motion->setTranslationZ(driver.translation.text, externalFunction(driver.translation));
                assembly->addMotion(motion);
                break;
            }
        }
    }

    if (simulation) {
        auto mbdSim = assembly->simulationParameters;
        mbdSim->settstart(simulation->tStart);
        mbdSim->settend(simulation->tEnd);
        mbdSim->sethout(simulation->tOutput);
        mbdSim->sethmin(simulation->hMin);
        mbdSim->sethmax(simulation->hMax);
        mbdSim->seterrorTol(simulation->errorTolerance);
    }
    return result;
}

// ------------------------------------------------------------------ backend

std::string OndselBackend::name() const
{
    return "Ondsel";
}

std::string OndselBackend::label() const
{
    return "OndselSolver";
}

Capabilities OndselBackend::capabilities() const
{
    Capabilities caps;
    caps.joints.set();
    caps.limits = true;
    caps.drivers = true;
    caps.kinematics = true;
    caps.drag = true;
    caps.redundancy = true;
    caps.conflicts = true;
    caps.nativeFormat = "asmt";
    return caps;
}

std::unique_ptr<Session> OndselBackend::createSession()
{
    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    // SolverDumpFile: a path, unset by default; every model handed to the
    // solver is written there as ASMT text (the identity gate reads it).
    return std::make_unique<OndselSession>(
        hGrp->GetBool("LogSolverDebug", false),
        hGrp->GetASCII("SolverDumpFile", "")
    );
}

// ------------------------------------------------------------------ session

namespace
{

// A redundant equation whose residual is still above this after the solve is a
// conflict: the solver dropped it to get a full-rank system, and the solution
// of the remaining equations does not satisfy it. Residuals are in model units
// (mm, OndselSolver's mbdUnits are all 1; mm^2 for the squared distances of
// DistancexyConstraintIJ) for displacement equations and dimensionless for
// direction cosines; a satisfied equation ends near 1e-12.
constexpr double conflictResidualTolerance = 1.0e-6;

// Re-evaluates the equation OndselSolver set aside as redundant at the solved
// configuration. RedundantConstraint stubs out every update, so the wrapped
// equation's aG is stale until asked here.
bool isConflicting(const std::shared_ptr<MbD::RedundantConstraint>& redundant)
{
    if (!redundant || !redundant->constraint) {
        return false;
    }
    try {
        redundant->constraint->postPosICIteration();
    }
    catch (...) {
        return false;
    }
    const double residual = redundant->constraint->aG;
    return !std::isfinite(residual) || std::abs(residual) > conflictResidualTolerance;
}

void setRotationMatrix(MbD::ASMTPart& part, const Base::Rotation& rot)
{
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    part.updateMbDFromRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
}

}  // namespace

OndselSession::OndselSession(bool debug, std::string dumpFile)
    : debug(debug)
    , dumpFile(std::move(dumpFile))
{}

OndselSession::~OndselSession() = default;

void OndselSession::dump() const
{
    if (!dumpFile.empty() && current.assembly) {
        current.assembly->outputFile(dumpFile);
    }
}

std::vector<BodyPose> OndselSession::refreshPoses()
{
    std::vector<BodyPose> moved;
    currentPoses.resize(current.parts.size());
    for (std::size_t i = 0; i < current.parts.size(); ++i) {
        Base::Placement plc = Ondsel::partPlacement(current.parts[i]);
        if (!plc.isSame(currentPoses[i])) {
            moved.push_back({static_cast<Id>(i), plc});
        }
        currentPoses[i] = plc;
    }
    return moved;
}

Status OndselSession::setPoses(const std::vector<BodyPose>& poses)
{
    if (!current.assembly) {
        return Status::Failed;
    }
    try {
        for (const auto& pose : poses) {
            if (pose.body >= current.parts.size() || !current.parts[pose.body]) {
                continue;
            }
            const auto& part = current.parts[pose.body];
            // As a drag step's target, and the pose a next step starts from
            // and goes back to
            const Base::Vector3d pos = pose.placement.getPosition();
            part->updateMbDFromPosition3D(
                std::make_shared<MbD::FullColumn<double>>(MbD::ListD {pos.x, pos.y, pos.z}));
            setRotationMatrix(*part, pose.placement.getRotation());
            part->oldPos3D = part->position3D;
            part->oldRotMat = part->rotationMatrix;
        }
        refreshPoses();
    }
    catch (const std::exception&) {
        return Status::Failed;
    }
    return Status::Ok;
}

std::vector<BodyPose> OndselSession::poses() const
{
    std::vector<BodyPose> result;
    result.reserve(currentPoses.size());
    for (std::size_t i = 0; i < currentPoses.size(); ++i) {
        result.push_back({static_cast<Id>(i), currentPoses[i]});
    }
    return result;
}

Status OndselSession::load(const Model& newModel)
{
    currentPoses.clear();
    try {
        model = newModel;
        current = Ondsel::toAsmt(model, nullptr, debug);
    }
    catch (...) {
        current = {};
        return Status::Failed;
    }
    dump();
    // The poses the solver starts from, as it reads them back.
    refreshPoses();
    return Status::Ok;
}

Status OndselSession::update(const Model& newModel)
{
    if (!current.assembly || !sameExceptDragWeights(model, newModel)) {
        return Status::NeedsReload;
    }
    // Drag weights reach the solver in beginDrag, never the ASMT model.
    for (std::size_t i = 0; i < model.bodies.size(); ++i) {
        model.bodies[i].dragWeight = newModel.bodies[i].dragWeight;
    }
    return Status::Ok;
}

Result OndselSession::solve(const SolveOptions& options)
{
    (void)options;
    Result result;
    if (!current.assembly) {
        result.message = "no model loaded";
        return result;
    }
    try {
        current.assembly->runPreDrag();
    }
    catch (const std::exception& e) {
        result.message = e.what();
        return result;
    }
    catch (...) {
        result.message = "unhandled exception";
        return result;
    }
    result.status = Status::Ok;
    result.moved = refreshPoses();
    return result;
}

Status OndselSession::beginDrag(const std::vector<Id>& bodies, const DragOptions& options)
{
    // The bodies Fixed joints hold together are one body of the model already
    // (groupedRigidly), so the option has nothing left to do here.
    (void)options;

    // Every solver body is built with mass 1 and unit inertia, and that stays
    // so for the static solve before the drag and the one endDrag() runs: in an
    // under-constrained assembly, OndselSolver's mass-weighted minimal motion
    // decides which part moves, so changing those masses would change what a
    // solve does. During the drag steps the heavier weights of the dragged
    // bodies are what make them follow the mouse and the others give way:
    // PosICDragNewtonRaphson's weights come from Part::fillqsuWeights, which
    // scales them by m / max(m) and aJ / max(m). So the weights go on the MbD
    // bodies the drag steps solve, after the pre-drag solve built them, and the
    // ASMT model keeps mass 1.
    if (!current.assembly || !current.assembly->mbdSystem) {
        return Status::Failed;
    }
    for (Id body : bodies) {
        if (body >= model.bodies.size() || body >= current.parts.size() || !current.parts[body]) {
            continue;
        }
        const double weight = model.bodies[body].dragWeight;
        if (weight <= 0.0 || weight == 1.0) {
            continue;
        }
        auto mbdPart = std::dynamic_pointer_cast<MbD::Part>(current.parts[body]->mbdObject);
        if (!mbdPart) {
            continue;
        }
        mbdPart->m = weight * model.bodies[body].mass.mass;
        // A heavier body is heavier in rotation too.
        const auto& principal = model.bodies[body].mass.principal;
        auto aJ = std::make_shared<MbD::DiagonalMatrix<double>>(3);
        for (std::size_t k = 0; k < 3; ++k) {
            aJ->at(k) = weight * principal[k];
        }
        mbdPart->aJ = aJ;
    }
    return Status::Ok;
}

Result OndselSession::dragStep(const std::vector<BodyPose>& targets)
{
    Result result;
    if (!current.assembly) {
        result.message = "no model loaded";
        return result;
    }

    bool restore = false;
    try {
        std::vector<std::shared_ptr<MbD::ASMTPart>> dragParts;
        for (const auto& target : targets) {
            if (target.body >= current.parts.size() || !current.parts[target.body]) {
                continue;
            }
            const auto& mbdPart = current.parts[target.body];
            dragParts.push_back(mbdPart);

            Base::Vector3d pos = target.placement.getPosition();
            mbdPart->updateMbDFromPosition3D(
                std::make_shared<MbD::FullColumn<double>>(MbD::ListD {pos.x, pos.y, pos.z})
            );
            setRotationMatrix(*mbdPart, target.placement.getRotation());
        }

        using PartList = std::vector<std::shared_ptr<MbD::ASMTPart>>;
        auto dragPartsVec = std::make_shared<PartList>(dragParts);
        // OndselSolver retries a failed step at half, a quarter... of the move;
        // when none converges it puts the parts back where the last step left them.
        if (current.assembly->runDragStep(dragPartsVec)) {
            result.status = Status::Ok;
        }
        else {
            result.status = Status::NotConverged;
            result.message = "the solver did not converge";
            restore = true;
        }
    }
    catch (const std::exception& e) {
        result.status = Status::Failed;
        result.message = e.what();
        restore = true;
    }
    catch (...) {
        result.status = Status::Failed;
        result.message = "unknown exception";
        restore = true;
    }

    if (restore) {
        // The targets went into the parts before the step; put them back on the
        // last good step so the next one starts from there.
        try {
            current.assembly->restorePosRot();
        }
        catch (...) {
        }
    }
    try {
        result.moved = refreshPoses();
    }
    catch (...) {
    }
    return result;
}

Result OndselSession::endDrag()
{
    Result result;
    if (!current.assembly) {
        result.message = "no model loaded";
        return result;
    }
    // A full re-solve at the dragged state, with the bodies at mass 1 again.
    current.assembly->runPostDrag();
    result.status = Status::Ok;
    result.moved = refreshPoses();
    return result;
}

Diagnostics OndselSession::analyse()
{
    Diagnostics diagnostics;
    if (!current.assembly || !current.assembly->mbdSystem) {
        return diagnostics;  // nothing solved: nothing known
    }

    // The MbD joint each model joint became in the last system built.
    std::unordered_map<const MbD::Item*, Id> jointIds;
    for (std::size_t i = 0; i < current.joints.size(); ++i) {
        if (current.joints[i] && current.joints[i]->mbdObject) {
            jointIds.emplace(current.joints[i]->mbdObject.get(), static_cast<Id>(i));
        }
    }

    // Today's convention: the assembly origin is a body too, and every body
    // has 6 freedoms; each equation the solver keeps takes one.
    int dof = static_cast<int>(1 + model.bodies.size()) * 6;

    current.assembly->mbdSystem->jointsMotionsDo([&](std::shared_ptr<MbD::Joint> jm) {
        if (!jm) {
            return;
        }
        // OndselSolver removes redundancy per equation: during the first
        // assembly solve, rows of the Jacobian found linearly dependent are
        // wrapped in a RedundantConstraint and left out. A joint is one MbD
        // joint with one equation per constraint, so per joint:
        //  - redundant: at least one equation removed;
        //  - partially redundant: some, not all, of its equations removed;
        //  - conflicting: a removed equation is not satisfied at the solution,
        //    i.e. the joint contradicts the others instead of repeating them.
        int nEquations = 0;
        int nRedundant = 0;
        bool isJointConflicting = false;

        jm->constraintsDo([&](std::shared_ptr<MbD::Constraint> con) {
            if (!con) {
                return;
            }
            ++nEquations;
            if (auto redundant = std::dynamic_pointer_cast<MbD::RedundantConstraint>(con)) {
                ++nRedundant;
                if (isConflicting(redundant)) {
                    isJointConflicting = true;
                }
                // A removed equation takes no freedom away: the remaining ones
                // are full rank, so their count is the Jacobian's rank.
                return;
            }
            --dof;
        });

        // Grounds and drivers count above; only the model's joints are reported.
        auto it = jointIds.find(jm.get());
        if (it == jointIds.end() || nRedundant == 0) {
            return;
        }
        diagnostics.redundant.push_back(it->second);
        if (nRedundant < nEquations) {
            diagnostics.partiallyRedundant.push_back(it->second);
        }
        if (isJointConflicting) {
            diagnostics.conflicting.push_back(it->second);
        }
    });

    diagnostics.dof = dof;
    diagnostics.known = Diagnostics::KnownDof | Diagnostics::KnownRedundant
        | Diagnostics::KnownConflicting | Diagnostics::KnownPartiallyRedundant;
    return diagnostics;
}

Status OndselSession::simulate(const SimulationSpec& spec, TrajectorySink& sink)
{
    try {
        current = Ondsel::toAsmt(model, &spec, debug);
        dump();
        current.assembly->runKINEMATIC();
        const std::size_t frames = current.assembly->numberOfFrames();
        std::vector<BodyPose> poses(current.parts.size());
        // runKINEMATIC records the INPUT first -- the poses as they were
        // handed in, not assembled -- then the initial condition at the
        // start time, then the steps (System::runKINEMATIC). The input is not
        // a frame of the run: played, it showed the parts where the last run
        // left them, at the start time.
        for (std::size_t frame = frames > 1 ? 1 : 0; frame < frames; ++frame) {
            current.assembly->updateForFrame(frame);
            for (std::size_t i = 0; i < current.parts.size(); ++i) {
                poses[i] = {static_cast<Id>(i), Ondsel::partPlacement(current.parts[i])};
            }
            const double time = frame < current.assembly->times->size()
                ? current.assembly->times->at(frame)
                : spec.tStart + static_cast<double>(frame) * spec.tOutput;
            if (!sink.frame(time, poses)) {
                return Status::Cancelled;
            }
        }
        return Status::Ok;
    }
    catch (...) {
        return Status::Failed;
    }
}

Status OndselSession::exportNative(const std::string& path)
{
    if (!current.assembly) {
        return Status::Failed;
    }
    current.assembly->outputFile(path);
    return Status::Ok;
}
