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

#ifndef GUI_ViewProviderAuxGroup_H
#define GUI_ViewProviderAuxGroup_H

#include "ViewProviderDocumentObject.h"

class SoGroup;

namespace App {
class AuxGroup;
}

namespace Gui {

/** The view provider of an App::AuxGroup
 *
 * Dragging, dropping, reordering and deleting are the owner's business, and
 * are passed to the owner's view provider, except that a curated group takes
 * and gives up the owner's children itself.
 */
class GuiExport ViewProviderAuxGroup : public ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Gui::ViewProviderAuxGroup);
    using inherited = ViewProviderDocumentObject;

public:
    ViewProviderAuxGroup();
    ~ViewProviderAuxGroup() override;

    void updateData(const App::Property *) override;

    bool canDelete(App::DocumentObject* obj) const override;
    bool canDragAndDropObject(App::DocumentObject*) const override;
    bool canDropObjects() const override {return true;}
    bool canDropObject(App::DocumentObject *) const override;
    void dropObject(App::DocumentObject*) override;
    bool canDragObject(App::DocumentObject*) const override;
    bool canDragObjects() const override {return true;}
    void dragObject(App::DocumentObject*) override;
    bool reorderObjects(const std::vector<App::DocumentObject *>&, App::DocumentObject *) override;
    bool canReorderObject(App::DocumentObject *, App::DocumentObject *) override;

    std::vector<App::DocumentObject*> claimChildren() const override;

    /** @name Editing an owning group
     *
     * A module registers the command that edits the owning groups of a role
     * (Part: Part_EditElements for "Elements"). It runs on a double-click of
     * the group, with the group selected, and is offered in its context menu.
     */
    //@{
    static void setEditCommand(const char *role, const char *command);
    bool doubleClicked() override;
    void setupContextMenu(QMenu *menu, QObject *receiver, const char *member) override;
    //@}

    /** @name 3D, of an owning group only
     *
     * The children of an owning group (the elements of an Elements group)
     * are drawn by it, as nothing else claims them: they are in its owner's
     * coordinate system, and its owner claims the group. A view of its
     * owner's children draws nothing; the owner draws them.
     */
    //@{
    void attach(App::DocumentObject *obj) override;
    std::vector<App::DocumentObject*> claimChildren3D() const override;
    SoGroup *getChildRoot() const override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char *mode) override;
    const char *getDefaultDisplayMode() const override;
    //@}

protected:
    App::AuxGroup *getAuxGroup() const;
    /// The view provider of the group's owner
    ViewProvider *getOwnerViewProvider() const;

private:
    SoGroup *pcChildren;
};

} // namespace Gui


#endif // GUI_ViewProviderAuxGroup_H
