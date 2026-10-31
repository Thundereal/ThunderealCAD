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
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Gui/QuantitySpinBox.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlange.h>
#include <Mod/PartDesign/App/FeatureSheetMetalRule.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>

#include "TaskSheetMetalParameters.h"

using namespace PartDesignGui;

namespace
{

/// Edges of the feature built on
class EdgeGate: public Gui::SelectionGate
{
public:
    explicit EdgeGate(App::DocumentObject* base)
        : base(base)
    {}
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        return obj && obj == base && sub && std::string(sub).rfind("Edge", 0) == 0;
    }

private:
    App::DocumentObject* base;
};

QString number(double v)
{
    return QString::number(v, 'g', 6);
}

}  // namespace

TaskSheetMetalParameters::TaskSheetMetalParameters(ViewProvider* vp,
                                                   const std::string& pixmap,
                                                   const QString& title,
                                                   const std::vector<const char*>& values,
                                                   bool edges)
    : TaskFeatureParameters(vp, nullptr, pixmap, title)
    , Gui::SelectionObserver(true, Gui::ResolveMode::OldStyleElement)
    , names(values)
{
    proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskSheetMetalParameters"));
    auto layout = new QVBoxLayout(proxy);

    auto grid = new QGridLayout;
    int row = 0;
    if (edges) {
        edgeList = new QListWidget(proxy);
        edgeList->setObjectName(QStringLiteral("listEdges"));
        edgeList->setMaximumHeight(110);
        edgeList->setToolTip(tr("The edges, each where a wall's skin meets its end"));
        auto remove = new QAction(tr("Remove"), edgeList);
        remove->setShortcut(QKeySequence::Delete);
        edgeList->addAction(remove);
        edgeList->setContextMenuPolicy(Qt::ActionsContextMenu);
        connect(remove, &QAction::triggered, this, &TaskSheetMetalParameters::removeEdge);
        grid->addWidget(edgeList, row++, 0, 1, 2);
        addButton = new QToolButton(proxy);
        addButton->setObjectName(QStringLiteral("buttonAdd"));
        addButton->setText(tr("Add"));
        addButton->setCheckable(true);
        addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        addButton->setToolTip(tr("Pick edges in the view"));
        connect(addButton, &QToolButton::toggled, this, &TaskSheetMetalParameters::startPick);
        grid->addWidget(addButton, row, 0);
        auto removeButton = new QToolButton(proxy);
        removeButton->setObjectName(QStringLiteral("buttonRemove"));
        removeButton->setText(tr("Remove"));
        removeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        removeButton->setToolTip(tr("Take the edge selected in the list out"));
        connect(removeButton, &QToolButton::clicked, this, &TaskSheetMetalParameters::removeEdge);
        grid->addWidget(removeButton, row++, 1);
    }
    for (const char* name : names) {
        addValue(grid, row++, name);
    }
    layout->addLayout(grid);

    rule = new QLabel(proxy);
    rule->setObjectName(QStringLiteral("labelRule"));
    rule->setWordWrap(true);
    layout->addWidget(rule);
    status = new QLabel(proxy);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);

    groupLayout()->addWidget(proxy);
    // what is wrong said after any recompute, not only the panel's own: one
    // that fails tells nothing of the object itself
    if (auto obj = vp ? vp->getObject() : nullptr) {
        recomputed = obj->getDocument()->signalRecomputed.connect(
            [this](const App::Document&, const std::vector<App::DocumentObject*>&) {
                QTimer::singleShot(0, this, [this] {
                    showStatus();
                });
            });
    }
    refresh();
}

TaskSheetMetalParameters::~TaskSheetMetalParameters()
{
    if (picking) {
        Gui::Selection().rmvSelectionGate();
    }
}

App::Property* TaskSheetMetalParameters::property(const char* name) const
{
    auto obj = vp ? vp->getObject() : nullptr;
    return obj ? obj->getPropertyByName(name) : nullptr;
}

void TaskSheetMetalParameters::addValue(QGridLayout* grid, int row, const char* name)
{
    App::Property* prop = property(name);
    if (!prop) {
        return;
    }
    const QString label = QString::fromLatin1(name);
    const QString tip = QString::fromUtf8(prop->getDocumentation());
    QWidget* box = nullptr;
    if (auto quantity = dynamic_cast<App::PropertyQuantity*>(prop)) {
        auto spin = new Gui::QuantitySpinBox(proxy);
        spin->setUnit(quantity->getUnit());
        spin->setMinimum(dynamic_cast<App::PropertyDistance*>(prop) ? -1e9 : 0.0);
        spin->setMaximum(1e9);
        connect(spin,
                qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                this,
                [this, name](double v) {
                    if (auto q = dynamic_cast<App::PropertyQuantity*>(property(name))) {
                        setupTransaction(tr(name));
                        q->setValue(v);
                        recomputeFeature();
                    }
                });
        box = spin;
    }
    else if (auto choice = dynamic_cast<App::PropertyEnumeration*>(prop)) {
        auto combo = new QComboBox(proxy);
        for (const auto& item : choice->getEnumVector()) {
            combo->addItem(QString::fromUtf8(item.c_str()));
        }
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, name](int i) {
            if (auto e = dynamic_cast<App::PropertyEnumeration*>(property(name))) {
                setupTransaction(tr(name));
                e->setValue(static_cast<long>(i));
                recomputeFeature();
            }
        });
        box = combo;
    }
    else if (dynamic_cast<App::PropertyBool*>(prop)) {
        auto check = new QCheckBox(label, proxy);
        connect(check, &QCheckBox::toggled, this, [this, name](bool on) {
            if (auto b = dynamic_cast<App::PropertyBool*>(property(name))) {
                setupTransaction(tr(name));
                b->setValue(on);
                recomputeFeature();
            }
        });
        box = check;
    }
    if (!box) {
        return;
    }
    box->setObjectName(label);
    box->setToolTip(tip);
    if (dynamic_cast<QCheckBox*>(box)) {
        grid->addWidget(box, row, 0, 1, 2);
    }
    else {
        grid->addWidget(new QLabel(label, proxy), row, 0);
        grid->addWidget(box, row, 1);
    }
    boxes[name] = box;
}

void TaskSheetMetalParameters::refresh()
{
    for (const auto& [name, box] : boxes) {
        App::Property* prop = property(name.c_str());
        QSignalBlocker block(box);
        if (auto q = dynamic_cast<App::PropertyQuantity*>(prop)) {
            static_cast<Gui::QuantitySpinBox*>(box)->setValue(q->getValue());
        }
        else if (auto e = dynamic_cast<App::PropertyEnumeration*>(prop)) {
            static_cast<QComboBox*>(box)->setCurrentIndex(static_cast<int>(e->getValue()));
        }
        else if (auto b = dynamic_cast<App::PropertyBool*>(prop)) {
            static_cast<QCheckBox*>(box)->setChecked(b->getValue());
        }
    }
    fillEdges();
    showStatus();
}

void TaskSheetMetalParameters::fillEdges()
{
    auto flange = vp ? dynamic_cast<PartDesign::SheetMetalFlange*>(vp->getObject()) : nullptr;
    if (!edgeList || !flange) {
        return;
    }
    QSignalBlocker block(edgeList);
    edgeList->clear();
    for (const auto& sub : flange->Edges.getSubValues()) {
        edgeList->addItem(QString::fromStdString(sub));
    }
}

void TaskSheetMetalParameters::showStatus()
{
    auto obj = vp ? vp->getObject() : nullptr;
    status->setText(!obj || obj->isValid() ? QString() : QString::fromUtf8(obj->getStatusString()));
    auto sheet = obj ? obj->getExtensionByType<PartDesign::SheetMetalExtension>(true) : nullptr;
    auto r = sheet ? sheet->getRule() : nullptr;
    if (!r) {
        rule->setText(tr("No rule"));
        return;
    }
    rule->setText(tr("Rule %1: %2 mm thick, %3 mm inner radius, K %4")
                      .arg(QString::fromUtf8(r->Label.getValue()),
                           number(r->Thickness.getValue()),
                           number(r->BendRadius.getValue()),
                           number(r->KFactor.getValue())));
}

void TaskSheetMetalParameters::finishedRecomputeFeature()
{
    showStatus();
}

void TaskSheetMetalParameters::startPick(bool on)
{
    auto flange = vp ? dynamic_cast<PartDesign::SheetMetalFlange*>(vp->getObject()) : nullptr;
    if (on == picking || !flange) {
        return;
    }
    if (on) {
        Gui::Selection().clearSelection();
        Gui::Selection().addSelectionGate(new EdgeGate(flange->BaseFeature.getValue()));
    }
    else {
        Gui::Selection().rmvSelectionGate();
    }
    picking = on;
}

void TaskSheetMetalParameters::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto flange = vp ? dynamic_cast<PartDesign::SheetMetalFlange*>(vp->getObject()) : nullptr;
    if (!picking || !flange || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    App::DocumentObject* base = flange->BaseFeature.getValue();
    App::DocumentObject* obj = msg.Object.getObject();
    const std::string sub = msg.pSubName ? msg.pSubName : "";
    if (!base || obj != base || sub.rfind("Edge", 0) != 0) {
        return;
    }
    auto subs = flange->Edges.getSubValues();
    if (std::find(subs.begin(), subs.end(), sub) == subs.end()) {
        setupTransaction(tr("add an edge"));
        subs.push_back(sub);
        flange->Edges.setValue(base, subs);
        fillEdges();
        recomputeFeature();
    }
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

void TaskSheetMetalParameters::removeEdge()
{
    auto flange = vp ? dynamic_cast<PartDesign::SheetMetalFlange*>(vp->getObject()) : nullptr;
    const int row = edgeList ? edgeList->currentRow() : -1;
    if (!flange || row < 0) {
        return;
    }
    auto subs = flange->Edges.getSubValues();
    if (row >= static_cast<int>(subs.size())) {
        return;
    }
    setupTransaction(tr("remove an edge"));
    subs.erase(subs.begin() + row);
    flange->Edges.setValue(flange->Edges.getValue(), subs);
    fillEdges();
    recomputeFeature();
}

TaskDlgSheetMetalParameters::TaskDlgSheetMetalParameters(ViewProvider* vp,
                                                         TaskSheetMetalParameters* panel)
    : TaskDlgFeatureParameters(vp)
{
    Content.push_back(panel);
}

#include "moc_TaskSheetMetalParameters.cpp"
