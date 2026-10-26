// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
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


#pragma once

#include <unordered_map>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>


namespace Part
{
class Element;
class ElementLink;
}  // namespace Part

namespace Assembly
{
class AssemblyObject;
class Joint;
class JointGroup;

class AssemblyExport AssemblyLink: public App::Part
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AssemblyLink);

public:
    AssemblyLink();
    ~AssemblyLink() override;

    PyObject* getPyObject() override;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssemblyLink";
    }

    App::DocumentObjectExecReturn* execute() override;

    // The linked assembly is the AssemblyObject that this AssemblyLink pseudo-links to recursively.
    AssemblyObject* getLinkedAssembly() const;
    // The parent assembly is the main assembly in which the linked assembly is contained
    AssemblyObject* getParentAssembly() const;

    // Overriding DocumentObject::getLinkedObject is giving bugs
    // This function returns the linked object, either an AssemblyObject or an AssemblyLink
    App::DocumentObject* getLinkedObject2(bool recurse = true) const;

    bool isRigid() const;

    /**
     * Update all of the components and joints from the Assembly
     */
    void updateContents();
    void updateParentJoints();

    void synchronizeComponents();
    void synchronizeJoints();
    void handleJointReference(
        App::DocumentObject* joint,
        App::DocumentObject* lJoint,
        const char* refName
    );
    void ensureNoJointGroup();
    JointGroup* ensureJointGroup();
    std::vector<App::DocumentObject*> getJoints();

    bool allowDuplicateLabel() const override;

    bool isEmpty() const;
    int numberOfComponents() const;

    App::PropertyXLink LinkedObject;
    App::PropertyBool Rigid;

    std::unordered_map<App::DocumentObject*, App::DocumentObject*> objLinkMap;

protected:
    /// get called by the container whenever a property has been changed
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;

private:
    /** The element link of this instance for side \a side of a C++ joint of
     * the linked assembly: the same element, through this instance's link to
     * the component, or a convenience element of this instance's own. \a
     * current, the mirror's element link, is kept and set when there is one.
     */
    ::Part::ElementLink* mirrorSide(Joint* joint, int side, ::Part::ElementLink* current);
    /// This instance's convenience element for one of the linked assembly's
    ::Part::Element* mirrorElement(::Part::Element* element);
    /// The (object, subname) of this instance a path of the linked assembly
    /// becomes: through the link to its component; false when not mapped
    bool mapPath(App::DocumentObject* obj,
                 const std::string& sub,
                 App::DocumentObject*& local,
                 std::string& localSub) const;

    std::unordered_map<App::DocumentObject*, std::string> objSubPrefixMap;
};


}  // namespace Assembly
