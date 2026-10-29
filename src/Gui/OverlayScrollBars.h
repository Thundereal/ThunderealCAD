// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
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

#ifndef GUI_OVERLAYSCROLLBARS_H
#define GUI_OVERLAYSCROLLBARS_H

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QRegion>
#include <QTimer>
#include <QVector>
#include <FCGlobal.h>

class QAbstractScrollArea;
class QScrollBar;
class QVariantAnimation;

namespace Gui {

/** A scroll area's two scroll bars laid over its picture, out of the way of
 * the overlaid docks (docs/HandsOnQueue.md entry 222; the reporter: the
 * spreadsheet "does need scroll bar for easy scrolling. Suggest a way to
 * coexist with overlay panels").
 *
 * (A) Where an overlaid dock -- or a task panel shown over the view --
 * stands over the area's right or bottom edge, the vertical bar stands just
 * inside that dock's edge and the horizontal one just above it
 * (OverlayManager::covered()); they move as the docks show, hide and are
 * resized. Elsewhere they stand at the area's own edges. The picture keeps
 * the whole area, under the see-through docks too.
 *
 * (B) They are thin and see-through, shown while the area scrolls and while
 * the pointer is near them or on them, and fade out a moment after.
 *
 * Near is told by where the pointer is, whatever widget its move goes to:
 * OverlayManager's event filter tells pointerMoved() of every move before it
 * decides whether to pass it on to the view under a see-through panel -- the
 * panel's own lists never saw a move passed on -- and whatever lies over an
 * area's edge takes no move from its bars (docs/HandsOnQueue.md entry 233).
 *
 * (C) A cover that reaches no side of the area -- a task panel over part of
 * the view, a dock short of its edge -- the bars go round as the view cell's
 * menu button and handles do: one shortened past a cover over one of its
 * ends, or else slid inward to just past it (entry 233).
 *
 * The area's own bars are turned off and kept: these follow their ranges,
 * steps and values, and set their values back. Made by and owned by the
 * area, and go with it; remove() takes them off a living area.
 */
class GuiExport OverlayScrollBars: public QObject
{
    Q_OBJECT

public:
    explicit OverlayScrollBars(QAbstractScrollArea* area);
    ~OverlayScrollBars() override;

    /// The bar laid over the area, for \a orientation
    QScrollBar* bar(Qt::Orientation orientation) const;
    /// Where the bars stand now, in the area's coordinates (empty: not shown)
    QRect barRect(Qt::Orientation orientation) const;

    /// Takes the bars off: the area's own come back with the policies they
    /// had, and this is deleted
    void remove();
    /// The bars laid over \a area, if any
    static OverlayScrollBars* of(QAbstractScrollArea* area);

    /// The pointer moved to \a global (screen coordinates): the bars of
    /// every area it is in that it is near show. Asked by OverlayManager's
    /// event filter, before a move is passed through a see-through panel.
    static void pointerMoved(const QPoint& global);
    /// What the bars leave free, in the area's coordinates: a view cell's
    /// menu button and corner handles (ViewAreaCell::placeChrome), which
    /// keep their corners. A bar that would run under one stops short of it.
    void keepClear(const QRegion& region);

    /// How thick a bar is
    static constexpr int Thickness = 10;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void follow(QScrollBar* own, QScrollBar* ours);
    void place();
    /// \a home moved off \a covers (area coordinates) within \a bounds,
    /// or \a home where no place is clear
    QRect goRound(const QRect& home, const QVector<QRect>& covers, const QRect& bounds) const;
    /// The viewport watched: the area's present one (a page swaps its own)
    void bindViewport();
    /// Shown, and the fade-out put off
    void reveal();
    /// Shown if the pointer, at \a global, is near a bar
    void revealNear(const QPoint& global);
    void fadeOut();
    bool pointerNear() const;
    bool isNearBar(const QPoint& at) const;
    /// How far the bars are faded in, 0 to 1
    qreal opacity() const;

    QPointer<QAbstractScrollArea> area;
    QPointer<QWidget> viewport;
    QPointer<QScrollBar> vbar;
    QPointer<QScrollBar> hbar;
    Qt::ScrollBarPolicy ownVPolicy = Qt::ScrollBarAsNeeded;
    Qt::ScrollBarPolicy ownHPolicy = Qt::ScrollBarAsNeeded;
    QVariantAnimation* fade = nullptr;
    QTimer idle;
    QRect vrect;
    QRect hrect;
    QRegion clear;
};

}  // namespace Gui

#endif  // GUI_OVERLAYSCROLLBARS_H
