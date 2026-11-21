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

#include <QEvent>
#include <QGroupBox>
#include <QVBoxLayout>

#include <Gui/PrefWidgets.h>

#include "DlgSettingsPMI.h"

using namespace PMIGui;

DlgSettingsPMI::DlgSettingsPMI(QWidget* parent)
    : PreferencePage(parent)
{
    setWindowTitle(tr("PMI"));
    auto layout = new QVBoxLayout(this);
    auto group = new QGroupBox(this);
    group->setObjectName(QStringLiteral("views"));
    auto inner = new QVBoxLayout(group);
    _newView = new Gui::PrefCheckBox(group);
    _newView->setObjectName(QStringLiteral("DoubleClickNewView"));
    _newView->setEntryName("DoubleClickNewView");
    _newView->setParamGrpPath("Mod/PMI");
    _newView->setChecked(false);
    inner->addWidget(_newView);
    layout->addWidget(group);
    layout->addStretch();
    retranslateUi();
}

DlgSettingsPMI::~DlgSettingsPMI() = default;

void DlgSettingsPMI::retranslateUi()
{
    setWindowTitle(tr("PMI"));
    if (auto group = findChild<QGroupBox*>(QStringLiteral("views"))) {
        group->setTitle(tr("PMI views"));
    }
    _newView->setText(tr("Double-click opens a PMI view in a new 3D view"));
    _newView->setToolTip(
        tr("Double-clicking a PMI view in the tree makes it active in a new 3D view of the "
           "document, placed beside the others, rather than in the active one; a view "
           "already active in one brings that one up. Each 3D view shows a PMI view of its "
           "own."));
}

void DlgSettingsPMI::saveSettings()
{
    _newView->onSave();
}

void DlgSettingsPMI::loadSettings()
{
    _newView->onRestore();
}

void DlgSettingsPMI::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

#include "moc_DlgSettingsPMI.cpp"
