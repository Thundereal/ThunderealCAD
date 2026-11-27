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

#include <memory>
#include <vector>
#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderDocumentObjectGroup.h>

#include <Mod/PMI/PMIGlobal.h>

class SoCoordinate3;
class SoDrawStyle;
class SoIndexedFaceSet;
class SoIndexedLineSet;
class SoMaterial;
class SoSeparator;
class SoGroup;
class SoSwitch;
class QMenu;
class TopoDS_Shape;

namespace PMIGui
{

/** Draws what Coin needs from a shape: its free edges as lines, its faces
 * filled and rimmed by a hairline -- a callout's strokes and fills
 * (docs/GDandT.md sec 6.5)
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
    /// The edges of \a shape's faces, followed within \a deflection: a
    /// model's faces outlined, curved edges and all
    void setOutline(const TopoDS_Shape& shape, double deflection);
    void setColor(float r, float g, float b);
    void setLineWidth(float w);
    void setDashed(bool dashed);
    void color(float& r, float& g, float& b) const;
    float lineWidth() const;

private:
    SoSeparator* _root;
    SoMaterial* _material;
    SoDrawStyle* _style;
    SoCoordinate3* _lineCoords;
    SoIndexedLineSet* _lines;
    SoCoordinate3* _faceCoords;
    SoIndexedFaceSet* _faces;
    SoCoordinate3* _rimCoords;
    SoIndexedLineSet* _rims;
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

    /// Taken into or out of a PMI view, or made: what each view a PMI view
    /// is active in hides is made again (sec 6.3)
    void refilter();
    /// With the PMI views it is in, each to be made active from here
    void setupContextMenu(QMenu* menu, QObject* receiver, const char* member) override;

    /// Cross-highlighting (sec 6.5): its faces outlined, while it is
    /// selected or preselected
    void showFaces(bool show);
    /// Lit: it applies to a face that is picked
    void setLit(bool lit);
    bool isLit() const
    {
        return _lit;
    }

private:
    void refresh();
    /// Presentation's parts that turn to read upright (PMI::Callout::
    /// UprightAt), each in its own frame under a node that mirrors it per
    /// frame as the view needs, the way a sketch's datum label turns
    void setUprights(std::size_t count);
    ShapeLines _drawing;
    SoSeparator* _turning = nullptr;
    std::vector<std::unique_ptr<ShapeLines>> _uprights;
    ShapeLines _outline;
    SoGroup* _body = nullptr;
    SoSwitch* _cross = nullptr;
    bool _lit = false;
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
