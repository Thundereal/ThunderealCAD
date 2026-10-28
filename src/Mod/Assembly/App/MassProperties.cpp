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

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Mat.hxx>

#include <App/Application.h>
#include <App/DocumentObject.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Quantity.h>
#include <Mod/Material/App/MaterialManager.h>
#include <Mod/Material/App/Materials.h>
#include <Mod/Part/App/FeatureElement.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "MassProperties.h"
#include "Physics.h"

using namespace Assembly;

FC_LOG_LEVEL_INIT("Assembly", true, true)

namespace
{

// Aluminum Generic, the default material's default (open question 11)
constexpr const char* AluminumGeneric = "9bf060e9-1663-44a2-88e2-2ff6ee858efe";
constexpr double AluminumDensity = 2.7e-6;  // kg/mm^3

// The density a material card gives, kg/mm^3; 0 when it gives none. The
// Default card, every shape's until a material is chosen, gives none: its
// 1 kg/m^3 is a placeholder (Standard/Default.FCMat), as its appearance-only
// parent is
double densityOf(const Materials::Material& material)
{
    const QString uuid = material.getUUID();
    if (uuid == QLatin1String("7f9fd73b-50c9-41d8-b7b2-575a030c1eeb")
        || uuid == QLatin1String("5dbb7be6-8b63-479b-ab4c-87be02ead973")
        || !material.hasPhysicalProperty(QStringLiteral("Density"))) {
        return 0.0;
    }
    const Base::Quantity density = material.getPhysicalQuantity(QStringLiteral("Density"));
    return density.isValid() && density.getValue() > 0.0 ? density.getValue() : 0.0;
}

// The path of an object from the assembly, "A.B.Obj.", through the
// sub-assemblies it is in; empty when it is not in the assembly
std::string pathFrom(const AssemblyObject* assembly, App::DocumentObject* obj)
{
    std::vector<App::DocumentObject*> chain {obj};
    auto parent = App::GeoFeatureGroupExtension::getGroupOfObject(obj);
    while (parent && parent != assembly) {
        chain.push_back(parent);
        parent = App::GeoFeatureGroupExtension::getGroupOfObject(parent);
    }
    if (parent != assembly) {
        return {};
    }
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        path += (*it)->getNameInDocument();
        path += '.';
    }
    return path;
}

// The shape features a component is made of, each by its path from the
// component: the component itself, what a link links to, or a container's
// children -- a Body is one, its features are not. Elements and element
// links are no matter: they repeat the shapes of others.
void collect(App::DocumentObject* obj,
             const std::string& path,
             std::vector<std::pair<Part::Feature*, std::string>>& leaves,
             int depth)
{
    if (!obj || depth > 100 || obj->isDerivedFrom<Part::Element>()
        || obj->isDerivedFrom<Part::ElementBinding>()) {
        return;
    }
    if (auto* feature = Base::freecad_dynamic_cast<Part::Feature>(obj->getLinkedObject(true))) {
        leaves.emplace_back(feature, path);
        return;
    }
    for (const auto& sub : obj->getSubObjects()) {
        collect(obj->getSubObject(sub.c_str()), path + sub, leaves, depth + 1);
    }
}

// The object's placement in the assembly: its own and its containers'
Base::Placement placementIn(const AssemblyObject* assembly, App::DocumentObject* obj)
{
    Base::Placement placement;
    for (auto* step = obj; step && step != assembly;
         step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
        if (auto* prop = step->getPlacementProperty()) {
            placement = prop->getValue() * placement;
        }
    }
    return placement;
}

// An inertia along a frame's axes, along those the frame is turned into:
// R I R^T
std::array<double, 6> turned(const std::array<double, 6>& inertia, const Base::Rotation& rotation)
{
    const double i[3][3] = {{inertia[0], inertia[3], inertia[4]},
                            {inertia[3], inertia[1], inertia[5]},
                            {inertia[4], inertia[5], inertia[2]}};
    double r[3][3];
    for (int c = 0; c < 3; ++c) {
        Base::Vector3d axis(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0);
        axis = rotation.multVec(axis);
        r[0][c] = axis.x;
        r[1][c] = axis.y;
        r[2][c] = axis.z;
    }
    double out[3][3] {};
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            for (int k = 0; k < 3; ++k) {
                for (int l = 0; l < 3; ++l) {
                    out[a][b] += r[a][k] * i[k][l] * r[b][l];
                }
            }
        }
    }
    return {out[0][0], out[1][1], out[2][2], out[0][1], out[0][2], out[1][2]};
}

// The mass properties of the solids of a shape, in its coordinates, at this
// density
MassProperties ofShape(const TopoDS_Shape& shape, double density)
{
    MassProperties total;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(it.Current(), props);
        // A reversed solid's volume and moments come negative
        const double sign = props.Mass() < 0.0 ? -1.0 : 1.0;
        const double volume = sign * props.Mass();
        if (volume <= 0.0) {
            continue;
        }
        MassProperties solid;
        solid.volume = volume;
        solid.mass = volume * density;
        const gp_Pnt centre = props.CentreOfMass();
        solid.com = Base::Vector3d(centre.X(), centre.Y(), centre.Z());
        // About the centre of mass, per unit density
        const gp_Mat m = props.MatrixOfInertia();
        const double scale = sign * density;
        solid.inertia = {m(1, 1) * scale,
                         m(2, 2) * scale,
                         m(3, 3) * scale,
                         m(1, 2) * scale,
                         m(1, 3) * scale,
                         m(2, 3) * scale};
        total.add(solid);
    }
    return total;
}

}  // namespace

void MassProperties::add(const MassProperties& other)
{
    volume += other.volume;
    if (other.mass <= 0.0) {
        return;
    }
    if (mass <= 0.0) {
        mass = other.mass;
        com = other.com;
        inertia = other.inertia;
        return;
    }
    const double total = mass + other.mass;
    const Base::Vector3d centre = (com * mass + other.com * other.mass) / total;
    // About the common centre: I + m (|d|^2 E - d d^T), d from it to the own
    auto moved = [&](const std::array<double, 6>& own, double m, const Base::Vector3d& at) {
        const Base::Vector3d d = at - centre;
        return std::array<double, 6> {own[0] + m * (d.y * d.y + d.z * d.z),
                                      own[1] + m * (d.x * d.x + d.z * d.z),
                                      own[2] + m * (d.x * d.x + d.y * d.y),
                                      own[3] - m * d.x * d.y,
                                      own[4] - m * d.x * d.z,
                                      own[5] - m * d.y * d.z};
    };
    const auto a = moved(inertia, mass, com);
    const auto b = moved(other.inertia, other.mass, other.com);
    for (std::size_t i = 0; i < inertia.size(); ++i) {
        inertia[i] = a[i] + b[i];
    }
    mass = total;
    com = centre;
}

double Assembly::defaultDensity(std::string* name)
{
    const std::string uuid =
        App::GetApplication()
            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Mod/Assembly")
            ->GetASCII("DefaultMaterial", AluminumGeneric);
    try {
        auto material =
            Materials::MaterialManager::getManager().getMaterial(QString::fromStdString(uuid));
        if (material) {
            if (const double density = densityOf(*material); density > 0.0) {
                if (name) {
                    *name = material->getName().toStdString();
                }
                return density;
            }
        }
    }
    catch (const Base::Exception& e) {
        FC_WARN("The default material " << uuid << " is not found: " << e.what());
    }
    catch (const std::exception& e) {
        FC_WARN("The default material " << uuid << " is not found: " << e.what());
    }
    if (name) {
        *name = "Aluminum Generic";
    }
    return AluminumDensity;
}

std::vector<MassProperties> Assembly::massPropertiesOf(const AssemblyObject* assembly)
{
    std::vector<MassProperties> results;
    if (!assembly) {
        return results;
    }
    std::string assumedName;
    const double assumed = defaultDensity(&assumedName);
    // The overrides, by component: the last of several
    std::map<App::DocumentObject*, MassOverride*> overrides;
    for (auto* child : assembly->Group.getValues()) {
        if (auto* given = Base::freecad_dynamic_cast<MassOverride>(child)) {
            if (auto* target = given->Component.getValue()) {
                if (overrides.count(target)) {
                    FC_WARN(target->getFullName() << " has more than one mass override; "
                                                  << given->getFullName() << " is used");
                }
                overrides[target] = given;
            }
        }
    }
    for (auto* obj : getAssemblyComponents(assembly)) {
        MassProperties component;
        component.obj = obj;
        auto overridden = overrides.find(obj);
        MassOverride* given = overridden != overrides.end() ? overridden->second : nullptr;
        const int mode = given ? given->Mode.getValue() : -1;
        // Full: nothing from the shape
        if (mode == 2) {
            const Base::Placement placement = placementIn(assembly, obj);
            component.origin = MassProperties::Origin::Override;
            component.mass = std::max(given->Mass.getValue(), 0.0);
            placement.multVec(given->CenterOfMass.getValue(), component.com);
            std::array<double, 6> inertia {};
            const auto& values = given->Inertia.getValues();
            for (std::size_t i = 0; i < inertia.size() && i < values.size(); ++i) {
                inertia[i] = values[i];
            }
            component.inertia = turned(inertia, placement.getRotation());
            results.push_back(std::move(component));
            continue;
        }
        const std::string path = pathFrom(assembly, obj);
        std::vector<std::pair<Part::Feature*, std::string>> leaves;
        if (!path.empty()) {
            collect(obj, std::string(), leaves, 0);
        }
        bool first = true;
        bool anyAssumed = false;
        for (const auto& [feature, sub] : leaves) {
            double density = 0.0;
            std::string material;
            try {
                const auto& card = feature->ShapeMaterial.getValue();
                density = densityOf(card);
                material = card.getName().toStdString();
            }
            catch (const Base::Exception&) {
                density = 0.0;
            }
            if (mode == 0) {
                // Density: the override's, whatever the material
                density = std::max(given->Density.getValue(), 0.0);
                material.clear();
            }
            else if (density <= 0.0) {
                density = assumed;
                material = assumedName;
                anyAssumed = true;
            }
            MassProperties piece;
            try {
                // In the assembly's coordinates, through the component
                const auto shape = Part::Feature::getTopoShape(assembly,
                                                               (path + sub).c_str(),
                                                               false,
                                                               nullptr,
                                                               nullptr,
                                                               true,
                                                               false);
                if (shape.isNull()) {
                    continue;
                }
                piece = ofShape(shape.getShape(), density);
            }
            catch (const Base::Exception& e) {
                FC_WARN(obj->getFullName()
                        << ": no mass properties for " << sub << ": " << e.what());
                continue;
            }
            catch (const Standard_Failure& e) {
                FC_WARN(obj->getFullName()
                        << ": no mass properties for " << sub << ": " << e.GetMessageString());
                continue;
            }
            if (piece.mass <= 0.0) {
                continue;
            }
            if (first) {
                component.material = material;
                component.density = density;
                first = false;
            }
            else if (component.material != material || component.density != density) {
                component.material.clear();
                component.density = 0.0;
            }
            component.add(piece);
        }
        if (component.mass > 0.0) {
            component.origin =
                anyAssumed ? MassProperties::Origin::Assumed : MassProperties::Origin::Shape;
        }
        if (mode == 0) {
            component.origin = MassProperties::Origin::Override;
            component.density = std::max(given->Density.getValue(), 0.0);
        }
        else if (mode == 1) {
            // Mass: the shape's centre, its inertia scaled with the mass; no
            // solid to scale, the mass at the component's origin
            const double mass = std::max(given->Mass.getValue(), 0.0);
            if (component.mass > 0.0) {
                const double scale = mass / component.mass;
                for (auto& value : component.inertia) {
                    value *= scale;
                }
            }
            else {
                component.com = placementIn(assembly, obj).getPosition();
            }
            component.mass = mass;
            component.density = component.volume > 0.0 ? mass / component.volume : 0.0;
            component.material.clear();
            component.origin = MassProperties::Origin::Override;
        }
        results.push_back(std::move(component));
    }
    return results;
}

MassProperties Assembly::totalOf(const std::vector<MassProperties>& parts)
{
    MassProperties total;
    bool first = true;
    for (const auto& part : parts) {
        if (part.mass <= 0.0) {
            continue;
        }
        if (first) {
            total.material = part.material;
            total.density = part.density;
            first = false;
        }
        else if (total.material != part.material || total.density != part.density) {
            total.material.clear();
            total.density = 0.0;
        }
        total.add(part);
        // Assumed if any is, else overridden if any is
        if (total.origin == MassProperties::Origin::None
            || part.origin == MassProperties::Origin::Assumed
            || (part.origin == MassProperties::Origin::Override
                && total.origin == MassProperties::Origin::Shape)) {
            total.origin = part.origin;
        }
    }
    return total;
}
