/***************************************************************************
 *   Copyright (c) 2012 Jan Rheinländer                                    *
 *                                   <jrheinlaender@users.sourceforge.net> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include "PreCompiled.h"

#ifndef _PreComp_
# include <QApplication>
# include <QAction>
# include <QKeyEvent>
# include <QMenu>
# include <QMouseEvent>
# include <QPushButton>
# include <QTimer>
# include <QCheckBox>
# include <QColor>
# include <QColorDialog>
# include <QHeaderView>
# include <QIcon>
# include <QPixmap>
# include <QPointer>
# include <QTreeWidget>
# include <cmath>
#endif

#include <algorithm>

#include <boost/algorithm/string/predicate.hpp>

#include <TopExp_Explorer.hxx>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/ElementNamingUtils.h>
#include <App/MappedElement.h>
#include <App/PropertyStandard.h>
#include <Base/AutoColor.h>
#include <Base/ServiceProvider.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Document.h>
#include <Gui/Command.h>
#include <Gui/Inventor/Draggers/GizmoStyleParameters.h>
#include <Gui/MainWindow.h>
#include <Gui/ViewParams.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/Selection.h>
#include <Gui/Tools.h>
#include <Gui/WaitCursor.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/Gui/ReferenceSelection.h>

#include "TaskDressUpParameters.h"
#include "Utils.h"


FC_LOG_LEVEL_INIT("PartDesign",true,true)

using namespace PartDesignGui;
using namespace Gui;

namespace {
// A row's place in the feature's order, in its element column
constexpr int RowOrderRole = Qt::UserRole + 2;

// "Edge2" before "Edge10": a run of digits compared as a number
int naturalCompare(const QString &a, const QString &b)
{
    int i = 0;
    int j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            int si = i;
            int sj = j;
            while (si < a.size() && a[si] == QLatin1Char('0'))
                ++si;
            while (sj < b.size() && b[sj] == QLatin1Char('0'))
                ++sj;
            int ei = si;
            int ej = sj;
            while (ei < a.size() && a[ei].isDigit())
                ++ei;
            while (ej < b.size() && b[ej].isDigit())
                ++ej;
            if (ei - si != ej - sj)
                return ei - si < ej - sj ? -1 : 1;
            int c = QStringView(a).mid(si, ei - si).compare(QStringView(b).mid(sj, ej - sj));
            if (c != 0)
                return c < 0 ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        int c = QString::compare(a.mid(i, 1), b.mid(j, 1), Qt::CaseInsensitive);
        if (c != 0)
            return c < 0 ? -1 : 1;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j ? -1 : 1;
    return 0;
}

// Numbers as numbers, the rest as text
int compareSortValues(const QVariant &a, const QVariant &b)
{
    auto isNumber = [](const QVariant &v) {
        switch (v.typeId()) {
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Bool:
            return true;
        default:
            return false;
        }
    };
    if (isNumber(a) && isNumber(b)) {
        double x = a.toDouble();
        double y = b.toDouble();
        return x < y ? -1 : (y < x ? 1 : 0);
    }
    return naturalCompare(a.toString(), b.toString());
}
}


/* TRANSLATOR PartDesignGui::TaskDressUpParameters */

TaskDressUpParameters::TaskDressUpParameters(ViewProviderDressUp *DressUpView, bool selectEdges, bool selectFaces, QWidget *parent)
    : TaskFeatureParameters(DressUpView,
                            parent,
                            DressUpView->featureIcon(),
                            DressUpView->getMenuName())
    , proxy(nullptr)
    , allowFaces(selectFaces)
    , allowEdges(selectEdges)
{
    selectionMode = none;
    // Steps of its own with PerViewEdit on (docs/HandsOnQueue.md entry 232)
    stepsPerView = true;

    onTopEnabled = Gui::ViewParams::getShowSelectionOnTop();
    if(!onTopEnabled)
        Gui::ViewParams::setShowSelectionOnTop(true);

    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, SIGNAL(timeout()), this, SLOT(onTimer()));
}

TaskDressUpParameters::~TaskDressUpParameters()
{
    // make sure to remove selection gate in all cases
    Gui::Selection().rmvSelectionGate();

    if(!onTopEnabled)
        Gui::ViewParams::setShowSelectionOnTop(false);

    clearButtons(none);
    exitSelectionMode();
}

QTreeWidgetItem *TaskDressUpParameters::getCurrentItem() const
{
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        return nullptr;
    QTreeWidgetItem *current = nullptr;
    auto sels = treeWidget->selectedItems();
    if (sels.size())
        current = sels[sels.size()-1];
    else {
        current = treeWidget->currentItem();
        if (!current)
            current = treeWidget->itemAt(QPoint(0,0));
    }
    return current;
}

void TaskDressUpParameters::setup(QLabel *label, QTreeWidget *widget, QCheckBox *_btnAdd, bool touched)
{
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        return;
    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    if(!pcDressUp || !pcDressUp->Base.getValue())
        return;

    PartDesignGui::addTaskCheckBox(DressUpView, proxy);
    if (auto boxLayout = qobject_cast<QBoxLayout*>(proxy->layout()))
        addUpdateViewCheckBox(boxLayout);

    messageLabel = label;
    messageLabel->hide();
    messageLabel->setWordWrap(true);

    btnAdd = _btnAdd;
    connect(btnAdd, SIGNAL(toggled(bool)), this, SLOT(onButtonRefAdd(bool)));

    treeWidget = widget;
    treeWidget->setRootIsDecorated(false);
    treeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    treeWidget->setMouseTracking(true);
    treeWidget->installEventFilter(this);
    // its double clicks (eventFilter())
    treeWidget->viewport()->installEventFilter(this);

    connect(treeWidget, SIGNAL(itemSelectionChanged()),
        this, SLOT(onItemSelectionChanged()));
    connect(treeWidget, SIGNAL(itemEntered(QTreeWidgetItem*,int)),
        this, SLOT(onItemEntered(QTreeWidgetItem*,int)));

    // A click on a column's title sorts the rows by it, a second the other
    // way, a third puts them back in the feature's order (sortRows())
    auto header = treeWidget->header();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicatorClearable(true);
    header->setSortIndicator(-1, Qt::AscendingOrder);
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this]() { sortRows(); });

    if(!deleteAction) {
        // Create context menu
        deleteAction = new QAction(tr("Remove"), this);
        deleteAction->setShortcut(Gui::QtTools::deleteKeySequence());
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // display shortcut behind the context menu entry
        deleteAction->setShortcutVisibleInContextMenu(true);
#endif
        widget->addAction(deleteAction);
        connect(deleteAction, SIGNAL(triggered()), this, SLOT(onRefDeleted()));
        widget->setContextMenuPolicy(Qt::ActionsContextMenu);
    }

    if(populate() || touched) {
        recomputeFeature(false);
    }
}

void TaskDressUpParameters::refresh()
{
    populate(true);
}

bool TaskDressUpParameters::populate(bool refresh)
{
    auto DressUpView = getDressUpView();
    if(!treeWidget || !DressUpView)
        return false;

    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    if(!pcDressUp || !pcDressUp->Base.getValue())
        return false;

    Base::StateLocker guard(busy);
    if (!refresh)
        setupTransaction();
    bool touched = PartDesignGui::populateGeometryReferences(treeWidget, pcDressUp->Base, refresh);
    populateOtherItems();
    for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i)
        treeWidget->topLevelItem(i)->setData(0, RowOrderRole, i);
    sortRows();
    return touched;
}

std::vector<QTreeWidgetItem*> TaskDressUpParameters::rowsInOrder() const
{
    std::vector<QTreeWidgetItem*> rows;
    if (!treeWidget)
        return rows;
    for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i)
        rows.push_back(treeWidget->topLevelItem(i));
    // a row added since the list was filled has no place yet: after the
    // others, as it is listed
    std::stable_sort(rows.begin(), rows.end(), [](QTreeWidgetItem *a, QTreeWidgetItem *b) {
        QVariant x = a->data(0, RowOrderRole);
        QVariant y = b->data(0, RowOrderRole);
        if (!y.isValid())
            return x.isValid();
        return x.isValid() && x.toInt() < y.toInt();
    });
    return rows;
}

QVariant TaskDressUpParameters::sortValue(QTreeWidgetItem *item, int column) const
{
    QString text = item->text(column);
    if (text.isEmpty())
        return {};
    if (column > 0) {
        bool ok = false;
        double value = item->data(column, Qt::UserRole).toDouble(&ok);
        if (ok)
            return value;
    }
    return text;
}

void TaskDressUpParameters::sortRows()
{
    if (!treeWidget)
        return;
    auto header = treeWidget->header();
    const int column = header->sortIndicatorSection();
    const Qt::SortOrder order = header->sortIndicatorOrder();
    std::vector<QTreeWidgetItem*> rows = rowsInOrder();
    if (column >= 0 && column != sortKeyColumn && column < treeWidget->columnCount()) {
        std::vector<std::pair<QVariant, QTreeWidgetItem*>> keyed;
        for (auto row : rows)
            keyed.emplace_back(sortValue(row, column), row);
        std::stable_sort(keyed.begin(), keyed.end(), [order](const auto &a, const auto &b) {
            // an empty cell last, either way
            if (!a.first.isValid() || !b.first.isValid())
                return a.first.isValid() && !b.first.isValid();
            int c = compareSortValues(a.first, b.first);
            return order == Qt::AscendingOrder ? c < 0 : c > 0;
        });
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i] = keyed[i].second;
    }
    else {
        // the feature's order, which the list may well be in already: no
        // column added before a sort is asked for
        bool same = true;
        for (int i = 0, count = static_cast<int>(rows.size()); i < count && same; ++i)
            same = treeWidget->topLevelItem(i) == rows[i];
        if (same)
            return;
    }

    // The rows put in that order by a hidden column of their places. A sort
    // of the model's, it keeps the selection, the open editors and the rows
    // under each (a fillet's points, whose order is the feature's).
    QSignalBlocker blocker(treeWidget);
    if (sortKeyColumn < 0) {
        sortKeyColumn = treeWidget->columnCount();
        treeWidget->setColumnCount(sortKeyColumn + 1);
        header->hideSection(sortKeyColumn);
    }
    for (int i = 0, count = static_cast<int>(rows.size()); i < count; ++i)
        rows[i]->setData(sortKeyColumn, Qt::DisplayRole, i);
    treeWidget->invisibleRootItem()->sortChildren(sortKeyColumn, Qt::AscendingOrder);
}

bool TaskDressUpParameters::showOnTop(bool enable,
        std::vector<App::SubObjectT> &&objs)
{
    if(onTopObjs.size()) {
        auto doc = Gui::Application::Instance->getDocument(
                onTopObjs.front().getDocumentName().c_str());
        if(doc) {
            auto view = Base::freecad_dynamic_cast<Gui::View3DInventor>(doc->getActiveView());
            if(view) {
                auto viewer = view->getViewer();
                for(auto &obj : onTopObjs) {
                    viewer->checkGroupOnTop(Gui::SelectionChanges(SelectionChanges::RmvSelection,
                                obj.getDocumentName().c_str(),
                                obj.getObjectName().c_str(),
                                obj.getSubName().c_str()), true);
                }
            }
        }
    }
    onTopObjs.clear();
    if(!enable)
        return true;

    if(objs.empty())
        objs.push_back(getInEdit());

    auto doc = Gui::Application::Instance->getDocument(
            objs.front().getDocumentName().c_str());
    if(!doc)
        return false;
    auto view = Base::freecad_dynamic_cast<Gui::View3DInventor>(doc->getActiveView());
    if(!view)
        return false;
    auto viewer = view->getViewer();
    for(auto &obj : objs) {
        viewer->checkGroupOnTop(Gui::SelectionChanges(SelectionChanges::AddSelection,
                    obj.getDocumentName().c_str(),
                    obj.getObjectName().c_str(),
                    obj.getSubName().c_str()), true);
    }
    onTopObjs = std::move(objs);
    return true;
}

QTreeWidgetItem *TaskDressUpParameters::getFirstFaceItem() const
{
    for (auto item : rowsInOrder()) {
        if (getGeometryItemText(item).startsWith("Face"))
            return item;
    }
    return nullptr;
}

void TaskDressUpParameters::addRemoveFirstFaceAction(QMenu &menu)
{
    QTreeWidgetItem *item = getFirstFaceItem();
    if (!item)
        return;
    menu.addSeparator();
    QAction *remove = menu.addAction(
        tr("Remove %1").arg(QString::fromLatin1(getGeometryItemText(item))));
    connect(remove, &QAction::triggered, this, [this]() {
        // after the menu: the removal recomputes and moves the handle
        QTimer::singleShot(0, this, [this]() {
            if (QTreeWidgetItem *item = getFirstFaceItem()) {
                treeWidget->clearSelection();
                item->setSelected(true);
                onRefDeleted();
            }
        });
    });
}

void TaskDressUpParameters::addAllEdges()
{
    auto DressUpView = getDressUpView();
    if (!DressUpView)
        return;

    PartDesign::DressUp* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    App::DocumentObject* base = pcDressUp->Base.getValue();
    if (!base)
        return;
    int count = Part::Feature::getTopoShape(base).countSubShapes(TopAbs_EDGE);
    auto subValues = pcDressUp->Base.getSubValues(false);
    std::size_t len = subValues.size();
    std::string name("Edge");
    for (int i=0; i<count; ++i) {
        name.resize(4);
        name += std::to_string(i+1);
        if (std::find(subValues.begin(), subValues.begin()+len, name) == subValues.begin()+len)
            subValues.push_back(name);
    }
    if (subValues.size() == len)
        return;
    try {
        setupTransaction(tr("add all edges"));
        pcDressUp->Base.setValue(base, subValues);
        recompute();
        populate(true);
    }
    catch (Base::Exception &e) {
        e.ReportException();
    }
}

void TaskDressUpParameters::clearButtons(const selectionModes notThis)
{
    if (notThis != refToggle && btnAdd) {
        QSignalBlocker blocker(btnAdd);
        btnAdd->setChecked(false);
        showOnTop(false);
    }
    auto DressUpView = getDressUpView();
    if(DressUpView)
        DressUpView->highlightReferences(false);
}

void TaskDressUpParameters::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto DressUpView = getDressUpView();
    if(!treeWidget || !DressUpView)
        return;

    if (msg.Type == Gui::SelectionChanges::SetPreselect
            || msg.Type == Gui::SelectionChanges::RmvPreselect) {
        onPreselectChanged(msg);
        return;
    }
    if (msg.Type == Gui::SelectionChanges::AddSelection
            || msg.Type == Gui::SelectionChanges::RmvSelection
            || msg.Type == Gui::SelectionChanges::ClrSelection
            || msg.Type == Gui::SelectionChanges::SetSelection) {
        syncPreviewSelection();
    }

    bool addSel = false;
    switch(msg.Type) {
    case Gui::SelectionChanges::ClrSelection: {
        showMessage();
        // A click clears the selection before it adds its pick: a missing
        // row selected until then is kept for that pick, and forgotten once
        // the click is over
        if (QTreeWidgetItem *item = getSoleMissingRow()) {
            missingPick = getGeometryItemText(item).constData();
            QTimer::singleShot(0, this, [this]() { missingPick.clear(); });
        }
        Base::StateLocker guard(busy);
        treeWidget->selectionModel()->clearSelection();
        break;
    }
    case Gui::SelectionChanges::RmvSelection:
        break;
    case Gui::SelectionChanges::AddSelection:
        addSel = true;
        break;
    default:
        return;
    }

    App::DocumentObject* base = this->getBase();
    auto selObj = msg.Object.getObject();
    if(!base || !selObj)
        return;

    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    if (selObj == pcDressUp && addSel && onPreviewSelected(msg))
        return;
    if(selObj == pcDressUp && addSel) {
        // The dress up feature itself is selected, trace the selected element
        // back to its base
        auto history = pcDressUp->getElementHistory(pcDressUp,msg.pSubName);
        const char *element = 0;
        std::string tmp;
        for(auto &hist : history) {
            if(hist.obj != base)
                continue;

            char type = pcDressUp->Shape.getShape().elementType(hist.element);
            if ((!allowFaces && (type == 'F' || type == 'W')) || (!allowEdges && type == 'E')
                    || (!allowVertexes && type == 'V'))
                continue;
            if(element) {
                showMessage("Ambiguous selection");
                return;
            }
            element = hist.element.appendToBufferWithPrefix(tmp);
        }
        if(element) {
            std::vector<App::SubObjectT> sels;
            sels.emplace_back(base, element);

            // base element found, check if it is already in the widget
            auto items = treeWidget->findItems(
                    QString::fromUtf8(sels.back().getOldElementName().c_str()), Qt::MatchExactly);

            if(items.size()) {
                if(selectionMode != refToggle) {
                    // element found, but we are not toggling, select and exit
                    Base::StateLocker guard(busy);
                    for(auto item : items)
                        item->setSelected(true);
                    return;
                }

                // We are toggling, so delete the found item, and call syncItems() below
                if(onTopObjs.empty()) {
                    for(auto item : items)
                        delete item;
                    sels.clear();
                }
            }

            if(msg.pOriginalMsg) {
                // We about dress up the current selected element, meaning that
                // this original element will be gone soon. So remove it from
                // the selection to avoid warning.
                Gui::Selection().rmvSelection(msg.pOriginalMsg->pDocName,
                                              msg.pOriginalMsg->pObjectName,
                                              msg.pOriginalMsg->pSubName);
            }

            if(onTopObjs.empty()) {
                if (sels.size() && replaceMissing(sels.back().getOldElementName()))
                    return;
                // If no on top display at the moment, just sync the reference
                // selection.
                syncItems(sels);
            } else {
                // If there is on top display, replace the current selection
                // with the base element selection, and let it trigger an
                // subsequent onSelectionChanged() event, which will be handled
                // by code below.
                std::string sub;
                auto obj = getInEdit(sub);
                if(obj) {
                    sub += element;
                    Gui::Selection().addSelection(obj->getDocument()->getName(),
                            obj->getNameInDocument(), sub.c_str());
                }
            }
        }
        return;
    }

    showMessage();

    if(!base || base != selObj)
        return;

    bool check = true;
    auto items = treeWidget->findItems(QString::fromUtf8(msg.pSubName), Qt::MatchExactly);
    if (items.empty() && addSel && msg.pSubName && replaceMissing(msg.pSubName))
        return;
    if(items.size()) {
        Base::StateLocker guard(busy);
        if(selectionMode == refToggle && addSel) {
            check = false;
            for(auto item : items)
                delete item;
        } else {
            for(auto item : items) {
                if(!item->isSelected())
                    item->setSelected(addSel);
            }
        }
    }

    if(addSel) {
        // In the add mode every click toggles: the pick is let go of, as a
        // pick of the preview is, or the next click on it deselects it, which
        // toggles nothing -- a corner's vertex, picked on the base, could be
        // added and never taken away (docs/HandsOnQueue.md entry 202 (a))
        const Gui::SelectionChanges &orig = msg.pOriginalMsg ? *msg.pOriginalMsg : msg;
        const std::string docName(orig.pDocName ? orig.pDocName : "");
        const std::string objName(orig.pObjectName ? orig.pObjectName : "");
        const std::string subName(orig.pSubName ? orig.pSubName : "");
        if(check)
            syncItems(Gui::Selection().getSelectionT());
        else
            syncItems();
        if (selectionMode == refToggle) {
            bool blocked = blockSelection(true);
            Gui::Selection().rmvSelection(docName.c_str(), objName.c_str(), subName.c_str());
            blockSelection(blocked);
        }
    }
}

bool TaskDressUpParameters::onPreviewSelected(const Gui::SelectionChanges& msg)
{
    // The preview picked in the 3D view, by the name the pick gave it before
    // it was resolved: "When (pre)selected in 3d view, resolve the element
    // name to base edge name. For missing element resolve to the ?Edge name.
    // So I am expecting the edit panel list item selection sync"
    // (docs/HandsOnQueue.md entry 169)
    const Gui::SelectionChanges &orig = msg.pOriginalMsg ? *msg.pOriginalMsg : msg;
    const std::string subname(orig.pSubName ? orig.pSubName : "");
    const std::string &prefix = PartDesign::FeatureAddSub::addsubElementPrefix();
    auto pos = subname.rfind(prefix);
    if (pos == std::string::npos)
        return false;
    const std::string docName(orig.pDocName ? orig.pDocName : "");
    const std::string objName(orig.pObjectName ? orig.pObjectName : "");
    auto dropPick = [&]() {
        Gui::Selection().rmvSelection(docName.c_str(), objName.c_str(), subname.c_str());
    };

    QTreeWidgetItem *item = getPreviewItem(subname.c_str() + pos + prefix.size());
    if (!item) {
        // nothing in the list to take it for: kept in the selection, but not
        // as a reference to toggle
        if (selectionMode == refToggle)
            dropPick();
        return true;
    }

    // A present edge's element is the base feature's edge: selected there in
    // its place, and taken from there as a pick of it would be -- its row
    // selected, or in the add mode removed
    if (getGeometryItemReference(item).isEmpty()) {
        std::string sub;
        auto obj = getInEdit(sub);
        if (!obj)
            return true;
        sub += getGeometryItemText(item).constData();
        dropPick();
        Gui::Selection().addSelection(obj->getDocument()->getName(),
                                      obj->getNameInDocument(), sub.c_str());
        return true;
    }

    // A missing one has nothing there: its row is all that names it, the
    // pick of its face stays the selection
    if (selectionMode == refToggle) {
        dropPick();
        {
            Base::StateLocker guard(busy);
            delete item;
        }
        syncItems();
        return true;
    }
    Base::StateLocker guard(busy);
    item->setSelected(true);
    return true;
}

QTreeWidgetItem *TaskDressUpParameters::getSoleMissingRow() const
{
    if (!treeWidget)
        return nullptr;
    QTreeWidgetItem *found = nullptr;
    for (auto item : treeWidget->selectedItems()) {
        if (auto parent = item->parent())
            item = parent;
        if (found && found != item)
            return nullptr;
        found = item;
    }
    if (!found || !isBaseItem(found) || getGeometryItemReference(found).isEmpty())
        return nullptr;
    return found;
}

bool TaskDressUpParameters::replaceMissing(const std::string &picked)
{
    // "when one missing (not failed) preview shape is selected, clicking
    // another edge (not already filleted) on base feature shall change the
    // edge reference inside the failed one. apply this to all other dressup
    // features" (docs/HandsOnQueue.md entry 188)
    std::string old;
    old.swap(missingPick);
    auto DressUpView = getDressUpView();
    if (!treeWidget || !DressUpView || selectionMode == refToggle)
        return false;
    // the element by its old style name, as the rows have it
    std::string element = picked;
    auto dot = element.rfind('.');
    if (dot != std::string::npos)
        element.erase(0, dot + 1);
    if (element.empty())
        return false;

    QTreeWidgetItem *item = nullptr;
    if (!old.empty()) {
        for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i) {
            auto row = treeWidget->topLevelItem(i);
            if (getGeometryItemText(row) == old.c_str()) {
                item = row;
                break;
            }
        }
        if (item && (!isBaseItem(item) || getGeometryItemReference(item).isEmpty()))
            item = nullptr;
    }
    else
        item = getSoleMissingRow();
    if (!item)
        return false;
    old = getGeometryItemText(item).constData();

    // an element of the kind the missing one was, and not one in use
    auto kind = [](const std::string &name) {
        std::size_t start = name.find_first_not_of(Data::missingPrefix());
        std::size_t end = name.find_first_of("0123456789", start);
        return start == std::string::npos ? std::string() : name.substr(start, end - start);
    };
    if (kind(old) != kind(element)
            || !treeWidget->findItems(QString::fromUtf8(element.c_str()), Qt::MatchExactly).empty())
        return false;

    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    App::DocumentObject* base = pcDressUp->Base.getValue();
    if (!base)
        return false;

    std::vector<std::string> subs;
    {
        Base::StateLocker guard(busy);
        // in its place in the list, with its values (a fillet's points and
        // group, a chamfer's sizes) under the new name
        item->setText(0, QString::fromUtf8(element.c_str()));
        setGeometryItemText(item, element);
        setGeometryItemReference(item, std::string());
        item->setForeground(0, QBrush());
        std::set<std::string> subset;
        for (auto row : rowsInOrder()) {
            std::string s = row->text(0).toStdString();
            if (subset.insert(s).second && isBaseItem(row))
                subs.push_back(std::move(s));
        }
        treeWidget->clearSelection();
        item->setSelected(true);
    }

    try {
        setupTransaction(tr("%1 in place of %2").arg(QString::fromStdString(element),
                                                     QString::fromStdString(old)));
        // what the values are kept under follows the reference, as when the
        // element map renames it
        pcDressUp->Base.signalUpdateElementReference(old, element);
        pcDressUp->Base.setValue(base, subs);
        recompute();
    }
    catch (Base::Exception &e) {
        e.ReportException();
    }

    // the new edge the selection, and its row, once the recompute has
    // refilled the list
    std::string sub;
    if (auto obj = getInEdit(sub)) {
        sub += element;
        App::SubObjectT sel(obj, sub.c_str());
        QTimer::singleShot(0, this, [this, sel, element]() {
            if (!treeWidget)
                return;
            {
                Base::StateLocker guard(busy);
                treeWidget->clearSelection();
                for (auto row : treeWidget->findItems(QString::fromUtf8(element.c_str()),
                                                      Qt::MatchExactly))
                    row->setSelected(true);
            }
            bool blocked = blockSelection(true);
            Gui::Selection().clearSelection();
            Gui::Selection().addSelection(sel.getDocumentName().c_str(),
                                          sel.getObjectName().c_str(),
                                          sel.getSubName().c_str());
            blockSelection(blocked);
        });
    }
    return true;
}

void TaskDressUpParameters::onPreselectChanged(const Gui::SelectionChanges& msg)
{
    // "Hover face should resolve the edge name too, which should trigger
    // edge preselection highlight and edit panel list item hover plus scroll
    // to view. Same if preselect edge in 3d view or hover list item, the
    // other two shall react" (docs/HandsOnQueue.md entry 169). A hover of
    // the preview's face of a present edge comes here as the base's edge
    // (ViewProviderDressUp::redirectEditingPick); a missing one's as the
    // face itself. A hover of the list preselects by itself (onTimer()).
    if (hoverFromList || !treeWidget)
        return;
    QTreeWidgetItem *item = nullptr;
    if (msg.Type == Gui::SelectionChanges::SetPreselect) {
        auto DressUpView = getDressUpView();
        auto* pcDressUp = DressUpView
            ? static_cast<PartDesign::DressUp*>(DressUpView->getObject()) : nullptr;
        App::DocumentObject *base = pcDressUp ? pcDressUp->getBaseObject(true) : nullptr;
        auto selObj = msg.Object.getObject();
        if (pcDressUp && selObj == pcDressUp) {
            const Gui::SelectionChanges &orig = msg.pOriginalMsg ? *msg.pOriginalMsg : msg;
            const std::string subname(orig.pSubName ? orig.pSubName : "");
            const std::string &prefix = PartDesign::FeatureAddSub::addsubElementPrefix();
            auto pos = subname.rfind(prefix);
            if (pos != std::string::npos)
                item = getPreviewItem(subname.c_str() + pos + prefix.size());
        }
        else if (base && selObj == base && msg.pSubName && msg.pSubName[0]) {
            for (auto found : treeWidget->findItems(QString::fromUtf8(msg.pSubName),
                                                    Qt::MatchExactly)) {
                if (!found->parent() && isBaseItem(found)) {
                    item = found;
                    break;
                }
            }
        }
    }
    setHoverItem(item);
    std::string subname;
    if (item && getItemElement(item, subname))
        lightPreviewFace(subname);
    else
        lightPreviewFace(std::string());
}

Gui::Gizmo* TaskDressUpParameters::getHoverHandle() const
{
    if (!treeWidget || !hoverItem || hoverFromList) {
        return nullptr;
    }
    // still a row of the list
    for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i) {
        if (treeWidget->topLevelItem(i) == hoverItem) {
            return itemHandle(hoverItem);
        }
    }
    return nullptr;
}

void TaskDressUpParameters::setHoverItem(QTreeWidgetItem *item)
{
    if (!treeWidget || item == hoverItem)
        return;
    // A look, not an edit: a changed background is an itemChanged, which a
    // fillet's panel takes for an edit of the row's values (and crashed
    // on). The view is told by the model, which this does not block.
    QSignalBlocker blocker(treeWidget);
    if (hoverItem) {
        // still a row of the list: one deleted meanwhile is not touched
        for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i) {
            if (treeWidget->topLevelItem(i) != hoverItem)
                continue;
            for (int col = 0; col < static_cast<int>(hoverBrushes.size()); ++col)
                hoverItem->setBackground(col, hoverBrushes[col]);
            break;
        }
    }
    hoverItem = item;
    hoverBrushes.clear();
    if (!item)
        return;
    // the look of a row under the pointer: the selection's colour, faint
    QColor color = treeWidget->palette().color(QPalette::Highlight);
    color.setAlpha(70);
    for (int col = 0, count = treeWidget->columnCount(); col < count; ++col) {
        hoverBrushes.push_back(item->background(col));
        item->setBackground(col, QBrush(color));
    }
    treeWidget->scrollToItem(item);
}

bool TaskDressUpParameters::getPreviewTarget(const char *element, App::SubObjectT &target)
{
    QTreeWidgetItem *item = getPreviewItem(element);
    if (!item || !getGeometryItemReference(item).isEmpty())
        return false;
    std::string sub;
    auto obj = getInEdit(sub);
    if (!obj)
        return false;
    sub += getGeometryItemText(item).constData();
    target = App::SubObjectT(obj, sub.c_str());
    return true;
}

QTreeWidgetItem *TaskDressUpParameters::getPreviewItem(const char *element)
{
    auto DressUpView = getDressUpView();
    if (!DressUpView || !treeWidget || !element || !element[0])
        return nullptr;
    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    App::DocumentObject *base = pcDressUp->Base.getValue();
    const Part::TopoShape preview = pcDressUp->DressUpShape.getShape();
    if (!base || preview.isNull())
        return nullptr;
    TopoDS_Shape picked;
    try {
        picked = preview.getSubShape(element, true);
    }
    catch (Base::Exception &) {
    }
    if (picked.IsNull())
        return nullptr;

    // The faces of the preview the element is, or lies on
    std::vector<TopoDS_Shape> faces;
    if (picked.ShapeType() == TopAbs_FACE)
        faces.push_back(picked);
    else {
        for (TopExp_Explorer f(preview.getShape(), TopAbs_FACE); f.More(); f.Next()) {
            for (TopExp_Explorer e(f.Current(), picked.ShapeType()); e.More(); e.Next()) {
                if (e.Current().IsSame(picked)) {
                    faces.push_back(f.Current());
                    break;
                }
            }
        }
    }

    QTreeWidgetItem *found = nullptr;
    bool ambiguous = false;
    auto take = [&](QTreeWidgetItem *item) {
        if (found && found != item)
            ambiguous = true;
        found = item;
    };

    // By the faces the feature keeps for each reference: a fillet's, and a
    // missing one's as it was last made (getItemElement(), the other way)
    for (int i = 0, count = treeWidget->topLevelItemCount(); i < count; ++i) {
        auto item = treeWidget->topLevelItem(i);
        if (!isBaseItem(item))
            continue;
        TopoDS_Shape made = pcDressUp->getMadeFaces(getGeometryItemText(item).constData());
        if (made.IsNull())
            continue;
        bool hit = false;
        for (TopExp_Explorer exp(made, TopAbs_FACE); !hit && exp.More(); exp.Next()) {
            for (const auto &face : faces)
                hit = hit || face.IsPartner(exp.Current());
        }
        if (hit)
            take(item);
    }
    if (found)
        return ambiguous ? nullptr : found;

    // Else by the history of the feature's own face the preview shows: back
    // to the base element it was made on (a chamfer's edge, a draft's face)
    const Part::TopoShape shape = pcDressUp->Shape.getShape();
    std::string tmp;
    for (const auto &face : faces) {
        for (int i = 1, c = shape.countSubShapes(TopAbs_FACE); i <= c; ++i) {
            if (!face.IsPartner(shape.getSubShape(TopAbs_FACE, i)))
                continue;
            const std::string name = "Face" + std::to_string(i);
            for (auto &hist : pcDressUp->getElementHistory(pcDressUp, name.c_str())) {
                if (hist.obj != base)
                    continue;
                tmp.clear();
                App::SubObjectT sel(base, hist.element.appendToBufferWithPrefix(tmp));
                auto items = treeWidget->findItems(
                        QString::fromUtf8(sel.getOldElementName().c_str()), Qt::MatchExactly);
                for (auto item : items) {
                    if (!item->parent() && isBaseItem(item))
                        take(item);
                }
            }
        }
    }
    return ambiguous ? nullptr : found;
}

void TaskDressUpParameters::onButtonRefAdd(bool checked)
{
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        return;

    // We no longer use highlightReferences now, but ShowOnTopSelection
    // instead. Turning off here just to be safe.
    DressUpView->highlightReferences(false);

    if(!checked) {
        clearButtons(none);
        exitSelectionMode();
        return;
    }

    std::string subname;
    auto obj = getInEdit(subname);
    if(obj) {
        bool blocked = blockSelection(true);
        Base::StateLocker guard(busy);
        for(auto item : treeWidget->selectedItems()) {
            std::string sub = subname + getGeometryItemText(item).constData();
            Gui::Selection().rmvSelection(obj->getDocument()->getName(),
                    obj->getNameInDocument(), sub.c_str());
            delete item;
        }
        blockSelection(blocked);
        syncItems(Gui::Selection().getSelectionT());
    }
    exitSelectionMode();
    selectionMode = refToggle;
    clearButtons(refToggle);
    Gui::Selection().clearSelection();
    AllowSelectionFlags allow;
    allow.setFlag(AllowSelection::EDGE, allowEdges);
    allow.setFlag(AllowSelection::FACE, allowFaces);
    allow.setFlag(AllowSelection::POINT, allowVertexes);
    allow.setFlag(AllowSelection::PLANAR, false);
    // The base's elements, and the preview's: a pick of it toggles the
    // reference that made it (onPreviewSelected(), entry 169). Refused, a
    // click on the preview -- drawn over the base's edges -- went nowhere.
    class DressUpSelection : public ReferenceSelection
    {
    public:
        DressUpSelection(const App::DocumentObject *base, AllowSelectionFlags allow,
                         const App::DocumentObject *dressUp)
            : ReferenceSelection(base, allow), dressUp(dressUp)
        {}
        bool allow(App::Document *pDoc, App::DocumentObject *pObj, const char *sSubName) override
        {
            return (pObj && pObj == dressUp)
                || ReferenceSelection::allow(pDoc, pObj, sSubName);
        }
    private:
        const App::DocumentObject *dressUp;
    };
    Gui::Selection().addSelectionGate(
            new DressUpSelection(this->getBase(), allow, DressUpView->getObject()));
}

void TaskDressUpParameters::onRefDeleted() {
    auto DressUpView = getDressUpView();
    if(!treeWidget || !DressUpView)
        return;

    Base::StateLocker guard(busy);
    for(auto item : treeWidget->selectedItems())
        delete item;

    syncItems();
}

bool TaskDressUpParameters::syncItems(const std::vector<App::SubObjectT> &sels) {
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        return false;

    std::set<std::string> subset;
    std::vector<std::string> subs;
    // the feature's order, not the list's sort
    for(auto item : rowsInOrder()) {
        std::string s = item->text(0).toStdString();
        if(subset.insert(s).second && isBaseItem(item))
            subs.push_back(std::move(s));
    }

    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    App::DocumentObject* base = pcDressUp->Base.getValue();

    if(sels.size()) {
        Base::StateLocker guard(busy);
        for(auto &sel : sels) {
            if(sel.getObject() != base)
                continue;
            std::string element = sel.getOldElementName();
            if(subset.count(element))
                continue;
            if((allowEdges && boost::starts_with(element,"Edge"))
                    || (allowFaces && (boost::starts_with(element,"Face")
                                       || boost::starts_with(element, "Wire")))
                    || (allowVertexes && boost::starts_with(element, "Vertex")))
            {
                subset.insert(element);
                auto item = new QTreeWidgetItem(treeWidget);
                item->setText(0, QString::fromUtf8(element.c_str()));
                setGeometryItemText(item, element.c_str());
                item->setSelected(true);
                if (isBaseItem(item))
                    subs.push_back(std::move(element));
                onNewItem(item);
            }
        }
    }

    syncOtherItems();
    const auto before = pcDressUp->Base.getSubValues();
    if(subs == before)
        return false;

    try {
        // what was added, what removed
        QStringList added;
        QStringList removed;
        for (const auto& s : subs) {
            if (std::find(before.begin(), before.end(), s) == before.end())
                added << QString::fromStdString(s);
        }
        for (const auto& s : before) {
            if (std::find(subs.begin(), subs.end(), s) == subs.end())
                removed << QString::fromStdString(s);
        }
        QStringList what;
        if (!added.isEmpty())
            what << tr("add %1").arg(added.join(QStringLiteral(", ")));
        if (!removed.isEmpty())
            what << tr("remove %1").arg(removed.join(QStringLiteral(", ")));
        setupTransaction(what.join(QStringLiteral("; ")));
        pcDressUp->Base.setValue(base, subs);
        recompute();
    } catch (Base::Exception &e) {
        e.ReportException();
    }
    return true;
}

void TaskDressUpParameters::recompute()
{
    recomputeFeature();
}

void TaskDressUpParameters::finishedRecomputeFeature()
{
    showMessage();
}

void TaskDressUpParameters::createAddAllEdgesAction(QTreeWidget* parentList)
{
    // creates a context menu, a shortcut for it and connects it to a slot function

    addAllEdgesAction = new QAction(tr("Add all edges"), this);
    addAllEdgesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    // display shortcut behind the context menu entry
    addAllEdgesAction->setShortcutVisibleInContextMenu(true);
#endif
    parentList->addAction(addAllEdgesAction);
    addAllEdgesAction->setEnabled(false);
    addAllEdgesAction->setStatusTip(tr("Adds all edges to the list box (active only when in add selection mode)."));
    parentList->setContextMenuPolicy(Qt::ActionsContextMenu);
}

Base::Color TaskDressUpParameters::autoGroupColor(int group) const
{
    // The hues a handle already wears: plain, dragged, selected and under the
    // pointer. A group's own colour keeps clear of them all, or a handle of
    // it reads as lit (docs/HandsOnQueue.md entries 196 and 202).
    std::vector<float> taken;
    auto take = [&taken](const QColor& color) {
        // a grey has no hue to keep clear of
        if (color.hsvSaturationF() > 0.25F) {
            taken.push_back(color.hsvHueF());
        }
    };
    auto fromBase = [](const Base::Color& c) {
        return QColor::fromRgbF(c.r, c.g, c.b);
    };
    // packed 0xRRGGBBAA, as the view's colours are kept
    auto fromPacked = [](unsigned long packed) {
        return QColor(int((packed >> 24) & 0xff), int((packed >> 16) & 0xff),
                      int((packed >> 8) & 0xff));
    };
    if (auto* styles = Base::provideService<Gui::StyleParameters::ParameterManager>()) {
        take(fromBase(styles->resolve(Gui::StyleParameters::LinearGizmoBaseColor)));
        take(fromBase(styles->resolve(Gui::StyleParameters::LinearGizmoActiveColor)));
    }
    take(fromPacked(Gui::ViewParams::getSelectionColor()));
    take(fromPacked(Gui::ViewParams::getHighlightColor()));

    // A colour the user picked for another group is taken too
    for (int id : groupIds()) {
        if (id != group) {
            if (auto picked = pickedGroupColor(id)) {
                take(*picked);
            }
        }
    }

    // The ids in turn from 1, each the hue farthest from every hue taken so
    // far -- the handle's, the picked ones, and the ids' before it
    // (Base::spreadHues): two groups close in id stay apart (the user, entry
    // 209: "group 2 and 3 has very similar color"). By the id alone, every id
    // before it counted whether it is a group now or not: counted among the
    // groups there are, a group that emptied moved every later one onto the
    // hue of the one before it -- an edge that joined group 3 from group 1
    // took nothing, group 3 took group 1's colour (entry 226). Past 64 ids
    // the spread comes round again.
    constexpr int Spread = 64;
    const int place = group > 0 ? (group - 1) % Spread + 1 : 1;
    const auto hues = Base::spreadHues(taken, place);
    QColor color = QColor::fromHsvF(hues.back(), 0.75F, 0.95F);
    return Base::Color(float(color.redF()), float(color.greenF()), float(color.blueF()));
}

App::PropertyMap* TaskDressUpParameters::getGroupColors() const
{
    auto vp = getDressUpView();
    auto obj = vp ? vp->getObject() : nullptr;
    return obj ? dynamic_cast<App::PropertyMap*>(obj->getPropertyByName("GroupColors"))
               : nullptr;
}

std::optional<QColor> TaskDressUpParameters::pickedGroupColor(int group) const
{
    if (auto colors = getGroupColors()) {
        const auto& values = colors->getValues();
        auto it = values.find(std::to_string(group));
        if (it != values.end()) {
            QColor color(QString::fromStdString(it->second));
            if (color.isValid()) {
                return color;
            }
        }
    }
    return std::nullopt;
}

Base::Color TaskDressUpParameters::groupColor(int group) const
{
    if (auto color = pickedGroupColor(group)) {
        return Base::Color(float(color->redF()), float(color->greenF()), float(color->blueF()));
    }
    return autoGroupColor(group);
}

QIcon TaskDressUpParameters::groupIcon(int group) const
{
    const Base::Color color = groupColor(group);
    QPixmap pixmap(12, 12);
    pixmap.fill(QColor::fromRgbF(color.r, color.g, color.b));
    return QIcon(pixmap);
}

void TaskDressUpParameters::setGroupColor(int group, const std::optional<QColor>& color)
{
    auto colors = getGroupColors();
    if (!colors || group <= 0) {
        return;
    }
    const std::string key = std::to_string(group);
    if (color) {
        colors->setValue(key, color->name().toStdString());
    }
    else if (colors->getValues().count(key)) {
        auto values = colors->getValues();
        values.erase(key);
        colors->setValues(values);
    }
    else {
        return;
    }
    // the rows may be made again: the current one stays current, not
    // selected anew (a selection reaches the view)
    const QString current = treeWidget && treeWidget->currentItem()
        ? treeWidget->currentItem()->text(0) : QString();
    groupColorsChanged();
    if (treeWidget && !current.isEmpty()) {
        const auto found = treeWidget->findItems(current, Qt::MatchExactly);
        if (!found.isEmpty() && treeWidget->currentItem() != found.front()) {
            Base::StateLocker guard(busy);
            treeWidget->setCurrentItem(found.front(), 0, QItemSelectionModel::NoUpdate);
        }
    }
    Q_EMIT groupColorsUpdated();
}

void TaskDressUpParameters::addGroupColorActions(QMenu& menu, int group)
{
    if (group <= 0 || !getGroupColors()) {
        return;
    }
    QAction* pick = menu.addAction(groupIcon(group), tr("Group colour..."));
    connect(pick, &QAction::triggered, this, [this, group]() {
        // after the menu: the dialog runs a loop of its own
        QTimer::singleShot(0, this, [this, group]() {
            pickGroupColor(group);
        });
    });
    QAction* automatic = menu.addAction(tr("Automatic group colour"));
    automatic->setEnabled(pickedGroupColor(group).has_value());
    connect(automatic, &QAction::triggered, this, [this, group]() {
        QTimer::singleShot(0, this, [this, group]() {
            setupTransaction(tr("group %1 colour automatic").arg(group));
            setGroupColor(group, std::nullopt);
        });
    });
}

void TaskDressUpParameters::pickGroupColor(int group)
{
    const Base::Color current = groupColor(group);
    QColor color = QColorDialog::getColor(QColor::fromRgbF(current.r, current.g, current.b),
                                          Gui::getMainWindow(),
                                          tr("The colour of group %1").arg(group));
    if (color.isValid()) {
        setupTransaction(tr("group %1 colour").arg(group));
        setGroupColor(group, color);
    }
}

void TaskDressUpParameters::createGroupColorActions(QTreeWidget* parentList)
{
    // the list's own menu: the current row's group, its colour picked or
    // made automatic again
    auto pick = new QAction(tr("Group colour..."), this);
    auto automatic = new QAction(tr("Automatic group colour"), this);
    parentList->addAction(pick);
    parentList->addAction(automatic);
    auto update = [this, parentList, pick, automatic]() {
        QTreeWidgetItem* item = parentList->currentItem();
        const int group = item ? itemGroup(item) : 0;
        pick->setEnabled(group > 0);
        pick->setIcon(group > 0 ? groupIcon(group) : QIcon());
        automatic->setEnabled(group > 0 && pickedGroupColor(group).has_value());
    };
    update();
    connect(parentList, &QTreeWidget::currentItemChanged, this, update);
    connect(this, &TaskDressUpParameters::groupColorsUpdated, this, update);
    connect(pick, &QAction::triggered, this, [this, parentList]() {
        if (QTreeWidgetItem* item = parentList->currentItem()) {
            const int group = itemGroup(item);
            if (group > 0) {
                QTimer::singleShot(0, this, [this, group]() {
                    pickGroupColor(group);
                });
            }
        }
    });
    connect(automatic, &QAction::triggered, this, [this, parentList]() {
        if (QTreeWidgetItem* item = parentList->currentItem()) {
            const int group = itemGroup(item);
            if (group > 0) {
                setupTransaction(tr("group %1 colour automatic").arg(group));
                setGroupColor(group, std::nullopt);
            }
        }
    });
}

void TaskDressUpParameters::showMessage(const char *msg) {
    auto DressUpView = getDressUpView();
    if(!messageLabel || !DressUpView)
        return;

    auto obj = DressUpView->getObject();
    if(!msg && obj->isError())
        msg = obj->getStatusString();
    if(!msg || !msg[0])
        messageLabel->hide();
    else {
        messageLabel->setText(QStringLiteral("<font color='red'>%1<br/>%2</font>").arg(
                tr("Recompute failed"), tr(msg)));
        messageLabel->show();
    }
}

void TaskDressUpParameters::onItemSelectionChanged()
{
    if(!treeWidget || busy)
        return;

    if(selectionMode == refToggle) {
        onRefDeleted();
        return;
    }

    std::string subname;
    auto obj = getInEdit(subname);
    if(!obj)
        return;

    std::set<QTreeWidgetItem*> itemSet;
    std::vector<std::string> subs;
    const std::string &prefix = PartDesign::FeatureAddSub::addsubElementPrefix();
    for(auto item : treeWidget->selectedItems()) {
        if (auto parent = item->parent())
            item = parent;
        if (itemSet.insert(item).second) {
            if (getGeometryItemReference(item).isEmpty()) {
                subs.push_back(subname + getGeometryItemText(item).constData());
                continue;
            }
            // A missing edge has nothing in the base to select: its preview
            // face stands for it, as a click on that face selects its row --
            // which also makes the next plain click in the view replace this
            // one, not add to it (docs/HandsOnQueue.md entry 194)
            std::string face;
            if (getItemElement(item, face) && face.find(prefix) != std::string::npos)
                subs.push_back(face);
        }
    }

    bool blocked = blockSelection(true);
    Gui::Selection().clearSelection();
    Gui::Selection().addSelections(obj->getDocument()->getName(),
            obj->getNameInDocument(), subs);
    blockSelection(blocked);
    // the panel heard none of it: the preview's faces in the selection colour
    syncPreviewSelection();
}

void TaskDressUpParameters::onActiveHandleChanged(Gui::Gizmo *handle)
{
    if (!treeWidget)
        return;
    // let go: the selection stays, the next handle made active selects again
    if (!handle) {
        activeHandleEdge.clear();
        return;
    }
    // a missing edge's name as its row and its handle may give it
    auto plain = [](std::string name) {
        if (boost::starts_with(name, Data::missingPrefix()))
            name.erase(0, Data::missingPrefix().size());
        return name;
    };
    const std::string edge = plain(handleEdge(handle));
    // the same edge's handle made anew by a recompute, and active again
    if (edge.empty() || edge == activeHandleEdge)
        return;
    activeHandleEdge = edge;
    // After the click: the view takes its release as a pick of nothing
    // selectable and clears the selection, the row's with it
    QPointer<TaskDressUpParameters> self(this);
    QTimer::singleShot(0, this, [self, edge, plain]() {
        if (!self || !self->treeWidget || self->activeHandleEdge != edge)
            return;
        for (auto row : self->rowsInOrder()) {
            if (plain(getGeometryItemText(row).constData()) != edge)
                continue;
            {
                QSignalBlocker blocker(self->treeWidget);
                self->treeWidget->clearSelection();
                row->setSelected(true);
            }
            self->treeWidget->scrollToItem(row);
            self->onItemSelectionChanged();
            return;
        }
    });
}

App::SubObjectT TaskDressUpParameters::getInEdit(App::DocumentObject *base, const char *sub) {
    std::string subname;
    auto obj = getInEdit(subname,base);
    if(obj) {
        if(sub)
            subname += sub;
        return App::SubObjectT(obj,subname.c_str());
    }
    return App::SubObjectT();
}

App::DocumentObject *TaskDressUpParameters::getInEdit(std::string &subname, App::DocumentObject *base)
{
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        return nullptr;

    auto* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    if(!base) {
        base = pcDressUp->Base.getValue();
        if(!base)
            return nullptr;
    }
    auto editDoc = Gui::Application::Instance->editDocumentOf(pcDressUp);
    if(!editDoc)
        return nullptr;

    ViewProviderDocumentObject *editVp = nullptr;
    editDoc->getInEdit(&editVp,&subname);
    if(!editVp)
        return nullptr;

    auto sobjs = editVp->getObject()->getSubObjectList(subname.c_str());
    if(sobjs.empty())
        return nullptr;

    for(;;) {
        if(sobjs.back() == base)
            break;
        sobjs.pop_back();
        if(sobjs.empty())
            break;
        std::string sub(base->getNameInDocument());
        sub += ".";
        if(sobjs.back()->getSubObject(sub.c_str())) {
            sobjs.push_back(base);
            break;
        }
    }

    std::ostringstream ss;
    for(size_t i=1;i<sobjs.size();++i)
        ss << sobjs[i]->getNameInDocument() << '.';

    subname = ss.str();
    return sobjs.size()?sobjs.front():base;
}

void TaskDressUpParameters::onItemEntered(QTreeWidgetItem *, int)
{
    enteredObject = treeWidget;
    timer->start(100);
}

bool TaskDressUpParameters::getItemElement(QTreeWidgetItem *item, std::string &subname)
{
    auto DressUpView = getDressUpView();
    if (!DressUpView)
        return false;
    if (auto parent = item->parent())
        item = parent;
    PartDesign::DressUp* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    Data::IndexedName indexed;
    // The face of the preview the edge's dress-up made, as the feature keeps
    // it by the edge's name: a present edge's own fillet face, not the base's
    // edge, and a missing one's face as last made, which has no name to be
    // traced by (docs/HandsOnQueue.md entry 160)
    auto previewFace = [&]() {
        TopoDS_Shape made = pcDressUp->getMadeFaces(getGeometryItemText(item).constData());
        auto preview = pcDressUp->DressUpShape.getShape();
        if (made.IsNull() || preview.isNull())
            return false;
        for (int i = 1, c = preview.countSubShapes(TopAbs_FACE); i <= c; ++i) {
            const TopoDS_Shape face = preview.getSubShape(TopAbs_FACE, i);
            for (TopExp_Explorer exp(made, TopAbs_FACE); exp.More(); exp.Next()) {
                if (face.IsPartner(exp.Current())) {
                    indexed = Data::IndexedName::fromConst("Face", i);
                    return true;
                }
            }
        }
        return false;
    };
    // The element named, under the edited object: the preview's behind its
    // prefix, else the feature's own
    auto under = [&](bool preview) {
        subname.clear();
        ViewProviderDocumentObject *editVp = nullptr;
        auto dressUpView = getDressUpView();
        auto editDoc = Gui::Application::Instance->editDocumentOf(
            dressUpView ? dressUpView->getObject() : nullptr);
        if (!editDoc)
            return false;
        editDoc->getInEdit(&editVp,&subname);
        if (preview)
            subname += PartDesign::FeatureAddSub::addsubElementPrefix();
        indexed.appendToStringBuffer(subname);
        return true;
    };
    if (previewFace())
        return under(true);

    QByteArray _ref = getGeometryItemReference(item);
    const char *ref = Data::isMappedElement(_ref.constData());
    if (!ref) {
        subname += getGeometryItemText(item).constData();
        return true;
    }
    std::string tmp;
    App::StringHasherRef docHasher;
    if (auto doc = pcDressUp->getDocument())
        docHasher = doc->getStringHasher();
    // Whether a face named \a name was made on the referenced element: its
    // name traced back, step by step, to the reference's.
    //
    // Not by TopoShape::traceElement(), which expands each hashed name by
    // the shape's own hasher alone. The preview shape of a dress-up in edit
    // has a hasher of its own, and one step back its names are the
    // document's: the trace stopped there for every reference, and the
    // pointer on a missing one lit nothing (docs/HandsOnQueue.md entry 109,
    // "broken" since the old fork). A name the shape's hasher does not know
    // is the document's, and so is every name behind it.
    auto traces = [&](const Part::TopoShape &shape, Data::MappedName name) {
        App::StringHasherRef hasher = shape.Hasher;
        for (int step = 0; step < 50; ++step) {
            tmp.clear();
            name.appendToBuffer(tmp);
            if (tmp == ref)
                return true;
            long tag = 0;
            int len = 0;
            if (name.findTagInElementName(&tag, &len, nullptr, nullptr, true) < 0
                    || len <= 0 || len > static_cast<int>(name.size()))
                return false;
            Data::MappedName head(name, 0, len);
            Data::MappedName origin = head.dehashElementName(hasher);
            if (origin == head && docHasher && hasher != docHasher) {
                hasher = docHasher;
                origin = head.dehashElementName(hasher);
            }
            if (origin == head)
                return false;
            name = origin;
        }
        return false;
    };
    // The face of a shape that was made on the referenced element
    auto lookup = [&](const Part::TopoShape &shape) {
        for (int i=1, c=shape.countSubShapes(TopAbs_FACE); i<=c; ++i) {
            indexed = Data::IndexedName::fromConst("Face", i);
            if (traces(shape, shape.getMappedName(indexed)))
                return true;
        }
        return false;
    };
    auto shape = pcDressUp->DressUpShape.getShape();
    bool found = !shape.isNull() && lookup(shape);
    // A feature in error may have no dress-up shape at all. The shape it
    // keeps from before -- what is shown of it meanwhile -- names its faces
    // by the edges they were made on, the missing ones too: the fillet's
    // own face is lit there.
    bool kept = false;
    if (!found) {
        kept = true;
        found = lookup(pcDressUp->Shape.getShape());
    }
    if (!found)
        return false;
    return under(!kept);
}

void TaskDressUpParameters::onTimer() {
    if(enteredObject != treeWidget || !treeWidget)
        return;

    // the list's own hover: the row is under the pointer, and what it
    // preselects is not to be taken back as a hover from the 3D view
    Base::StateLocker guard(hoverFromList);
    setHoverItem(nullptr);
    auto item = treeWidget->itemAt(treeWidget->viewport()->mapFromGlobal(QCursor::pos()));
    if(!item) {
        Gui::Selection().rmvPreselect();
        lightPreviewFace(std::string());
        return;
    }
    std::string subname;
    auto obj = getInEdit(subname);
    // A present edge (an item without a kept reference) is preselected on
    // the base feature as well, its fillet face lit beside it: "hovering
    // normal edge should highlight the edge on base feature too"
    // (docs/HandsOnQueue.md entry 160). A missing one has no edge there;
    // its kept face alone is preselected.
    std::string baseEdge;
    if (obj) {
        auto top = item->parent() ? item->parent() : item;
        if (getGeometryItemReference(top).isEmpty())
            baseEdge = subname + getGeometryItemText(top).constData();
    }
    if(obj && getItemElement(item, subname)) {
        Gui::Selection().setPreselect(obj->getDocument()->getName(),
                                      obj->getNameInDocument(),
                                      baseEdge.empty() ? subname.c_str() : baseEdge.c_str(),
                                      0,0,0,
                                      Gui::SelectionChanges::MsgSource::TreeView,
                                      true);
        lightPreviewFace(subname);
    }
    else if (!baseEdge.empty()) {
        Gui::Selection().setPreselect(obj->getDocument()->getName(),
                                      obj->getNameInDocument(),
                                      baseEdge.c_str(),0,0,0,
                                      Gui::SelectionChanges::MsgSource::TreeView,
                                      true);
        lightPreviewFace(std::string());
    }
    else {
        Gui::Selection().rmvPreselect();
        lightPreviewFace(std::string());
    }
}

void TaskDressUpParameters::syncPreviewSelection()
{
    // A selection that names the preview's face itself -- a failed or
    // missing edge's, which a click on it or its row selects (a working
    // edge's face selects the base's edge instead) -- shows the face in the
    // selection colour (the reporter 2026-10-19 13:49, entry 182)
    auto view = getDressUpView();
    auto obj = view ? view->getObject() : nullptr;
    if (!obj || !obj->isAttachedToDocument())
        return;
    const std::string &prefix = PartDesign::FeatureAddSub::addsubElementPrefix();
    const std::string owner = std::string(obj->getNameInDocument()) + ".";
    std::vector<int> faces;
    for (const auto &sel : Gui::Selection().getCompleteSelection(Gui::ResolveMode::NoResolve)) {
        if (!sel.pObject || sel.pObject->getDocument() != obj->getDocument() || !sel.SubName)
            continue;
        const std::string subname(sel.SubName);
        auto pos = subname.rfind(prefix);
        if (pos == std::string::npos)
            continue;
        // the preview of this feature: its name ends the path before the prefix,
        // or the feature is the selected object itself
        const bool own = sel.pObject == obj
            || (pos >= owner.size() && subname.compare(pos - owner.size(), owner.size(), owner) == 0);
        if (!own)
            continue;
        const char *element = subname.c_str() + pos + prefix.size();
        int face = 0;
        if (boost::starts_with(element, "Face"))
            face = std::atoi(element + 4);
        else if (QTreeWidgetItem *item = getPreviewItem(element)) {
            // an edge of a missing edge's face, which a thin face is picked
            // by within the pick radius -- a browser's, mostly (entry 189):
            // it selects that row as the face does, and shows it as the face
            if (!getGeometryItemReference(item).isEmpty())
                face = previewFaceOf(getGeometryItemText(item).constData());
        }
        if (face > 0 && std::find(faces.begin(), faces.end(), face) == faces.end())
            faces.push_back(face);
    }
    // A failed edge that is still there: a click on its face selects the
    // base's edge (ViewProviderDressUp::redirectEditingPick), but the red
    // face is what stands for it, so that face shows selected with it (the
    // reporter's PrismLocker in scanner, 2026-10-19 15:16)
    auto dressUp = Base::freecad_dynamic_cast<PartDesign::DressUp>(obj);
    App::DocumentObject *base = dressUp ? dressUp->getBaseObject(true) : nullptr;
    const auto &failed = dressUp ? dressUp->FailedEdges.getValues() : std::vector<std::string>();
    if (base && !failed.empty()) {
        for (const auto &sel : Gui::Selection().getSelectionEx(
                 obj->getDocument()->getName(), App::DocumentObject::getClassTypeId(),
                 Gui::ResolveMode::OldStyleElement)) {
            if (sel.getObject() != base)
                continue;
            for (const auto &name : sel.getSubNames()) {
                if (std::find(failed.begin(), failed.end(), name) == failed.end())
                    continue;
                const int face = previewFaceOf(name);
                if (face > 0 && std::find(faces.begin(), faces.end(), face) == faces.end())
                    faces.push_back(face);
            }
        }
    }
    view->selectPreviewFaces(faces);
}

int TaskDressUpParameters::previewFaceOf(const std::string &edge) const
{
    // the face of the preview the feature keeps for \a edge, as last made:
    // by partner with the faces getMadeFaces() names (as getItemElement())
    auto view = getDressUpView();
    auto dressUp = view ? Base::freecad_dynamic_cast<PartDesign::DressUp>(view->getObject()) : nullptr;
    if (!dressUp)
        return 0;
    TopoDS_Shape made = dressUp->getMadeFaces(edge);
    auto preview = dressUp->DressUpShape.getShape();
    if (made.IsNull() || preview.isNull())
        return 0;
    for (int i = 1, c = preview.countSubShapes(TopAbs_FACE); i <= c; ++i) {
        const TopoDS_Shape face = preview.getSubShape(TopAbs_FACE, i);
        for (TopExp_Explorer exp(made, TopAbs_FACE); exp.More(); exp.Next()) {
            if (face.IsPartner(exp.Current()))
                return i;
        }
    }
    return 0;
}

void TaskDressUpParameters::lightPreviewFace(const std::string &subname)
{
    // A face of the preview, named behind the add/sub prefix: lit by its
    // colour as well, because the preselection of it lights nothing under a
    // per-view edit (ViewProviderAddSub::highlightPreviewFace()). Anything
    // else, and no name at all, puts the preview back in its own colour.
    auto view = getDressUpView();
    if (!view)
        return;
    int face = 0;
    const std::string &prefix = PartDesign::FeatureAddSub::addsubElementPrefix();
    auto pos = subname.rfind(prefix);
    if (pos != std::string::npos) {
        const char *element = subname.c_str() + pos + prefix.size();
        if (boost::starts_with(element, "Face"))
            face = std::atoi(element + 4);
    }
    view->highlightPreviewFace(face);
}

std::vector<std::string> TaskDressUpParameters::getReferences() const
{
    auto DressUpView = getDressUpView();
    if(!DressUpView) 
        return {};

    PartDesign::DressUp* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    std::vector<std::string> result = pcDressUp->Base.getSubValues(false);
    return result;
}

Part::Feature* TaskDressUpParameters::getBase(void) const
{
    auto DressUpView = getDressUpView();
    if(!DressUpView)
        THROWM(Base::RuntimeError, "No view object")

    PartDesign::DressUp* pcDressUp = static_cast<PartDesign::DressUp*>(DressUpView->getObject());
    // Unlikely but this may throw an exception in case we are started to edit an object which base feature
    // was deleted. This exception will be likely unhandled inside the dialog and pass upper, But an error
    // message inside the report view is better than a SEGFAULT.
    // Generally this situation should be prevented in ViewProviderDressUp::setEdit()
    return pcDressUp->getBaseObject();
}

void TaskDressUpParameters::exitSelectionMode()
{
    selectionMode = none;
    Gui::Selection().rmvSelectionGate();
    Gui::Selection().clearSelection();
}

bool TaskDressUpParameters::handleEvent(QEvent *e)
{
    if (e && e->type() == QEvent::ShortcutOverride) {
        QKeyEvent * kevent = static_cast<QKeyEvent*>(e);
        if (deleteAction && Gui::QtTools::matches(kevent, deleteAction->shortcut())) {
            kevent->accept();
            return true;
        }
        if (addAllEdgesAction && Gui::QtTools::matches(kevent, addAllEdgesAction->shortcut())) {
            kevent->accept();
            return true;
        }
    }
    else if (e && e->type() == QEvent::KeyPress) {
        QKeyEvent * kevent = static_cast<QKeyEvent*>(e);
        if (deleteAction && deleteAction->isEnabled() &&
            Gui::QtTools::matches(kevent, deleteAction->shortcut())) {
            deleteAction->trigger();
            return true;
        }
        if (addAllEdgesAction && addAllEdgesAction->isEnabled() &&
            Gui::QtTools::matches(kevent, addAllEdgesAction->shortcut())) {
            addAllEdgesAction->trigger();
            return true;
        }
    }
    else if (e && e->type() == QEvent::Leave) {
        enteredObject = nullptr;
        timer->stop();
        Base::StateLocker guard(hoverFromList);
        Gui::Selection().rmvPreselect();
        lightPreviewFace(std::string());
    }
    return false;
}

bool TaskDressUpParameters::event(QEvent *e)
{
    return handleEvent(e) || Gui::TaskView::TaskBox::event(e);
}

bool TaskDressUpParameters::eventFilter(QObject *o, QEvent *e)
{
    // A double click on a value's cell edits it (entry 202 (f): "Double click
    // the group column shall allow editting the group"). Asked for here: in
    // fillet-groups-and-preview-menu.py the view's own trigger opened no
    // editor on the cell (editable, the trigger on, nothing being edited),
    // while the same cell asked for directly did -- why, not found.
    if (treeWidget && o == treeWidget->viewport() && e->type() == QEvent::MouseButtonDblClick) {
        auto me = static_cast<QMouseEvent*>(e);
        const QModelIndex index = treeWidget->indexAt(me->position().toPoint());
        if (me->button() == Qt::LeftButton && index.isValid() && index.column() != 0
                && (index.flags() & Qt::ItemIsEditable)) {
            treeWidget->edit(index);
            return true;
        }
    }
    if(treeWidget && o == treeWidget) {
        switch(e->type()) {
        case QEvent::Leave:
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress: {
            if (handleEvent(e))
                return true;
        }
        default:
            break;
        }
    }
    // the base's: a panel box holding an undo step while it has the focus
    return TaskFeatureParameters::eventFilter(o,e);
}

void TaskDressUpParameters::heldStepBegun()
{
    failedAtStepStart.clear();
    errorAtStepStart.clear();
    auto dressUp = vp ? Base::freecad_dynamic_cast<PartDesign::DressUp>(vp->getObject()) : nullptr;
    if (!dressUp || !dressUp->isError())
        return;
    failedAtStepStart = dressUp->FailedEdges.getValues();
    if (const char *error = dressUp->getStatusString())
        errorAtStepStart = error;
}

std::vector<std::string> TaskDressUpParameters::failedSinceStepBegun(bool &whole) const
{
    whole = false;
    std::vector<std::string> fresh;
    auto dressUp = vp ? Base::freecad_dynamic_cast<PartDesign::DressUp>(vp->getObject()) : nullptr;
    if (!dressUp || !dressUp->isError())
        return fresh;
    const auto &failed = dressUp->FailedEdges.getValues();
    // Nothing named: the feature as a whole failed
    if (failed.empty()) {
        whole = true;
        return fresh;
    }
    for (const auto &edge : failed) {
        if (std::find(failedAtStepStart.begin(), failedAtStepStart.end(), edge)
                == failedAtStepStart.end())
            fresh.push_back(edge);
    }
    // The same edges, and another error: the result itself
    const char *error = dressUp->getStatusString();
    if (fresh.empty() && errorAtStepStart != (error ? error : ""))
        whole = true;
    return fresh;
}

//**************************************************************************
//**************************************************************************
// TaskDialog
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TaskDlgDressUpParameters::TaskDlgDressUpParameters(ViewProviderDressUp *DressUpView)
    : TaskDlgFeatureParameters(DressUpView)
    , parameter(nullptr)
{
    assert(DressUpView);
}

TaskDlgDressUpParameters::~TaskDlgDressUpParameters() = default;

//==== calls from the TaskView ===============================================================

bool TaskDlgDressUpParameters::accept()
{
    // OK's own undo step, the Base set below in it (entry 136)
    parameter->beginAcceptStep();
    std::vector<std::string> refs = parameter->getReferences();
    std::stringstream str;
    str << Gui::Command::getObjectCmd(vp->getObject()) << ".Base = ("
        << Gui::Command::getObjectCmd(parameter->getBase()) << ",[";
    for (const auto & ref : refs)
        str << "\"" << ref << "\",";
    str << "])";
    Gui::Command::runCommand(Gui::Command::Doc,str.str().c_str());
    return TaskDlgFeatureParameters::accept();
}

bool TaskDlgDressUpParameters::reject()
{
    // Its own steps, taken back by the base (docs/HandsOnQueue.md entry 232)
    if (!holdsTransaction())
        return TaskDlgFeatureParameters::reject();
    auto editDoc = Gui::Application::Instance->editDocumentOf(vp ? vp->getObject() : nullptr);
    if (editDoc) {
        App::Document* doc = editDoc->getDocument();
        // Down to the transaction open when the edit began; when that one
        // was left empty (and so dropped), to the first step the panel
        // opened (entry 136: a step per operation)
        bool undone = parameter->getTransactionID()
            && doc->undo(parameter->getTransactionID());
        if (!undone && parameter->getFirstStepID())
            doc->undo(parameter->getFirstStepID());
    }

    return TaskDlgFeatureParameters::reject();
}

#include "moc_TaskDressUpParameters.cpp"
