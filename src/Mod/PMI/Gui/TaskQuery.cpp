// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/BitmapFactory.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/PMI/App/Api.h>
#include <Mod/PMI/App/Callout.h>
#include "TaskQuery.h"

using namespace PMIGui;

TaskQuery::TaskQuery(QWidget* parent)
    : QWidget(parent)
    , Gui::SelectionObserver(true, Gui::ResolveMode::NoResolve)
{
    setObjectName(QStringLiteral("PMITaskQuery"));
    auto layout = new QVBoxLayout(this);
    _what = new QLabel(tr("Pick a face of an annotated part."), this);
    _what->setObjectName(QStringLiteral("what"));
    _what->setWordWrap(true);
    layout->addWidget(_what);
    _list = new QListWidget(this);
    _list->setObjectName(QStringLiteral("applies"));
    layout->addWidget(_list);
    connect(_list, &QListWidget::itemClicked, this, &TaskQuery::clicked);
    update();
}

void TaskQuery::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    if (msg.Type != Gui::SelectionChanges::AddSelection
        && msg.Type != Gui::SelectionChanges::SetSelection) {
        return;
    }
    if (!_pending) {
        _pending = true;
        QTimer::singleShot(0, this, [this]() { update(); });
    }
}

void TaskQuery::update()
{
    _pending = false;
    // The last face picked
    auto sel = Gui::Selection().getCompleteSelection(Gui::ResolveMode::NoResolve);
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) {
        if (show(it->pObject, it->SubName)) {
            return;
        }
    }
}

bool TaskQuery::show(App::DocumentObject* top, const char* subname)
{
    if (!top || !subname || !*subname) {
        return false;
    }
    App::DocumentObject* solid = nullptr;
    std::string face, why;
    if (!PMI::Api::solidReference(top, subname, solid, face, why)) {
        return false;
    }
    auto applies = PMI::Api::query(solid, face);
    _list->clear();
    _items.clear();
    _what->setText(tr("%1 of %2: %n specification(s)", nullptr, int(applies.size()))
                       .arg(QString::fromStdString(face),
                            QString::fromUtf8(solid->Label.getValue())));
    for (auto& a : applies) {
        QString how = a.how == "on" ? tr("on it")
                                    : tr("cites %1").arg(QString::fromStdString(a.how));
        QString text = QStringLiteral("%1  %2  (%3)")
                           .arg(QString::fromUtf8(a.callout->Label.getValue()),
                                QString::fromUtf8(a.callout->EquivalentText.getValue()),
                                how);
        auto item = new QListWidgetItem(text, _list);
        item->setData(Qt::UserRole, int(_items.size()));
        _items.emplace_back(a.callout);
    }
    return true;
}

void TaskQuery::clicked(QListWidgetItem* item)
{
    int i = item ? item->data(Qt::UserRole).toInt() : -1;
    if (i < 0 || i >= int(_items.size())) {
        return;
    }
    auto obj = _items[i].getObject();
    if (!obj) {
        return;
    }
    Gui::Selection().clearSelection();
    Gui::Selection().addSelection(obj->getDocument()->getName(), obj->getNameInDocument());
}

TaskDlgQuery::TaskDlgQuery()
{
    _widget = new TaskQuery;
    auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("PMI_Query"),
                                          tr("What applies to a face"), true, nullptr);
    box->groupLayout()->addWidget(_widget);
    Content.push_back(box);
}

#include "moc_TaskQuery.cpp"
