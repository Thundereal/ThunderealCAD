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

#ifndef PARTDESIGN_AuxGroup_H
#define PARTDESIGN_AuxGroup_H

#include <fastsignals/signal.h>
#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesign
{
class Body;

/** For grouping non PartDesign::Feature objects in Body
 *
 * The type is told by the name: Sketches, Datums, Misc and Surfaces gather
 * what they take from the body by themselves; any other name (a user's
 * group) holds what was put in them. A surface group takes what a module
 * registered with the body as "Surface" (Part::BodyBase::allowType), which
 * Misc then leaves to it; the body makes one when the first surface comes
 * (Body::setAutoAuxGroup). Made active in a view it offers the Surface tool
 * bars (Gui::ToolContext).
 */
class PartDesignExport AuxGroup : public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::AuxGroup);

    typedef App::DocumentObject inherited;

public:
    AuxGroup();

    App::PropertyLinkList Group;
    App::PropertyLinkHidden _Body;

    virtual const char* getViewProviderName(void) const override {
        return "PartDesignGui::ViewProviderAuxGroup";
    }

    virtual void onDocumentRestored() override;
    virtual void onChanged(const App::Property* prop) override;
    virtual bool allowDuplicateLabel() const override;

    bool isObjectAllowed(const App::DocumentObject *obj) const;

    enum GroupType {
        UnknownGroup,
        SketchGroup,
        DatumGroup,
        MiscGroup,
        SurfaceGroup,
        OtherGroup,
    };
    GroupType getGroupType() const;
    PartDesign::Body * getBody() const;

protected:
    void refresh();
    void attachBody();

private:
    fastsignals::scoped_connection connBody;
    mutable GroupType groupType = UnknownGroup;
};

} //namespace PartDesign


#endif // PARTDESIGN_AuxGroup_H
