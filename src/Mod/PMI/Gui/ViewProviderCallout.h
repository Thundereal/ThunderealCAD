// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#ifndef PMIGUI_VIEWPROVIDERCALLOUT_H
#define PMIGUI_VIEWPROVIDERCALLOUT_H

#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderDocumentObjectGroup.h>

#include <Mod/PMI/PMIGlobal.h>

class SoCoordinate3;
class SoDrawStyle;
class SoIndexedFaceSet;
class SoIndexedLineSet;
class SoMaterial;
class SoSeparator;
class TopoDS_Shape;

namespace PMIGui
{

/** Draws what Coin needs from a shape: its free edges as lines, its faces
 * filled -- a callout's strokes and fills (docs/GDandT.md sec 6.5)
 */
class PMIGuiExport ShapeLines
{
public:
    ShapeLines();
    ~ShapeLines();
    SoSeparator* root() const
    {
        return _root;
    }
    void set(const TopoDS_Shape& shape);
    void setColor(float r, float g, float b);
    void setLineWidth(float w);
    void setDashed(bool dashed);

private:
    SoSeparator* _root;
    SoMaterial* _material;
    SoDrawStyle* _style;
    SoCoordinate3* _lineCoords;
    SoIndexedLineSet* _lines;
    SoCoordinate3* _faceCoords;
    SoIndexedFaceSet* _faces;
};

/** A tolerance, a datum, a dimension, a note: its Presentation, on top of
 * the model, coloured by its condition -- remapped amber, broken and
 * invalid red
 */
class PMIGuiExport ViewProviderCallout: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMIGui::ViewProviderCallout);

public:
    ViewProviderCallout();
    ~ViewProviderCallout() override;

    App::PropertyColor Color;
    App::PropertyFloatConstraint LineWidth;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    void onChanged(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    QIcon getIcon() const override;
    bool useNewSelectionModel() const override
    {
        return true;
    }
    /// A geometric tolerance is edited in the frame builder
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;

private:
    void refresh();
    ShapeLines _drawing;
};

/** A toleranced feature: nothing for the faces themselves, the derived
 * axis, centre plane or point drawn dashed as construction geometry
 * (upstream's PMI_ConstructionGeometry)
 */
class PMIGuiExport ViewProviderFeature: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMIGui::ViewProviderFeature);

public:
    ViewProviderFeature();
    ~ViewProviderFeature() override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    QIcon getIcon() const override;
    bool useNewSelectionModel() const override
    {
        return true;
    }

private:
    void refresh();
    ShapeLines _drawing;
};

/// The annotation set: double-clicked, made active, which offers the PMI
/// tool bars in PartDesign (Gui::ToolContext)
class PMIGuiExport ViewProviderAnnotationSet: public Gui::ViewProviderDocumentObjectGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMIGui::ViewProviderAnnotationSet);

public:
    ViewProviderAnnotationSet();
    bool doubleClicked() override;
    QIcon getIcon() const override;
};

/// The key of the active annotation set in a view
static const char PMIKEY[] = "pmi";

}  // namespace PMIGui

#endif  // PMIGUI_VIEWPROVIDERCALLOUT_H
