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

#ifndef GUI_CONTROLROW_H
#define GUI_CONTROLROW_H

#include <QFrame>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

namespace Gui
{

/** A dock's row of controls, scrolled sideways when the dock is narrower
 * than the row.
 *
 * Laid out flat in a QHBoxLayout, such a row is the dock's minimum width:
 * the Animation timeline's (some twenty widgets) asked for 1446 px and the
 * Transaction log's branch row for 1045, and docked side by side the main
 * window asked for 3000 px on a 1920 screen (docs/SmokeTesting.md, step 1).
 *
 * As high as the row, and the scroll bar's height more only while there is
 * something to scroll: the height changes when the dock is resized across
 * the row's width, never with what the row shows.
 *
 *   auto row = new ControlRow(parent);
 *   auto inner = new QWidget(row);
 *   auto bar = new QHBoxLayout(inner);   // the controls, as before
 *   row->setWidget(inner);
 */
class ControlRow: public QScrollArea
{
public:
    explicit ControlRow(QWidget* parent)
        : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        return {widget() ? widget()->sizeHint().width() : 0, rowHeight()};
    }

    QSize minimumSizeHint() const override
    {
        return {80, rowHeight()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QScrollArea::resizeEvent(event);
        const bool scrolls =
            widget() && widget()->minimumSizeHint().width() > event->size().width();
        if (scrolls != scrolling) {
            scrolling = scrolls;
            updateGeometry();
        }
    }

private:
    int rowHeight() const
    {
        int h = widget() ? widget()->sizeHint().height() : 0;
        if (scrolling) {
            h += horizontalScrollBar()->sizeHint().height();
        }
        return h;
    }

    bool scrolling = false;
};

}  // namespace Gui

#endif  // GUI_CONTROLROW_H
