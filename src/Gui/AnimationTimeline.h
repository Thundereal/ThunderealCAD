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

#ifndef GUI_ANIMATIONTIMELINE_H
#define GUI_ANIMATIONTIMELINE_H

/** The animation timeline (docs/Animation.md 3.9): a dock window, so it can
 * be an overlay like any other, showing the chosen Animation of the active
 * document -- transport, the time ruler, a row of keys per track -- and the
 * clock that plays it.
 */

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <fastsignals/signal.h>

#include <App/DocumentObserver.h>
#include <Gui/DockWindow.h>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QToolButton;

namespace App
{
class Animation;
class Document;
class DocumentObject;
class ObjectIdentifier;
class Property;
}  // namespace App

namespace Gui
{

/// The Animation the timeline shows for `doc` (the active document when
/// null): the one chosen in it, else the active one, else the first
GuiExport App::Animation* activeAnimation(App::Document* doc = nullptr);
/// Choose `anim` for its document's timeline
GuiExport void chooseAnimation(App::Animation* anim);
/** The path an Animation names `prop` by -- "Box.Placement.Base.z",
 * "Box.ViewObject.Transparency" -- with `sub` the sub-path below the
 * property, if any; empty when `prop` is no document object's or its
 * view provider's
 */
GuiExport std::string animationPath(const App::Property& prop,
                                    const App::ObjectIdentifier* sub = nullptr);
/// The colour the property editor marks a row with: keyed, animated, or
/// changed and not keyed (Blender's three); invalid for none
GuiExport QColor animationKeyMark(const App::Property& prop,
                                  const App::ObjectIdentifier* sub = nullptr,
                                  bool* driven = nullptr);

/// The wall clock that plays the chosen Animation, one per process
class GuiExport AnimationPlayer: public QObject
{
    Q_OBJECT

public:
    static AnimationPlayer* instance();

    void play(App::Animation* anim);
    void pause();
    /// Pause and put the rest values back
    void stop();
    bool isPlaying() const
    {
        return timer.isActive();
    }
    App::Animation* animation() const;

Q_SIGNALS:
    void stateChanged();

private:
    AnimationPlayer();
    void tick();

    QTimer timer;
    QElapsedTimer clock;
    App::DocumentObjectT anim;
    double startTime = 0;
    int direction = 1;
};

namespace DockWnd
{

class AnimationKeysView;

class GuiExport AnimationTimeline: public Gui::DockWindow
{
    Q_OBJECT

public:
    explicit AnimationTimeline(QWidget* parent = nullptr);
    ~AnimationTimeline() override;

    const char* getName() const override
    {
        return "AnimationTimeline";
    }

    static AnimationTimeline* instance();
    /// Show the timeline, raised
    static void showTimeline();

    App::Animation* animation() const;

private:
    void refresh();
    void refreshValues();
    void refreshAnimations();
    void onAnimationChosen(int index);
    void onSlotChangedObject(const App::DocumentObject& obj, const App::Property& prop);

    QComboBox* animations = nullptr;
    QToolButton* playButton = nullptr;
    QToolButton* recordButton = nullptr;
    QDoubleSpinBox* timeBox = nullptr;
    QDoubleSpinBox* startBox = nullptr;
    QDoubleSpinBox* endBox = nullptr;
    QDoubleSpinBox* rateBox = nullptr;
    QDoubleSpinBox* speedBox = nullptr;
    QComboBox* loopBox = nullptr;
    QComboBox* recomputeBox = nullptr;
    QLabel* emptyLabel = nullptr;
    AnimationKeysView* keys = nullptr;
    QTimer refreshTimer;
    bool updating = false;
    std::vector<fastsignals::scoped_connection> conns;
};

}  // namespace DockWnd
}  // namespace Gui

#endif  // GUI_ANIMATIONTIMELINE_H
