# What-if proposals for include_graph.py whatif.
# cut: parent -> child   drops that #include edge ("<src>" = every .cpp that includes it directly)
# add: parent -> child   adds an edge (where the child is in the TU's real header set)
# needs: regex           a TU whose .cpp/.h matches keeps the cut children (it would include them)
# Without --no-iwyu every TU that names something a lost header declares gets it back.

name: P01 ActiveObjectList.h includes TreeItemMode.h, not Tree.h
cut: src/Gui/ActiveObjectList.h -> src/Gui/Tree.h
add: src/Gui/ActiveObjectList.h -> src/Gui/TreeItemMode.h

name: P02 Gui/Document.h includes View.h, not MDIView.h
cut: src/Gui/Document.h -> src/Gui/MDIView.h
add: src/Gui/Document.h -> src/Gui/View.h

name: P03 Gui/Document.h forward-declares App::Document
cut: src/Gui/Document.h -> src/App/Document.h
needs: getDocument\(\)\s*->|getDocument\(\)\.

name: P04 ViewProviderFeaturePython.h: one inline call out of line, drop Gui/Document.h
cut: src/Gui/ViewProviderFeaturePython.h -> src/Gui/Document.h

name: P05 MaterialValue.h includes Base/Quantity.h, not Gui/MetaTypes.h
cut: src/Mod/Material/App/MaterialValue.h -> src/Gui/MetaTypes.h
add: src/Mod/Material/App/MaterialValue.h -> src/Base/Quantity.h

name: P06 PropertyMaterial.h forward-declares Material and XMLReader
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Mod/Material/App/Materials.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Base/Reader.h

name: P07 ElementMap.h drops its unused App/Application.h
cut: src/App/ElementMap.h -> src/App/Application.h

name: P08 Exception.h: FileException keeps a path, forward-declares FileInfo
cut: src/Base/Exception.h -> src/Base/FileInfo.h

name: P09 Observer.h: Subject's logging out of line, no Console.h / Exception.h
cut: src/Base/Observer.h -> src/Base/Console.h
cut: src/Base/Observer.h -> src/Base/Exception.h

name: P10 PropertyAppearance/PropertyAppearanceList out of PropertyStandard.h
cut: src/App/PropertyStandard.h -> src/App/AppearanceList.h
cut: src/App/PropertyStandard.h -> src/App/FileBlobManager.h
cut: src/App/PropertyStandard.h -> src/App/MaterialAppearance.h
add: src/App/Link.h -> src/App/AppearanceList.h
add: src/App/Part.h -> src/App/AppearanceList.h
add: src/App/FeatureTest.h -> src/App/AppearanceList.h
add: src/App/LinkAppearance.h -> src/App/AppearanceList.h
add: src/Gui/ViewProviderGeometryObject.h -> src/App/AppearanceList.h
add: src/Gui/ViewProviderLink.h -> src/App/AppearanceList.h
add: src/Gui/LinkLooks.h -> src/App/AppearanceList.h
add: src/Gui/propertyeditor/PropertyItem.h -> src/App/AppearanceList.h
add: src/Gui/DlgMaterialPropertiesImp.h -> src/App/AppearanceList.h
add: src/Mod/Part/App/PartFeature.h -> src/App/AppearanceList.h
add: src/Mod/Part/Gui/ViewProviderExt.h -> src/App/AppearanceList.h
add: src/Mod/Part/Gui/ViewProviderReference.h -> src/App/AppearanceList.h
add: src/Mod/Material/App/MaterialClipboard.h -> src/App/AppearanceList.h
add: src/Mod/Mesh/Gui/ViewProviderCurvature.h -> src/App/AppearanceList.h

name: P11 StringHasher.h drops CXX/Objects.hxx
cut: src/App/StringHasher.h -> src/CXX/Objects.hxx

name: P12 Command.h forward-declares Gui::Application
cut: src/Gui/Command.h -> src/Gui/Application.h
needs: FCMD_SET_EDIT

name: P13 SoFCUnifiedSelection.h forward-declares View3DInventorViewer
cut: src/Gui/SoFCUnifiedSelection.h -> src/Gui/View3DInventorViewer.h

name: P14 hash_combine in its own tiny header; TopoShape.h / InventorBase.h drop Base/Tools.h
cut: src/Mod/Part/App/TopoShape.h -> src/Base/Tools.h
cut: src/Gui/InventorBase.h -> src/Base/Tools.h

name: P15 ViewProviderSketch.h drops App/Part.h, Gui/Document.h, BodyBase.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/App/Part.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Gui/Document.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Mod/Part/App/BodyBase.h

name: P16 DocumentObject.h forward-declares Placement/PropertyPlacement (no PropertyGeo.h)
cut: src/App/DocumentObject.h -> src/App/PropertyGeo.h
cut: src/App/DocumentObject.h -> src/Base/Placement.h

name: P17 QuantitySpinBox.h includes Base/Quantity.h, not Gui/MetaTypes.h
cut: src/Gui/QuantitySpinBox.h -> src/Gui/MetaTypes.h
add: src/Gui/QuantitySpinBox.h -> src/Base/Quantity.h

name: I01 stale direct includes of App/Document.h in .cpp files
cut: <src> -> src/App/Document.h

name: I02 stale direct includes of Gui/Application.h in .cpp files
cut: <src> -> src/Gui/Application.h

name: I03 stale direct includes of Gui/Document.h in .cpp files
cut: <src> -> src/Gui/Document.h

name: I04 stale direct includes of Gui/Command.h in .cpp files
cut: <src> -> src/Gui/Command.h

name: I05 stale direct includes of Gui/View3DInventorViewer.h in .cpp files
cut: <src> -> src/Gui/View3DInventorViewer.h

name: I06 stale direct includes of App/Application.h in .cpp files
cut: <src> -> src/App/Application.h

name: I07 stale direct includes of Base/Tools.h in .cpp files
cut: <src> -> src/Base/Tools.h

name: C01 Gui/Document.h diet (P02+P03+P04+P15 + I03)
cut: src/Gui/Document.h -> src/Gui/MDIView.h
add: src/Gui/Document.h -> src/Gui/View.h
cut: src/Gui/Document.h -> src/App/Document.h
cut: src/Gui/ViewProviderFeaturePython.h -> src/Gui/Document.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Gui/Document.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/App/Part.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Mod/Part/App/BodyBase.h
cut: <src> -> src/Gui/Document.h
needs@src/App/Document.h: getDocument\(\)\s*->|getDocument\(\)\.

name: C02 Material decoupling (P05+P06+P17)
cut: src/Mod/Material/App/MaterialValue.h -> src/Gui/MetaTypes.h
add: src/Mod/Material/App/MaterialValue.h -> src/Base/Quantity.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Mod/Material/App/Materials.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Base/Reader.h
cut: src/Gui/QuantitySpinBox.h -> src/Gui/MetaTypes.h
add: src/Gui/QuantitySpinBox.h -> src/Base/Quantity.h

name: C03 everything above together
cut: src/Gui/ActiveObjectList.h -> src/Gui/Tree.h
add: src/Gui/ActiveObjectList.h -> src/Gui/TreeItemMode.h
cut: src/Gui/Document.h -> src/Gui/MDIView.h
add: src/Gui/Document.h -> src/Gui/View.h
cut: src/Gui/Document.h -> src/App/Document.h
cut: src/Gui/ViewProviderFeaturePython.h -> src/Gui/Document.h
cut: src/Mod/Material/App/MaterialValue.h -> src/Gui/MetaTypes.h
add: src/Mod/Material/App/MaterialValue.h -> src/Base/Quantity.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Mod/Material/App/Materials.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/Base/Reader.h
cut: src/App/ElementMap.h -> src/App/Application.h
cut: src/Base/Exception.h -> src/Base/FileInfo.h
cut: src/Base/Observer.h -> src/Base/Console.h
cut: src/Base/Observer.h -> src/Base/Exception.h
cut: src/App/PropertyStandard.h -> src/App/AppearanceList.h
cut: src/App/PropertyStandard.h -> src/App/FileBlobManager.h
cut: src/App/PropertyStandard.h -> src/App/MaterialAppearance.h
add: src/App/Link.h -> src/App/AppearanceList.h
add: src/App/Part.h -> src/App/AppearanceList.h
add: src/App/FeatureTest.h -> src/App/AppearanceList.h
add: src/App/LinkAppearance.h -> src/App/AppearanceList.h
add: src/Gui/ViewProviderGeometryObject.h -> src/App/AppearanceList.h
add: src/Gui/ViewProviderLink.h -> src/App/AppearanceList.h
add: src/Gui/LinkLooks.h -> src/App/AppearanceList.h
add: src/Gui/propertyeditor/PropertyItem.h -> src/App/AppearanceList.h
add: src/Gui/DlgMaterialPropertiesImp.h -> src/App/AppearanceList.h
add: src/Mod/Part/App/PartFeature.h -> src/App/AppearanceList.h
add: src/Mod/Part/Gui/ViewProviderExt.h -> src/App/AppearanceList.h
add: src/Mod/Part/Gui/ViewProviderReference.h -> src/App/AppearanceList.h
add: src/Mod/Material/App/MaterialClipboard.h -> src/App/AppearanceList.h
add: src/Mod/Mesh/Gui/ViewProviderCurvature.h -> src/App/AppearanceList.h
cut: src/App/StringHasher.h -> src/CXX/Objects.hxx
cut: src/Gui/Command.h -> src/Gui/Application.h
cut: src/Gui/SoFCUnifiedSelection.h -> src/Gui/View3DInventorViewer.h
cut: src/Mod/Part/App/TopoShape.h -> src/Base/Tools.h
cut: src/Gui/InventorBase.h -> src/Base/Tools.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/App/Part.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Gui/Document.h
cut: src/Mod/Sketcher/Gui/ViewProviderSketch.h -> src/Mod/Part/App/BodyBase.h
cut: src/App/DocumentObject.h -> src/App/PropertyGeo.h
cut: src/App/DocumentObject.h -> src/Base/Placement.h
cut: src/Gui/QuantitySpinBox.h -> src/Gui/MetaTypes.h
add: src/Gui/QuantitySpinBox.h -> src/Base/Quantity.h
needs@src/Gui/Application.h: FCMD_SET_EDIT
needs@src/App/Document.h: getDocument\(\)\s*->|getDocument\(\)\.
