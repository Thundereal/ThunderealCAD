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
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#endif

#include <App/Document.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/App/FeatureThicken.h>

#include "TaskThickenParameters.h"
#include "Utils.h"

using namespace PartDesignGui;

namespace
{

/// Faces, or objects with faces given whole; not the feature itself
class FaceGate: public Gui::SelectionGate
{
public:
    explicit FaceGate(App::DocumentObject* feature)
        : feature(feature)
    {}
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        if (!obj || obj == feature || !obj->isDerivedFrom<Part::Feature>()) {
            return false;
        }
        std::string element(sub ? sub : "");
        if (element.empty()) {
            return Part::Feature::getTopoShape(obj).countSubShapes(TopAbs_FACE) > 0;
        }
        return element.rfind("Face", 0) == 0;
    }

private:
    App::DocumentObject* feature;
};

}  // namespace

TaskThickenParameters::TaskThickenParameters(ViewProvider* vp, QWidget* parent)
    : TaskFeatureParameters(vp, parent, "PartDesign_Thicken", tr("Thicken parameters"))
    , Gui::SelectionObserver(true, Gui::ResolveMode::OldStyleElement)
{
    proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskThickenParameters"));
    auto layout = new QVBoxLayout(proxy);

    auto grid = new QGridLayout;
    list = new QListWidget(proxy);
    list->setObjectName(QStringLiteral("listFaces"));
    list->setMaximumHeight(110);
    list->setToolTip(tr("The faces made into a solid: pick them in the view with Add"));
    auto remove = new QAction(tr("Remove"), list);
    remove->setObjectName(QStringLiteral("actionRemove"));
    remove->setShortcut(QKeySequence::Delete);
    list->addAction(remove);
    list->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(remove, &QAction::triggered, this, &TaskThickenParameters::removeCurrent);
    grid->addWidget(list, 0, 0, 1, 2);
    add = new QToolButton(proxy);
    add->setObjectName(QStringLiteral("buttonAdd"));
    add->setText(tr("Add"));
    add->setCheckable(true);
    add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    add->setToolTip(tr("Pick faces, or objects in the tree, to thicken"));
    connect(add, &QToolButton::toggled, this, &TaskThickenParameters::startPick);
    grid->addWidget(add, 1, 0);
    auto removeButton = new QToolButton(proxy);
    removeButton->setObjectName(QStringLiteral("buttonRemove"));
    removeButton->setText(tr("Remove"));
    removeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    removeButton->setToolTip(tr("Take the face selected in the list out"));
    connect(removeButton, &QToolButton::clicked, this, &TaskThickenParameters::removeCurrent);
    grid->addWidget(removeButton, 1, 1);

    grid->addWidget(new QLabel(tr("Thickness"), proxy), 2, 0);
    thickness = new Gui::QuantitySpinBox(proxy);
    thickness->setObjectName(QStringLiteral("thickness"));
    thickness->setUnit(Base::Unit::Length);
    thickness->setMinimum(0.0);
    thickness->setMaximum(1e9);
    grid->addWidget(thickness, 2, 1);
    reversed = new QCheckBox(tr("Reversed"), proxy);
    reversed->setObjectName(QStringLiteral("checkReversed"));
    reversed->setToolTip(tr("Against the faces' normals"));
    grid->addWidget(reversed, 3, 0);
    midplane = new QCheckBox(tr("Symmetric to faces"), proxy);
    midplane->setObjectName(QStringLiteral("checkMidplane"));
    midplane->setToolTip(tr("Half the thickness on each side of the faces"));
    grid->addWidget(midplane, 3, 1);
    grid->addWidget(new QLabel(tr("Join"), proxy), 4, 0);
    join = new QComboBox(proxy);
    join->setObjectName(QStringLiteral("comboJoin"));
    join->addItems({tr("Arc"), tr("Intersection")});
    join->setToolTip(tr("Across an edge between faces: round it, or run on until they meet"));
    grid->addWidget(join, 4, 1);
    layout->addLayout(grid);

    status = new QLabel(proxy);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);

    addOperationCombo(layout);
    groupLayout()->addWidget(proxy);

    connect(thickness,
            qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
            this,
            [this](double v) {
                if (auto f = feature()) {
                    setupTransaction(tr("thickness"));
                    f->Value.setValue(v);
                    recomputeFeature();
                }
            });
    connect(reversed, &QCheckBox::toggled, this, [this](bool on) {
        if (auto f = feature()) {
            setupTransaction(tr("reversed"));
            f->Reversed.setValue(on);
            recomputeFeature();
        }
    });
    connect(midplane, &QCheckBox::toggled, this, [this](bool on) {
        if (auto f = feature()) {
            setupTransaction(tr("symmetric"));
            f->Midplane.setValue(on);
            recomputeFeature();
        }
    });
    connect(join, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (auto f = feature()) {
            setupTransaction(tr("join"));
            f->Join.setValue((long)i);
            recomputeFeature();
        }
    });
    refresh();
}

TaskThickenParameters::~TaskThickenParameters()
{
    if (picking) {
        Gui::Selection().rmvSelectionGate();
    }
}

PartDesign::Thicken* TaskThickenParameters::feature() const
{
    return vp ? dynamic_cast<PartDesign::Thicken*>(vp->getObject()) : nullptr;
}

void TaskThickenParameters::fillList()
{
    auto f = feature();
    if (!f) {
        return;
    }
    QSignalBlocker block(list);
    list->clear();
    const auto& objs = f->Faces.getValues();
    const auto subs = f->Faces.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        QString text = QString::fromUtf8(objs[i]->Label.getValue());
        if (!subs[i].empty()) {
            text += QLatin1Char('.') + QString::fromStdString(subs[i]);
        }
        list->addItem(text);
    }
}

void TaskThickenParameters::refresh()
{
    auto f = feature();
    if (!f) {
        return;
    }
    fillList();
    {
        QSignalBlocker b1(thickness);
        QSignalBlocker b2(reversed);
        QSignalBlocker b3(midplane);
        QSignalBlocker b4(join);
        thickness->setValue(f->Value.getValue());
        reversed->setChecked(f->Reversed.getValue());
        midplane->setChecked(f->Midplane.getValue());
        join->setCurrentIndex((int)f->Join.getValue());
    }
    showStatus();
}

void TaskThickenParameters::showStatus()
{
    auto f = feature();
    status->setText(!f || f->isValid() ? QString() : QString::fromUtf8(f->getStatusString()));
}

void TaskThickenParameters::finishedRecomputeFeature()
{
    showStatus();
}

void TaskThickenParameters::startPick(bool on)
{
    if (on == picking) {
        return;
    }
    if (on) {
        Gui::Selection().clearSelection();
        Gui::Selection().addSelectionGate(new FaceGate(vp ? vp->getObject() : nullptr));
    }
    else {
        Gui::Selection().rmvSelectionGate();
    }
    picking = on;
}

void TaskThickenParameters::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto f = feature();
    if (!picking || !f || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    // as picked, from the top: outside the body, bound into it (as a
    // sketch's external geometry is)
    App::SubObjectT picked = msg.pOriginalMsg ? msg.pOriginalMsg->Object : msg.Object;
    setupTransaction(tr("add a face"));
    App::SubObjectT link;
    try {
        link = importExternalObject(picked, /*report*/ false);
    }
    catch (Base::Exception& e) {
        status->setText(QString::fromUtf8(e.what()));
        return;
    }
    if (!link.getObject()) {
        return;
    }
    auto objs = f->Faces.getValues();
    auto subs = f->Faces.getSubValues();
    objs.push_back(link.getSubObject() ? link.getSubObject() : link.getObject());
    subs.push_back(link.getSubObject() ? link.getOldElementName() : link.getSubName());
    f->Faces.setValues(objs, subs);
    fillList();
    recomputeFeature();
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

void TaskThickenParameters::removeCurrent()
{
    auto f = feature();
    int row = list->currentRow();
    if (!f || row < 0) {
        return;
    }
    auto objs = f->Faces.getValues();
    auto subs = f->Faces.getSubValues();
    if (row >= (int)objs.size() || row >= (int)subs.size()) {
        return;
    }
    setupTransaction(tr("remove a face"));
    objs.erase(objs.begin() + row);
    subs.erase(subs.begin() + row);
    f->Faces.setValues(objs, subs);
    fillList();
    recomputeFeature();
}

TaskDlgThickenParameters::TaskDlgThickenParameters(ViewProvider* vp)
    : TaskDlgFeatureParameters(vp)
{
    Content.push_back(new TaskThickenParameters(vp));
}

#include "moc_TaskThickenParameters.cpp"
