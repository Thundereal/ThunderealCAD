// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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

// TEMPORARY, for the identity gate (docs/AssemblySolverInterface.md sec 6.2):
// AssemblyObject's ASMT builder as it was before the builder switch (sec 6.3
// step 3), the functions renamed legacy_*, otherwise verbatim. The preference
// Mod/Assembly/SolverLegacyBuilder selects it, so one process can build the
// same document both ways. Removed once the gate has recorded its references.

#include <cmath>
#include <numbers>
#include <sstream>

#include <Precision.hxx>

#include <App/Application.h>
#include <App/Document.h>
#include <App/Link.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Tools.h>

#include <OndselSolver/CREATE.h>
#include <OndselSolver/ASMTSimulationParameters.h>
#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTJoint.h>
#include <OndselSolver/ASMTAngleJoint.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTGearJoint.h>
#include <OndselSolver/ASMTRevoluteJoint.h>
#include <OndselSolver/ASMTCylindricalJoint.h>
#include <OndselSolver/ASMTTranslationalJoint.h>
#include <OndselSolver/ASMTSphericalJoint.h>
#include <OndselSolver/ASMTParallelAxesJoint.h>
#include <OndselSolver/ASMTPerpendicularJoint.h>
#include <OndselSolver/ASMTPointInPlaneJoint.h>
#include <OndselSolver/ASMTPointInLineJoint.h>
#include <OndselSolver/ASMTLineInPlaneJoint.h>
#include <OndselSolver/ASMTPlanarJoint.h>
#include <OndselSolver/ASMTRevCylJoint.h>
#include <OndselSolver/ASMTCylSphJoint.h>
#include <OndselSolver/ASMTRackPinionJoint.h>
#include <OndselSolver/ASMTRotationLimit.h>
#include <OndselSolver/ASMTTranslationLimit.h>
#include <OndselSolver/ASMTRotationalMotion.h>
#include <OndselSolver/ASMTTranslationalMotion.h>
#include <OndselSolver/ASMTGeneralMotion.h>
#include <OndselSolver/ASMTScrewJoint.h>
#include <OndselSolver/ASMTSphSphJoint.h>
#include <OndselSolver/ExternalSystem.h>

#include "../AssemblyObject.h"
#include "../AssemblyParams.h"
#include "../AssemblyUtils.h"

using namespace Assembly;
using namespace MbD;

namespace
{

struct RotationJointSide
{
    App::DocumentObject* joint = nullptr;
    const char* carrierRefName = nullptr;
    const char* carrierPlcName = nullptr;
};

Base::Placement getJointSideGlobalPlacement(
    App::DocumentObject* joint,
    const char* refName,
    const char* plcName
)
{
    if (!joint) {
        return {};
    }

    auto* ref = joint->getPropertyByName<App::PropertyXLinkSub>(refName);
    if (!ref) {
        return {};
    }

    return App::GeoFeature::getGlobalPlacement(nullptr, ref)
        * App::GeoFeature::getPlacementFromProp(joint, plcName);
}

bool axesAreCoaxial(const Base::Placement& plc1, const Base::Placement& plc2)
{
    Base::Vector3d axis1 = plc1.getRotation().multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d axis2 = plc2.getRotation().multVec(Base::Vector3d(0, 0, 1));
    if (axis1.Length() <= Precision::Confusion() || axis2.Length() <= Precision::Confusion()) {
        return false;
    }
    axis1.Normalize();
    axis2.Normalize();

    if (1.0 - std::abs(axis1 * axis2) > Precision::Confusion()) {
        return false;
    }

    const Base::Vector3d delta = plc2.getPosition() - plc1.getPosition();
    return delta.Cross(axis1).Length() <= Precision::Confusion();
}

bool gearJointNeedsCarrierMarker(App::DocumentObject* joint)
{
    const auto plc1 = getJointSideGlobalPlacement(joint, "Reference1", "Placement1");
    const auto plc2 = getJointSideGlobalPlacement(joint, "Reference2", "Placement2");
    return axesAreCoaxial(plc1, plc2);
}

bool isRotationCarrierJoint(JointType type)
{
    return type == JointType::Revolute || type == JointType::Cylindrical;
}

bool findRotationCarrierForGearSide(
    AssemblyObject* assembly,
    App::DocumentObject* gearJoint,
    const char* gearRefName,
    const char* gearPlcName,
    RotationJointSide& carrierSide
)
{
    auto* gearPart = getMovingPartFromRef(gearJoint, gearRefName);
    if (!gearPart) {
        return false;
    }

    const auto gearPlc = getJointSideGlobalPlacement(gearJoint, gearRefName, gearPlcName);
    bool foundCarrier = false;
    bool ambiguousCarrier = false;

    for (auto* joint : assembly->getJoints(false, true)) {
        if (!joint || joint == gearJoint || !getJointActivated(joint)) {
            continue;
        }
        if (!isRotationCarrierJoint(getJointType(joint))) {
            continue;
        }

        auto matchesSide = [&](const char* movingRefName,
                               const char* movingPlcName,
                               const char* carrierRefName,
                               const char* carrierPlcName) {
            if (getMovingPartFromRef(joint, movingRefName) != gearPart) {
                return false;
            }

            const auto rotationPlc = getJointSideGlobalPlacement(joint, movingRefName, movingPlcName);
            if (!axesAreCoaxial(gearPlc, rotationPlc)) {
                return false;
            }

            if (foundCarrier) {
                ambiguousCarrier = true;
                return false;
            }
            foundCarrier = true;
            carrierSide.joint = joint;
            carrierSide.carrierRefName = carrierRefName;
            carrierSide.carrierPlcName = carrierPlcName;
            return true;
        };

        matchesSide("Reference1", "Placement1", "Reference2", "Placement2");
        matchesSide("Reference2", "Placement2", "Reference1", "Placement1");
    }

    return foundCarrier && !ambiguousCarrier;
}

void legacy_setGearJointCarrierMarkerIfAvailable(
    AssemblyObject* assembly,
    App::DocumentObject* joint,
    const std::shared_ptr<ASMTJoint>& mbdJoint
)
{
    auto gearJoint = std::dynamic_pointer_cast<ASMTGearJoint>(mbdJoint);
    if (!gearJoint) {
        return;
    }

    RotationJointSide side1;
    RotationJointSide side2;
    const bool hasCarrier1
        = findRotationCarrierForGearSide(assembly, joint, "Reference1", "Placement1", side1);
    const bool hasCarrier2
        = findRotationCarrierForGearSide(assembly, joint, "Reference2", "Placement2", side2);
    if (!hasCarrier1 || !hasCarrier2) {
        return;
    }

    auto* carrierPart1 = getMovingPartFromRef(side1.joint, side1.carrierRefName);
    auto* carrierPart2 = getMovingPartFromRef(side2.joint, side2.carrierRefName);
    if (!carrierPart1 || !carrierPart2
        || assembly->legacy_getMbDPart(carrierPart1) != assembly->legacy_getMbDPart(carrierPart2)) {
        return;
    }

    const std::string carrierMarkerName = joint->getFullName() + "-Carrier";
    std::string fullMarkerNameK = assembly->legacy_handleOneSideOfJoint(
        side1.joint,
        side1.carrierRefName,
        side1.carrierPlcName,
        carrierMarkerName
    );
    if (!fullMarkerNameK.empty()) {
        gearJoint->setMarkerK(fullMarkerNameK);
    }
}

}  // namespace

std::shared_ptr<ASMTAssembly> AssemblyObject::legacy_makeMbdAssembly()
{
    auto assembly = CREATE<ASMTAssembly>::With();
    assembly->externalSystem->freecadAssemblyObject = this;
    assembly->setName("OndselAssembly");

    ParameterGrp::handle hPgr = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );

    assembly->setDebug(hPgr->GetBool("LogSolverDebug", Assembly::AssemblyParams::defaultLogSolverDebug()));
    return assembly;
}

void AssemblyObject::legacy_fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& name)
{
    if (!obj) {
        return;
    }

    std::string markerName1 = "marker-" + obj->getFullName();
    auto mbdMarker1 = legacy_makeMbdMarker(markerName1, plc);
    mbdAssembly->addMarker(mbdMarker1);

    std::shared_ptr<ASMTPart> mbdPart = legacy_getMbDPart(obj);

    std::string markerName2 = "FixingMarker";
    Base::Placement basePlc = Base::Placement();
    auto mbdMarker2 = legacy_makeMbdMarker(markerName2, basePlc);
    mbdPart->addMarker(mbdMarker2);

    markerName1 = "/OndselAssembly/" + mbdMarker1->name;
    markerName2 = "/OndselAssembly/" + mbdPart->name + "/" + mbdMarker2->name;

    auto mbdJoint = CREATE<ASMTFixedJoint>::With();
    mbdJoint->setName(name);
    mbdJoint->setMarkerI(markerName1);
    mbdJoint->setMarkerJ(markerName2);

    mbdAssembly->addJoint(mbdJoint);
}

void AssemblyObject::legacy_jointParts(std::vector<App::DocumentObject*> joints)
{
    for (auto* joint : joints) {
        if (!joint) {
            continue;
        }

        std::vector<std::shared_ptr<MbD::ASMTJoint>> mbdJoints = legacy_makeMbdJoint(joint);
        for (auto& mbdJoint : mbdJoints) {
            mbdAssembly->addJoint(mbdJoint);
        }
    }
}

void Assembly::AssemblyObject::legacy_create_mbdSimulationParameters(App::DocumentObject* sim)
{
    auto mbdSim = mbdAssembly->simulationParameters;
    if (!sim) {
        return;
    }
    auto valueOf = [](DocumentObject* docObj, const char* propName) {
        auto* prop = dynamic_cast<App::PropertyFloat*>(docObj->getPropertyByName(propName));
        if (!prop) {
            return 0.0;
        }
        return prop->getValue();
    };
    mbdSim->settstart(valueOf(sim, "aTimeStart"));
    mbdSim->settend(valueOf(sim, "bTimeEnd"));
    mbdSim->sethout(valueOf(sim, "cTimeStepOutput"));
    mbdSim->sethmin(1.0e-9);
    mbdSim->sethmax(1.0);
    mbdSim->seterrorTol(valueOf(sim, "fGlobalErrorTolerance"));
}

std::shared_ptr<ASMTJoint> AssemblyObject::legacy_makeMbdJointOfType(App::DocumentObject* joint, JointType type)
{
    switch (type) {
        case JointType::Fixed:
            if (bundleFixed) {
                return nullptr;
            }
            return CREATE<ASMTFixedJoint>::With();

        case JointType::Revolute:
            return CREATE<ASMTRevoluteJoint>::With();

        case JointType::Cylindrical:
            return CREATE<ASMTCylindricalJoint>::With();

        case JointType::Slider:
            return CREATE<ASMTTranslationalJoint>::With();

        case JointType::Ball:
            return CREATE<ASMTSphericalJoint>::With();

        case JointType::Distance:
            return legacy_makeMbdJointDistance(joint);

        case JointType::Parallel:
            return CREATE<ASMTParallelAxesJoint>::With();

        case JointType::Perpendicular:
            return CREATE<ASMTPerpendicularJoint>::With();

        case JointType::Angle: {
            double angle = fabs(Base::toRadians(getJointAngle(joint)));
            if (fmod(angle, 2 * std::numbers::pi) < Precision::Confusion()) {
                return CREATE<ASMTParallelAxesJoint>::With();
            }
            auto mbdJoint = CREATE<ASMTAngleJoint>::With();
            mbdJoint->theIzJz = angle;
            return mbdJoint;
        }

        case JointType::RackPinion: {
            auto mbdJoint = CREATE<ASMTRackPinionJoint>::With();
            mbdJoint->pitchRadius = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Screw: {
            int slidingIndex = slidingPartIndex(joint);
            if (slidingIndex == 0) {  // invalid this joint needs a slider
                return nullptr;
            }

            if (slidingIndex != 1) {
                swapJCS(joint);  // make sure that sliding is first.
            }

            auto mbdJoint = CREATE<ASMTScrewJoint>::With();
            mbdJoint->pitch = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Gears: {
            auto mbdJoint = CREATE<ASMTGearJoint>::With();
            mbdJoint->radiusI = getJointDistance(joint);
            mbdJoint->radiusJ = getJointDistance2(joint);
            return mbdJoint;
        }

        case JointType::Belt: {
            auto mbdJoint = CREATE<ASMTGearJoint>::With();
            mbdJoint->radiusI = getJointDistance(joint);
            mbdJoint->radiusJ = -getJointDistance2(joint);
            return mbdJoint;
        }

        default:
            return nullptr;
    }
}

std::shared_ptr<ASMTJoint> AssemblyObject::legacy_makeMbdJointDistance(App::DocumentObject* joint)
{
    DistanceType type = getDistanceType(joint);

    std::string elt1 = getElementFromProp(joint, "Reference1");
    std::string elt2 = getElementFromProp(joint, "Reference2");
    auto* obj1 = getLinkedObjFromRef(joint, "Reference1");
    auto* obj2 = getLinkedObjFromRef(joint, "Reference2");

    switch (type) {
        case DistanceType::PointPoint: {
            // Point to point distance, or ball joint if distance=0.
            double distance = getJointDistance(joint);
            if (distance < Precision::Confusion()) {
                return CREATE<ASMTSphericalJoint>::With();
            }
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = distance;
            return mbdJoint;
        }

        // Edge - edge cases
        case DistanceType::LineLine: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::LineCircle: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CircleCircle: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getEdgeRadius(obj1, elt1)
                + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        // Face - Face cases
        case DistanceType::PlanePlane: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PlaneCylinder: {
            auto mbdJoint = CREATE<ASMTLineInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneSphere: {
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneTorus: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::CylinderCylinder: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderSphere: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderTorus: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::TorusTorus: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::TorusSphere: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::SphereSphere: {
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        // Point - Face cases
        case DistanceType::PointPlane: {
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCylinder: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        case DistanceType::PointSphere: {
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        // Edge - Face cases
        case DistanceType::LinePlane: {
            auto mbdJoint = CREATE<ASMTLineInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        // Point - Edge cases
        case DistanceType::PointLine: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCurve: {
            // For other curves we do a point in plane-of-the-curve.
            // Maybe it would be best tangent / distance to the conic?
            // For arcs and circles we could use ASMTRevSphJoint. But is it better than
            // pointInPlane?
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        default: {
            // by default we make a planar joint.
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }
    }
}

std::vector<std::shared_ptr<MbD::ASMTJoint>> AssemblyObject::legacy_makeMbdJoint(App::DocumentObject* joint)
{
    if (!joint) {
        return {};
    }

    JointType jointType = getJointType(joint);

    std::shared_ptr<ASMTJoint> mbdJoint = legacy_makeMbdJointOfType(joint, jointType);
    if (!mbdJoint || !legacy_isMbDJointValid(joint)) {
        return {};
    }
    if ((jointType == JointType::Gears || jointType == JointType::Belt)
        && gearJointNeedsCarrierMarker(joint)) {
        legacy_setGearJointCarrierMarkerIfAvailable(this, joint, mbdJoint);
    }

    std::string fullMarkerNameI, fullMarkerNameJ;
    if (jointType == JointType::RackPinion) {
        legacy_getRackPinionMarkers(joint, fullMarkerNameI, fullMarkerNameJ);
    }
    else {
        fullMarkerNameI = legacy_handleOneSideOfJoint(joint, "Reference1", "Placement1");
        fullMarkerNameJ = legacy_handleOneSideOfJoint(joint, "Reference2", "Placement2");
    }
    if (fullMarkerNameI == "" || fullMarkerNameJ == "") {
        return {};
    }

    mbdJoint->setName(joint->getFullName());
    mbdJoint->setMarkerI(fullMarkerNameI);
    mbdJoint->setMarkerJ(fullMarkerNameJ);

    // Add limits if needed. We do not add if this is a simulation or their might clash.
    if (motions.empty()) {
        if (jointType == JointType::Slider || jointType == JointType::Cylindrical) {
            auto* pLenMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMin"));
            auto* pLenMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMax")
            );

            if (pLenMin && pLenMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minLength = pLenMin->getValue();
                double maxLength = pLenMax->getValue();

                if ((minLength > maxLength) && minEnabled && maxEnabled) {
                    pLenMin->setValue(maxLength);
                    pLenMax->setValue(minLength);
                    minLength = maxLength;
                    maxLength = pLenMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    auto limit = ASMTTranslationLimit::With();
                    limit->setName(joint->getFullName() + "-LimitLenMin");
                    limit->setMarkerI(fullMarkerNameI);
                    limit->setMarkerJ(fullMarkerNameJ);
                    limit->settype("=>");
                    limit->setlimit(std::to_string(minLength));
                    limit->settol("1.0e-9");
                    mbdAssembly->addLimit(limit);
                }

                if (maxEnabled) {
                    auto limit2 = ASMTTranslationLimit::With();
                    limit2->setName(joint->getFullName() + "-LimitLenMax");
                    limit2->setMarkerI(fullMarkerNameI);
                    limit2->setMarkerJ(fullMarkerNameJ);
                    limit2->settype("=<");
                    limit2->setlimit(std::to_string(maxLength));
                    limit2->settol("1.0e-9");
                    mbdAssembly->addLimit(limit2);
                }
            }
        }
        if (jointType == JointType::Revolute || jointType == JointType::Cylindrical) {
            auto* pRotMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMin"));
            auto* pRotMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMax")
            );

            if (pRotMin && pRotMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minAngle = pRotMin->getValue();
                double maxAngle = pRotMax->getValue();
                if ((minAngle > maxAngle) && minEnabled && maxEnabled) {
                    pRotMin->setValue(maxAngle);
                    pRotMax->setValue(minAngle);
                    minAngle = maxAngle;
                    maxAngle = pRotMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    auto limit = ASMTRotationLimit::With();
                    limit->setName(joint->getFullName() + "-LimitRotMin");
                    limit->setMarkerI(fullMarkerNameI);
                    limit->setMarkerJ(fullMarkerNameJ);
                    limit->settype("=>");
                    limit->setlimit(std::to_string(minAngle) + "*pi/180.0");
                    limit->settol("1.0e-9");
                    mbdAssembly->addLimit(limit);
                }

                if (maxEnabled) {
                    auto limit2 = ASMTRotationLimit::With();
                    limit2->setName(joint->getFullName() + "-LimitRotMax");
                    limit2->setMarkerI(fullMarkerNameI);
                    limit2->setMarkerJ(fullMarkerNameJ);
                    limit2->settype("=<");
                    limit2->setlimit(std::to_string(maxAngle) + "*pi/180.0");
                    limit2->settol("1.0e-9");
                    mbdAssembly->addLimit(limit2);
                }
            }
        }
    }
    std::vector<App::DocumentObject*> done;

    auto replaceInitialValue =
        [](std::string& form, App::DocumentObject* jnt, const std::string& mType) {
            if (form.find("initialValue") != std::string::npos) {
                double val = getJointCurrentValue(jnt, mType == "Angular");

                std::ostringstream out;
                out.precision(10);
                out << val;
                std::string valStr = out.str();

                size_t pos;
                while ((pos = form.find("initialValue")) != std::string::npos) {
                    form.replace(pos, 12, valStr);
                }
            }
        };

    // Add motions if needed
    for (auto* motion : motions) {
        if (std::ranges::find(done, motion) != done.end()) {
            continue;  // don't process twice (can happen in case of cylindrical)
        }

        auto* pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion->getPropertyByName("Joint"));
        if (!pJoint) {
            continue;
        }
        App::DocumentObject* motionJoint = pJoint->getValue();
        if (joint != motionJoint) {
            continue;
        }

        auto* pType = dynamic_cast<App::PropertyEnumeration*>(motion->getPropertyByName("MotionType"));
        auto* pFormula = dynamic_cast<App::PropertyString*>(motion->getPropertyByName("Formula"));
        if (!pType || !pFormula) {
            continue;
        }
        std::string formula = pFormula->getValue();
        if (formula == "") {
            continue;
        }
        std::string motionType = pType->getValueAsString();

        replaceInitialValue(formula, joint, motionType);

        // check if there is a second motion as cylindrical can have both,
        // in which case the solver needs a general motion.
        for (auto* motion2 : motions) {
            pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion2->getPropertyByName("Joint"));
            if (!pJoint) {
                continue;
            }
            motionJoint = pJoint->getValue();
            if (joint != motionJoint || motion2 == motion) {
                continue;
            }

            auto* pType2 = dynamic_cast<App::PropertyEnumeration*>(
                motion2->getPropertyByName("MotionType")
            );
            auto* pFormula2 = dynamic_cast<App::PropertyString*>(motion2->getPropertyByName("Formula"));
            if (!pType2 || !pFormula2) {
                continue;
            }
            std::string formula2 = pFormula2->getValue();
            if (formula2 == "") {
                continue;
            }
            std::string motionType2 = pType2->getValueAsString();
            if (motionType2 == motionType) {
                continue;  // only if both motions are different. ie one angular and one linear.
            }

            replaceInitialValue(formula2, joint, motionType2);

            auto ASMTmotion = CREATE<ASMTGeneralMotion>::With();
            ASMTmotion->setName(joint->getFullName() + "-ScrewMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->rIJI->atiput(2, motionType == "Angular" ? formula2 : formula);
            ASMTmotion->angIJJ->atiput(2, motionType == "Angular" ? formula : formula2);
            mbdAssembly->addMotion(ASMTmotion);

            done.push_back(motion2);
        }

        if (motionType == "Angular") {
            auto ASMTmotion = CREATE<ASMTRotationalMotion>::With();
            ASMTmotion->setName(joint->getFullName() + "-AngularMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->setRotationZ(formula);
            mbdAssembly->addMotion(ASMTmotion);
        }
        else if (motionType == "Linear") {
            auto ASMTmotion = CREATE<ASMTTranslationalMotion>::With();
            ASMTmotion->setName(joint->getFullName() + "-LinearMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->setTranslationZ(formula);
            mbdAssembly->addMotion(ASMTmotion);
        }
    }

    return {mbdJoint};
}

std::string AssemblyObject::legacy_handleOneSideOfJoint(
    App::DocumentObject* joint,
    const char* propRefName,
    const char* propPlcName,
    const std::string& markerName
)
{
    App::DocumentObject* part = getMovingPartFromRef(joint, propRefName);
    App::DocumentObject* obj = getObjFromJointRef(joint, propRefName);

    if (!part || !obj) {
        Base::Console()
            .Warning("The property %s of Joint %s is bad.\n", propRefName, joint->getFullName());
        return "";
    }

    MbDPartData data = legacy_getMbDData(part);
    std::shared_ptr<ASMTPart> mbdPart = data.part;
    Base::Placement plc = getPlacementFromProp(joint, propPlcName);
    // Now we have plc which is the JCS placement, but its relative to the Object, not to the
    // containing Part.
    auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(propRefName));
    if (!ref) {
        return "";
    }

    // This plc adjustment should be necessary only if obj != part. But for some objects like
    // draft links, we can have obj == part and still need to get global placement to adjust
    // by the element placement.
    Base::Placement obj_global_plc = getGlobalPlacement(nullptr, ref);
    plc = obj_global_plc * plc;
    // Note part is supposed to be root of ref, so we could use part.Placement directly.
    Base::Placement part_global_plc = getGlobalPlacement(part, ref);
    plc = part_global_plc.inverse() * plc;

    // check if we need to add an offset in case of bundled parts.
    if (!data.offsetPlc.isIdentity()) {
        plc = data.offsetPlc * plc;
    }

    std::string markerNameCopy = markerName.empty() ? joint->getFullName() : markerName;
    auto mbdMarker = legacy_makeMbdMarker(markerNameCopy, plc);
    mbdPart->addMarker(mbdMarker);

    return "/OndselAssembly/" + mbdPart->name + "/" + markerNameCopy;
}

void AssemblyObject::legacy_getRackPinionMarkers(
    App::DocumentObject* joint,
    std::string& markerNameI,
    std::string& markerNameJ
)
{
    // ASMT rack pinion joint must get the rack as I and pinion as J.
    // - rack marker has to have Z axis parallel to pinion Z axis.
    // - rack marker has to have X axis parallel to the sliding axis.
    // The user will have selected the sliding marker so we need to transform it.
    // And we need to detect which marker is the rack.

    int slidingIndex = slidingPartIndex(joint);
    if (slidingIndex == 0) {
        return;
    }

    if (slidingIndex != 1) {
        swapJCS(joint);  // make sure that rack is first.
    }

    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
    Base::Placement plc1 = getPlacementFromProp(joint, "Placement1");

    App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
    Base::Placement plc2 = getPlacementFromProp(joint, "Placement2");

    if (!part1 || !obj1) {
        Base::Console().Warning("Reference1 of Joint %s is bad.\n", joint->getFullName());
        return;
    }

    // For the pinion nothing special needed :
    markerNameJ = legacy_handleOneSideOfJoint(joint, "Reference2", "Placement2");

    // For the rack we need to change the placement :
    // make the pinion plc relative to the rack placement.
    auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
    auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
    if (!ref1 || !ref2) {
        return;
    }
    Base::Placement pinion_global_plc = getGlobalPlacement(obj2, ref2);
    plc2 = pinion_global_plc * plc2;
    Base::Placement rack_global_plc = getGlobalPlacement(obj1, ref1);
    plc2 = rack_global_plc.inverse() * plc2;

    // The rot of the rack placement should be the same as the pinion, but with X axis along the
    // slider axis.
    Base::Rotation rot = plc2.getRotation();
    // the yaw of rot has to be the same as plc1
    Base::Vector3d currentZAxis = rot.multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d currentXAxis = rot.multVec(Base::Vector3d(1, 0, 0));
    Base::Vector3d targetXAxis = plc1.getRotation().multVec(Base::Vector3d(0, 0, 1));

    // Calculate the angle between the current X axis and the target X axis
    double yawAdjustment = currentXAxis.GetAngle(targetXAxis);

    // Determine the direction of the yaw adjustment using cross product
    Base::Vector3d crossProd = currentXAxis.Cross(targetXAxis);
    if (currentZAxis * crossProd < 0) {  // If cross product is in opposite direction to Z axis
        yawAdjustment = -yawAdjustment;
    }

    // Create a yaw rotation around the Z axis
    Base::Rotation yawRotation(currentZAxis, yawAdjustment);

    // Combine the initial rotation with the yaw adjustment
    Base::Rotation adjustedRotation = yawRotation * rot;
    plc1.setRotation(adjustedRotation);

    // Then end of processing similar to legacy_handleOneSideOfJoint :
    MbDPartData data1 = legacy_getMbDData(part1);
    std::shared_ptr<ASMTPart> mbdPart = data1.part;
    if (obj1->getNameInDocument() != part1->getNameInDocument()) {
        plc1 = rack_global_plc * plc1;

        Base::Placement part_global_plc = getGlobalPlacement(part1, ref1);
        plc1 = part_global_plc.inverse() * plc1;
    }
    // check if we need to add an offset in case of bundled parts.
    if (!data1.offsetPlc.isIdentity()) {
        plc1 = data1.offsetPlc * plc1;
    }

    std::string markerName = joint->getFullName();
    auto mbdMarker = legacy_makeMbdMarker(markerName, plc1);
    mbdPart->addMarker(mbdMarker);

    markerNameI = "/OndselAssembly/" + mbdPart->name + "/" + markerName;
}

bool AssemblyObject::legacy_isMbDJointValid(App::DocumentObject* joint)
{
    // When dragging a part, we are bundling fixed parts together.
    // This may lead to a conflicting joint that is self referencing a MbD part.
    // The solver crash when fed such a bad joint. So we make sure it does not happen.
    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
    if (!part1 || !part2) {
        return false;
    }

    // If this joint is self-referential it must be ignored.
    if (legacy_getMbDPart(part1) == legacy_getMbDPart(part2)) {
        Base::Console().Warning(
            "Assembly: Ignoring joint (%s) because its parts are connected by a fixed "
            "joint bundle. This joint is a conflicting or redundant constraint.\n",
            joint->getFullLabel()
        );
        return false;
    }
    return true;
}

AssemblyObject::MbDPartData AssemblyObject::legacy_getMbDData(App::DocumentObject* part)
{
    auto it = objectPartMap.find(part);
    if (it != objectPartMap.end()) {
        // part has been associated with an ASMTPart before
        return it->second;
    }

    // Associate objects that belong to an active rigid cluster.
    if (auto* rep = getRigidRepresentative(part)) {
        Base::Placement repPlc = getPlacementFromProp(rep, "Placement");

        std::shared_ptr<ASMTPart> mbdPart;
        const auto repMapped = objectPartMap.find(rep);
        if (repMapped != objectPartMap.end()) {
            mbdPart = repMapped->second.part;
        }
        else {
            std::string repName = rep->getFullName();
            mbdPart = legacy_makeMbdPart(repName, repPlc);
            mbdAssembly->addPart(mbdPart);
            objectPartMap[rep] = {Solver::NoId, Base::Placement(), mbdPart};
        }

        if (const auto* members = getRigidMembers(rep)) {
            for (auto* member : *members) {
                if (!member || objectPartMap.find(member) != objectPartMap.end()) {
                    continue;
                }

                Base::Placement memberPlc = getPlacementFromProp(member, "Placement");
                objectPartMap[member] = {Solver::NoId, repPlc.inverse() * memberPlc, mbdPart};
            }
        }

        auto mapped = objectPartMap.find(part);
        if (mapped != objectPartMap.end()) {
            return mapped->second;
        }
    }

    // part has not been associated with an ASMTPart before
    std::string str = part->getFullName();
    Base::Placement plc = getPlacementFromProp(part, "Placement");
    std::shared_ptr<ASMTPart> mbdPart = legacy_makeMbdPart(str, plc);
    mbdAssembly->addPart(mbdPart);
    MbDPartData data = {Solver::NoId, Base::Placement(), mbdPart};
    objectPartMap[part] = data;  // Store the association

    // Associate other objects connected with fixed joints
    if (bundleFixed) {
        auto addConnectedFixedParts = [&](App::DocumentObject* currentPart, auto& self) -> void {
            std::vector<App::DocumentObject*> joints = getJointsOfPart(currentPart);
            for (auto* joint : joints) {
                JointType jointType = getJointType(joint);
                if (jointType == JointType::Fixed) {
                    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
                    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
                    App::DocumentObject* partToAdd = currentPart == part1 ? part2 : part1;

                    if (objectPartMap.find(partToAdd) != objectPartMap.end()) {
                        // already added
                        continue;
                    }

                    Base::Placement plci = getPlacementFromProp(partToAdd, "Placement");
                    MbDPartData partData = {Solver::NoId, plc.inverse() * plci, mbdPart};
                    objectPartMap[partToAdd] = partData;  // Store the association

                    // Recursively call for partToAdd
                    self(partToAdd, self);
                }
            }
        };

        addConnectedFixedParts(part, addConnectedFixedParts);
    }
    return data;
}

std::shared_ptr<ASMTPart> AssemblyObject::legacy_getMbDPart(App::DocumentObject* part)
{
    if (!part) {
        return nullptr;
    }
    return legacy_getMbDData(part).part;
}

std::shared_ptr<ASMTPart> AssemblyObject::legacy_makeMbdPart(std::string& name, Base::Placement plc, double mass)
{
    auto mbdPart = CREATE<ASMTPart>::With();
    mbdPart->setName(name);

    auto massMarker = CREATE<ASMTPrincipalMassMarker>::With();
    massMarker->setMass(mass);
    massMarker->setDensity(1.0);
    massMarker->setMomentOfInertias(1.0, 1.0, 1.0);
    mbdPart->setPrincipalMassMarker(massMarker);

    Base::Vector3d pos = plc.getPosition();
    mbdPart->setPosition3D(pos.x, pos.y, pos.z);

    // TODO : replace with quaternion to simplify
    Base::Rotation rot = plc.getRotation();
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdPart->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);

    return mbdPart;
}

std::shared_ptr<ASMTMarker> AssemblyObject::legacy_makeMbdMarker(std::string& name, Base::Placement& plc)
{
    auto mbdMarker = CREATE<ASMTMarker>::With();
    mbdMarker->setName(name);

    Base::Vector3d pos = plc.getPosition();
    mbdMarker->setPosition3D(pos.x, pos.y, pos.z);

    // TODO : replace with quaternion to simplify
    Base::Rotation rot = plc.getRotation();
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdMarker->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);

    return mbdMarker;
}
