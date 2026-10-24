/****************************************************************************
 *   Copyright (c) 2020 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#ifndef APP_AuxGroup_H
#define APP_AuxGroup_H

#include <string>
#include <fastsignals/signal.h>
#include "DocumentObject.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"

namespace App
{
class AuxGroup;
class GroupExtension;

/** The interface of a container that shows its children through aux groups
 *
 * An aux group is a view of its owner's children: the owner keeps them in
 * its own Group, and the aux group lists some of them under one tree item.
 * The owner decides which children a group of a given role takes.
 */
class AppExport AuxGroupOwner
{
public:
    virtual ~AuxGroupOwner();

    /// Whether the given aux group of this owner takes the object
    virtual bool isAuxGroupAllowed(const AuxGroup *group, const DocumentObject *obj) const = 0;

    /** Whether the object belongs to this owner
     *
     * A curated aux group takes only what belongs to its owner. The default
     * is membership of the owner's Group.
     */
    virtual bool isAuxGroupChild(const DocumentObject *obj) const;
};

/** A group showing some of its owner's children, which it does not own
 *
 * The owner is held by a hidden link property that a derived class names
 * (setOwnerProperty()), so that each kind of owner keeps the name its
 * files were written with.
 *
 * A group of a role lists every child of its owner the owner allows for that
 * role, in the owner's order. A group without a role is curated: it lists
 * the children the user put in it.
 */
class AppExport AuxGroup : public DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::AuxGroup);

    using inherited = DocumentObject;

public:
    AuxGroup();
    ~AuxGroup() override;

    /// The children shown, computed from the owner's Group
    PropertyLinkList Group;
    /// The kind of children shown; empty for a curated group
    PropertyString Role;

    const char* getViewProviderName() const override {
        return "Gui::ViewProviderAuxGroup";
    }

    void onDocumentRestored() override;
    void onChanged(const Property* prop) override;
    bool allowDuplicateLabel() const override;

    /** The role of this group; empty for a curated group
     *
     * It is the Role property, or when that is empty, what the derived
     * class reads from the object's name (getDefaultRole()), as for a file
     * written before Role existed.
     */
    const std::string &getRole() const;
    /// Whether the user picks the children of this group
    bool isCurated() const {
        return getRole().empty();
    }

    /// The owner, as a document object
    DocumentObject *getOwner() const;
    /// The owner's interface, or null if the owner has none
    AuxGroupOwner *getAuxGroupOwner() const;
    /// The group extension of the owner, holding the children shown here
    GroupExtension *getOwnerGroup() const;
    /// Set the owner
    void setOwner(DocumentObject *owner);

    /// Whether this group takes the object, as its owner says
    bool isObjectAllowed(const DocumentObject *obj) const;

    /// Whether any child is visible, for the group as one item of its owner
    bool isChildVisible() const;
    /// Show or hide every child
    void setChildrenVisible(bool visible);

protected:
    /// Called by a derived class's constructor with its owner link property
    void setOwnerProperty(PropertyLinkHidden *prop);

    /// The role of a group whose Role property is empty
    virtual std::string getDefaultRole() const;

    void refresh();
    void attachOwner();

private:
    PropertyLinkHidden *ownerProp = nullptr;
    fastsignals::scoped_connection connOwner;
    /// The default role, read once the object has a name
    mutable std::string defaultRole;
    mutable bool defaultRoleValid = false;
};

/// An aux group of an App::Part (and so of an assembly)
class AppExport PartAuxGroup : public AuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PartAuxGroup);

public:
    PartAuxGroup();

    /// The part this group belongs to
    PropertyLinkHidden _Owner;
};

}  // namespace App


#endif  // APP_AuxGroup_H
