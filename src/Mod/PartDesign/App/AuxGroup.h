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

#include <App/AuxGroup.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesign
{
class Body;

/** For grouping non PartDesign::Feature objects in Body
 *
 * The owner is kept in _Body, the name its files have always used.
 */
class PartDesignExport AuxGroup : public App::AuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::AuxGroup);

    typedef App::AuxGroup inherited;

public:
    AuxGroup();

    App::PropertyLinkHidden _Body;

    virtual const char* getViewProviderName(void) const override {
        return "PartDesignGui::ViewProviderAuxGroup";
    }

    virtual bool allowDuplicateLabel() const override;

    /// The roles of a Body's groups
    static const char *SketchRole;
    static const char *DatumRole;
    static const char *MiscRole;

    enum GroupType {
        UnknownGroup,
        SketchGroup,
        DatumGroup,
        MiscGroup,
        OtherGroup,
    };
    GroupType getGroupType() const;
    PartDesign::Body * getBody() const;

protected:
    /// A file written before Role says the role with the name
    virtual std::string getDefaultRole() const override;
};

} //namespace PartDesign


#endif // PARTDESIGN_AuxGroup_H
