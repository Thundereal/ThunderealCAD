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
# include <QHelpEvent>
# include <QMouseEvent>
# include <QPaintEvent>
# include <QPainter>
# include <QScrollBar>
# include <QStyle>
# include <QToolTip>
# include <algorithm>
# include <cstdlib>
#endif

#include "StickyColumnTreeWidget.h"

using namespace PartDesignGui;

namespace {
// marks an editor hidden because the first column covers it whole
const char *HiddenUnderSticky = "hiddenUnderStickyColumn";
}

///////////////////////////////////////////////////////////////////////////////

StickyColumnHeader::StickyColumnHeader(QWidget *parent)
    : QHeaderView(Qt::Horizontal, parent)
{
    // as QTreeView makes its own
    setSectionsMovable(true);
    setStretchLastSection(true);
    setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // the first title is not where a section's change repaints
    connect(this, &QHeaderView::sortIndicatorChanged, this, [this]() { viewport()->update(); });
    connect(this, &QHeaderView::sectionResized, this, [this]() { viewport()->update(); });
}

int StickyColumnHeader::stickySection() const
{
    for (int i = 0; i < count(); ++i) {
        int logical = logicalIndex(i);
        if (!isSectionHidden(logical))
            return logical;
    }
    return -1;
}

int StickyColumnHeader::stickyWidth() const
{
    if (orientation() != Qt::Horizontal || isRightToLeft() || offset() <= 0)
        return 0;
    const int section = stickySection();
    if (section < 0)
        return 0;
    const int width = sectionSize(section);
    // a first column as wide as the view leaves nothing to scroll beside it
    if (width <= 0 || width >= viewport()->width())
        return 0;
    return width;
}

bool StickyColumnHeader::inSticky(const QPoint &pos) const
{
    const int width = stickyWidth();
    const int margin = style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this);
    return width > 0 && pos.x() >= 0 && pos.x() < width - margin;
}

bool StickyColumnHeader::onStickyEdge(const QPoint &pos) const
{
    const int width = stickyWidth();
    const int margin = style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this);
    return width > 0 && std::abs(pos.x() - width) <= margin;
}

void StickyColumnHeader::paintEvent(QPaintEvent *e)
{
    QHeaderView::paintEvent(e);
    const int width = stickyWidth();
    if (width <= 0)
        return;
    // the first title over the scrolled ones, as QHeaderView paints each
    QPainter painter(viewport());
    painter.save();
    QHeaderView::paintSection(&painter, QRect(0, 0, width, viewport()->height()), stickySection());
    painter.restore();
}

void StickyColumnHeader::paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const
{
    const int width = stickyWidth();
    if (width <= 0) {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        return;
    }
    // the first one is painted at the left by paintEvent(), the others
    // only beside it
    if (logicalIndex == stickySection())
        return;
    painter->save();
    painter->setClipRect(QRect(width, 0, viewport()->width() - width, viewport()->height()),
                         Qt::IntersectClip);
    QHeaderView::paintSection(painter, rect, logicalIndex);
    painter->restore();
}

void StickyColumnHeader::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (e->button() == Qt::LeftButton && !baseDrag) {
        const int section = stickySection();
        if (onStickyEdge(pos) && sectionResizeMode(section) == QHeaderView::Interactive) {
            resizing = true;
            e->accept();
            return;
        }
        if (inSticky(pos)) {
            pressed = true;
            e->accept();
            return;
        }
    }
    if (e->button() == Qt::LeftButton)
        baseDrag = true;
    QHeaderView::mousePressEvent(e);
}

void StickyColumnHeader::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (resizing) {
        resizeSection(stickySection(), std::max(minimumSectionSize(), pos.x()));
        return;
    }
    if (pressed)
        return;
    if (!baseDrag && (inSticky(pos) || onStickyEdge(pos))) {
        // the scrolled section under the pointer is not the one hovered,
        // nor its edge the one to drag
        QMouseEvent away(e->type(), QPointF(-1, pos.y()), e->globalPosition(), e->button(),
                         e->buttons(), e->modifiers());
        QHeaderView::mouseMoveEvent(&away);
        if (onStickyEdge(pos))
            setCursor(Qt::SplitHCursor);
        else
            unsetCursor();
        return;
    }
    QHeaderView::mouseMoveEvent(e);
}

void StickyColumnHeader::mouseReleaseEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (resizing) {
        resizing = false;
        return;
    }
    if (pressed) {
        pressed = false;
        if (inSticky(pos))
            clickSticky();
        return;
    }
    baseDrag = false;
    QHeaderView::mouseReleaseEvent(e);
}

void StickyColumnHeader::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!baseDrag && e->button() == Qt::LeftButton) {
        if (onStickyEdge(pos))
            return;
        if (inSticky(pos)) {
            // a second click, as QHeaderView takes one
            pressed = true;
            return;
        }
    }
    if (e->button() == Qt::LeftButton)
        baseDrag = true;
    QHeaderView::mouseDoubleClickEvent(e);
}

void StickyColumnHeader::clickSticky()
{
    if (!sectionsClickable())
        return;
    const int section = stickySection();
    // the sort indicator turned as QHeaderView turns it
    if (isSortIndicatorShown()) {
        if (sortIndicatorSection() != section)
            setSortIndicator(section, Qt::AscendingOrder);
        else if (sortIndicatorOrder() == Qt::AscendingOrder)
            setSortIndicator(section, Qt::DescendingOrder);
        else if (isSortIndicatorClearable())
            setSortIndicator(-1, Qt::AscendingOrder);
        else
            setSortIndicator(section, Qt::AscendingOrder);
    }
    Q_EMIT sectionClicked(section);
}

bool StickyColumnHeader::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto he = static_cast<QHelpEvent*>(e);
        if (inSticky(he->pos())) {
            QVariant tip = model() ? model()->headerData(stickySection(), orientation(),
                                                         Qt::ToolTipRole)
                                   : QVariant();
            if (tip.isValid() && !tip.toString().isEmpty()) {
                QToolTip::showText(he->globalPos(), tip.toString(), this);
                return true;
            }
            // no title tip of its own: the header's
            QHelpEvent away(QEvent::ToolTip, QPoint(-1, he->pos().y()), he->globalPos());
            return QHeaderView::viewportEvent(&away);
        }
    }
    return QHeaderView::viewportEvent(e);
}

///////////////////////////////////////////////////////////////////////////////

StickyColumnTreeWidget::StickyColumnTreeWidget(QWidget *parent)
    : QTreeWidget(parent)
{
    setHeader(new StickyColumnHeader(this));
}

int StickyColumnTreeWidget::stickyWidth() const
{
    auto sticky = dynamic_cast<StickyColumnHeader*>(header());
    return sticky ? sticky->stickyWidth() : 0;
}

QModelIndex StickyColumnTreeWidget::indexAt(const QPoint &point) const
{
    const int width = stickyWidth();
    // the first column's cell where it is drawn, not the scrolled one under it
    if (width > 0 && point.x() >= 0 && point.x() < width)
        return QTreeWidget::indexAt(QPoint(point.x() - header()->offset(), point.y()));
    return QTreeWidget::indexAt(point);
}

QRect StickyColumnTreeWidget::visualRect(const QModelIndex &index) const
{
    QRect rect = QTreeWidget::visualRect(index);
    auto sticky = dynamic_cast<StickyColumnHeader*>(header());
    if (sticky && index.isValid() && sticky->stickyWidth() > 0
            && index.column() == sticky->stickySection())
        rect.translate(header()->offset(), 0);
    return rect;
}

void StickyColumnTreeWidget::scrollTo(const QModelIndex &index, ScrollHint hint)
{
    const int keep = horizontalScrollBar()->value();
    QTreeWidget::scrollTo(index, hint);
    // the first column is in sight however far the others are scrolled:
    // a row brought into view leaves them where they are
    auto sticky = dynamic_cast<StickyColumnHeader*>(header());
    if (sticky && index.isValid() && index.column() == sticky->stickySection()
            && sticky->sectionSize(index.column()) < viewport()->width())
        horizontalScrollBar()->setValue(keep);
}

void StickyColumnTreeWidget::paintEvent(QPaintEvent *e)
{
    const int width = stickyWidth();
    if (width <= 0) {
        QTreeWidget::paintEvent(e);
        return;
    }
    // the other columns, beside the first (drawRow() clips them)
    pass = Pass::Others;
    QTreeWidget::paintEvent(e);

    // then the first column's cells, drawn as if nothing were scrolled: the
    // rows the strip crosses, down to the last row (below it QTreeView fills
    // the whole width, done already)
    QRect strip = e->region().boundingRect() & QRect(0, 0, width, viewport()->height());
    QTreeWidgetItem *last = topLevelItemCount() ? topLevelItem(topLevelItemCount() - 1) : nullptr;
    while (last && last->isExpanded() && last->childCount())
        last = last->child(last->childCount() - 1);
    if (last)
        strip.setBottom(std::min(strip.bottom(), QTreeWidget::visualRect(indexFromItem(last)).bottom()));
    if (last && !strip.isEmpty()) {
        pass = Pass::Sticky;
        QPaintEvent sticky(QRect(-header()->offset(), strip.top(), width, strip.height()));
        QTreeWidget::paintEvent(&sticky);
    }
    pass = Pass::All;
}

void StickyColumnTreeWidget::drawRow(QPainter *painter, const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    const int width = pass == Pass::All ? 0 : stickyWidth();
    if (width <= 0) {
        QTreeWidget::drawRow(painter, option, index);
        return;
    }
    const QRect row = option.rect;
    painter->save();
    if (pass == Pass::Others) {
        painter->setClipRect(QRect(width, row.top(), viewport()->width() - width, row.height()),
                             Qt::IntersectClip);
    }
    else {
        painter->setClipRect(QRect(0, row.top(), width, row.height()), Qt::IntersectClip);
        painter->translate(header()->offset(), 0);
    }
    QTreeWidget::drawRow(painter, option, index);
    painter->restore();
}

void StickyColumnTreeWidget::scrollContentsBy(int dx, int dy)
{
    QTreeWidget::scrollContentsBy(dx, dy);
    // a sideways scroll moves what was drawn, the first column with it
    if (dx) {
        viewport()->update();
        header()->viewport()->update();
    }
    maskEditors();
}

void StickyColumnTreeWidget::updateEditorGeometries()
{
    QTreeWidget::updateEditorGeometries();
    maskEditors();
}

void StickyColumnTreeWidget::maskEditors()
{
    // (the first column has no editor of its own: a dress-up's element is
    // not edited in place)
    const int width = stickyWidth();
    for (auto child : viewport()->children()) {
        auto editor = qobject_cast<QWidget*>(child);
        if (!editor || editor->isWindow())
            continue;
        const QRect geometry = editor->geometry();
        if (width <= 0 || geometry.left() >= width) {
            if (!editor->mask().isEmpty())
                editor->clearMask();
            if (editor->property(HiddenUnderSticky).toBool()) {
                editor->setProperty(HiddenUnderSticky, QVariant());
                editor->show();
            }
            continue;
        }
        if (geometry.right() < width) {
            if (editor->isVisible()) {
                editor->setProperty(HiddenUnderSticky, true);
                editor->hide();
            }
            continue;
        }
        if (editor->property(HiddenUnderSticky).toBool()) {
            editor->setProperty(HiddenUnderSticky, QVariant());
            editor->show();
        }
        const int cut = width - geometry.left();
        editor->setMask(QRegion(cut, 0, geometry.width() - cut, geometry.height()));
    }
}
