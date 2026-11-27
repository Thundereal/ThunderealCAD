// SPDX-License-Identifier: LGPL-2.1-or-later
/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 ***************************************************************************/

#ifndef IMPORT_OCCTFORKOCAF_H
#define IMPORT_OCCTFORKOCAF_H

/** The fork's functions on an XCAF document that only the exchange needs
 *
 * Mod/Part/App/OCCTFork.h is the list of what FreeCAD needs of the fork;
 * these two are kept here rather than there because they take OCAF types
 * (TDF_Label, XCAFDoc_LayerTool::ItemLayer), whose inlines reach TKLCAF:
 * Part does not link the OCAF toolkits, and a link that refuses an
 * undefined symbol (Linux) failed on them. Looked up by name through
 * Part::lookUpOCCT like every other, so Import still loads on upstream's
 * libraries.
 */

#include <NCollection_Sequence.hxx>
#include <XCAFDoc_LayerTool.hxx>

class TDF_Label;

namespace Import
{

/// Puts a shape's item on a layer as the file had it (the fork's
/// XCAFDoc_LayerTool_AddItemLayer); false without it, where XCAF's layers by
/// name are all there is
bool addOCCTItemLayer(const TDF_Label& item, const XCAFDoc_LayerTool::ItemLayer& layer);

/// The layers a shape's item is on as the file had them (the fork's
/// XCAFDoc_LayerTool_GetItemLayers); false when it is on none, and without it
bool getOCCTItemLayers(const TDF_Label& item,
                       NCollection_Sequence<XCAFDoc_LayerTool::ItemLayer>& layers);

}  // namespace Import

#endif  // IMPORT_OCCTFORKOCAF_H
