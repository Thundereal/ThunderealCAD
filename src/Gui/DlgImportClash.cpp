/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
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
# include <QComboBox>
# include <QDialog>
# include <QDialogButtonBox>
# include <QHeaderView>
# include <QLabel>
# include <QPushButton>
# include <QTreeWidget>
# include <QVBoxLayout>
#endif

#include <App/DocumentObject.h>

#include "DlgImportClash.h"
#include "MainWindow.h"

using namespace Gui;

namespace
{
QString objectText(const App::DocumentObject* obj)
{
    const QString label = QString::fromUtf8(obj->Label.getValue());
    const QString name = QString::fromUtf8(obj->getNameInDocument());
    return label == name ? label : QStringLiteral("%1 (%2)").arg(label, name);
}
}  // namespace

bool Gui::askImportClashes(std::vector<App::Document::ImportClash>& clashes)
{
    if (clashes.empty())
        return true;
    QDialog dlg(getMainWindow());
    dlg.setObjectName(QStringLiteral("DlgImportClash"));
    dlg.setWindowTitle(QObject::tr("Objects already here"));
    auto layout = new QVBoxLayout(&dlg);
    auto intro = new QLabel(
        QObject::tr("These objects are already in %1. Assign: the object here takes the state "
                    "brought, and what links to it still does. Duplicate: a new object beside it. "
                    "The objects brought refer to each other as chosen: one assigned refers to a "
                    "duplicated one by its copy.")
            .arg(QString::fromUtf8(clashes.front().live->getDocument()->Label.getValue())),
        &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto list = new QTreeWidget(&dlg);
    list->setObjectName(QStringLiteral("clashes"));
    list->setRootIsDecorated(false);
    list->setHeaderLabels({QObject::tr("Brought"), QObject::tr("Here"), QObject::tr("Take")});
    std::vector<QComboBox*> takes;
    for (std::size_t i = 0; i < clashes.size(); ++i) {
        const auto& c = clashes[i];
        auto item = new QTreeWidgetItem(list, {objectText(c.brought), objectText(c.live)});
        auto take = new QComboBox(list);
        take->setObjectName(QStringLiteral("take%1").arg(i));
        take->addItem(QObject::tr("Duplicate"));
        take->addItem(QObject::tr("Assign"));
        take->setCurrentIndex(c.assign ? 1 : 0);
        list->setItemWidget(item, 2, take);
        takes.push_back(take);
    }
    list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(list);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    auto assignAll = buttons->addButton(QObject::tr("Assign all"), QDialogButtonBox::ActionRole);
    assignAll->setObjectName(QStringLiteral("assignAll"));
    auto duplicateAll =
        buttons->addButton(QObject::tr("Duplicate all"), QDialogButtonBox::ActionRole);
    duplicateAll->setObjectName(QStringLiteral("duplicateAll"));
    layout->addWidget(buttons);
    auto all = [&](int index) {
        for (auto take : takes)
            take->setCurrentIndex(index);
        dlg.accept();
    };
    QObject::connect(assignAll, &QPushButton::clicked, &dlg, [&]() { all(1); });
    QObject::connect(duplicateAll, &QPushButton::clicked, &dlg, [&]() { all(0); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(640, 320);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    for (std::size_t i = 0; i < clashes.size(); ++i)
        clashes[i].assign = takes[i]->currentIndex() == 1;
    return true;
}

ImportClashAsk::ImportClashAsk()
    : App::Document::ImportClashScope(Ask)
{
    ask = [this](std::vector<App::Document::ImportClash>& clashes) {
        const bool ok = askImportClashes(clashes);
        _all = ok && !clashes.empty();
        for (const auto& c : clashes)
            _all = _all && c.assign;
        return ok;
    };
}
