// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

#ifndef GUI_VIEWPROVIDER_ANIMATION_H
#define GUI_VIEWPROVIDER_ANIMATION_H

#include "ViewProviderDocumentObject.h"

namespace Gui
{

/** An App::Animation's tree item (docs/Animation.md 3.9). Its keys are no
 * objects, so it has no children. Double click makes it the timeline's and
 * the document's active Animation; no task dialog is opened.
 */
class GuiExport ViewProviderAnimation: public ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Gui::ViewProviderAnimation);
    using inherited = ViewProviderDocumentObject;

public:
    ViewProviderAnimation();
    ~ViewProviderAnimation() override;

    bool doubleClicked() override;
    void setupContextMenu(QMenu* menu, QObject* receiver, const char* member) override;
    bool isShow() const override
    {
        return true;
    }
    bool useNewSelectionModel() const override
    {
        return false;
    }

protected:
    bool setEdit(int ModNum) override;
};

}  // namespace Gui

#endif  // GUI_VIEWPROVIDER_ANIMATION_H
