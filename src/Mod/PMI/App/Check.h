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

#ifndef PMI_CHECK_H
#define PMI_CHECK_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace App
{
class DocumentObject;
}

namespace PMI
{

class AnnotationSet;

/** The rules of docs/GDandT.md sec 8: what a standard allows, what
 * references must hold, what the model says. App side, so it runs headless
 * and in tests; the task panels and the tree show what it finds.
 */
namespace Check
{
enum Severity
{
    Warning,
    Error,
};

struct PMIExport Issue
{
    const App::DocumentObject* object = nullptr;
    Severity severity = Error;
    std::string message;
};

/// The rules of one object on its own (the standard from its set)
PMIExport std::vector<Issue> object(const App::DocumentObject* obj);

/// Every object of the set, and the rules across them: datum letters
/// defined once and used only when defined
PMIExport std::vector<Issue> set(const AnnotationSet* set);
}  // namespace Check

}  // namespace PMI

#endif  // PMI_CHECK_H
