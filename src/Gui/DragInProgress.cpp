// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include "PreCompiled.h"

#include <QTimer>

#include <App/DocumentObject.h>

#include "Application.h"
#include "Document.h"
#include "DragInProgress.h"
#include "ViewProviderDocumentObject.h"

using namespace Gui;

namespace
{
int depth = 0;
std::function<void()> held;
/// which wait for a recompute after a drag is the current one
unsigned waiting = 0;
/// how long a held pop waits after the drag for the edited object's
/// recompute to say whether its error stands, before looking itself
constexpr int SettleMs = 1500;

bool editedObjectInError()
{
    Gui::Document* doc = Application::Instance ? Application::Instance->editDocument() : nullptr;
    auto vp = doc ? dynamic_cast<ViewProviderDocumentObject*>(doc->getInEdit()) : nullptr;
    App::DocumentObject* obj = vp ? vp->getObject() : nullptr;
    return obj && obj->isError();
}

void letGo(bool pop)
{
    std::function<void()> fn;
    fn.swap(held);
    ++waiting;
    if (pop && fn) {
        fn();
    }
}
}  // namespace

void DragInProgress::begin()
{
    ++depth;
}

void DragInProgress::end()
{
    if (depth <= 0) {
        return;
    }
    if (--depth > 0 || !held) {
        return;
    }
    // The drag's last value is still to be recomputed: its recompute says
    // (recomputed()); if none comes, the edited object is looked at
    const unsigned wait = ++waiting;
    QTimer::singleShot(SettleMs, [wait]() {
        if (wait == waiting && depth == 0 && held) {
            letGo(editedObjectInError());
        }
    });
}

bool DragInProgress::active()
{
    return depth > 0;
}

void DragInProgress::holdPop(std::function<void()> pop)
{
    held = std::move(pop);
}

void DragInProgress::recomputed(bool errorStands)
{
    if (depth > 0 || !held) {
        return;
    }
    letGo(errorStands);
}
