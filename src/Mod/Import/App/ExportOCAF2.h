/****************************************************************************
 *   Copyright (c) 2018 Zheng, Lei (realthunder) <realthunder.dev@gmail.com>*
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

#ifndef IMPORT_EXPORTOCAF2_H
#define IMPORT_EXPORTOCAF2_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFDoc_LayerTool.hxx>

#include <App/MaterialAppearance.h>
#include <Mod/Import/ImportGlobal.h>
#include "RenderMaterial.h"
#include "Tools.h"

namespace App
{
class DocumentObject;
}

namespace Part
{
class TopoShape;
}

namespace Import
{

/** What an export pass is given (ExportOCAF2::addPostExport): the XCAF
 * document as exportObjects() has filled it, before a writer writes it, and
 * the label each object exported went to. The PMI module adds its GD&T here
 * (docs/GDandT.md sec 5.2), as it reads a file's after an import
 * (PostImport).
 */
struct ImportExport PostExport
{
    Handle(TDocStd_Document) hDoc;
    /// The objects exported, every one with a label: the containers' and
    /// what they hold, and what links stand for
    std::vector<App::DocumentObject*> objects;
    /// The label \a obj went to -- an assembly component, or the shape --
    /// or a null label
    std::function<TDF_Label(App::DocumentObject*)> labelOf;
};

struct ImportExport ExportOCAFOptions
{
    ExportOCAFOptions();
    App::Color defaultColor;
    bool exportHidden = true;
    bool keepPlacement = false;
};

class ImportExport ExportOCAF2
{
public:
    using GetShapeColorsFunc =
        std::function<std::map<std::string, App::Color>(App::DocumentObject*, const char*)>;
    /// Resolve an object's per-object render (PBR) material, typically
    /// from the view provider's Render_* dynamic properties. Return false
    /// when the object has none (no material is written then).
    using GetRenderMaterialFunc = std::function<bool(App::DocumentObject*, RenderMaterial&)>;
    /// Resolve an object's per-face appearance as whole materials, one per
    /// face, when a field beyond diffuse varies across the faces. Return
    /// false when a colour list carries everything -- the colour labels
    /// already say it then. \a pbr reports the appearance's PBR mode; the
    /// slots then carry base colour / roughness / F0 tint with metallic in
    /// the specular alpha, and the appearance always exports as materials
    /// (metallic and roughness have no colour-label channel).
    using GetShapeAppearanceFunc =
        std::function<bool(App::DocumentObject*, std::vector<App::MaterialAppearance>&, bool& pbr)>;
    explicit ExportOCAF2(Handle(TDocStd_Document) hDoc,
                         GetShapeColorsFunc func = GetShapeColorsFunc());

    /** @name The looks, asked of the objects
     *
     * docs/ShapeAppearanceDesign.md sec 14.6.7: what a shape's elements
     * are drawn as is the object's, and what a link lays over what it
     * shows the link's, so an export with no view provider has them. These
     * are a GetShapeColorsFunc and a GetShapeAppearanceFunc.
     */
    //@{
    /** The colours an object draws its faces or edges in
     *
     * @param subname: "Face*" or "Edge*" for every element of the kind,
     *                 or the hidden marker and "*" for what is not shown
     * @return The kind's own colour under its name ("Face"), and a colour
     *         for each element that differs ("Face3"); for a link the
     *         elements it gives a colour, by their paths
     */
    static std::map<std::string, App::Color> objectColors(App::DocumentObject* obj,
                                                         const char* subname);
    /// The whole materials of an object's faces, where they say what
    /// colours cannot
    static bool objectAppearance(App::DocumentObject* obj,
                                 std::vector<App::MaterialAppearance>& mats, bool& pbr);
    //@}

    void setGetRenderMaterial(GetRenderMaterialFunc func)
    {
        getRenderMaterial = std::move(func);
    }

    void setGetShapeAppearance(GetShapeAppearanceFunc func)
    {
        getShapeAppearance = std::move(func);
    }

    static ExportOCAFOptions customExportOptions();
    void setExportOptions(ExportOCAFOptions opts)
    {
        options = opts;
    }
    void setExportHiddenObject(bool enable)
    {
        options.exportHidden = enable;
    }
    void setKeepPlacement(bool enable)
    {
        options.keepPlacement = enable;
    }
    void exportObjects(std::vector<App::DocumentObject*>& objs, const char* name = nullptr);
    bool canFallback(std::vector<App::DocumentObject*> objs);

    /** Run \a pass at the end of every exportObjects(), replacing the one
     * of that name: it adds to the document what a module of its own
     * makes of the objects exported (the PMI module, their GD&T)
     */
    static void addPostExport(const std::string& name,
                              std::function<void(const PostExport&)> pass);

    /** @name What an object says of how it is written
     *
     * Dynamic properties an import gives (the PMI module's, of a STEP file:
     * docs/GDandT.md sec 10.26), read by exportObjects()
     */
    //@{
    /// An App::Part written as one shape -- one STEP product, each child a
    /// representation of it -- not as an assembly of its children: a
    /// PropertyBool of this name, true
    static constexpr const char* OneShapeProperty = "ExportAsOneShape";
    /// An object left out of the shapes written, and what it holds: a part's
    /// supplemental geometry, which the PMI pass writes as such (a group
    /// with a PropertyBool of this name, true)
    static constexpr const char* SupplementalProperty = "SupplementalGeometry";
    /// Whether \a obj is left out (SupplementalProperty)
    static bool isSupplemental(const App::DocumentObject* obj);
    /// Whether \a obj is written as one shape (OneShapeProperty)
    static bool isOneShape(const App::DocumentObject* obj);
    /// The container an import made of a file's several free shapes (NIST
    /// FTC-09's four products, of no assembly): written as they were, each a
    /// free shape, not an assembly of them -- a PropertyBool of this name, true
    static constexpr const char* FreeShapesProperty = "ExportAsFreeShapes";
    /// The layers the file put an object and its parts on, written again
    /// each as it was (two of one name stay two: NIST STC-06's layer '1'):
    /// a PropertyStringList of this name, each "element<TAB>ordinal<TAB>
    /// hidden<TAB>name<TAB>description", the element empty for the whole
    static constexpr const char* StepLayersProperty = "StepLayers";
    /// One entry of StepLayersProperty: its element (empty for the whole)
    /// and its layer; false when it is no such entry
    static bool parseStepLayer(const std::string& entry,
                               std::string& element,
                               XCAFDoc_LayerTool::ItemLayer& layer);
    /// A product's user defined attributes, written to STEP as such: PropertyMap
    /// of text values, of integers, of reals (as their text)
    static constexpr const char* AttributesProperty = "StepAttributes";
    static constexpr const char* IntegerAttributesProperty = "StepIntegerAttributes";
    static constexpr const char* RealAttributesProperty = "StepRealAttributes";
    /// The measure a real attribute was given as (LENGTH_MEASURE: the real in mm, written in
    /// the file's length unit): PropertyMap of the real's key and the measure
    static constexpr const char* AttributeMeasuresProperty = "StepAttributeMeasures";
    /// Written with geometric validation properties computed of its shape: a
    /// PropertyBool of this name, true
    static constexpr const char* ValidationProperty = "StepValidation";
    /// The documents its GD&T cites (the standards it follows): PropertyStringList, each
    /// its fields one a line, as STEPCAFControl_Reader keeps them (XCAFDoc::CitedDocumentsGUID)
    static constexpr const char* DocumentsProperty = "StepDocuments";
    //@}

private:
    TDF_Label exportObject(App::DocumentObject* obj,
                           const char* sub,
                           TDF_Label parent,
                           const char* name = nullptr);
    void setupObject(TDF_Label label,
                     App::DocumentObject* obj,
                     const Part::TopoShape& shape,
                     const std::string& prefix,
                     const char* name = nullptr,
                     bool force = false);
    void setName(TDF_Label label, App::DocumentObject* obj, const char* name = nullptr);
    /// The STEP attributes and validation properties of \a obj on \a label
    void setupMetadata(TDF_Label label, App::DocumentObject* obj, const Part::TopoShape& shape);
    void setInvisible(TDF_Label label);
    TDF_Label findComponent(const char* subname, TDF_Label label, TDF_LabelSequence& labels);
    /// Run the passes added by addPostExport()
    void runPostExport();

private:
    Handle(TDocStd_Document) pDoc;
    Handle(XCAFDoc_ShapeTool) aShapeTool;
    /// The layers \a obj's property of them puts \a shape (\a ownL's) and its
    /// elements on (StepLayersProperty), on \a ownL and sub-shape labels of
    /// \a shapeL, where the writer finds them
    void setItemLayers(TDF_Label shapeL,
                       TDF_Label ownL,
                       const Part::TopoShape& shape,
                       App::DocumentObject* obj);
    Handle(XCAFDoc_ColorTool) aColorTool;

    std::unordered_map<App::DocumentObject*, TDF_Label> myObjects;
    /// The children of a container written as one shape (OneShapeProperty): its label
    std::unordered_map<App::DocumentObject*, TDF_Label> myWithin;
    /// setupObject() gives an object's own colours only, not its elements'
    bool myObjectColorsOnly = false;

    std::unordered_map<TDF_Label, std::vector<std::string>, LabelHasher> myNames;

    std::set<std::pair<App::DocumentObject*, std::string>> mySetups;

    std::vector<App::DocumentObject*> groupLinks;

    GetShapeColorsFunc getShapeColors;
    GetRenderMaterialFunc getRenderMaterial;
    GetShapeAppearanceFunc getShapeAppearance;

    ExportOCAFOptions options;
};

}  // namespace Import

#endif  // IMPORT_EXPORTOCAF2_H
