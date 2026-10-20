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
# include <set>
#endif

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <App/Application.h>
#include <App/Document.h>
#include <App/LibraryPaths.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>

#include "LibraryPathsGui.h"
#include "FileDialog.h"
#include "MainWindow.h"

using namespace Gui;
using App::LibraryPaths;

namespace
{

QString tr(const char* text)
{
    return QCoreApplication::translate("Gui::LibraryPaths", text);
}

QString q(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

/// The question is not asked while the Gui makes a change it applies itself.
bool quiet = false;
/// Nor again while it is asked.
bool asking = false;

/// Documents restored and not yet reported, by name.
std::vector<std::string>& opened()
{
    static std::vector<std::string> names;
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------

LibraryPathsEditor::LibraryPathsEditor(QWidget* parent)
    : QGroupBox(tr("Library paths"), parent)
{
    auto layout = new QVBoxLayout(this);
    auto info = new QLabel(tr("A link to a file under one of these folders is saved as the "
                              "library's name and the path under it, and finds the file where "
                              "the name points on each machine."),
                           this);
    info->setWordWrap(true);
    layout->addWidget(info);

    table = new QTableWidget(0, 3, this);
    table->setObjectName(QStringLiteral("libraryPathsTable"));
    table->setHorizontalHeaderLabels({tr("Name"), tr("Folder"), QString()});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(120);
    layout->addWidget(table);

    auto buttons = new QHBoxLayout;
    auto add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("libraryPathsAdd"));
    auto remove = new QPushButton(tr("Remove"), this);
    remove->setObjectName(QStringLiteral("libraryPathsRemove"));
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);

    QObject::connect(add, &QPushButton::clicked, this, [this]() {
        addRow(QString(), QString());
        table->editItem(table->item(table->rowCount() - 1, 0));
    });
    QObject::connect(remove, &QPushButton::clicked, this, [this]() {
        std::set<int> rows;
        for (auto item : table->selectedItems())
            rows.insert(item->row());
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            table->removeRow(*it);
    });
}

void LibraryPathsEditor::addRow(const QString& name, const QString& folder)
{
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(name));
    table->setItem(row, 1, new QTableWidgetItem(folder));
    auto button = new QPushButton(tr("Browse..."), table);
    QObject::connect(button, &QPushButton::clicked, this, [this, button]() {
        for (int i = 0; i < table->rowCount(); ++i) {
            if (table->cellWidget(i, 2) == button) {
                browse(i);
                return;
            }
        }
    });
    table->setCellWidget(row, 2, button);
}

void LibraryPathsEditor::browse(int row)
{
    auto item = table->item(row, 1);
    QString dir = FileDialog::getExistingDirectory(this, tr("Library folder"),
                                                   item ? item->text() : QString());
    if (!dir.isEmpty() && item)
        item->setText(dir);
}

void LibraryPathsEditor::load()
{
    table->setRowCount(0);
    for (auto& v : LibraryPaths::getSystem())
        addRow(q(v.first), q(v.second));
}

void LibraryPathsEditor::save()
{
    LibraryPaths::Map entered;
    QStringList refused;
    for (int i = 0; i < table->rowCount(); ++i) {
        auto nameItem = table->item(i, 0);
        auto folderItem = table->item(i, 1);
        const std::string name = nameItem ? nameItem->text().trimmed().toStdString() : std::string();
        const std::string folder = folderItem ? folderItem->text().trimmed().toStdString() : std::string();
        if (name.empty() && folder.empty())
            continue;
        if (!LibraryPaths::isValidName(name)) {
            refused << tr("'%1': a name is a letter, then letters, digits or '_'").arg(q(name));
            continue;
        }
        try {
            entered[name] = LibraryPaths::cleanFolder(folder);
        }
        catch (Base::Exception&) {
            refused << tr("'%1': '%2' is not an absolute path").arg(q(name), q(folder));
        }
    }

    const LibraryPaths::Map before = LibraryPaths::getSystem();
    for (auto& v : before) {
        if (!entered.count(v.first))
            LibraryPaths::removeSystem(v.first);
    }
    for (auto& v : entered) {
        auto it = before.find(v.first);
        if (it == before.end() || it->second != v.second)
            LibraryPaths::setSystem(v.first, v.second);
    }
    if (!refused.isEmpty()) {
        QMessageBox::warning(this, tr("Library paths"),
                             tr("These library paths were not saved:") + QStringLiteral("\n\n")
                                 + refused.join(QStringLiteral("\n")));
    }
    // The question, through the signal: asked as for a change from Python.
    LibraryPaths::changed(before);
    load();
}

// ---------------------------------------------------------------------------

void LibraryPathsGui::askToApply(QWidget* parent)
{
    if (asking)
        return;
    auto affected = LibraryPaths::affected();
    if (affected.empty())
        return;
    Base::FlagToggler<> guard(asking, false);

    QStringList lines;
    for (auto& v : affected) {
        lines << tr("%1: %2 link(s)")
                     .arg(q(v.document->Label.getStrValue()))
                     .arg(v.links);
    }
    QMessageBox box(QMessageBox::Question,
                    tr("Library paths"),
                    tr("The library paths now resolve links of open documents to other files:"),
                    QMessageBox::Yes | QMessageBox::No,
                    parent ? parent : getMainWindow());
    box.setObjectName(QStringLiteral("libraryPathsApply"));
    box.setInformativeText(lines.join(QStringLiteral("\n"))
                           + QStringLiteral("\n\n")
                           + tr("Apply the change to them now? Left, they take it when "
                                "opened again."));
    box.button(QMessageBox::Yes)->setText(tr("Apply now"));
    box.button(QMessageBox::No)->setText(tr("Leave them"));
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes)
        return;

    // By name: applying opens files, and a document may go meanwhile.
    std::vector<std::string> names;
    for (auto& v : affected)
        names.emplace_back(v.document->getName());
    for (auto& name : names) {
        auto doc = App::GetApplication().getDocument(name.c_str());
        if (!doc)
            continue;
        try {
            LibraryPaths::apply(*doc);
        }
        catch (Base::Exception& e) {
            e.reportException();
        }
    }
}

namespace
{

/// Set `name` in the settings to a folder the user picks, and apply it to
/// `docName`; the other documents it affects are asked about. Whether a
/// folder was picked.
bool setFromDialog(QWidget* parent, const std::string& docName, const std::string& name,
                   const std::string& hint)
{
    QString start = q(hint);
    if (!start.isEmpty() && !QFileInfo(start).exists())
        start.clear();
    QString dir = FileDialog::getExistingDirectory(
        parent, tr("Folder of library '%1'").arg(q(name)), start);
    if (dir.isEmpty())
        return false;
    const auto before = LibraryPaths::getSystem();
    try {
        LibraryPaths::setSystem(name, dir.toStdString());
        {
            Base::FlagToggler<> guard(quiet, false);
            LibraryPaths::changed(before);
        }
        if (auto doc = App::GetApplication().getDocument(docName.c_str()))
            LibraryPaths::apply(*doc);
    }
    catch (Base::Exception& e) {
        e.reportException();
        return false;
    }
    LibraryPathsGui::askToApply(parent);
    return true;
}

}  // namespace

void LibraryPathsGui::reportOpened(const std::vector<std::string>& docNames)
{
    struct Entry
    {
        std::string doc;
        QString label;
        LibraryPaths::Problem problem;
    };
    std::vector<Entry> entries;
    for (auto& name : docNames) {
        auto doc = App::GetApplication().getDocument(name.c_str());
        if (!doc)
            continue;
        for (auto& p : LibraryPaths::problems(*doc))
            entries.push_back({name, q(doc->Label.getStrValue()), std::move(p)});
    }
    if (entries.empty())
        return;

    auto dialog = new QDialog(getMainWindow());
    dialog->setObjectName(QStringLiteral("libraryPathsReport"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Library paths"));
    auto layout = new QVBoxLayout(dialog);
    for (auto& e : entries) {
        auto row = new QHBoxLayout;
        QString text;
        auto& p = e.problem;
        if (p.unresolved) {
            if (p.systemFolder.empty())
                text = tr("%1: library '%2' is not set here (the document had %3); "
                          "%4 link(s) not found.")
                           .arg(e.label, q(p.name),
                                p.documentFolder.empty() ? tr("none") : q(p.documentFolder))
                           .arg(p.links);
            else
                text = tr("%1: library '%2' (%3) does not hold what %4 link(s) name.")
                           .arg(e.label, q(p.name), q(p.systemFolder))
                           .arg(p.links);
        }
        else {
            text = tr("%1: library '%2': the document uses %3, the settings say %4.")
                       .arg(e.label, q(p.name), q(p.documentFolder), q(p.systemFolder));
        }
        auto label = new QLabel(text, dialog);
        label->setWordWrap(true);
        row->addWidget(label, 1);
        auto button = new QPushButton(
            p.unresolved ? tr("Set library path...") : tr("Use the settings' folder"), dialog);
        button->setObjectName(p.unresolved ? QStringLiteral("libraryPathsSet")
                                           : QStringLiteral("libraryPathsUseSettings"));
        button->setProperty("libraryName", q(p.name));
        button->setProperty("documentName", q(e.doc));
        const std::string docName = e.doc;
        const std::string libName = p.name;
        const std::string hint = p.documentFolder;
        const bool unresolved = p.unresolved;
        QPointer<QPushButton> self(button);
        QObject::connect(button, &QPushButton::clicked, dialog,
                         [dialog, self, docName, libName, hint, unresolved]() {
            bool done = false;
            if (unresolved)
                done = setFromDialog(dialog, docName, libName, hint);
            else if (auto doc = App::GetApplication().getDocument(docName.c_str())) {
                try {
                    LibraryPaths::useSystemFolder(*doc, libName);
                    done = true;
                }
                catch (Base::Exception& e) {
                    e.reportException();
                }
            }
            if (done && self)
                self->setEnabled(false);
        });
        row->addWidget(button);
        layout->addLayout(row);
    }
    auto box = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(box);
    dialog->show();
}

void LibraryPathsGui::init()
{
    static bool done;
    if (done)
        return;
    done = true;
    // Never disconnected: they live as long as the application.
    LibraryPaths::signalChanged().connect(
        [](const LibraryPaths::Map&, const LibraryPaths::Map&) {
            if (!quiet)
                askToApply();
        });
    App::GetApplication().signalFinishRestoreDocument.connect([](const App::Document& doc) {
        opened().emplace_back(doc.getName());
    });
    App::GetApplication().signalFinishOpenDocument.connect([]() {
        if (opened().empty())
            return;
        // Once the documents' views are up.
        QTimer::singleShot(0, []() {
            std::vector<std::string> names;
            names.swap(opened());
            reportOpened(names);
        });
    });
}
