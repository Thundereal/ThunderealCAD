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
# include <QComboBox>
# include <QDialogButtonBox>
# include <QDir>
# include <QFileInfo>
# include <QHBoxLayout>
# include <QHeaderView>
# include <QLabel>
# include <QLineEdit>
# include <QMessageBox>
# include <QPushButton>
# include <QTreeWidget>
# include <QVBoxLayout>
#endif

#include <App/Application.h>
#include <App/Document.h>
#include <App/ImportSet.h>
#include <App/PropertyLinks.h>
#include <Base/Console.h>
#include <Base/Exception.h>

#include "DlgImportLinkedSet.h"
#include "FileDialog.h"
#include "MainWindow.h"

FC_LOG_LEVEL_INIT("Gui", true, true)

using namespace Gui;

namespace
{

enum Column
{
    TheirFile,
    Ours,
    PairedBy,
    Action,
    WriteTo,
    State,
};

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

std::string ss(const QString& s)
{
    return s.toUtf8().constData();
}

}  // namespace

DlgImportLinkedSet::DlgImportLinkedSet(App::Document* top, QWidget* parent)
    : QDialog(parent)
    , _docName(top->getName())
{
    setObjectName(QStringLiteral("DlgImportLinkedSet"));
    setWindowTitle(tr("Import a Linked Set"));
    auto layout = new QVBoxLayout(this);
    auto intro = new QLabel(
        tr("Another copy of this document's set of linked files, brought in as branches of "
           "ours and merged file by file. Choose the copy's top file, check how its files "
           "pair with ours, import, then merge."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Their top:"), this));
    _theirs = new QLineEdit(this);
    _theirs->setObjectName(QStringLiteral("theirs"));
    row->addWidget(_theirs, 1);
    auto browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &DlgImportLinkedSet::browse);
    row->addWidget(browseButton);
    _read = new QPushButton(tr("Read"), this);
    _read->setObjectName(QStringLiteral("read"));
    connect(_read, &QPushButton::clicked, this, &DlgImportLinkedSet::readSet);
    row->addWidget(_read);
    layout->addLayout(row);

    _files = new QTreeWidget(this);
    _files->setObjectName(QStringLiteral("files"));
    _files->setRootIsDecorated(false);
    _files->setHeaderLabels(
        {tr("Their file"), tr("Ours"), tr("Paired by"), tr("Action"), tr("Written to"), tr("State")});
    layout->addWidget(_files);

    _state = new QLabel(this);
    _state->setObjectName(QStringLiteral("state"));
    _state->setWordWrap(true);
    layout->addWidget(_state);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    _import = buttons->addButton(tr("Import"), QDialogButtonBox::ActionRole);
    _import->setObjectName(QStringLiteral("import"));
    _import->setEnabled(false);
    connect(_import, &QPushButton::clicked, this, &DlgImportLinkedSet::bringIn);
    _merge = buttons->addButton(tr("Merge"), QDialogButtonBox::ActionRole);
    _merge->setObjectName(QStringLiteral("merge"));
    _merge->setEnabled(false);
    connect(_merge, &QPushButton::clicked, this, &DlgImportLinkedSet::merge);
    connect(buttons, &QDialogButtonBox::rejected, this, &DlgImportLinkedSet::reject);
    layout->addWidget(buttons);

    // Ours: the documents the top's links reach, a file of theirs may be
    // paired with -- the top first.
    std::vector<App::Document*> queue {top};
    std::set<App::Document*> seen {top};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        App::Document* doc = queue[i];
        if (!doc->testStatus(App::Document::VersionDoc) && !doc->FileName.getStrValue().empty())
            _ours.emplace_back(doc->getName(),
                               QFileInfo(qs(doc->FileName.getStrValue())).fileName());
        auto outList = App::PropertyXLink::getDocumentOutList(doc);
        for (auto linked : outList[doc]) {
            if (linked && seen.insert(linked).second)
                queue.push_back(linked);
        }
    }
    resize(820, 420);
}

App::Document* DlgImportLinkedSet::top() const
{
    return App::GetApplication().getDocument(_docName.c_str());
}

void DlgImportLinkedSet::browse()
{
    const QString file = FileDialog::getOpenFileName(
        this, tr("Their top file"), _theirs->text(),
        QStringLiteral("%1 (*.FCStd *.TStd)").arg(tr("FreeCAD document")));
    if (file.isEmpty())
        return;
    _theirs->setText(QDir::toNativeSeparators(file));
    readSet();
}

void DlgImportLinkedSet::fillActions(QTreeWidgetItem* item, bool paired, bool certain)
{
    auto combo = qobject_cast<QComboBox*>(_files->itemWidget(item, Action));
    if (!combo)
        return;
    combo->clear();
    if (paired) {
        combo->addItem(tr("Merge"), int(App::SetMergeAction::Merge));
        combo->addItem(tr("Take theirs"), int(App::SetMergeAction::TakeTheirs));
        combo->addItem(tr("Keep ours"), int(App::SetMergeAction::KeepOurs));
        // Q8: a guess is taken whole rather than merged.
        combo->setCurrentIndex(certain ? 0 : 1);
    }
    else {
        combo->addItem(tr("Write"), int(App::SetMergeAction::Write));
        combo->addItem(tr("Leave out"), int(App::SetMergeAction::Drop));
    }
    if (auto edit = qobject_cast<QLineEdit*>(_files->itemWidget(item, WriteTo)))
        edit->setEnabled(!paired);
}

void DlgImportLinkedSet::readSet()
{
    _files->clear();
    _import->setEnabled(false);
    _merge->setEnabled(false);
    _imported = {};
    auto doc = top();
    const QString theirs = _theirs->text().trimmed();
    if (!doc || theirs.isEmpty())
        return;
    App::SetImportPlan plan;
    try {
        plan = App::planSetImport(*doc, ss(QDir::fromNativeSeparators(theirs)));
    }
    catch (Base::Exception& e) {
        _state->setText(qs(e.what()));
        return;
    }
    const QDir ourDir = QFileInfo(qs(doc->FileName.getStrValue())).absoluteDir();
    int row = 0;
    for (const auto& f : plan.files) {
        auto item = new QTreeWidgetItem(_files);
        item->setText(TheirFile, qs(f.relative));
        item->setToolTip(TheirFile, qs(f.theirs));
        item->setData(TheirFile, Qt::UserRole, qs(f.theirs));
        auto ours = new QComboBox(_files);
        ours->setObjectName(QStringLiteral("ours%1").arg(row));
        ours->addItem(tr("(none: theirs alone)"), QString());
        for (const auto& [name, label] : _ours) {
            ours->addItem(label, qs(name));
            if (f.ours && name == f.ours->getName())
                ours->setCurrentIndex(ours->count() - 1);
        }
        // What the reading paired it with: a row left so is not paired by hand.
        item->setData(Ours, Qt::UserRole, ours->currentData());
        _files->setItemWidget(item, Ours, ours);
        QString how = qs(App::setPairingName(f.pairing));
        if (f.ambiguous)
            how = tr("two of ours alike: say which");
        else if (f.ours && !f.certain())
            how = tr("%1 (a guess)").arg(how);
        item->setText(PairedBy, how);
        auto action = new QComboBox(_files);
        action->setObjectName(QStringLiteral("action%1").arg(row));
        _files->setItemWidget(item, Action, action);
        auto writeTo = new QLineEdit(_files);
        writeTo->setObjectName(QStringLiteral("writeTo%1").arg(row));
        writeTo->setText(QDir::toNativeSeparators(
            QDir::cleanPath(ourDir.absoluteFilePath(qs(f.relative)))));
        _files->setItemWidget(item, WriteTo, writeTo);
        fillActions(item, f.ours != nullptr, f.certain());
        // Paired by hand: certain, so merged by default.
        connect(ours, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, item, ours](int) {
                    fillActions(item, !ours->currentData().toString().isEmpty(), true);
                });
        ++row;
    }
    _files->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    QString text = tr("%n file(s) in their set.", "", int(plan.files.size()));
    if (!plan.shared.empty())
        text += QLatin1Char(' ') + tr("%n file(s) both use, left as they are.", "",
                                      int(plan.shared.size()));
    if (!plan.missing.empty())
        text += QLatin1Char(' ') + tr("%n linked file(s) not there.", "",
                                      int(plan.missing.size()));
    _state->setText(text);
    _import->setEnabled(!plan.files.empty());
}

void DlgImportLinkedSet::bringIn()
{
    auto doc = top();
    if (!doc)
        return;
    try {
        // Read again: what is open may have changed since.
        auto plan = App::planSetImport(*doc, ss(QDir::fromNativeSeparators(_theirs->text().trimmed())));
        for (int i = 0; i < _files->topLevelItemCount(); ++i) {
            auto item = _files->topLevelItem(i);
            auto ours = qobject_cast<QComboBox*>(_files->itemWidget(item, Ours));
            if (ours->currentData() == item->data(Ours, Qt::UserRole))
                continue;
            const std::string name = ss(ours->currentData().toString());
            App::Document* pick = name.empty() ? nullptr
                : App::GetApplication().getDocument(name.c_str());
            App::pairByHand(plan, ss(item->data(TheirFile, Qt::UserRole).toString()), pick);
        }
        _imported = App::importSet(plan);
    }
    catch (Base::Exception& e) {
        QMessageBox::critical(this, windowTitle(), qs(e.what()));
        return;
    }
    showImported();
}

void DlgImportLinkedSet::resume(const App::PendingSet& set)
{
    _files->clear();
    _imported = set.imported;
    _theirs->setText(QDir::toNativeSeparators(qs(set.imported.theirTop)));
    int row = 0;
    for (const auto& f : set.imported.files) {
        auto item = new QTreeWidgetItem(_files);
        item->setText(TheirFile, qs(f.relative.empty() ? f.theirs : f.relative));
        item->setToolTip(TheirFile, qs(f.theirs));
        item->setData(TheirFile, Qt::UserRole, qs(f.theirs));
        auto ours = new QComboBox(_files);
        ours->setObjectName(QStringLiteral("ours%1").arg(row));
        ours->addItem(f.ourFile.empty() ? tr("(none: theirs alone)")
                                        : QFileInfo(qs(f.ourFile)).fileName(),
                      qs(f.ourFile));
        _files->setItemWidget(item, Ours, ours);
        item->setText(PairedBy, qs(App::setPairingName(f.pairing)));
        auto action = new QComboBox(_files);
        action->setObjectName(QStringLiteral("action%1").arg(row));
        _files->setItemWidget(item, Action, action);
        auto writeTo = new QLineEdit(_files);
        writeTo->setObjectName(QStringLiteral("writeTo%1").arg(row));
        writeTo->setText(QDir::toNativeSeparators(qs(f.writeTo)));
        _files->setItemWidget(item, WriteTo, writeTo);
        fillActions(item, !f.ourFile.empty(), true);
        const int at = action->findData(int(f.action));
        if (at >= 0)
            action->setCurrentIndex(at);
        ++row;
    }
    _files->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    showImported();
}

void DlgImportLinkedSet::showImported()
{
    for (int i = 0; i < _files->topLevelItemCount(); ++i) {
        auto item = _files->topLevelItem(i);
        if (auto ours = _files->itemWidget(item, Ours))
            ours->setEnabled(false);
        const std::string theirs = ss(item->data(TheirFile, Qt::UserRole).toString());
        for (const auto& r : _imported.files) {
            if (r.theirs != theirs)
                continue;
            if (!r.error.empty())
                item->setText(State, qs(r.error));
            else if (r.linksOnly) {
                // Nothing of theirs but its links (sec 35.15 G1): ours kept
                item->setText(State, tr("branch %1, its links only").arg(qs(r.branch)));
                auto combo = qobject_cast<QComboBox*>(_files->itemWidget(item, Action));
                const int at = combo ? combo->findData(int(r.action)) : -1;
                if (at >= 0)
                    combo->setCurrentIndex(at);
            }
            else if (!r.branch.empty())
                item->setText(State, tr("branch %1").arg(qs(r.branch)));
            else if (r.held)
                item->setText(State, tr("held"));
            else if (!r.ourFile.empty())
                item->setText(State, tr("nothing new"));
        }
    }
    _read->setEnabled(false);
    _theirs->setEnabled(false);
    _import->setEnabled(false);
    _merge->setEnabled(true);
    _state->setText(tr("Imported as branches of ours. Merge to take them in, as one undo step."));
}

QTreeWidgetItem* DlgImportLinkedSet::lineOf(const std::string& theirs) const
{
    for (int i = 0; i < _files->topLevelItemCount(); ++i) {
        auto item = _files->topLevelItem(i);
        if (ss(item->data(TheirFile, Qt::UserRole).toString()) == theirs)
            return item;
    }
    return nullptr;
}

std::vector<App::SetMergeFile> DlgImportLinkedSet::listed() const
{
    std::vector<App::SetMergeFile> files;
    for (const auto& r : _imported.files) {
        App::SetMergeFile f;
        f.theirs = r.theirs;
        f.ourFile = r.ourFile;
        f.branch = r.branch;
        f.held = r.held;
        f.action = r.action;
        if (auto item = lineOf(r.theirs)) {
            if (auto action = qobject_cast<QComboBox*>(_files->itemWidget(item, Action)))
                f.action = App::SetMergeAction(action->currentData().toInt());
            if (auto writeTo = qobject_cast<QLineEdit*>(_files->itemWidget(item, WriteTo)))
                f.writeTo = ss(QDir::fromNativeSeparators(writeTo->text().trimmed()));
        }
        files.push_back(std::move(f));
    }
    return files;
}

void DlgImportLinkedSet::merge()
{
    auto doc = top();
    if (!doc)
        return;
    std::vector<App::SetMergeFile> files = listed();

    // Q4 (b), sec 35.16 G5: what will not resolve after the merge, said
    // first, on each file's line; the files kept ours that it needs offered
    // to be merged too.
    std::vector<std::vector<std::string>> refs;
    try {
        refs = App::previewSetReferences(*doc, files);
    }
    catch (Base::Exception& e) {
        FC_WARN("set merge: references not checked: " << e.what());
    }
    QStringList lines;
    std::set<std::string> needed;   // their files kept ours that a reference needs
    for (std::size_t i = 0; i < refs.size() && i < files.size(); ++i) {
        auto item = lineOf(files[i].theirs);
        if (!item)
            continue;
        QStringList mine;
        for (const auto& ref : refs[i]) {
            mine << qs(ref);
            // "Owner.Prop -> File.FCStd#Object[.Element]": the file's name
            const auto arrow = ref.find(" -> ");
            const auto hash = ref.find('#', arrow == std::string::npos ? 0 : arrow);
            if (arrow == std::string::npos || hash == std::string::npos)
                continue;
            const QString name = qs(ref.substr(arrow + 4, hash - arrow - 4));
            for (const auto& f : files) {
                if (f.action == App::SetMergeAction::KeepOurs && !f.branch.empty()
                        && QFileInfo(qs(f.ourFile)).fileName() == name)
                    needed.insert(f.theirs);
            }
        }
        item->setToolTip(State, mine.join(QLatin1Char('\n')));
        if (!mine.isEmpty())
            item->setText(State, tr("%n reference(s) will not resolve", nullptr, mine.size()));
        lines << mine;
    }
    if (!lines.isEmpty()) {
        QStringList shown = lines.mid(0, 10);
        if (lines.size() > shown.size())
            shown << tr("... and %n more", nullptr, int(lines.size() - shown.size()));
        QMessageBox box(QMessageBox::Warning, windowTitle(),
                        tr("%n reference(s) will not resolve after the merge:", nullptr,
                           lines.size())
                            + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n')),
                        QMessageBox::Cancel, this);
        QPushButton* too = nullptr;
        if (!needed.empty()) {
            QStringList names;
            for (const auto& theirs : needed)
                names << QFileInfo(qs(theirs)).fileName();
            too = box.addButton(tr("Merge %1 too").arg(names.join(QStringLiteral(", "))),
                                QMessageBox::AcceptRole);
        }
        auto asListed = box.addButton(tr("Merge as listed"), QMessageBox::AcceptRole);
        box.setDefaultButton(too ? too : asListed);
        box.exec();
        if (box.clickedButton() == too && too) {
            for (const auto& theirs : needed) {
                auto item = lineOf(theirs);
                auto action = item
                    ? qobject_cast<QComboBox*>(_files->itemWidget(item, Action)) : nullptr;
                const int at =
                    action ? action->findData(int(App::SetMergeAction::Merge)) : -1;
                if (at >= 0)
                    action->setCurrentIndex(at);
            }
            merge();   // checked again, with them merged
            return;
        }
        if (box.clickedButton() != asListed)
            return;
    }

    App::SetMergeOptions options;
    options.name = ss(tr("Merge their set"));
    options.set = _imported.id;
    App::SetMergeResult result;
    try {
        result = App::mergeSet(*doc, files, options);
        bool there = false;
        for (const auto& f : result.files)
            there = there || f.error.find("is there") != std::string::npos;
        if (result.refused && there
                && QMessageBox::question(this, windowTitle(),
                                         tr("A file of theirs would be written where a file is "
                                            "already. Write over it?"))
                       == QMessageBox::Yes) {
            options.overwrite = true;
            result = App::mergeSet(*doc, files, options);
        }
    }
    catch (Base::Exception& e) {
        QMessageBox::critical(this, windowTitle(), qs(e.what()));
        return;
    }
    QStringList errors;
    for (const auto& f : result.files) {
        if (!f.error.empty())
            errors << QStringLiteral("%1: %2").arg(QFileInfo(qs(f.theirs)).fileName(), qs(f.error));
        // Q4 (b): in the report after, too
        for (const auto& ref : f.references)
            errors << tr("%1: does not resolve: %2")
                          .arg(QFileInfo(qs(f.theirs)).fileName(), qs(ref));
    }
    if (result.refused) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Nothing was merged:\n%1").arg(errors.join(QLatin1Char('\n'))));
        return;
    }
    if (!errors.isEmpty())
        QMessageBox::warning(this, windowTitle(),
                             tr("Merged, but:\n%1").arg(errors.join(QLatin1Char('\n'))));
    getMainWindow()->showMessage(tr("Their set merged"));
    accept();
}

#include "moc_DlgImportLinkedSet.cpp"
