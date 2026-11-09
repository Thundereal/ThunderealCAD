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

#ifndef PARTGUI_ViewProviderAuxGroup_H
#define PARTGUI_ViewProviderAuxGroup_H

#include <Gui/ViewProviderAuxGroup.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesignGui {

/// The view provider of a Body's aux group, which only adds the icons
class PartDesignGuiExport ViewProviderAuxGroup : public Gui::ViewProviderAuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderAuxGroup);
    typedef Gui::ViewProviderAuxGroup inherited;

public:
    ViewProviderAuxGroup();
    virtual ~ViewProviderAuxGroup();

    virtual void attach(App::DocumentObject*) override;
    /// A surface group's: made active in the view, or let go (PDSURFACEKEY);
    /// the rest -- drag, drop, reorder, delete -- is Gui::ViewProviderAuxGroup's
    virtual bool doubleClicked() override;
};

} // namespace PartDesignGui


#endif // PARTGUI_ViewProviderAuxGroup_H
