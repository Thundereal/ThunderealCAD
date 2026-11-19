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

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <Base/Tools.h>
#include <App/Document.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/PMI/App/Advisor.h>
#include <Mod/PMI/App/AnnotationSet.h>
#include <Mod/PMI/App/Api.h>
#include <Mod/PMI/App/Check.h>
#include <Mod/PMI/App/Datum.h>
#include <Mod/PMI/App/GeometricTolerance.h>
#include <Mod/PMI/App/TolerancedFeature.h>

#include "TaskGeometricTolerance.h"

using namespace PMIGui;
using GT = PMI::GeometricTolerance;

namespace
{
QString label(int characteristic)
{
    switch (characteristic) {
        case GT::ProfileOfLine:
            return QObject::tr("Profile of a line");
        case GT::ProfileOfSurface:
            return QObject::tr("Profile of a surface");
        case GT::CircularRunout:
            return QObject::tr("Circular runout");
        case GT::TotalRunout:
            return QObject::tr("Total runout");
        default:
            return QString::fromLatin1(GT::CharacteristicEnums[characteristic]);
    }
}

QString zoneLabel(int zone)
{
    switch (zone) {
        case GT::DiameterZone:
            return QObject::tr("Diameter (cylinder)");
        case GT::SphericalDiameterZone:
            return QObject::tr("Spherical diameter");
        default:
            return QObject::tr("Between two surfaces");
    }
}

/// "B(M)" into "B" and "M"
void splitCompartment(const std::string& text, std::string& datum, std::string& modifier)
{
    auto open = text.find('(');
    datum = text.substr(0, open);
    modifier.clear();
    if (open != std::string::npos && text.back() == ')') {
        modifier = text.substr(open + 1, text.size() - open - 2);
    }
}
}  // namespace

TaskGeometricTolerance::TaskGeometricTolerance(GT* gt, QWidget* parent)
    : QWidget(parent)
    , _obj(gt)
{
    setObjectName(QStringLiteral("PMITaskGeometricTolerance"));
    auto layout = new QVBoxLayout(this);
    auto form = new QFormLayout;
    layout->addLayout(form);

    _characteristic = new QComboBox(this);
    _characteristic->setObjectName(QStringLiteral("characteristic"));
    form->addRow(tr("Characteristic"), _characteristic);

    _zone = new QComboBox(this);
    _zone->setObjectName(QStringLiteral("zone"));
    form->addRow(tr("Zone"), _zone);

    _value = new QDoubleSpinBox(this);
    _value->setObjectName(QStringLiteral("value"));
    _value->setDecimals(4);
    _value->setRange(0.0, 1000.0);
    _value->setSingleStep(0.01);
    _value->setSuffix(QStringLiteral(" mm"));
    form->addRow(tr("Tolerance"), _value);

    _material = new QComboBox(this);
    _material->setObjectName(QStringLiteral("material"));
    _material->addItem(tr("None"));
    _material->addItem(tr("Maximum material (M)"));
    _material->addItem(tr("Least material (L)"));
    form->addRow(tr("Material condition"), _material);

    auto mods = new QGridLayout;
    _free = new QCheckBox(tr("Free state (F)"), this);
    _tangent = new QCheckBox(tr("Tangent plane (T)"), this);
    _common = new QCheckBox(tr("Common zone (CZ)"), this);
    _statistical = new QCheckBox(tr("Statistical (ST)"), this);
    _projected = new QCheckBox(tr("Projected (P)"), this);
    _projected->setObjectName(QStringLiteral("projected"));
    _projectedLength = new QDoubleSpinBox(this);
    _projectedLength->setObjectName(QStringLiteral("projectedLength"));
    _projectedLength->setRange(0.0, 10000.0);
    _projectedLength->setSuffix(QStringLiteral(" mm"));
    mods->addWidget(_free, 0, 0);
    mods->addWidget(_tangent, 0, 1);
    mods->addWidget(_common, 1, 0);
    mods->addWidget(_statistical, 1, 1);
    mods->addWidget(_projected, 2, 0);
    mods->addWidget(_projectedLength, 2, 1);
    form->addRow(tr("Modifiers"), mods);

    auto datums = new QHBoxLayout;
    const char* names[] = {"primary", "secondary", "tertiary"};
    for (int i = 0; i < 3; ++i) {
        auto col = new QVBoxLayout;
        _datum[i] = new QComboBox(this);
        _datum[i]->setObjectName(QString::fromLatin1(names[i]));
        _datum[i]->setEditable(true);
        _datumMaterial[i] = new QComboBox(this);
        _datumMaterial[i]->setObjectName(QStringLiteral("%1Material").arg(QLatin1String(names[i])));
        _datumMaterial[i]->addItems({QString(), QStringLiteral("M"), QStringLiteral("L")});
        col->addWidget(_datum[i]);
        col->addWidget(_datumMaterial[i]);
        datums->addLayout(col);
    }
    form->addRow(tr("Datums"), datums);

    _issues = new QLabel(this);
    _issues->setObjectName(QStringLiteral("issues"));
    _issues->setWordWrap(true);
    layout->addWidget(_issues);

    fill();

    auto changed = [this]() {
        if (!_filling) {
            apply();
        }
    };
    connect(_characteristic, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        if (!_filling) {
            apply();
            offer();
        }
    });
    connect(_zone, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    connect(_value, qOverload<double>(&QDoubleSpinBox::valueChanged), this, changed);
    connect(_material, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    for (auto box : {_free, _tangent, _common, _statistical, _projected}) {
        connect(box, &QCheckBox::toggled, this, changed);
    }
    connect(_projectedLength, qOverload<double>(&QDoubleSpinBox::valueChanged), this, changed);
    for (int i = 0; i < 3; ++i) {
        connect(_datum[i], &QComboBox::currentTextChanged, this, changed);
        connect(_datumMaterial[i], qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    }
}

GT* TaskGeometricTolerance::object() const
{
    return freecad_cast<GT*>(_obj.getObject());
}

void TaskGeometricTolerance::fill()
{
    auto gt = object();
    if (!gt) {
        return;
    }
    Base::StateLocker guard(_filling);
    auto feat = gt->getFeature();
    _chars = PMI::Advisor::characteristics(feat);
    if (std::find(_chars.begin(), _chars.end(), gt->Characteristic.getValue()) == _chars.end()) {
        // What it is stays offered, even if the advisor would not
        _chars.push_back(gt->Characteristic.getValue());
    }
    _characteristic->clear();
    for (int c : _chars) {
        _characteristic->addItem(label(c));
    }
    _characteristic->setCurrentIndex(int(std::find(_chars.begin(), _chars.end(),
                                                   gt->Characteristic.getValue())
                                         - _chars.begin()));
    _value->setValue(gt->Value.getValue());
    _material->setCurrentIndex(gt->MaterialCondition.getValue());
    const auto& mods = gt->Modifiers.getValues();
    auto has = [&](const char* m) {
        return std::find(mods.begin(), mods.end(), m) != mods.end();
    };
    _free->setChecked(has("F"));
    _tangent->setChecked(has("T"));
    _common->setChecked(has("CZ"));
    _statistical->setChecked(has("ST"));
    _projected->setChecked(has("P"));
    _projectedLength->setValue(gt->ProjectedLength.getValue());

    // The letters of the feature's solid
    QStringList letters {QString()};
    if (auto set = gt->getSet()) {
        auto solid = PMI::AnnotationSet::solidOf(feat);
        for (auto obj : set->Group.getValues()) {
            if (auto d = freecad_cast<PMI::Datum*>(obj)) {
                if (!solid || PMI::AnnotationSet::solidOf(d->getFeature()) == solid) {
                    letters << QString::fromUtf8(d->Letter.getValue());
                }
            }
        }
    }
    std::vector<std::string> comps;
    if (auto drf = gt->getDatumSystem()) {
        comps = drf->Compartments.getValues();
    }
    for (int i = 0; i < 3; ++i) {
        _datum[i]->clear();
        _datum[i]->addItems(letters);
        std::string datum, modifier;
        if (i < int(comps.size())) {
            splitCompartment(comps[i], datum, modifier);
        }
        _datum[i]->setCurrentText(QString::fromUtf8(datum.c_str()));
        _datumMaterial[i]->setCurrentText(QString::fromUtf8(modifier.c_str()));
    }
    offer();
}

void TaskGeometricTolerance::offer()
{
    auto gt = object();
    if (!gt) {
        return;
    }
    Base::StateLocker guard(_filling);
    auto feat = gt->getFeature();
    int c = gt->Characteristic.getValue();
    _zones = PMI::Advisor::zoneShapes(feat, c);
    _zone->clear();
    for (int z : _zones) {
        _zone->addItem(zoneLabel(z));
    }
    auto it = std::find(_zones.begin(), _zones.end(), gt->ZoneShape.getValue());
    _zone->setCurrentIndex(it == _zones.end() ? 0 : int(it - _zones.begin()));
    _material->setEnabled(PMI::Advisor::materialCondition(feat, c));
    int datums = PMI::Advisor::datums(c);
    for (int i = 0; i < 3; ++i) {
        _datum[i]->setEnabled(datums >= 0);
        _datumMaterial[i]->setEnabled(datums >= 0);
    }
    report();
}

void TaskGeometricTolerance::apply()
{
    auto gt = object();
    if (!gt) {
        return;
    }
    int ci = _characteristic->currentIndex();
    if (ci >= 0 && ci < int(_chars.size())) {
        gt->Characteristic.setValue(long(_chars[ci]));
    }
    int zi = _zone->currentIndex();
    if (zi >= 0 && zi < int(_zones.size())) {
        gt->ZoneShape.setValue(long(_zones[zi]));
    }
    gt->Value.setValue(_value->value());
    gt->MaterialCondition.setValue(_material->isEnabled() ? long(_material->currentIndex()) : 0L);
    std::vector<std::string> mods;
    if (_free->isChecked()) {
        mods.emplace_back("F");
    }
    if (_tangent->isChecked()) {
        mods.emplace_back("T");
    }
    if (_common->isChecked()) {
        mods.emplace_back("CZ");
    }
    if (_statistical->isChecked()) {
        mods.emplace_back("ST");
    }
    if (_projected->isChecked()) {
        mods.emplace_back("P");
    }
    gt->Modifiers.setValues(mods);
    gt->ProjectedLength.setValue(_projected->isChecked() ? _projectedLength->value() : 0.0);

    // The frame: one per distinct set of compartments, shared
    std::vector<std::string> comps;
    if (PMI::Advisor::datums(gt->Characteristic.getValue()) >= 0) {
        for (int i = 0; i < 3; ++i) {
            std::string d = _datum[i]->currentText().trimmed().toUtf8().constData();
            if (d.empty()) {
                break;
            }
            std::string m = _datumMaterial[i]->currentText().toUtf8().constData();
            comps.push_back(m.empty() ? d : d + "(" + m + ")");
        }
    }
    App::DocumentObject* drf = nullptr;
    if (!comps.empty()) {
        drf = PMI::Api::frame(gt->getSet(), comps);
        if (drf) {
            drf->recomputeFeature();
        }
    }
    gt->DatumSystem.setValue(drf);
    gt->recomputeFeature();
    report();
}

void TaskGeometricTolerance::report()
{
    auto gt = object();
    if (!gt) {
        return;
    }
    QStringList lines;
    std::vector<PMI::Check::Issue> issues;
    if (auto set = gt->getSet()) {
        for (auto& issue : PMI::Check::set(set)) {
            if (issue.object == gt
                || (gt->getDatumSystem() && issue.object == gt->getDatumSystem())) {
                issues.push_back(issue);
            }
        }
    }
    for (auto& issue : issues) {
        lines << QStringLiteral("<span style='color:%1'>%2</span>")
                     .arg(issue.severity == PMI::Check::Error ? QStringLiteral("#c00000")
                                                              : QStringLiteral("#a06000"),
                          QString::fromUtf8(issue.message.c_str()).toHtmlEscaped());
    }
    QString frame = QString::fromUtf8(gt->EquivalentText.getValue()).toHtmlEscaped();
    _issues->setText(QStringLiteral("<b>%1</b><br/>%2")
                         .arg(frame, lines.isEmpty() ? tr("No problems.") : lines.join(QStringLiteral("<br/>"))));
}

// ---- the dialog ----

TaskDlgGeometricTolerance::TaskDlgGeometricTolerance(GT* gt)
    : _obj(gt)
{
    _widget = new TaskGeometricTolerance(gt);
    auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("PMI_GeometricTolerance"),
                                          tr("Feature control frame"), true, nullptr);
    box->groupLayout()->addWidget(_widget);
    Content.push_back(box);
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Edit geometric tolerance"));
}

bool TaskDlgGeometricTolerance::accept()
{
    if (auto obj = _obj.getObject()) {
        obj->getDocument()->recompute();
    }
    Gui::Command::commitCommand();
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    return true;
}

bool TaskDlgGeometricTolerance::reject()
{
    Gui::Command::abortCommand();
    Gui::Command::doCommand(Gui::Command::Gui, "Gui.ActiveDocument.resetEdit()");
    if (auto obj = _obj.getObject()) {
        obj->getDocument()->recompute();
    }
    return true;
}

#include "moc_TaskGeometricTolerance.cpp"
