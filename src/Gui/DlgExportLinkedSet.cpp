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
# include <algorithm>
# include <QCheckBox>
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
#include <App/ExportSet.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>

#include "DlgExportLinkedSet.h"
#include "Command.h"
#include "FileDialog.h"
#include "MainWindow.h"

FC_LOG_LEVEL_INIT("Gui", true, true)

using namespace Gui;

DlgExportLinkedSet::DlgExportLinkedSet(App::Document* top, QWidget* parent)
    : QDialog(parent)
    , _docName(top->getName())
{
    setObjectName(QStringLiteral("DlgExportLinkedSet"));
    setWindowTitle(tr("Export with Linked Files"));
    auto layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("Every file the document links to is written into the folder, with the links "
           "between them. A pinned version or a branch becomes a file of its own, so the "
           "set opens in any FreeCAD."),
        this));
    qobject_cast<QLabel*>(layout->itemAt(0)->widget())->setWordWrap(true);

    _files = new QTreeWidget(this);
    _files->setObjectName(QStringLiteral("files"));
    _files->setRootIsDecorated(false);
    _files->setHeaderLabels({tr("Document"), tr("Is"), tr("Written as")});
    layout->addWidget(_files);

    auto row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Folder:"), this));
    _folder = new QLineEdit(this);
    _folder->setObjectName(QStringLiteral("folder"));
    row->addWidget(_folder, 1);
    auto browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &DlgExportLinkedSet::browse);
    row->addWidget(browseButton);
    layout->addLayout(row);

    _history = new QCheckBox(tr("With history"), this);
    _history->setObjectName(QStringLiteral("history"));
    _history->setToolTip(tr("Each file keeps its history, for someone who also uses it. "
                            "Without, a file brought back pairs with this one by its save."));
    layout->addWidget(_history);

    _libraries = new QCheckBox(tr("Leave library files in their library"), this);
    _libraries->setObjectName(QStringLiteral("libraries"));
    _libraries->setToolTip(tr("A file in a library, that only links into the library reach, is "
                              "not written: the links to it keep the library, for someone with "
                              "the same library. Without, it is written under _external."));
    connect(_libraries, &QCheckBox::toggled, this, &DlgExportLinkedSet::fill);
    layout->addWidget(_libraries);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &DlgExportLinkedSet::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &DlgExportLinkedSet::reject);
    layout->addWidget(buttons);
    _export = buttons->button(QDialogButtonBox::Ok);

    // The option only when library links reach a file it would leave.
    try {
        App::ExportSetOptions options;
        options.keepLibraries = true;
        const auto plan = App::planLinkedSet(*top, options);
        _libraries->setVisible(std::any_of(plan.files.begin(), plan.files.end(),
                                           [](const auto& f) { return f.path.empty(); }));
    }
    catch (Base::Exception&) {
        _libraries->setVisible(false);
    }
    fill();

    // Beside the document, named after it.
    const QFileInfo file(QString::fromUtf8(top->FileName.getValue()));
    if (!file.fileName().isEmpty())
        _folder->setText(QDir::toNativeSeparators(
            file.absoluteDir().absoluteFilePath(file.completeBaseName() + tr("-set"))));
    resize(640, 400);
}

void DlgExportLinkedSet::fill()
{
    _files->clear();
    _written = 0;
    _export->setEnabled(true);
    auto top = App::GetApplication().getDocument(_docName.c_str());
    if (!top) {
        _export->setEnabled(false);
        return;
    }
    // What the export will write, from the plan it writes by.
    try {
        App::ExportSetOptions options;
        options.keepLibraries = _libraries->isChecked();
        for (const auto& f : App::planLinkedSet(*top, options).files) {
            QString is;
            if (!f.branch.empty() && f.version > 0)
                is = tr("branch %1 at version %2")
                         .arg(QString::fromUtf8(f.branch.c_str()))
                         .arg(f.version);
            else if (!f.branch.empty())
                is = tr("branch %1").arg(QString::fromUtf8(f.branch.c_str()));
            else if (f.version > 0)
                is = tr("pinned version %1").arg(f.version);
            else
                is = tr("the file");
            auto item = new QTreeWidgetItem(_files);
            item->setText(0, f.document ? QString::fromUtf8(f.document->Label.getValue())
                                        : QString());
            item->setToolTip(0, QString::fromUtf8(f.file.c_str()));
            item->setText(1, is);
            if (f.path.empty())
                item->setText(2, tr("left in library %1")
                                     .arg(QString::fromUtf8(f.library.c_str())));
            else {
                item->setText(2, QString::fromUtf8(f.path.c_str()));
                ++_written;
            }
        }
    }
    catch (Base::Exception& e) {
        auto item = new QTreeWidgetItem(_files);
        item->setText(0, QString::fromUtf8(e.what()));
        _export->setEnabled(false);
    }
    _files->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

void DlgExportLinkedSet::browse()
{
    const QString dir = FileDialog::getExistingDirectory(this, tr("Export with Linked Files"),
                                                         _folder->text());
    if (!dir.isEmpty())
        _folder->setText(QDir::toNativeSeparators(dir));
}

void DlgExportLinkedSet::accept()
{
    const QString folder = _folder->text().trimmed();
    if (folder.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose the folder to write into."));
        return;
    }
    const QDir dir(folder);
    bool overwrite = false;
    if (dir.exists()
            && !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
        if (QMessageBox::question(this, windowTitle(),
                                  tr("The folder is not empty. Write the files into it anyway, "
                                     "over any of the same name?"))
                != QMessageBox::Yes)
            return;
        overwrite = true;
    }
    const std::string path = QDir::fromNativeSeparators(folder).toUtf8().constData();
    try {
        Command::doCommand(Command::Doc,
                           "App.getDocument('%s').exportLinkedSet(u'%s', withHistory=%s, "
                           "overwrite=%s, keepLibraries=%s)",
                           _docName.c_str(), Base::Tools::escapeEncodeString(path).c_str(),
                           _history->isChecked() ? "True" : "False",
                           overwrite ? "True" : "False",
                           _libraries->isChecked() ? "True" : "False");
    }
    catch (Base::Exception& e) {
        QMessageBox::critical(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    getMainWindow()->showMessage(tr("%1 files written to %2").arg(_written)
                                     .arg(folder));
    QDialog::accept();
}

#include "moc_DlgExportLinkedSet.cpp"
