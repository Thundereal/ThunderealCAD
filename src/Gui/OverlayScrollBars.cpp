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

#include "PreCompiled.h"

#ifndef _PreComp_
# include <QAbstractScrollArea>
# include <QCursor>
# include <QEvent>
# include <QGraphicsOpacityEffect>
# include <QMouseEvent>
# include <QScrollBar>
# include <QVariantAnimation>
# include <algorithm>
#endif

#include "OverlayScrollBars.h"
#include "OverlayManager.h"

using namespace Gui;

namespace {

// Thin and see-through: a handle on nothing, no arrows
const char* BarSheet =
    "QScrollBar { background: transparent; border: none; margin: 0px; }"
    "QScrollBar:vertical { width: 10px; }"
    "QScrollBar:horizontal { height: 10px; }"
    "QScrollBar::handle { background: rgba(128, 128, 128, 150); border: none;"
    " border-radius: 3px; margin: 2px; }"
    "QScrollBar::handle:hover, QScrollBar::handle:pressed {"
    " background: rgba(128, 128, 128, 220); }"
    "QScrollBar::handle:vertical { min-height: 24px; }"
    "QScrollBar::handle:horizontal { min-width: 24px; }"
    "QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none;"
    " background: none; }"
    "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }";

// How long the bars stay after the last reason to show them
constexpr int StayMs = 1200;
// How near a bar the pointer shows it
constexpr int Near = 3 * OverlayScrollBars::Thickness;

}  // namespace

OverlayScrollBars::OverlayScrollBars(QAbstractScrollArea* scrollArea)
    : QObject(scrollArea)
    , area(scrollArea)
{
    auto make = [this](Qt::Orientation orientation, QGraphicsOpacityEffect*& effect) {
        // the area's child, not the viewport's: a scroll moves the viewport's
        auto bar = new QScrollBar(orientation, area);
        bar->setObjectName(orientation == Qt::Vertical ? QStringLiteral("OverlayScrollBarV")
                                                       : QStringLiteral("OverlayScrollBarH"));
        bar->setStyleSheet(QString::fromLatin1(BarSheet));
        bar->setCursor(Qt::ArrowCursor);
        effect = new QGraphicsOpacityEffect(bar);
        effect->setOpacity(0.0);
        bar->setGraphicsEffect(effect);
        bar->hide();
        bar->installEventFilter(this);
        return bar;
    };
    vbar = make(Qt::Vertical, veffect);
    hbar = make(Qt::Horizontal, heffect);

    // the area's own: off, and followed
    ownVPolicy = area->verticalScrollBarPolicy();
    ownHPolicy = area->horizontalScrollBarPolicy();
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    follow(area->verticalScrollBar(), vbar);
    follow(area->horizontalScrollBar(), hbar);

    fade = new QVariantAnimation(this);
    connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        veffect->setOpacity(value.toReal());
        heffect->setOpacity(value.toReal());
    });
    connect(fade, &QVariantAnimation::finished, this, [this]() {
        // faded out: nothing left to take a click over the picture
        if (veffect->opacity() <= 0.0) {
            vbar->hide();
            hbar->hide();
        }
    });
    idle.setSingleShot(true);
    connect(&idle, &QTimer::timeout, this, &OverlayScrollBars::fadeOut);

    area->installEventFilter(this);
    area->viewport()->installEventFilter(this);
    area->viewport()->setMouseTracking(true);
    if (auto manager = OverlayManager::instance()) {
        connect(manager, &OverlayManager::coverChanged, this, &OverlayScrollBars::place);
        connect(manager, &OverlayManager::layoutChanged, this, &OverlayScrollBars::place);
    }
    place();
}

OverlayScrollBars::~OverlayScrollBars() = default;

void OverlayScrollBars::remove()
{
    fade->stop();
    idle.stop();
    if (area) {
        area->removeEventFilter(this);
        area->viewport()->removeEventFilter(this);
        area->setVerticalScrollBarPolicy(ownVPolicy);
        area->setHorizontalScrollBarPolicy(ownHPolicy);
    }
    delete vbar;
    delete hbar;
    delete this;
}

OverlayScrollBars* OverlayScrollBars::of(QAbstractScrollArea* area)
{
    return area ? area->findChild<OverlayScrollBars*>(QString(), Qt::FindDirectChildrenOnly)
                : nullptr;
}

void OverlayScrollBars::keepClear(const QRegion& region)
{
    if (region == clear) {
        return;
    }
    clear = region;
    place();
}

QScrollBar* OverlayScrollBars::bar(Qt::Orientation orientation) const
{
    return orientation == Qt::Vertical ? vbar : hbar;
}

QRect OverlayScrollBars::barRect(Qt::Orientation orientation) const
{
    return orientation == Qt::Vertical ? vrect : hrect;
}

void OverlayScrollBars::follow(QScrollBar* own, QScrollBar* ours)
{
    auto copy = [own, ours]() {
        ours->setRange(own->minimum(), own->maximum());
        ours->setPageStep(own->pageStep());
        ours->setSingleStep(own->singleStep());
        ours->setValue(own->value());
    };
    copy();
    connect(own, &QScrollBar::rangeChanged, this, [this, copy]() {
        copy();
        place();
    });
    connect(own, &QScrollBar::valueChanged, this, [this, ours](int value) {
        ours->setValue(value);
        // shown while the area scrolls
        reveal();
    });
    connect(ours, &QScrollBar::valueChanged, own, &QScrollBar::setValue);
}

void OverlayScrollBars::place()
{
    if (!area) {
        return;
    }
    QWidget* viewport = area->viewport();
    const QRect view = viewport->geometry();
    const int width = view.width();
    const int height = view.height();

    // What an overlaid dock (or a task panel over the view) leaves free: the
    // bars just inside a dock on the right and above one at the bottom, the
    // horizontal one from past a dock on the left, the vertical one from
    // below one at the top. A cover that spans the area from side to side
    // is a dock at the top or the bottom, not one on a side.
    int left = 0;
    int top = 0;
    int right = width;
    int bottom = height;
    if (auto manager = OverlayManager::instance()) {
        const QRegion covered = manager->covered(viewport);
        for (const QRect& r : covered) {
            const bool atLeft = r.left() <= 0;
            const bool atRight = r.right() >= width - 1;
            const bool atTop = r.top() <= 0;
            const bool atBottom = r.bottom() >= height - 1;
            // A cover at a corner is one dock, on one side: a side dock
            // reaches above a viewport that begins below the sheet's header,
            // and taken for one at the top as well it left the bars no room
            // between the two and sent them under the docks. It is on the
            // side it runs more of the area along.
            const bool corner = (atLeft != atRight) && (atTop != atBottom);
            const bool alongSide = double(r.height()) / height > double(r.width()) / width;
            const bool side = (atLeft != atRight) && (!corner || alongSide);
            const bool across = (atTop != atBottom) && !side;
            if (side && atRight) {
                right = std::min(right, r.left());
            }
            if (side && atLeft) {
                left = std::max(left, r.right() + 1);
            }
            if (across && atBottom) {
                bottom = std::min(bottom, r.top());
            }
            if (across && atTop) {
                top = std::max(top, r.bottom() + 1);
            }
        }
    }
    // too little left between the docks: the area's own edges
    if (right - left < 4 * Thickness || bottom - top < 4 * Thickness) {
        left = 0;
        top = 0;
        right = width;
        bottom = height;
    }

    for (auto pair : {std::make_pair(area->verticalScrollBar(), vbar),
                      std::make_pair(area->horizontalScrollBar(), hbar)}) {
        pair.second->setPageStep(pair.first->pageStep());
        pair.second->setSingleStep(pair.first->singleStep());
    }
    // none where the area had its own turned off for good
    const bool showV = ownVPolicy != Qt::ScrollBarAlwaysOff
        && area->verticalScrollBar()->maximum() > area->verticalScrollBar()->minimum();
    const bool showH = ownHPolicy != Qt::ScrollBarAlwaysOff
        && area->horizontalScrollBar()->maximum() > area->horizontalScrollBar()->minimum();
    vrect = showV ? QRect(view.left() + right - Thickness, view.top() + top, Thickness,
                          bottom - top - (showH ? Thickness : 0))
                  : QRect();
    hrect = showH ? QRect(view.left() + left, view.top() + bottom - Thickness,
                          right - left - (showV ? Thickness : 0), Thickness)
                  : QRect();
    // A view cell's handles keep their corners: a bar running under one
    // stops short of it, at the end it is nearer; one left too short to
    // take hold of is not shown.
    for (const QRect& r : clear) {
        if (vrect.intersects(r)) {
            if (r.center().y() <= vrect.center().y()) {
                vrect.setTop(r.bottom() + 1);
            }
            else {
                vrect.setBottom(r.top() - 1);
            }
        }
        if (hrect.intersects(r)) {
            if (r.center().x() <= hrect.center().x()) {
                hrect.setLeft(r.right() + 1);
            }
            else {
                hrect.setRight(r.left() - 1);
            }
        }
    }
    if (vrect.height() < 3 * Thickness) {
        vrect = QRect();
    }
    if (hrect.width() < 3 * Thickness) {
        hrect = QRect();
    }
    const bool shown = veffect->opacity() > 0.0 || fade->state() == QAbstractAnimation::Running;
    vbar->setGeometry(vrect);
    hbar->setGeometry(hrect);
    vbar->setVisible(shown && !vrect.isEmpty());
    hbar->setVisible(shown && !hrect.isEmpty());
    vbar->raise();
    hbar->raise();
}

void OverlayScrollBars::reveal()
{
    if (!area) {
        return;
    }
    vbar->setVisible(!vrect.isEmpty());
    hbar->setVisible(!hrect.isEmpty());
    vbar->raise();
    hbar->raise();
    const qreal now = veffect->opacity();
    if (now < 1.0 && !(fade->state() == QAbstractAnimation::Running
                       && fade->endValue().toReal() >= 1.0)) {
        fade->stop();
        fade->setDuration(120);
        fade->setStartValue(now);
        fade->setEndValue(1.0);
        fade->start();
    }
    idle.start(StayMs);
}

void OverlayScrollBars::fadeOut()
{
    // not while the pointer is near or on one, nor while one is dragged
    if (pointerNear() || vbar->isSliderDown() || hbar->isSliderDown()) {
        idle.start(StayMs);
        return;
    }
    fade->stop();
    fade->setDuration(300);
    fade->setStartValue(veffect->opacity());
    fade->setEndValue(0.0);
    fade->start();
}

bool OverlayScrollBars::pointerNear() const
{
    if (!area) {
        return false;
    }
    const QPoint at = area->mapFromGlobal(QCursor::pos());
    for (const QRect& r : {vrect, hrect}) {
        if (!r.isEmpty() && r.adjusted(-Near, -Near, Near, Near).contains(at)) {
            return true;
        }
    }
    return false;
}

bool OverlayScrollBars::eventFilter(QObject* watched, QEvent* event)
{
    if (!area) {
        return false;
    }
    switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
            if (watched == area || watched == area->viewport()) {
                place();
            }
            break;
        case QEvent::MouseMove:
            if (watched == area->viewport()) {
                // the pointer coming near a bar shows it
                const QPoint at = area->viewport()->mapTo(
                    area, static_cast<QMouseEvent*>(event)->position().toPoint());
                for (const QRect& r : {vrect, hrect}) {
                    if (!r.isEmpty() && r.adjusted(-Near, -Near, Near, Near).contains(at)) {
                        reveal();
                        break;
                    }
                }
            }
            break;
        case QEvent::Enter:
        case QEvent::MouseButtonPress:
            if (watched == vbar || watched == hbar) {
                reveal();
            }
            break;
        default:
            break;
    }
    return false;
}

#include "moc_OverlayScrollBars.cpp"
