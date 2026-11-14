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

#include "PreCompiled.h"

#include <QMenu>

#include <App/Animation.h>
#include <App/AutoTransaction.h>

#include "AnimationTimeline.h"
#include "Application.h"
#include "BitmapFactory.h"
#include "Command.h"
#include "ViewProviderAnimation.h"

using namespace Gui;

PROPERTY_SOURCE(Gui::ViewProviderAnimation, Gui::ViewProviderDocumentObject)

ViewProviderAnimation::ViewProviderAnimation()
{
    sPixmap = "Animation";
}

ViewProviderAnimation::~ViewProviderAnimation() = default;

bool ViewProviderAnimation::doubleClicked()
{
    auto anim = freecad_dynamic_cast<App::Animation>(getObject());
    if (!anim) {
        return false;
    }
    chooseAnimation(anim);
    DockWnd::AnimationTimeline::showTimeline();
    anim->activate();
    return true;
}

bool ViewProviderAnimation::setEdit(int ModNum)
{
    // Its edit is making it the timeline's: no task dialog (3.10)
    if (ModNum == ViewProvider::Default) {
        doubleClicked();
        return false;
    }
    return inherited::setEdit(ModNum);
}

void ViewProviderAnimation::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    auto anim = freecad_dynamic_cast<App::Animation>(getObject());
    if (!anim) {
        return;
    }
    App::DocumentObjectT objT(anim);
    auto get = [objT] { return freecad_dynamic_cast<App::Animation>(objT.getObject()); };
    menu->addAction(BitmapFactory().iconFromTheme("Animation"), QObject::tr("Show in the timeline"),
                    [get] {
                        if (auto a = get()) {
                            chooseAnimation(a);
                            DockWnd::AnimationTimeline::showTimeline();
                        }
                    });
    menu->addAction(BitmapFactory().iconFromTheme("AnimationPlay"), QObject::tr("Play"), [get] {
        if (auto a = get()) {
            chooseAnimation(a);
            AnimationPlayer::instance()->play(a);
        }
    });
    menu->addAction(BitmapFactory().iconFromTheme("AnimationStop"),
                    QObject::tr("Stop: put the design back"), [get] {
                        if (auto a = get()) {
                            if (AnimationPlayer::instance()->animation() == a) {
                                AnimationPlayer::instance()->stop();
                            }
                            else if (a->Active.getValue()) {
                                a->Active.setValue(false);
                            }
                        }
                    });
    menu->addAction(QObject::tr("Remove unresolved tracks"), [get] {
        auto a = get();
        if (!a) {
            return;
        }
        std::vector<int> gone;
        for (auto& t : a->Tracks.getValues()) {
            if (a->state(t.id) == App::AnimationTrackState::Unresolved) {
                gone.push_back(t.id);
            }
        }
        if (gone.empty()) {
            return;
        }
        App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Remove unresolved tracks"));
        for (int id : gone) {
            a->Tracks.removeTrack(id);
        }
    });
    inherited::setupContextMenu(menu, receiver, member);
}
