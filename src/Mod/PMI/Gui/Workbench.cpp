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

#include "PreCompiled.h"

#include <Gui/MenuManager.h>
#include <Gui/ToolBarManager.h>

#include "Workbench.h"

using namespace PMIGui;

TYPESYSTEM_SOURCE(PMIGui::Workbench, Gui::StdWorkbench)

Workbench::Workbench() = default;

Workbench::~Workbench() = default;

void Workbench::addTools(Gui::ToolBarItem& toolbar)
{
    toolbar << "PMI_Datum"
            << "PMI_GeometricTolerance"
            << "PMI_Dimension"
            << "PMI_Note"
            << "Separator"
            << "PMI_View"
            << "PMI_Query"
            << "PMI_Check";
}

Gui::MenuItem* Workbench::setupMenuBar() const
{
    Gui::MenuItem* root = StdWorkbench::setupMenuBar();
    Gui::MenuItem* item = root->findItem("&Windows");
    auto pmi = new Gui::MenuItem;
    root->insertItem(item, pmi);
    pmi->setCommand("&PMI");
    *pmi << "PMI_Datum"
         << "PMI_GeometricTolerance"
         << "PMI_Dimension"
         << "PMI_Note"
         << "Separator"
         << "PMI_View"
         << "PMI_Query"
         << "PMI_Check";
    return root;
}

Gui::ToolBarItem* Workbench::setupToolBars() const
{
    Gui::ToolBarItem* root = StdWorkbench::setupToolBars();
    auto pmi = new Gui::ToolBarItem(root);
    pmi->setCommand("PMI");
    addTools(*pmi);
    return root;
}
