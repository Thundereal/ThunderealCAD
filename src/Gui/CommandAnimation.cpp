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

/** The animation commands (docs/Animation.md 3.9 and 6), with the old
 * branch's names and icons.
 */

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>

#include <QApplication>

#include <App/Animation.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/GeoFeature.h>
#include <Base/Interpreter.h>

#include "Action.h"
#include "AnimationTimeline.h"
#include "Application.h"
#include "Command.h"
#include "MainWindow.h"
#include "Selection/Selection.h"
#include "MDIView.h"

using namespace Gui;

namespace
{

App::Animation* current()
{
    return activeAnimation();
}

/// Set Time to `t`; at the time already shown, evaluate there
void goTo(App::Animation* anim, double t)
{
    if (std::fabs(anim->Time.getValue() - t) > App::Anim::TimeEpsilon) {
        anim->Time.setValue(t);
    }
    else {
        anim->activate();
    }
}

/// The active document's Animation, made when there is none
App::Animation* currentOrNew()
{
    if (auto anim = current()) {
        return anim;
    }
    auto doc = App::GetApplication().getActiveDocument();
    if (!doc) {
        return nullptr;
    }
    Command::doCommand(Command::Doc, "App.ActiveDocument.addObject('App::Animation', 'Animation')");
    auto anims = doc->getObjectsOfType(App::Animation::getClassTypeId());
    if (anims.empty()) {
        return nullptr;
    }
    auto anim = static_cast<App::Animation*>(anims.back());
    chooseAnimation(anim);
    return anim;
}

}  // namespace

DEF_STD_CMD_A(StdCmdAnimationNew)

StdCmdAnimationNew::StdCmdAnimationNew()
    : Command("Std_AnimationNew")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("New animation");
    sToolTipText = QT_TR_NOOP("Make an animation: key frames for any property, played in the "
                              "timeline");
    sWhatsThis = "Std_AnimationNew";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationNew";
}

void StdCmdAnimationNew::activated(int)
{
    auto doc = App::GetApplication().getActiveDocument();
    if (!doc) {
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "New animation"));
    doCommand(Doc, "App.ActiveDocument.addObject('App::Animation', 'Animation')");
    commitCommand();
    auto anims = doc->getObjectsOfType(App::Animation::getClassTypeId());
    if (!anims.empty()) {
        chooseAnimation(static_cast<App::Animation*>(anims.back()));
    }
    DockWnd::AnimationTimeline::showTimeline();
}

bool StdCmdAnimationNew::isActive()
{
    return App::GetApplication().getActiveDocument() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationTimeline)

StdCmdAnimationTimeline::StdCmdAnimationTimeline()
    : Command("Std_AnimationTimeline")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Timeline");
    sToolTipText = QT_TR_NOOP("Show the animation timeline");
    sWhatsThis = "Std_AnimationTimeline";
    sStatusTip = sToolTipText;
    sPixmap = "Animation";
}

void StdCmdAnimationTimeline::activated(int)
{
    DockWnd::AnimationTimeline::showTimeline();
}

bool StdCmdAnimationTimeline::isActive()
{
    return true;
}

DEF_STD_CMD_A(StdCmdAnimationPlay)

StdCmdAnimationPlay::StdCmdAnimationPlay()
    : Command("Std_AnimationPlay")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Play");
    sToolTipText = QT_TR_NOOP("Play the animation; while it plays, pause it");
    sWhatsThis = "Std_AnimationPlay";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationPlay";
    eType = 0;
}

void StdCmdAnimationPlay::activated(int)
{
    auto player = AnimationPlayer::instance();
    auto anim = current();
    if (!anim) {
        return;
    }
    if (player->isPlaying() && player->animation() == anim) {
        player->pause();
    }
    else {
        player->play(anim);
    }
}

bool StdCmdAnimationPlay::isActive()
{
    return current() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationPause)

StdCmdAnimationPause::StdCmdAnimationPause()
    : Command("Std_AnimationPause")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Pause");
    sToolTipText = QT_TR_NOOP("Pause playing; the pose stays");
    sWhatsThis = "Std_AnimationPause";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationPause";
    eType = 0;
}

void StdCmdAnimationPause::activated(int)
{
    AnimationPlayer::instance()->pause();
}

bool StdCmdAnimationPause::isActive()
{
    return AnimationPlayer::instance()->isPlaying();
}

DEF_STD_CMD_A(StdCmdAnimationStop)

StdCmdAnimationStop::StdCmdAnimationStop()
    : Command("Std_AnimationStop")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Stop");
    sToolTipText = QT_TR_NOOP("Stop playing and put the design back: every animated property "
                              "takes its value from before the animation");
    sWhatsThis = "Std_AnimationStop";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationStop";
    eType = 0;
}

void StdCmdAnimationStop::activated(int)
{
    auto player = AnimationPlayer::instance();
    auto anim = current();
    if (player->animation() && (!anim || player->animation() == anim)) {
        player->stop();
    }
    else if (anim && anim->Active.getValue()) {
        anim->Active.setValue(false);
    }
}

bool StdCmdAnimationStop::isActive()
{
    auto anim = current();
    return anim && (anim->Active.getValue() || AnimationPlayer::instance()->isPlaying());
}

DEF_STD_CMD_A(StdCmdAnimationStart)

StdCmdAnimationStart::StdCmdAnimationStart()
    : Command("Std_AnimationStart")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Go to start");
    sToolTipText = QT_TR_NOOP("Go to the start of the play range");
    sWhatsThis = "Std_AnimationStart";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationPrev";
    eType = 0;
}

void StdCmdAnimationStart::activated(int)
{
    if (auto anim = current()) {
        goTo(anim, anim->Start.getValue());
    }
}

bool StdCmdAnimationStart::isActive()
{
    return current() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationPrev)

StdCmdAnimationPrev::StdCmdAnimationPrev()
    : Command("Std_AnimationPrev")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Previous key");
    sToolTipText = QT_TR_NOOP("Go to the key before the current time. Hold Ctrl for the frame "
                              "before");
    sWhatsThis = "Std_AnimationPrev";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationPrev";
    eType = 0;
}

void StdCmdAnimationPrev::activated(int)
{
    auto anim = current();
    if (!anim) {
        return;
    }
    if (QApplication::queryKeyboardModifiers() & Qt::ControlModifier) {
        double rate = std::max(1.0, anim->FrameRate.getValue());
        goTo(anim, (std::round(anim->Time.getValue() * rate) - 1) / rate);
    }
    else {
        goTo(anim, anim->nextKeyTime(false));
    }
}

bool StdCmdAnimationPrev::isActive()
{
    return current() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationNext)

StdCmdAnimationNext::StdCmdAnimationNext()
    : Command("Std_AnimationNext")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Next key");
    sToolTipText = QT_TR_NOOP("Go to the key after the current time. Hold Ctrl for the frame "
                              "after");
    sWhatsThis = "Std_AnimationNext";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationNext";
    eType = 0;
}

void StdCmdAnimationNext::activated(int)
{
    auto anim = current();
    if (!anim) {
        return;
    }
    if (QApplication::queryKeyboardModifiers() & Qt::ControlModifier) {
        double rate = std::max(1.0, anim->FrameRate.getValue());
        goTo(anim, (std::round(anim->Time.getValue() * rate) + 1) / rate);
    }
    else {
        goTo(anim, anim->nextKeyTime(true));
    }
}

bool StdCmdAnimationNext::isActive()
{
    return current() != nullptr;
}

DEF_STD_CMD_AC(StdCmdAnimationRecord)

StdCmdAnimationRecord::StdCmdAnimationRecord()
    : Command("Std_AnimationRecord")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Record");
    sToolTipText = QT_TR_NOOP("Record: every property changed is keyed at the current time. "
                              "A drag keys where it ends");
    sWhatsThis = "Std_AnimationRecord";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationRecord";
    eType = 0;
}

Action* StdCmdAnimationRecord::createAction()
{
    Action* action = Command::createAction();
    action->setCheckable(true);
    return action;
}

void StdCmdAnimationRecord::activated(int)
{
    auto anim = currentOrNew();
    if (!anim) {
        return;
    }
    anim->Recording.setValue(!anim->Recording.getValue());
    if (_pcAction) {
        _pcAction->setChecked(anim->Recording.getValue(), true);
    }
}

bool StdCmdAnimationRecord::isActive()
{
    auto anim = current();
    if (_pcAction) {
        _pcAction->setChecked(anim && anim->Recording.getValue(), true);
    }
    return App::GetApplication().getActiveDocument() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationNewFrame)

StdCmdAnimationNewFrame::StdCmdAnimationNewFrame()
    : Command("Std_AnimationNewFrame")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Key selection");
    sToolTipText = QT_TR_NOOP("Key the placement of each object selected at the current time");
    sWhatsThis = "Std_AnimationNewFrame";
    sStatusTip = sToolTipText;
    sPixmap = "AnimationNewFrame";
    eType = 0;
}

void StdCmdAnimationNewFrame::activated(int)
{
    std::vector<App::DocumentObject*> objs;
    for (auto& sel : Selection().getSelectionEx()) {
        auto obj = sel.getObject();
        if (obj && obj->isDerivedFrom(App::GeoFeature::getClassTypeId())
            && std::find(objs.begin(), objs.end(), obj) == objs.end()) {
            objs.push_back(obj);
        }
    }
    if (objs.empty()) {
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Key selection"));
    auto anim = currentOrNew();
    if (!anim) {
        abortCommand();
        return;
    }
    try {
        for (auto obj : objs) {
            if (obj->getDocument() != anim->getDocument() || obj == anim) {
                continue;
            }
            anim->keyNow(anim->parsePath(std::string(obj->getNameInDocument()) + ".Placement"));
        }
        commitCommand();
    }
    catch (Base::Exception& e) {
        e.ReportException();
        abortCommand();
    }
    DockWnd::AnimationTimeline::showTimeline();
}

bool StdCmdAnimationNewFrame::isActive()
{
    return Selection().size() > 0;
}

DEF_STD_CMD_A(StdCmdAnimationAddStrip)

StdCmdAnimationAddStrip::StdCmdAnimationAddStrip()
    : Command("Std_AnimationAddStrip")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Add as strips");
    sToolTipText = QT_TR_NOOP("Play the Animations selected in the timeline's Animation, one "
                              "after another: strips to place, repeat, reverse and add");
    sWhatsThis = "Std_AnimationAddStrip";
    sStatusTip = sToolTipText;
    sPixmap = "Animation";
    eType = 0;
}

namespace
{
std::vector<App::Animation*> selectedAnimations(App::Animation* except)
{
    std::vector<App::Animation*> res;
    for (auto& sel : Selection().getSelectionEx()) {
        auto anim = freecad_dynamic_cast<App::Animation>(sel.getObject());
        if (anim && anim != except && std::find(res.begin(), res.end(), anim) == res.end()) {
            res.push_back(anim);
        }
    }
    return res;
}
}  // namespace

void StdCmdAnimationAddStrip::activated(int)
{
    auto anim = current();
    auto children = selectedAnimations(anim);
    if (!anim || children.empty()) {
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Add strips"));
    try {
        for (auto child : children) {
            anim->addStrip(child, App::AnimationStrip());
        }
        commitCommand();
    }
    catch (Base::Exception& e) {
        e.ReportException();
        abortCommand();
    }
    DockWnd::AnimationTimeline::showTimeline();
}

bool StdCmdAnimationAddStrip::isActive()
{
    auto anim = current();
    return anim && !selectedAnimations(anim).empty();
}

DEF_STD_CMD_A(StdCmdAnimationKeyCamera)

StdCmdAnimationKeyCamera::StdCmdAnimationKeyCamera()
    : Command("Std_AnimationKeyCamera")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Key the camera");
    sToolTipText = QT_TR_NOOP("Key the active 3D view's camera at the current time: the "
                              "animation then flies the view");
    sWhatsThis = "Std_AnimationKeyCamera";
    sStatusTip = sToolTipText;
    sPixmap = "camera-photo";
    eType = 0;
}

void StdCmdAnimationKeyCamera::activated(int)
{
    auto view = getMainWindow()->activeWindow();
    std::string name = view ? view->getPersistentName() : std::string();
    openCommand(QT_TRANSLATE_NOOP("Command", "Key the camera"));
    auto anim = currentOrNew();
    if (!anim) {
        abortCommand();
        return;
    }
    try {
        int track = anim->handlerTrackOf("Camera", name, true);
        anim->setKey(track, anim->Time.getValue());
        commitCommand();
    }
    catch (Base::Exception& e) {
        e.ReportException();
        abortCommand();
    }
    DockWnd::AnimationTimeline::showTimeline();
}

bool StdCmdAnimationKeyCamera::isActive()
{
    return App::GetApplication().getActiveDocument() != nullptr
        && getMainWindow()->activeWindow() != nullptr;
}

DEF_STD_CMD_A(StdCmdAnimationExport)

StdCmdAnimationExport::StdCmdAnimationExport()
    : Command("Std_AnimationExport")
{
    sGroup = "Animation";
    sMenuText = QT_TR_NOOP("Export animation...");
    sToolTipText = QT_TR_NOOP("Save the animation as a video, a GIF, an image sequence or an "
                              "animated glTF: every frame of its range, as the active 3D view "
                              "shows it");
    sWhatsThis = "Std_AnimationExport";
    sStatusTip = sToolTipText;
    sPixmap = "Std_Export";
    eType = 0;
}

void StdCmdAnimationExport::activated(int)
{
    auto anim = current();
    if (!anim || !anim->getDocument()) {
        return;
    }
    // The exporter is Python's, shared with the Assembly simulation
    // (freecad.videoexport, docs/Animation.md sec 9)
    try {
        Base::Interpreter().runStringArg(
            "from freecad import videoexport as _fcve\n"
            "_fcve.exportAnimationDialog(FreeCAD.getDocument('%s').getObject('%s'))",
            anim->getDocument()->getName(),
            anim->getNameInDocument());
    }
    catch (Base::Exception& e) {
        e.ReportException();
    }
}

bool StdCmdAnimationExport::isActive()
{
    return current() != nullptr;
}

namespace Gui
{

void registerAnimationCamera();

void CreateAnimationCommands()
{
    registerAnimationCamera();
    CommandManager& mgr = Application::Instance->commandManager();
    mgr.addCommand(new StdCmdAnimationKeyCamera());
    mgr.addCommand(new StdCmdAnimationExport());
    mgr.addCommand(new StdCmdAnimationAddStrip());
    mgr.addCommand(new StdCmdAnimationNew());
    mgr.addCommand(new StdCmdAnimationTimeline());
    mgr.addCommand(new StdCmdAnimationPlay());
    mgr.addCommand(new StdCmdAnimationPause());
    mgr.addCommand(new StdCmdAnimationStop());
    mgr.addCommand(new StdCmdAnimationStart());
    mgr.addCommand(new StdCmdAnimationPrev());
    mgr.addCommand(new StdCmdAnimationNext());
    mgr.addCommand(new StdCmdAnimationRecord());
    mgr.addCommand(new StdCmdAnimationNewFrame());
}

}  // namespace Gui
