/***************************************************************************
 *   Copyright (c) 2015 Stefan Tröger <stefantroeger@gmx.net>              *
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


#ifndef PARTGUI_ViewProviderPrimitive_H
#define PARTGUI_ViewProviderPrimitive_H

#include "ViewProviderAddSub.h"

namespace PartDesignGui {

class TaskDlgFeatureParameters;

class PartDesignGuiExport ViewProviderPrimitive : public ViewProviderAddSub
{
    using inherited = ViewProviderAddSub;
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderPrimitive);

public:
    /// constructor
    ViewProviderPrimitive();
    /// destructor
    ~ViewProviderPrimitive() override;

    /// Its dialog's changes steps of their own with PerViewEdit on, the
    /// attachment's too (docs/HandsOnQueue.md entry 232)
    bool panelOwnsItsSteps() const override
    {
        return true;
    }

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;

protected:
    void setupContextMenu(QMenu* menu, QObject* receiver, const char* member) override;
    TaskDlgFeatureParameters *getEditDialog() override;
    
    std::string                 displayMode;
};

} // namespace PartDesignGui


#endif // PARTGUI_ViewProviderBoolean_H
