// SPDX-License-Identifier: LGPL-2.1-or-later
/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include <TDF_Label.hxx>

#include <Mod/Part/App/OCCTFork.h>

#include "OCCTForkOCAF.h"

namespace Import
{

typedef void (*FuncAddItemLayer)(const TDF_Label&, const XCAFDoc_LayerTool::ItemLayer&);

bool addOCCTItemLayer(const TDF_Label& item, const XCAFDoc_LayerTool::ItemLayer& layer)
{
    static const auto func =
        (FuncAddItemLayer)Part::lookUpOCCT("TKXCAF", "XCAFDoc_LayerTool_AddItemLayer");
    if (!func) {
        return false;
    }
    func(item, layer);
    return true;
}

typedef bool (*FuncGetItemLayers)(const TDF_Label&,
                                  NCollection_Sequence<XCAFDoc_LayerTool::ItemLayer>&);

bool getOCCTItemLayers(const TDF_Label& item,
                       NCollection_Sequence<XCAFDoc_LayerTool::ItemLayer>& layers)
{
    static const auto func =
        (FuncGetItemLayers)Part::lookUpOCCT("TKXCAF", "XCAFDoc_LayerTool_GetItemLayers");
    layers.Clear();
    return func && func(item, layers);
}

}  // namespace Import
