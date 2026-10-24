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
# include <QCheckBox>
# include <QComboBox>
# include <QDialog>
# include <QDialogButtonBox>
# include <QHeaderView>
# include <QLabel>
# include <QPushButton>
# include <QTreeWidget>
# include <QVBoxLayout>
#endif

#include <set>

#include <App/Application.h>
#include <App/DocumentObject.h>

#include "DlgImportClash.h"
#include "DlgObjectSelection.h"
#include "MainWindow.h"
#include "MergeDocuments.h"

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

bool Gui::askImportClashes(std::vector<App::Document::ImportClash>& clashes, bool* copyLook)
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
    // Sec 35.18: what is assigned takes the look brought too, unless unchecked
    auto look = new QCheckBox(QObject::tr("Assigned objects take the look brought too"), &dlg);
    look->setObjectName(QStringLiteral("copyLook"));
    look->setChecked(true);
    layout->addWidget(look);
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
    if (copyLook)
        *copyLook = look->isChecked();
    return true;
}

bool Gui::askPaste(App::Document* doc, const std::vector<App::DocumentObject*>& brought,
                   std::map<std::string, App::Document::ImportTake>& takes, bool* copyLook)
{
    takes.clear();
    // Paired by UID and type (sec 36.4); a copy beside its source in its
    // own document is no clash
    std::map<App::DocumentObject*, App::DocumentObject*> pairs;
    for (auto obj : brought) {
        if (!obj || !obj->isAttachedToDocument() || obj->getDocument() == doc
                || obj->getObjectUid().empty())
            continue;
        auto live = doc->getObjectByUid(obj->getObjectUid());
        if (live && live->getTypeId() == obj->getTypeId())
            pairs.emplace(obj, live);
    }
    if (pairs.empty())
        return true;
    // Listed from what nothing brought depends on, as a copy lists them
    const std::set<App::DocumentObject*> all(brought.begin(), brought.end());
    std::vector<App::DocumentObject*> roots;
    for (auto obj : brought) {
        if (!obj || !obj->isAttachedToDocument())
            continue;
        bool root = true;
        for (auto in : obj->getInList())
            root = root && !all.count(in);
        if (root)
            roots.push_back(obj);
    }
    DlgObjectSelection dlg(roots, getMainWindow());
    dlg.setObjectName(QStringLiteral("DlgPasteSelection"));
    dlg.setWindowTitle(QObject::tr("Paste: objects already here"));
    dlg.setPaste(doc, pairs);
    // Sec 35.18: what is assigned takes the look brought too, unless unchecked
    auto look = new QCheckBox(QObject::tr("Assigned objects take the look brought too"));
    look->setObjectName(QStringLiteral("copyLook"));
    look->setChecked(true);
    dlg.addCheckBox(look);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (copyLook)
        *copyLook = look->isChecked();
    for (const auto& [obj, take] : dlg.getTakes()) {
        if (obj->isAttachedToDocument())
            takes[obj->getNameInDocument()] = take;
    }
    return true;
}

bool Gui::pasteAsked(App::Document* doc,
                     const std::function<std::unique_ptr<std::istream>()>& open,
                     const char* transaction,
                     std::vector<App::DocumentObject*>& imported)
{
    // Answered before the step opens (37.7 item 13): a served client's
    // undo meanwhile has no step to close, and a client's write is a step
    // of its own, not the paste's. One a scan missed is duplicated, as
    // nothing answered it.
    App::Document::ImportClashScope scope(App::Document::ImportClashScope::Answered);
    std::string from;
    std::vector<std::string> uids;
    if (auto in = open())
        uids = MergeDocuments::scanUids(*in, from);
    bool clash = false;
    if (from != doc->TransientDir.getStrValue()) {
        for (const auto& uid : uids)
            clash = clash || doc->getObjectByUid(uid) != nullptr;
    }
    if (clash) {
        // The stream read once more, into a scratch document, for the
        // dialog to show what it brings and what depends on what
        auto& app = App::GetApplication();
        const std::string name = app.getUniqueDocumentName("PasteLook");
        App::Document* scratch = app.newDocument(name.c_str(), name.c_str(), false, true, false);
        struct Closer
        {
            std::string name;
            ~Closer()
            {
                App::GetApplication().closeDocument(name.c_str());
            }
        } closer {scratch->getName()};
        std::vector<App::DocumentObject*> objs;
        std::map<std::string, std::string> streamName;   // scratch name -> the stream's
        {
            App::Document::ImportClashScope duplicate(App::Document::ImportClashScope::Duplicate);
            MergeDocuments look(scratch);
            auto in = open();
            if (!in)
                return false;
            objs = look.importObjects(*in);
            for (const auto& kv : look.names())
                streamName[kv.second] = kv.first;
        }
        std::map<std::string, App::Document::ImportTake> takes;
        bool copyLook = true;
        if (!askPaste(doc, objs, takes, &copyLook))
            return false;
        scope.copyLook = copyLook;
        for (const auto& [name, take] : takes) {
            auto it = streamName.find(name);
            scope.answers[it != streamName.end() ? it->second : name] = take;
        }
    }
    auto in = open();
    if (!in)
        return false;
    doc->openTransaction(transaction);
    MergeDocuments merge(doc);
    imported = merge.importObjects(*in);
    return true;
}

ImportClashAsk::ImportClashAsk()
    : App::Document::ImportClashScope(Ask)
{
    ask = [this](std::vector<App::Document::ImportClash>& clashes) {
        bool look = true;
        const bool ok = askImportClashes(clashes, &look);
        copyLook = look;
        _all = ok && !clashes.empty();
        for (const auto& c : clashes)
            _all = _all && c.assign;
        return ok;
    };
}
