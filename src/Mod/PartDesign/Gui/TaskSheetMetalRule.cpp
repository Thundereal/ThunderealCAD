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
#include <algorithm>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#endif

#include <App/Document.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/QuantitySpinBox.h>
#include <Mod/Material/App/Exceptions.h>
#include <Mod/Material/App/MaterialManager.h>
#include <Mod/Material/App/ModelUuids.h>
#include <Mod/Material/Gui/MaterialTreeWidget.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeatureSheetMetalRule.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>

#include "TaskSheetMetalParameters.h"
#include "TaskSheetMetalRule.h"
#include "ViewProviderSheetMetal.h"

using namespace PartDesignGui;

namespace
{

/// The list's first entry: not a card, the body's material
const QString BodyEntry = QStringLiteral("body");

/// The values the card gives, in the order the panel says them
const char* const cardValues[] = {"BendRadius", "KFactor", "MinFlange", "AllowanceMethod"};
/// The rule's own, after them
const char* const ownValues[] = {"MinHoleToBend",
                                 "Gap",
                                 "ReliefShape",
                                 "ReliefWidth",
                                 "ReliefDepth",
                                 "CornerReliefShape",
                                 "CornerReliefSize",
                                 "CornerReliefCover"};

QString number(double v)
{
    return QString::number(v, 'g', 6);
}

bool isOverridden(const PartDesign::SheetMetalRule* rule, const std::string& name)
{
    const auto& names = rule->Overridden.getValues();
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool hasSheetMetal(const PartDesign::SheetMetalRule* rule)
{
    return rule->ShapeMaterial.getValue().hasPhysicalModel(
        Materials::ModelUUIDs::ModelUUID_SheetMetal_SheetMetal);
}

/// Whether the rule's card is its body's: the rule follows the body
bool followsBody(PartDesign::SheetMetalRule* rule)
{
    auto body = PartDesign::Body::findBodyOf(rule);
    return body
        && body->ShapeMaterial.getValue().getUUID() == rule->ShapeMaterial.getValue().getUUID();
}

}  // namespace

TaskSheetMetalRule::TaskSheetMetalRule(ViewProviderSheetMetalRule* vp)
    : TaskBox(Gui::BitmapFactory().pixmap("PartDesign_SheetMetalBaseWall"),
              tr("Sheet metal rule"),
              true,
              nullptr)
    , vp(vp)
{
    proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskSheetMetalRule"));
    auto layout = new QVBoxLayout(proxy);

    layout->addWidget(new QLabel(tr("Card"), proxy));
    cards = new MatGui::MaterialTreeWidget(proxy);
    cards->setObjectName(QStringLiteral("materialCard"));
    cards->setToolTip(tr("The material card the rule takes its values from"));
    if (rule() && PartDesign::Body::findBodyOf(rule())) {
        cards->setLeadingEntry(BodyEntry,
                               tr("The body's material"),
                               tr("Follow the body's material card"));
    }
    connect(cards, &MatGui::MaterialTreeWidget::onMaterial, this, &TaskSheetMetalRule::chooseCard);
    connect(cards,
            &MatGui::MaterialTreeWidget::leadingEntrySelected,
            this,
            &TaskSheetMetalRule::followBody);
    layout->addWidget(cards);
    card = new QLabel(proxy);
    card->setObjectName(QStringLiteral("labelCard"));
    card->setWordWrap(true);
    layout->addWidget(card);

    auto grid = new QGridLayout;
    int row = 0;
    addValue(grid, row++, "Thickness", false);
    for (const char* name : cardValues) {
        addValue(grid, row++, name, true);
    }
    for (const char* name : ownValues) {
        addValue(grid, row++, name, false);
    }
    layout->addLayout(grid);

    users = new QLabel(proxy);
    users->setObjectName(QStringLiteral("labelUsers"));
    users->setWordWrap(true);
    layout->addWidget(users);
    status = new QLabel(proxy);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);

    groupLayout()->addWidget(proxy);

    if (auto obj = rule()) {
        // a value the card gives in, or one set elsewhere, shown here
        auto later = [this] {
            QTimer::singleShot(0, this, &TaskSheetMetalRule::refresh);
        };
        changedObject = obj->getDocument()->signalChangedObject.connect(
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

PartDesign::SheetMetalRule* TaskSheetMetalRule::rule() const
{
    return vp ? dynamic_cast<PartDesign::SheetMetalRule*>(vp->getObject()) : nullptr;
}

void TaskSheetMetalRule::addValue(QGridLayout* grid, int row, const char* name, bool fromCard)
{
    auto obj = rule();
    App::Property* prop = obj ? obj->getPropertyByName(name) : nullptr;
    if (!prop) {
        return;
    }
    // the value set, then everything that follows the rule rebuilt
    auto set = [this, name](auto apply) {
        if (auto r = rule()) {
            apply(r->getPropertyByName(name));
            changed();
        }
    };
    QWidget* box = nullptr;
    if (auto quantity = dynamic_cast<App::PropertyQuantity*>(prop)) {
        auto spin = new Gui::QuantitySpinBox(proxy);
        spin->setUnit(quantity->getUnit());
        spin->setMinimum(0.0);
        spin->setMaximum(1e9);
        connect(spin,
                qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                this,
                [set](double v) {
                    set([v](App::Property* p) {
                        if (auto q = dynamic_cast<App::PropertyQuantity*>(p)) {
                            q->setValue(v);
                        }
                    });
                });
        box = spin;
    }
    else if (auto value = dynamic_cast<App::PropertyFloat*>(prop)) {
        auto spin = new QDoubleSpinBox(proxy);
        spin->setDecimals(4);
        spin->setRange(0.0, 1.0);
        spin->setSingleStep(0.01);
        // a value with a range of its own: that range
        auto ranged = dynamic_cast<App::PropertyFloatConstraint*>(value);
        if (auto c = ranged ? ranged->getConstraints() : nullptr) {
            spin->setRange(c->LowerBound, c->UpperBound);
            spin->setSingleStep(c->StepSize);
        }
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [set](double v) {
            set([v](App::Property* p) {
                if (auto f = dynamic_cast<App::PropertyFloat*>(p)) {
                    f->setValue(v);
                }
            });
        });
        box = spin;
    }
    else if (auto choice = dynamic_cast<App::PropertyEnumeration*>(prop)) {
        auto combo = new QComboBox(proxy);
        for (const auto& item : choice->getEnumVector()) {
            combo->addItem(sheetMetalWords(item.c_str()));
        }
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [set](int i) {
            set([i](App::Property* p) {
                if (auto e = dynamic_cast<App::PropertyEnumeration*>(p)) {
                    e->setValue(static_cast<long>(i));
                }
            });
        });
        box = combo;
    }
    if (!box) {
        return;
    }
    box->setObjectName(QString::fromLatin1(name));
    box->setToolTip(QString::fromUtf8(prop->getDocumentation()));
    // a spin box asks for the width of its largest value; four columns have
    // to fit the task view
    box->setMinimumWidth(80);
    grid->addWidget(new QLabel(sheetMetalWords(name), proxy), row, 0);
    grid->addWidget(box, row, 1);
    boxes[name] = box;
    if (!fromCard) {
        return;
    }
    auto source = new QLabel(proxy);
    source->setObjectName(QStringLiteral("source") + QString::fromLatin1(name));
    grid->addWidget(source, row, 2);
    sources[name] = source;
    auto reset = new QToolButton(proxy);
    reset->setObjectName(QStringLiteral("reset") + QString::fromLatin1(name));
    // an icon only: the panel is as narrow as the task view
    reset->setIcon(Gui::BitmapFactory().iconFromTheme("edit-undo"));
    reset->setText(tr("Card"));
    reset->setToolButtonStyle(Qt::ToolButtonIconOnly);
    reset->setToolTip(tr("Give the value back to the card"));
    connect(reset, &QToolButton::clicked, this, [this, name] {
        if (auto r = rule()) {
            r->resetValue(name);
            changed();
        }
    });
    grid->addWidget(reset, row, 3);
    resets[name] = reset;
}

void TaskSheetMetalRule::showValue(const std::string& name)
{
    auto obj = rule();
    auto it = boxes.find(name);
    App::Property* prop = obj ? obj->getPropertyByName(name.c_str()) : nullptr;
    if (it == boxes.end() || !prop) {
        return;
    }
    QWidget* box = it->second;
    box->setEnabled(!prop->testStatus(App::Property::ReadOnly));
    // only what differs: a box being typed in is left as it is
    QSignalBlocker block(box);
    if (auto q = dynamic_cast<App::PropertyQuantity*>(prop)) {
        auto spin = static_cast<Gui::QuantitySpinBox*>(box);
        if (spin->rawValue() != q->getValue()) {
            spin->setValue(q->getValue());
        }
    }
    else if (auto f = dynamic_cast<App::PropertyFloat*>(prop)) {
        auto spin = static_cast<QDoubleSpinBox*>(box);
        if (spin->value() != f->getValue()) {
            spin->setValue(f->getValue());
        }
    }
    else if (auto e = dynamic_cast<App::PropertyEnumeration*>(prop)) {
        auto combo = static_cast<QComboBox*>(box);
        if (combo->currentIndex() != static_cast<int>(e->getValue())) {
            combo->setCurrentIndex(static_cast<int>(e->getValue()));
        }
    }
}

void TaskSheetMetalRule::refresh()
{
    for (const auto& entry : boxes) {
        showValue(entry.first);
    }
    showCard();
    showStatus();
}

void TaskSheetMetalRule::showCard()
{
    auto obj = rule();
    if (!obj) {
        return;
    }
    const auto& mat = obj->ShapeMaterial.getValue();
    const bool body = followsBody(obj);
    const bool sheet = hasSheetMetal(obj);
    const bool gauge = sheet && obj->hasGauge();
    const QString whose = body ? tr("the body's material") : tr("its own card");
    QString text;
    if (!sheet) {
        text = tr("%1, %2: no sheet metal values; the rule's own are used");
    }
    else if (gauge) {
        text = tr("%1, %2: a gauge row for %3 mm");
    }
    else {
        text = tr("%1, %2: no gauge row for %3 mm; the values are kept");
    }
    text = text.arg(mat.getName(), whose);
    if (sheet) {
        text = text.arg(number(obj->Thickness.getValue()));
    }
    card->setText(text);

    for (const auto& [name, source] : sources) {
        const bool hand = isOverridden(obj, name);
        // the method comes from the card as it is; the rest from its gauge row
        const bool given = sheet && (gauge || name == "AllowanceMethod");
        source->setText(hand    ? tr("set by hand")
                        : given ? tr("from the card")
                                : tr("the rule's own"));
        resets[name]->setEnabled(hand);
    }

    // the list shows the card, the body's entry where it is the body's
    const QString uuid = mat.getUUID();
    if (body) {
        cards->setMaterial(BodyEntry);
    }
    else if (cards->getMaterialUUID() != uuid) {
        try {
            Materials::MaterialManager::getManager().getMaterial(uuid);
            cards->setMaterial(uuid);
        }
        catch (const Materials::MaterialNotFound&) {
            // a card of the document's own, not in a library
            cards->setMaterial(QString());
        }
    }
}

void TaskSheetMetalRule::showStatus()
{
    auto obj = rule();
    if (!obj) {
        return;
    }
    QStringList names;
    QString wrong;
    for (auto in : obj->getInList()) {
        auto sheet = in->getExtensionByType<PartDesign::SheetMetalExtension>(true);
        if (!sheet || sheet->getRule() != obj) {
            continue;
        }
        names << QString::fromUtf8(in->Label.getValue());
        if (wrong.isEmpty() && !in->isValid()) {
            wrong = QStringLiteral("%1: %2").arg(QString::fromUtf8(in->Label.getValue()),
                                                 QString::fromUtf8(in->getStatusString()));
        }
    }
    users->setText(names.isEmpty() ? tr("No feature follows the rule")
                                   : tr("Followed by %1").arg(names.join(QStringLiteral(", "))));
    if (wrong.isEmpty() && !obj->isValid()) {
        wrong = QString::fromUtf8(obj->getStatusString());
    }
    status->setText(wrong);
}

void TaskSheetMetalRule::chooseCard(const QString& uuid)
{
    auto obj = rule();
    if (!obj || uuid == BodyEntry) {
        return;
    }
    std::shared_ptr<Materials::Material> mat;
    try {
        mat = Materials::MaterialManager::getManager().getMaterial(uuid);
    }
    catch (const Materials::MaterialNotFound&) {
        return;
    }
    if (mat && mat->getUUID() != obj->ShapeMaterial.getValue().getUUID()) {
        obj->ShapeMaterial.setValue(*mat);
        changed();
    }
}

void TaskSheetMetalRule::followBody()
{
    auto obj = rule();
    auto body = obj ? PartDesign::Body::findBodyOf(obj) : nullptr;
    if (body && !followsBody(obj)) {
        obj->ShapeMaterial.setValue(body->ShapeMaterial.getValue());
        changed();
    }
}

void TaskSheetMetalRule::changed()
{
    // every feature following the rule, not the rule alone
    if (auto obj = rule()) {
        obj->getDocument()->recompute();
    }
    refresh();
}

TaskDlgSheetMetalRule::TaskDlgSheetMetalRule(ViewProviderSheetMetalRule* vp)
    : vp(vp)
{
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Edit sheet metal rule"));
    Content.push_back(new TaskSheetMetalRule(vp));
}

bool TaskDlgSheetMetalRule::accept()
{
    if (auto obj = vp ? vp->getObject() : nullptr) {
        obj->getDocument()->recompute();
    }
    Gui::Command::commitCommand();
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    return true;
}

bool TaskDlgSheetMetalRule::reject()
{
    auto obj = vp ? vp->getObject() : nullptr;
    Gui::Command::abortCommand();
    if (obj && obj->isAttachedToDocument()) {
        // the abort puts the values back; the features follow them again
        obj->getDocument()->recompute();
    }
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    return true;
}

#include "moc_TaskSheetMetalRule.cpp"
