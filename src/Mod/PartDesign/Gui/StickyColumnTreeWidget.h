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

#ifndef PARTDESIGNGUI_STICKYCOLUMNTREEWIDGET_H
#define PARTDESIGNGUI_STICKYCOLUMNTREEWIDGET_H

#include <QHeaderView>
#include <QTreeWidget>

namespace PartDesignGui {

/** The header of a StickyColumnTreeWidget: its first column's title stays
 * at the left while the others scroll, and a click there, or a drag of its
 * right edge, is that column's.
 */
class StickyColumnHeader : public QHeaderView
{
public:
    explicit StickyColumnHeader(QWidget *parent = nullptr);

    /** The width of the first column while it stands over the others:
     * scrolled sideways, and narrower than the view. 0 otherwise.
     */
    int stickyWidth() const;
    /// The logical index of the first column shown
    int stickySection() const;

protected:
    void paintEvent(QPaintEvent *e) override;
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    bool viewportEvent(QEvent *e) override;

private:
    bool inSticky(const QPoint &pos) const;
    bool onStickyEdge(const QPoint &pos) const;
    void clickSticky();

    /// a press on the first title, a drag of its edge
    bool pressed = false;
    bool resizing = false;
    /// a press QHeaderView took, whose drag it keeps wherever it goes
    bool baseDrag = false;
};

/** A tree widget whose first column (a dress-up's element) stays at the left
 * while the other columns scroll sideways under it: the rows' first cells
 * painted over the scrolled ones, and a click, a hover or a tool tip there
 * taken as theirs.
 */
class StickyColumnTreeWidget : public QTreeWidget
{
public:
    explicit StickyColumnTreeWidget(QWidget *parent = nullptr);

    QModelIndex indexAt(const QPoint &point) const override;
    QRect visualRect(const QModelIndex &index) const override;
    void scrollTo(const QModelIndex &index, ScrollHint hint = EnsureVisible) override;

protected:
    void paintEvent(QPaintEvent *e) override;
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override;
    void scrollContentsBy(int dx, int dy) override;
    void updateEditorGeometries() override;

private:
    int stickyWidth() const;
    /// The open editors under the first column cut where it begins
    void maskEditors();

    enum class Pass { All, Others, Sticky };
    mutable Pass pass = Pass::All;
};

} // namespace PartDesignGui

#endif // PARTDESIGNGUI_STICKYCOLUMNTREEWIDGET_H
