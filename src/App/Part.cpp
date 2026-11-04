/***************************************************************************
 *   Copyright (c) 2014 Jürgen Riegel <juergen.riegel@web.de>              *
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

#include <App/DocumentObject.h>

#include "Application.h"
#include "Document.h"
#include "DocumentObjectExtension.h"
#include "DocumentObserver.h"
#include "Link.h"
#include "LinkAppearance.h"
#include "Origin.h"
#include "OriginFeature.h"
#include "Part.h"
#include "PartPy.h"


using namespace App;


PROPERTY_SOURCE_WITH_EXTENSIONS(App::Part, App::GeoFeature)


//===========================================================================
// Part
//===========================================================================


Part::Part()
{
    ADD_PROPERTY_TYPE(Type,(""),"Part", App::Prop_None,"");
    ADD_PROPERTY_TYPE(Material, (nullptr), 0, App::Prop_None, "The Material for this Part");
    ADD_PROPERTY_TYPE(Meta, (), "Part", App::Prop_None, "Map with additional meta information");

    // No Uid of its own: the part's is the object's (getObjectUid(),
    // docs/TransactionLogJournal.md sec 36.7), and Python's Uid answers it
    ADD_PROPERTY_TYPE(Id, (""), "Part", App::Prop_None, "ID (Part-Number) of the Item");

    // license stuff (leave them empty to avoid confusion with imported 3rd party STEP/IGES files)
    ADD_PROPERTY_TYPE(License, (""), "Part", App::Prop_None, "License string of the Item");
    ADD_PROPERTY_TYPE(LicenseURL, (""), "Part", App::Prop_None, "URL to the license text/contract");

    ADD_PROPERTY_TYPE(ColoredElements, (0), 0, (PropertyType)(Prop_ReadOnly|Prop_Hidden), "");

    // What the part lays over what it holds, as a link does
    // (docs/ShapeAppearanceDesign.md sec 14.6.4): the store, and the names
    // over it, which are in no file
    // "#1": the group's rank. A property view shows the groups by rank, then
    // by name, and without the rank this one stood first, above "Base"
    // (Gui/PropertyGroupName.h)
    static const char *appearances = "Appearances#1";
    ADD_PROPERTY_TYPE(ElementAppearance, (0), appearances,
            (PropertyType)(Prop_Hidden|Prop_Output),
            "The looks the part gives what it holds: its own, and those of the\n"
            "elements it names");
    ElementAppearance.setPathNames(true);
    ColoredElements.setStatus(Property::Legacy, true);
    const auto name = (PropertyType)(Prop_NoPersist|Prop_Output|Prop_NoRecompute);
    ADD_PROPERTY_TYPE(OverrideMaterial, (false), appearances, name,
            "Give what the part holds a look of the part's own");
    const App::MaterialAppearance mat = LinkAppearance::defaultLook(true);
    ADD_PROPERTY_TYPE(ShapeAppearance, (mat), appearances, name,
            "The look the part gives what it holds, where it gives one");
    ShapeAppearance.setStatus(Property::MaterialEdit, true);
    ShapeAppearance.setWriter([this](const AppearanceList &, const AppearanceList &after, int) {
        LinkAppearance::writeAppearance(LinkAppearance::namesOf(this), after, mirroringLooks);
    });

    static const char *GroupModeEnums[] = {"Normal", "Auto", "Link", nullptr};
    GroupMode.setEnums(GroupModeEnums);
    ADD_PROPERTY_TYPE(GroupMode, (long(GroupModeNormal)), "Part", App::Prop_None,
            "How an existing object is added.\n\n"
            "Normal: move it in, out of the group it was in.\n\n"
            "Auto: move it in, or add a link to it owned by this part when it\n"
            "cannot be moved: it is in another document, another part or body\n"
            "holds it, or it is here already.\n\n"
            "Link: always add a link owned by this part.");

    ADD_PROPERTY_TYPE(Elements, (nullptr), "Part", (App::PropertyType)(Prop_Hidden|Prop_ReadOnly),
            "The group owning the elements this part declares");
    Elements.setScope(LinkScope::Child);

    OriginGroupExtension::initExtension(this);

    ExportMode.setStatus(Property::Hidden,false);
    ExportMode.setValue(ExportByVisibility);
}

Part::~Part() = default;

void Part::onChanged(const Property *prop)
{
    if (prop == &ElementAppearance || prop == &ColoredElements || prop == &OverrideMaterial) {
        LinkAppearance::onChanged(LinkAppearance::namesOf(this), prop, mirroringLooks);
    }
    GeoFeature::onChanged(prop);
}

void Part::onDocumentRestored()
{
    LinkAppearance::onRestored(LinkAppearance::namesOf(this), mirroringLooks);
    GeoFeature::onDocumentRestored();
}

static App::Part *_getPartOfObject(const DocumentObject *obj,
                                   std::set<const DocumentObject*> *objset)
{
    if (!obj || !obj->getNameInDocument())
        return nullptr;
    // as a Part is a geofeaturegroup it must directly link to all
    // objects it contains, even if they are in additional groups etc.
    // But we still must call 'hasObject()' to exclude link brought in by
    // expressions.
    for (auto inObj : obj->getInList()) {
        if (objset && !objset->insert(inObj).second)
            continue;
        auto group = inObj->getExtensionByType<GeoFeatureGroupExtension>(true);
        if(group && group->hasObject(obj)) {
            if(inObj->isDerivedFrom(App::Part::getClassTypeId()))
                return static_cast<App::Part*>(inObj);
            else if (objset)
                return _getPartOfObject(inObj, objset);
            // Only one parent geofeature group per object, so break
            break;
        }
    }

    return nullptr;
}

App::Part *Part::getPartOfObject (const DocumentObject* obj, bool recursive) {
    if (!recursive)
        return _getPartOfObject(obj, nullptr);
    std::set<const DocumentObject *> objset;
    objset.insert(obj);
    return _getPartOfObject(obj, &objset);
}


const char *Part::PartsRole = "Parts";

bool Part::isOwnedLink(const DocumentObject *obj) const
{
    if (!obj || !obj->isAttachedToDocument() || !isAttachedToDocument())
        return false;
    auto link = Base::freecad_dynamic_cast<const App::Link>(obj);
    return link && link->_LinkOwner.getValue() == getID();
}

bool Part::needsLink(const DocumentObject *obj) const
{
    if (!obj || !obj->isAttachedToDocument())
        return false;
    if (obj->getDocument() != getDocument())
        return true;
    if (Group.find(obj->getNameInDocument()) == obj)
        return true;
    auto owner = GeoFeatureGroupExtension::getGroupOfObject(obj);
    if (owner && owner != this)
        return true;
    // Moving it in takes along what its placement depends on
    for (auto dep : GeoFeatureGroupExtension::getCSRelevantLinks(obj)) {
        auto depOwner = GeoFeatureGroupExtension::getGroupOfObject(dep);
        if (depOwner && depOwner != this)
            return true;
    }
    return false;
}

std::vector<DocumentObject*> Part::prepareObjectsToAdd(const std::vector<DocumentObject*> &objs)
{
    auto doc = getDocument();
    long mode = GroupMode.getValue();
    if (mode == GroupModeNormal || !doc || doc->isPerformingTransaction()
            || doc->testStatus(Document::Restoring))
        return objs;

    auto inList = getInListEx(true);
    std::vector<DocumentObject*> res;
    res.reserve(objs.size());
    for (auto obj : objs) {
        // Left to the group to refuse, and never linked: an owned link of
        // ours, an aux group, a container of this part
        if (!obj || !obj->isAttachedToDocument() || obj == this
                || inList.count(obj) || isOwnedLink(obj)
                || obj->isDerivedFrom<AuxGroup>()
                || obj->isDerivedFrom<App::OriginFeature>()
                || obj->isDerivedFrom<App::Origin>()
                || (mode == GroupModeAuto && !needsLink(obj))) {
            res.push_back(obj);
            continue;
        }

        auto link = new App::Link;
        link->_LinkOwner.setValue(getID());
        doc->addObject(link, doc->getUniqueObjectName("Link").c_str());
        link->setLink(-1, obj);
        link->Label.setValue(obj->Label.getValue());
        // Where the object is, so that nothing jumps
        link->Placement.setValue(globalPlacement().inverse()
                * GeoFeature::getGlobalPlacement(obj));
        // A link in Link mode stands in for an original at the top level
        if (mode == GroupModeLink && obj->getDocument() == doc
                && !GroupExtension::getAnyGroupOfObject(obj))
            obj->Visibility.setValue(false);
        res.push_back(link);
    }
    return res;
}

void Part::onObjectsRemoved(const std::vector<DocumentObject*> &objs)
{
    auto doc = getDocument();
    if (!doc || doc->isPerformingTransaction() || doc->testStatus(Document::Restoring))
        return;
    std::vector<App::DocumentObjectT> owned;
    for (auto obj : objs) {
        if (!isOwnedLink(obj) || obj->isRemoving())
            continue;
        if (GeoFeatureGroupExtension::isMovingObject()) {
            // Taken by another group: not ours any more
            static_cast<App::Link*>(obj)->_LinkOwner.setValue(0);
            continue;
        }
        owned.emplace_back(obj);
    }
    for (auto &objT : owned) {
        if (auto obj = objT.getObject())
            doc->removeObject(obj->getNameInDocument());
    }
}

bool Part::isAuxGroupAllowed(const AuxGroup *group, const DocumentObject *obj) const
{
    if (!group || !obj || obj->isDerivedFrom<AuxGroup>())
        return false;
    if (group->getRole() != PartsRole)
        return false;
    // The components are what no other group of a role takes
    for (auto child : Group.getValues()) {
        auto other = Base::freecad_dynamic_cast<AuxGroup>(child);
        if (other && other != group && other->getOwner() == this && !other->isCurated()
                && other->getRole() != PartsRole && isAuxGroupAllowed(other, obj))
            return false;
    }
    return true;
}

AuxGroup *Part::getAuxGroup(const char *role) const
{
    for (auto child : Group.getValues()) {
        auto group = Base::freecad_dynamic_cast<AuxGroup>(child);
        if (group && group->getOwner() == this && group->getRole() == role)
            return group;
    }
    return nullptr;
}

AuxGroup *Part::addPartsGroup()
{
    if (auto group = getAuxGroup(PartsRole))
        return group;
    auto doc = getDocument();
    if (!doc)
        return nullptr;
    auto group = static_cast<AuxGroup*>(doc->addObject(getPartsGroupType(), "Parts"));
    // The role first: a group with none is a user's, whose label is unique
    group->Role.setValue(PartsRole);
    group->Label.setValue("Parts");
    // A group the part makes is its own child, whatever its GroupMode
    auto children = Group.getValues();
    children.push_back(group);
    Group.setValues(children);
    group->setOwner(this);
    return group;
}

void Part::removePartsGroup()
{
    auto group = getAuxGroup(PartsRole);
    if (!group)
        return;
    group->setOwner(nullptr);
    getDocument()->removeObject(group->getNameInDocument());
}

AuxGroup *Part::getElementsGroup(bool create)
{
    auto group = Base::freecad_dynamic_cast<AuxGroup>(Elements.getValue());
    if (group || !create || !getDocument())
        return group;
    group = static_cast<AuxGroup*>(getDocument()->addObject("App::PartAuxGroup", "Elements"));
    group->Role.setValue(AuxGroup::ElementsRole);
    group->Label.setValue("Elements");
    Elements.setValue(group);
    group->setOwner(this);
    return group;
}

std::vector<DocumentObject*> Part::getOwnedObjects() const
{
    auto owned = GeoFeature::getOwnedObjects();
    for (auto child : Group.getValues()) {
        auto group = Base::freecad_dynamic_cast<AuxGroup>(child);
        if (group && group->getOwner() == this && !group->getRole().empty())
            owned.push_back(group);
    }
    if (auto elements = Base::freecad_dynamic_cast<AuxGroup>(Elements.getValue()))
        owned.push_back(elements);
    return owned;
}

DocumentObject* Part::getOwnedCounterpart(const DocumentObject* owned,
                                          const DocumentObject* liveOwner,
                                          OwnedPaste& how) const
{
    auto group = Base::freecad_dynamic_cast<const AuxGroup>(owned);
    auto live = Base::freecad_dynamic_cast<const Part>(liveOwner);
    if (!group || !live || group->getOwner() != this)
        return GeoFeature::getOwnedCounterpart(owned, liveOwner, how);
    how = OwnedPaste::FollowOwner;
    if (group == Elements.getValue())
        return live->Elements.getValue();
    return live->getAuxGroup(group->getRole().c_str());
}

int Part::getAuxGroupRank(const AuxGroup *group)
{
    static const char *roles[] = {"Elements", "Joints", "Physics", "Simulations", "Views"};
    const auto &role = group->getRole();
    int rank = 0;
    for (auto r : roles) {
        if (role == r)
            return rank;
        ++rank;
    }
    if (role == PartsRole)
        return rank + 1;
    return rank;
}

PyObject *Part::getPyObject()
{
    if (PythonObject.is(Py::_None())){
        // ref counter is set to 1
        PythonObject = Py::Object(new PartPy(this),true);
    }
    return Py::new_reference_to(PythonObject);
}

void Part::handleChangedPropertyType(Base::XMLReader &reader, const char *TypeName, App::Property *prop)
{
    // Migrate Material from App::PropertyMap to App::PropertyLink
    if (!strcmp(TypeName, "App::PropertyMap")) {
        App::PropertyMap oldvalue;
        oldvalue.Restore(reader);
        if (oldvalue.getSize()) {
            auto oldprop = static_cast<App::PropertyMap*>(addDynamicProperty("App::PropertyMap", "Material_old", "Base"));
            oldprop->setValues(oldvalue.getValues());
        }
    } else {
        App::GeoFeature::handleChangedPropertyType(reader, TypeName, prop);
    }
}

void Part::handleChangedPropertyName(Base::XMLReader &reader, const char *TypeName, const char *PropName)
{
    if (!restoreUidProperty(reader, TypeName, PropName))
        App::GeoFeature::handleChangedPropertyName(reader, TypeName, PropName);
}

// Python feature ---------------------------------------------------------

// Not quite sure yet making Part derivable in Python is good Idea!
// JR 2014

//namespace App {
///// @cond DOXERR
//PROPERTY_SOURCE_TEMPLATE(App::PartPython, App::Part)
//template<> const char* App::PartPython::getViewProviderName(void) const {
//    return "Gui::ViewProviderPartPython";
//}
//template<> PyObject* App::PartPython::getPyObject(void) {
//    if (PythonObject.is(Py::_None())) {
//        // ref counter is set to 1
//        PythonObject = Py::Object(new FeaturePythonPyT<App::PartPy>(this),true);
//    }
//    return Py::new_reference_to(PythonObject);
//}
///// @endcond
//
//// explicit template instantiation
//template class AppExport FeaturePythonT<App::Part>;
//}
