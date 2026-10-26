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
#include <QTimer>
#include <FCGlobal.h>

class QAbstractScrollArea;
class QGraphicsOpacityEffect;
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

    /// How thick a bar is
    static constexpr int Thickness = 10;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void follow(QScrollBar* own, QScrollBar* ours);
    void place();
    /// Shown, and the fade-out put off
    void reveal();
    void fadeOut();
    bool pointerNear() const;

    QPointer<QAbstractScrollArea> area;
    QPointer<QScrollBar> vbar;
    QPointer<QScrollBar> hbar;
    Qt::ScrollBarPolicy ownVPolicy = Qt::ScrollBarAsNeeded;
    Qt::ScrollBarPolicy ownHPolicy = Qt::ScrollBarAsNeeded;
    QGraphicsOpacityEffect* veffect = nullptr;
    QGraphicsOpacityEffect* heffect = nullptr;
    QVariantAnimation* fade = nullptr;
    QTimer idle;
    QRect vrect;
    QRect hrect;
};

}  // namespace Gui

#endif  // GUI_OVERLAYSCROLLBARS_H
