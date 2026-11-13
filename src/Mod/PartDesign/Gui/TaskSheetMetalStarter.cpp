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

#include <iomanip>
#include <sstream>

#ifndef _PreComp_
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QTimer>
#include <QWidget>
#endif

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Unit.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/QuantitySpinBox.h>
#include <Mod/PartDesign/App/Body.h>

#include "TaskSheetMetalStarter.h"

using namespace PartDesignGui;

namespace
{

/// The kinds, as PartDesign.makeStarterShape() spells them
const char* const kinds[] = {"L", "U", "Tub", "Hat", "Box"};

Gui::QuantitySpinBox* lengthBox(QWidget* parent, const char* name, double value)
{
    auto box = new Gui::QuantitySpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setUnit(Base::Unit::Length);
    box->setMinimum(0.0);
    box->setMaximum(1e6);
    box->setValue(value);
    return box;
}

}  // namespace

TaskSheetMetalStarter::TaskSheetMetalStarter(PartDesign::Body* body)
    : TaskBox(Gui::BitmapFactory().pixmap("PartDesign_SheetMetalStarter"),
              tr("Starter shape"),
              true,
              nullptr)
    , body(body)
{
    auto widget = new QWidget(this);
    auto form = new QFormLayout(widget);

    kind = new QComboBox(widget);
    kind->setObjectName(QStringLiteral("Kind"));
    for (const char* k : kinds) {
        kind->addItem(QString::fromLatin1(k), QString::fromLatin1(k));
    }
    form->addRow(tr("Kind"), kind);
    length = lengthBox(widget, "Length", 100.0);
    form->addRow(tr("Length (X)"), length);
    width = lengthBox(widget, "Width", 60.0);
    form->addRow(tr("Width (Y)"), width);
    height = lengthBox(widget, "Height", 25.0);
    form->addRow(tr("Height"), height);
    flangeWidth = lengthBox(widget, "FlangeWidth", 10.0);
    flangeWidth->setToolTip(tr("A hat's lip, past the legs; a box's, in from their outsides"));
    form->addRow(tr("Lip width"), flangeWidth);
    centered = new QCheckBox(tr("Centred on the origin"), widget);
    centered->setObjectName(QStringLiteral("Centered"));
    centered->setChecked(true);
    form->addRow(centered);
    message = new QLabel(widget);
    message->setObjectName(QStringLiteral("Message"));
    message->setWordWrap(true);
    form->addRow(message);
    groupLayout()->addWidget(widget);

    // made again a moment after the last change, not on each key typed
    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(300);
    connect(timer, &QTimer::timeout, this, [this]() { make(); });
    auto later = [this]() { changed(); };
    connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), this, later);
    for (auto box : {length, width, height, flangeWidth}) {
        connect(box, qOverload<double>(&Gui::QuantitySpinBox::valueChanged), this, later);
    }
    connect(centered, &QCheckBox::toggled, this, later);
    changed();
}

void TaskSheetMetalStarter::changed()
{
    const QString k = kind->currentData().toString();
    flangeWidth->setEnabled(k == QStringLiteral("Hat") || k == QStringLiteral("Box"));
    timer->start();
}

std::string TaskSheetMetalStarter::script() const
{
    std::ostringstream ss;
    ss << std::setprecision(12) << "import PartDesign\n"
       << "PartDesign.makeStarterShape(" << body.getObjectPython() << ", '"
       << kind->currentData().toString().toStdString() << "', " << length->rawValue() << ", "
       << width->rawValue() << ", " << height->rawValue()
       << ", FlangeWidth=" << flangeWidth->rawValue()
       << ", Centered=" << (centered->isChecked() ? "True" : "False") << ")";
    return ss.str();
}

void TaskSheetMetalStarter::say(const QString& text, bool refused)
{
    message->setText(text);
    message->setStyleSheet(refused ? QStringLiteral("color: red;") : QString());
}

bool TaskSheetMetalStarter::make()
{
    timer->stop();
    if (!body.getObject()) {
        say(tr("The body is gone"), true);
        good = false;
        return false;
    }
    drop();
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Starter Shape"));
    open = true;
    try {
        Base::Interpreter().runString(script().c_str());
        say(tr("The outside sizes; thickness and radius are the sheet's rule's."), false);
        good = true;
    }
    catch (const Base::Exception& e) {
        // the Python error's own words, without its type
        QString why = QString::fromUtf8(e.what());
        const int colon = why.indexOf(QStringLiteral(": "));
        if (why.startsWith(QStringLiteral("ValueError")) && colon > 0) {
            why = why.mid(colon + 2);
        }
        say(why, true);
        good = false;
    }
    return good;
}

void TaskSheetMetalStarter::drop()
{
    if (open) {
        Gui::Command::abortCommand();
        open = false;
    }
}

bool TaskSheetMetalStarter::keep()
{
    if (timer->isActive() && !make()) {
        return false;
    }
    if (!good) {
        return false;
    }
    // made once more by the logged command, so a macro has it, and kept
    drop();
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Starter Shape"));
    try {
        Gui::Command::runCommand(Gui::Command::Doc, script().c_str());
    }
    catch (const Base::Exception& e) {
        Gui::Command::abortCommand();
        say(QString::fromUtf8(e.what()), true);
        good = false;
        return false;
    }
    Gui::Command::commitCommand();
    return true;
}

TaskDlgSheetMetalStarter::TaskDlgSheetMetalStarter(PartDesign::Body* body)
    : starter(new TaskSheetMetalStarter(body))
{
    Content.push_back(starter);
}

bool TaskDlgSheetMetalStarter::accept()
{
    return starter->keep();
}

bool TaskDlgSheetMetalStarter::reject()
{
    starter->drop();
    return true;
}

#include "moc_TaskSheetMetalStarter.cpp"
