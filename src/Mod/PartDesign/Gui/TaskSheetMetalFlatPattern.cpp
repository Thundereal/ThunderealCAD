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

#ifndef _PreComp_
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#endif

#include <App/Document.h>
#include <App/PropertyUnits.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>

#include "TaskSheetMetalFlatPattern.h"
#include "TaskSheetMetalParameters.h"
#include "ViewProviderSheetMetal.h"

using namespace PartDesignGui;

namespace
{

/// Faces of the folded part only
class FaceGate: public Gui::SelectionGate
{
public:
    explicit FaceGate(App::DocumentObject* source)
        : source(source)
    {}
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        return obj && obj == source && sub && std::string(sub).rfind("Face", 0) == 0;
    }

private:
    App::DocumentObject* source;
};

/// What a bill of materials takes, in the order the panel says them
const char* const valueNames[] = {"FlatLength",
                                  "FlatWidth",
                                  "FlatArea",
                                  "Thickness",
                                  "MaterialName",
                                  "BendCount",
                                  "MaxBendAngle"};

}  // namespace

TaskSheetMetalFlatPattern::TaskSheetMetalFlatPattern(ViewProviderSheetMetalFlatPattern* vp)
    : TaskBox(Gui::BitmapFactory().pixmap("PartDesign_SheetMetalFlatPattern"),
              tr("Flat pattern"),
              true,
              nullptr)
    , Gui::SelectionObserver(true, Gui::ResolveMode::OldStyleElement)
    , vp(vp)
{
    auto proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskSheetMetalFlatPattern"));
    auto layout = new QVBoxLayout(proxy);

    source = new QLabel(proxy);
    source->setObjectName(QStringLiteral("labelSource"));
    source->setWordWrap(true);
    layout->addWidget(source);

    layout->addWidget(new QLabel(tr("Laid on XY, the skin of"), proxy));
    roots = new QListWidget(proxy);
    roots->setObjectName(QStringLiteral("listRootFace"));
    roots->setMaximumHeight(50);
    roots->setToolTip(tr("The wall's skin the flat lies on; the first wall's when none"));
    layout->addWidget(roots);
    auto buttons = new QGridLayout;
    pick = new QToolButton(proxy);
    pick->setObjectName(QStringLiteral("buttonPick"));
    pick->setText(tr("Pick a face"));
    pick->setCheckable(true);
    pick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pick->setToolTip(tr("Pick in the view the skin to lay on XY"));
    connect(pick, &QToolButton::toggled, this, &TaskSheetMetalFlatPattern::startPick);
    buttons->addWidget(pick, 0, 0);
    auto first = new QToolButton(proxy);
    first->setObjectName(QStringLiteral("buttonFirstWall"));
    first->setText(tr("First wall"));
    first->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    first->setToolTip(tr("Lay the first wall's skin on XY"));
    connect(first, &QToolButton::clicked, this, &TaskSheetMetalFlatPattern::firstWall);
    buttons->addWidget(first, 0, 1);
    layout->addLayout(buttons);

    auto grid = new QGridLayout;
    int row = 0;
    for (const char* name : valueNames) {
        grid->addWidget(new QLabel(sheetMetalWords(name), proxy), row, 0);
        auto value = new QLabel(proxy);
        value->setObjectName(QString::fromLatin1(name));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(value, row++, 1);
        values[name] = value;
    }
    layout->addLayout(grid);

    status = new QLabel(proxy);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);

    groupLayout()->addWidget(proxy);

    if (auto obj = flat()) {
        // shown again after any change of it or recompute, its own or not
        auto later = [this] {
            QTimer::singleShot(0, this, &TaskSheetMetalFlatPattern::refresh);
        };
        changed = obj->getDocument()->signalChangedObject.connect(
            [obj, later](const App::DocumentObject& o, const App::Property&) {
                if (&o == obj) {
                    later();
                }
            });
        recomputed = obj->getDocument()->signalRecomputed.connect(
            [later](const App::Document&, const std::vector<App::DocumentObject*>&) {
                later();
            });
    }
    refresh();
}

TaskSheetMetalFlatPattern::~TaskSheetMetalFlatPattern()
{
    if (picking) {
        Gui::Selection().rmvSelectionGate();
    }
}

PartDesign::SheetMetalFlatPattern* TaskSheetMetalFlatPattern::flat() const
{
    return vp ? dynamic_cast<PartDesign::SheetMetalFlatPattern*>(vp->getObject()) : nullptr;
}

void TaskSheetMetalFlatPattern::refresh()
{
    auto obj = flat();
    if (!obj) {
        return;
    }
    auto folded = obj->Source.getValue();
    source->setText(folded ? tr("Lays flat: %1").arg(QString::fromUtf8(folded->Label.getValue()))
                           : tr("Lays flat: nothing yet"));
    roots->clear();
    if (obj->RootFace.getValue()) {
        for (const auto& sub : obj->RootFace.getSubValues()) {
            roots->addItem(QString::fromStdString(sub));
        }
    }
    for (const auto& [name, label] : values) {
        App::Property* prop = obj->getPropertyByName(name.c_str());
        QString text;
        if (auto q = dynamic_cast<App::PropertyQuantity*>(prop)) {
            text = QString::fromStdString(q->getQuantityValue().getUserString());
        }
        else if (auto s = dynamic_cast<App::PropertyString*>(prop)) {
            text = QString::fromUtf8(s->getValue());
        }
        else if (auto i = dynamic_cast<App::PropertyInteger*>(prop)) {
            text = QString::number(i->getValue());
        }
        label->setText(text);
    }
    status->setText(obj->isValid() ? QString() : QString::fromUtf8(obj->getStatusString()));
}

void TaskSheetMetalFlatPattern::startPick(bool on)
{
    auto obj = flat();
    if (on == picking || !obj) {
        return;
    }
    if (on) {
        Gui::Selection().clearSelection();
        Gui::Selection().addSelectionGate(new FaceGate(obj->Source.getValue()));
    }
    else {
        Gui::Selection().rmvSelectionGate();
    }
    picking = on;
}

void TaskSheetMetalFlatPattern::setRoot(App::DocumentObject* folded, const std::string& face)
{
    auto obj = flat();
    if (!obj) {
        return;
    }
    if (folded) {
        obj->RootFace.setValue(folded, {face});
    }
    else {
        obj->RootFace.setValue(nullptr);
    }
    obj->recomputeFeature();
    refresh();
}

void TaskSheetMetalFlatPattern::firstWall()
{
    pick->setChecked(false);
    setRoot(nullptr, {});
}

void TaskSheetMetalFlatPattern::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto obj = flat();
    if (!picking || !obj || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    App::DocumentObject* folded = obj->Source.getValue();
    const std::string sub = msg.pSubName ? msg.pSubName : "";
    if (!folded || msg.Object.getObject() != folded || sub.rfind("Face", 0) != 0) {
        return;
    }
    // one face: picked, the pick is done
    QTimer::singleShot(0, this, [this, folded, sub] {
        pick->setChecked(false);
        setRoot(folded, sub);
        Gui::Selection().clearSelection();
    });
}

TaskDlgSheetMetalFlatPattern::TaskDlgSheetMetalFlatPattern(ViewProviderSheetMetalFlatPattern* vp)
    : vp(vp)
{
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Edit flat pattern"));
    Content.push_back(new TaskSheetMetalFlatPattern(vp));
}

bool TaskDlgSheetMetalFlatPattern::accept()
{
    // a feature recomputed alone stays touched: the document's recompute
    // settles it, in the step OK makes
    if (auto obj = vp ? vp->getObject() : nullptr) {
        obj->getDocument()->recompute();
    }
    Gui::Command::commitCommand();
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    return true;
}

bool TaskDlgSheetMetalFlatPattern::reject()
{
    auto obj = vp ? vp->getObject() : nullptr;
    Gui::Command::abortCommand();
    if (obj && obj->isAttachedToDocument()) {
        // what was there, recomputed: the abort puts the values back only
        obj->getDocument()->recompute();
    }
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    return true;
}

#include "moc_TaskSheetMetalFlatPattern.cpp"
