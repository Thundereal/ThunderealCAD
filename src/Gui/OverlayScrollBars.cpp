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
# include <QApplication>
# include <QCursor>
# include <QEvent>
# include <QMouseEvent>
# include <QPainter>
# include <QScrollBar>
# include <QStyle>
# include <QStyleOptionSlider>
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

namespace {

/** A bar that paints itself at an opacity. A QGraphicsOpacityEffect on it
 * did nothing on the screen inside an overlaid dock, whose panel is drawn
 * through an effect of its own: a bar shown by the pointer stayed unseen
 * until the list was repainted by a scroll (docs/HandsOnQueue.md entry 233,
 * the reporter: "overlay panels only show scroll bar when scrolling with
 * mouse wheel"; on the real screen the bar said shown and 10 of its 3250
 * pixels changed).
 */
class FadeBar: public QScrollBar
{
public:
    using QScrollBar::QScrollBar;

    qreal opacity() const
    {
        return _opacity;
    }

    void setOpacity(qreal value)
    {
        if (value != _opacity) {
            _opacity = value;
            // for whoever looks from outside, a test for one
            setProperty("overlayOpacity", value);
            update();
        }
    }

protected:
    // QScrollBar::paintEvent(), at the opacity
    void paintEvent(QPaintEvent*) override
    {
        if (_opacity <= 0.0) {
            return;
        }
        QPainter painter(this);
        painter.setOpacity(_opacity);
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        opt.subControls = QStyle::SC_All;
        if (isSliderDown()) {
            opt.activeSubControls = QStyle::SC_ScrollBarSlider;
            opt.state |= QStyle::State_Sunken;
        }
        else if (underMouse()) {
            opt.activeSubControls = style()->hitTestComplexControl(
                QStyle::CC_ScrollBar, &opt, mapFromGlobal(QCursor::pos()), this);
        }
        else {
            opt.activeSubControls = QStyle::SC_None;
        }
        style()->drawComplexControl(QStyle::CC_ScrollBar, &opt, &painter, this);
    }

private:
    qreal _opacity = 0.0;
};

FadeBar* fadeBar(QScrollBar* bar)
{
    return static_cast<FadeBar*>(bar);
}

// Every area's bars, for pointerMoved()
std::vector<OverlayScrollBars*>& allBars()
{
    static std::vector<OverlayScrollBars*> all;
    return all;
}

}  // namespace

OverlayScrollBars::OverlayScrollBars(QAbstractScrollArea* scrollArea)
    : QObject(scrollArea)
    , area(scrollArea)
{
    auto make = [this](Qt::Orientation orientation) {
        // the area's child, not the viewport's: a scroll moves the viewport's
        auto bar = new FadeBar(orientation, area);
        bar->setObjectName(orientation == Qt::Vertical ? QStringLiteral("OverlayScrollBarV")
                                                       : QStringLiteral("OverlayScrollBarH"));
        bar->setStyleSheet(QString::fromLatin1(BarSheet));
        bar->setCursor(Qt::ArrowCursor);
        bar->setProperty("overlayOpacity", 0.0);
        bar->hide();
        bar->installEventFilter(this);
        return bar;
    };
    vbar = make(Qt::Vertical);
    hbar = make(Qt::Horizontal);

    // the area's own: off, and followed
    ownVPolicy = area->verticalScrollBarPolicy();
    ownHPolicy = area->horizontalScrollBarPolicy();
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    follow(area->verticalScrollBar(), vbar);
    follow(area->horizontalScrollBar(), hbar);

    fade = new QVariantAnimation(this);
    connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        fadeBar(vbar)->setOpacity(value.toReal());
        fadeBar(hbar)->setOpacity(value.toReal());
    });
    connect(fade, &QVariantAnimation::finished, this, [this]() {
        // faded out: nothing left to take a click over the picture
        if (opacity() <= 0.0) {
            vbar->hide();
            hbar->hide();
        }
    });
    idle.setSingleShot(true);
    connect(&idle, &QTimer::timeout, this, &OverlayScrollBars::fadeOut);

    area->installEventFilter(this);
    bindViewport();
    allBars().push_back(this);
    if (auto manager = OverlayManager::instance()) {
        connect(manager, &OverlayManager::coverChanged, this, &OverlayScrollBars::place);
        connect(manager, &OverlayManager::layoutChanged, this, &OverlayScrollBars::place);
    }
    place();
}

OverlayScrollBars::~OverlayScrollBars()
{
    auto& all = allBars();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void OverlayScrollBars::pointerMoved(const QPoint& global)
{
    // A move goes to each widget up the line that ignores it: once for each
    // place is enough
    static QPoint last(INT_MIN, INT_MIN);
    if (global == last) {
        return;
    }
    last = global;
    for (auto bars : allBars()) {
        bars->revealNear(global);
    }
}

void OverlayScrollBars::bindViewport()
{
    // A view may swap its viewport (a drawing page does, for its renderer):
    // the new one is watched for its size, and tracks the pointer, without
    // which no move over it is made at all
    QWidget* now = area ? area->viewport() : nullptr;
    if (now == viewport) {
        return;
    }
    if (viewport) {
        viewport->removeEventFilter(this);
    }
    viewport = now;
    if (viewport) {
        viewport->installEventFilter(this);
        viewport->setMouseTracking(true);
    }
}

void OverlayScrollBars::remove()
{
    fade->stop();
    idle.stop();
    if (area) {
        area->removeEventFilter(this);
        if (viewport) {
            viewport->removeEventFilter(this);
        }
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
    bindViewport();
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
    // A dock in a corner is a dock along one edge: the one it reaches in from
    // the least, as the task panel host reads them (roomIn()). A dock at the
    // side ends where the one at the bottom begins, so it reaches the top
    // and not the bottom, and was taken for a dock at the top as well; the
    // two then left "too little" between them and both bars went back to the
    // area's edges, under the docks, where the pointer coming near them was
    // the docks' (docs/HandsOnQueue.md entry 233). The covers one by one for
    // that: a region's bands cut such a dock in two.
    QVector<QRect> covers;
    if (auto manager = OverlayManager::instance()) {
        // an empty Tasks panel is no cover (the reporter, entry 233)
        covers = manager->coverRects(viewport, true);
        const int none = width + height;
        for (const QRect& r : covers) {
            const bool atLeft = r.left() <= 0;
            const bool atRight = r.right() >= width - 1;
            const bool atTop = r.top() <= 0;
            const bool atBottom = r.bottom() >= height - 1;
            // how far in from each edge it touches; a strip from edge to
            // edge lies along the edge between them
            int depth[4] = {atLeft && !atRight ? r.right() + 1 : none,
                            atRight && !atLeft ? width - r.left() : none,
                            atTop && !atBottom ? r.bottom() + 1 : none,
                            atBottom && !atTop ? height - r.top() : none};
            const int edge = int(std::min_element(depth, depth + 4) - depth);
            if (depth[edge] >= none) {
                continue;
            }
            switch (edge) {
                case 0:
                    left = std::max(left, r.right() + 1);
                    break;
                case 1:
                    right = std::min(right, r.left());
                    break;
                case 2:
                    top = std::max(top, r.bottom() + 1);
                    break;
                default:
                    bottom = std::min(bottom, r.top());
                    break;
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
    // (C) What still stands over a bar reaches no side of the area: round
    // it, as the view cell's menu button and handles go round what covers
    // them (ViewAreaCell::placeChrome, entry 83). An area in an overlaid
    // panel has the panel itself over the whole of it: nowhere to go, and
    // its bars stay at its edges.
    if (!covers.isEmpty()) {
        QVector<QRect> local;
        for (const QRect& r : covers) {
            local.push_back(r.translated(view.topLeft()));
        }
        vrect = goRound(vrect, local, view);
        hrect = goRound(hrect, local, view);
        // the vertical one moved inward: the horizontal one stops short of it
        if (!vrect.isEmpty() && hrect.intersects(vrect)) {
            hrect.setRight(vrect.left() - 1);
        }
    }
    const bool shown = opacity() > 0.0 || fade->state() == QAbstractAnimation::Running;
    vbar->setGeometry(vrect);
    hbar->setGeometry(hrect);
    vbar->setVisible(shown && !vrect.isEmpty());
    hbar->setVisible(shown && !hrect.isEmpty());
    vbar->raise();
    hbar->raise();
}

QRect OverlayScrollBars::goRound(const QRect& home, const QVector<QRect>& covers,
                                 const QRect& bounds) const
{
    if (home.isEmpty()) {
        return home;
    }
    const bool vertical = home.height() > home.width();
    // shortened to no less than this; else slid
    const int least = std::max(4 * Thickness, (vertical ? home.height() : home.width()) / 2);
    QRect at = home;
    for (int turn = 0; turn < 8; ++turn) {
        QRect hit;
        for (const QRect& r : covers) {
            if (r.intersects(at)) {
                hit = r;
                break;
            }
        }
        if (hit.isNull()) {
            return at;
        }
        // Shortened from the end that leaves more of it, past the cover --
        // which may lie anywhere along it, a small panel near one end too --
        // if that leaves enough; else slid inward to just past the cover
        if (vertical) {
            const int below = at.bottom() - hit.bottom();
            const int above = hit.top() - at.top();
            if (below >= above && below >= least) {
                at.setTop(hit.bottom() + 1);
            }
            else if (above > below && above >= least) {
                at.setBottom(hit.top() - 1);
            }
            else {
                at.moveRight(hit.left() - 1);
            }
            if (at.left() < bounds.left()) {
                break;
            }
        }
        else {
            const int after = at.right() - hit.right();
            const int before = hit.left() - at.left();
            if (after >= before && after >= least) {
                at.setLeft(hit.right() + 1);
            }
            else if (before > after && before >= least) {
                at.setRight(hit.left() - 1);
            }
            else {
                at.moveBottom(hit.top() - 1);
            }
            if (at.top() < bounds.top()) {
                break;
            }
        }
    }
    // no clear place: where it was
    return home;
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
    const qreal now = opacity();
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
    fade->setStartValue(opacity());
    fade->setEndValue(0.0);
    fade->start();
}

qreal OverlayScrollBars::opacity() const
{
    return vbar ? fadeBar(vbar)->opacity() : 0.0;
}

bool OverlayScrollBars::isNearBar(const QPoint& at) const
{
    for (const QRect& r : {vrect, hrect}) {
        if (!r.isEmpty() && r.adjusted(-Near, -Near, Near, Near).contains(at)) {
            return true;
        }
    }
    return false;
}

bool OverlayScrollBars::pointerNear() const
{
    return area && isNearBar(area->mapFromGlobal(QCursor::pos()));
}

void OverlayScrollBars::revealNear(const QPoint& global)
{
    // over the area itself: not the bars of a panel or a cell beside it
    if (!area || !area->isVisible()) {
        return;
    }
    const QPoint at = area->mapFromGlobal(global);
    if (area->rect().contains(at) && isNearBar(at)) {
        reveal();
    }
}

bool OverlayScrollBars::eventFilter(QObject* watched, QEvent* event)
{
    if (!area) {
        return false;
    }
    switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
            if (watched == area || watched == viewport) {
                place();
            }
            break;
        case QEvent::ChildAdded:
            // a viewport swapped in (QAbstractScrollArea::setViewport)
            if (watched == area) {
                bindViewport();
            }
            break;
        // The pointer coming near a bar shows it: pointerMoved()
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
