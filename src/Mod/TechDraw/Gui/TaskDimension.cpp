/***************************************************************************
 *   Copyright (c) 2021 Uwe Stöhr <uwestoehr@lyx.org>                      *
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
# include <cmath>
# include <QListWidget>
# include <QMessageBox>
# include <QTimer>
#endif // #ifndef _PreComp_

#include <Base/Console.h>

#include <App/Document.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/EditSteps.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection.h>
#include <Gui/SelectionObject.h>
#include <Mod/TechDraw/App/DrawUtil.h>
#include <Mod/TechDraw/App/DrawViewPart.h>

#include "ui_TaskDimension.h"
#include "TaskDimension.h"
#include "QGIViewDimension.h"
#include "ViewProviderDimension.h"


using namespace Gui;
using namespace TechDraw;
using namespace TechDrawGui;

TaskDimension::TaskDimension(QGIViewDimension *parent, ViewProviderDimension *dimensionVP) :
    ui(new Ui_TaskDimension)
{
    m_parent = parent;
    m_dimensionVP = dimensionVP;
    // Each change a step of its own with PerViewEdit on, so the edit may
    // run beside another view's (docs/HandsOnQueue.md entry 232)
    m_steps = new Gui::EditSteps(dimensionVP, this);

    ui->setupUi(this);

    // Tolerancing
    ui->cbTheoreticallyExact->setChecked(parent->getDimFeat()->TheoreticalExact.getValue());
    connect(ui->cbTheoreticallyExact, &QCheckBox::stateChanged, this, &TaskDimension::onTheoreticallyExactChanged);
    // if TheoreticalExact disable tolerances
    if (parent->getDimFeat()->TheoreticalExact.getValue()) {
        ui->cbEqualTolerance->setDisabled(true);
        ui->qsbOvertolerance->setDisabled(true);
        ui->qsbUndertolerance->setDisabled(true);
        ui->leFormatSpecifierOverTolerance->setDisabled(true);
        ui->leFormatSpecifierUnderTolerance->setDisabled(true);
    }
    ui->cbEqualTolerance->setChecked(parent->getDimFeat()->EqualTolerance.getValue());
    connect(ui->cbEqualTolerance, &QCheckBox::stateChanged, this, &TaskDimension::onEqualToleranceChanged);
    // if EqualTolerance overtolernace must not be negative
    if (parent->getDimFeat()->EqualTolerance.getValue())
        ui->qsbOvertolerance->setMinimum(0.0);
    if ((parent->getDimFeat()->Type.isValue("Angle")) ||
        (parent->getDimFeat()->Type.isValue("Angle3Pt"))) {
        ui->qsbOvertolerance->setUnit(Base::Unit::Angle);
        ui->qsbUndertolerance->setUnit(Base::Unit::Angle);
    }
    else {
        ui->qsbOvertolerance->setUnit(Base::Unit::Length);
        ui->qsbUndertolerance->setUnit(Base::Unit::Length);
    }
    ui->qsbOvertolerance->setValue(parent->getDimFeat()->OverTolerance.getValue());
    ui->qsbUndertolerance->setValue(parent->getDimFeat()->UnderTolerance.getValue());
    connect(ui->qsbOvertolerance, qOverload<double>(&QuantitySpinBox::valueChanged), this, &TaskDimension::onOvertoleranceChanged);
    connect(ui->qsbUndertolerance, qOverload<double>(&QuantitySpinBox::valueChanged), this, &TaskDimension::onUndertoleranceChanged);
    // undertolerance is disabled when EqualTolerance is true
    if (ui->cbEqualTolerance->isChecked()) {
        ui->qsbUndertolerance->setDisabled(true);
        ui->leFormatSpecifierUnderTolerance->setDisabled(true);
    }

    // Formatting
    std::string StringValue = parent->getDimFeat()->FormatSpec.getValue();
    QString qs = QString::fromUtf8(StringValue.data(), StringValue.size());
    ui->leFormatSpecifier->setText(qs);
    connect(ui->leFormatSpecifier, &QLineEdit::textChanged, this, &TaskDimension::onFormatSpecifierChanged);
    ui->cbArbitrary->setChecked(parent->getDimFeat()->Arbitrary.getValue());
    connect(ui->cbArbitrary, &QCheckBox::stateChanged, this, &TaskDimension::onArbitraryChanged);
    StringValue = parent->getDimFeat()->FormatSpecOverTolerance.getValue();
    qs = QString::fromUtf8(StringValue.data(), StringValue.size());
    ui->leFormatSpecifierOverTolerance->setText(qs);
    StringValue = parent->getDimFeat()->FormatSpecUnderTolerance.getValue();
    qs = QString::fromUtf8(StringValue.data(), StringValue.size());
    ui->leFormatSpecifierUnderTolerance->setText(qs);
    connect(ui->leFormatSpecifierOverTolerance, &QLineEdit::textChanged, this, &TaskDimension::onFormatSpecifierOverToleranceChanged);
    connect(ui->leFormatSpecifierUnderTolerance, &QLineEdit::textChanged, this, &TaskDimension::onFormatSpecifierUnderToleranceChanged);
    ui->cbArbitraryTolerances->setChecked(parent->getDimFeat()->ArbitraryTolerances.getValue());
    connect(ui->cbArbitraryTolerances, &QCheckBox::stateChanged, this, &TaskDimension::onArbitraryTolerancesChanged);

    // Display Style
    if (dimensionVP) {
        ui->cbArrowheads->setChecked(dimensionVP->FlipArrowheads.getValue());
        connect(ui->cbArrowheads, &QCheckBox::stateChanged, this, &TaskDimension::onFlipArrowheadsChanged);
        ui->dimensionColor->setColor(dimensionVP->Color.getValue().asValue<QColor>());
        connect(ui->dimensionColor, &ColorButton::changed, this, &TaskDimension::onColorChanged);
        ui->qsbFontSize->setValue(dimensionVP->Fontsize.getValue());
        ui->qsbFontSize->setUnit(Base::Unit::Length);
        ui->qsbFontSize->setMinimum(0);
        connect(ui->qsbFontSize, qOverload<double>(&QuantitySpinBox::valueChanged), this, &TaskDimension::onFontsizeChanged);
        ui->comboDrawingStyle->setCurrentIndex(dimensionVP->StandardAndStyle.getValue());
        connect(ui->comboDrawingStyle, qOverload<int>(&QComboBox::currentIndexChanged), this, &TaskDimension::onDrawingStyleChanged);
    }

    // Lines
    ui->rbOverride->setChecked(parent->getDimFeat()->AngleOverride.getValue());
    connect(ui->rbOverride, &QRadioButton::toggled, this, &TaskDimension::onOverrideToggled);
    ui->dsbDimAngle->setValue(parent->getDimFeat()->LineAngle.getValue());
    connect(ui->dsbDimAngle, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &TaskDimension::onDimAngleChanged);
    ui->dsbExtAngle->setValue(parent->getDimFeat()->ExtensionAngle.getValue());
    connect(ui->dsbExtAngle, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &TaskDimension::onExtAngleChanged);
    connect(ui->pbDimUseDefault, &QPushButton::clicked, this, &TaskDimension::onDimUseDefaultClicked);
    connect(ui->pbDimUseSelection, &QPushButton::clicked, this, &TaskDimension::onDimUseSelectionClicked);
    connect(ui->pbExtUseDefault, &QPushButton::clicked, this, &TaskDimension::onExtUseDefaultClicked);
    connect(ui->pbExtUseSelection, &QPushButton::clicked, this, &TaskDimension::onExtUseSelectionClicked);

    // References
    connect(ui->pbReattach, &QPushButton::toggled, this, &TaskDimension::onReattachToggled);
    fillReferences();
    showMessage();

    if (!m_steps->isOn()) {
        Gui::Document* doc = m_dimensionVP->getDocument();
        doc->openCommand("TaskDimension");
    }
    // once the edit is up, so the change is the edit's: its step, or in the
    // transaction it holds
    QTimer::singleShot(0, this, [this]() { takeByName(); });
}

TechDraw::DrawViewDimension* TaskDimension::getDim() const
{
    return m_dimensionVP
        ? dynamic_cast<TechDraw::DrawViewDimension*>(m_dimensionVP->getObject())
        : nullptr;
}

void TaskDimension::takeByName()
{
    auto dim = getDim();
    if (!dim || dim->findByName().empty()) {
        return;
    }
    if (m_steps->isOn()) {
        m_steps->step();
    }
    m_refound = dim->repointByName();
    if (m_refound.empty()) {
        return;
    }
    for (auto& found : m_refound) {
        Base::Console().Warning("%s: reference %s taken by its name, now %s; check it, then OK "
                                "keeps it, Cancel leaves the reference as it was\n",
                                dim->Label.getValue(), found.was.c_str(), found.now.c_str());
    }
    recomputeFeature();
}

void TaskDimension::fillReferences()
{
    auto dim = getDim();
    if (!dim) {
        return;
    }
    const int current = ui->lwReferences->currentRow();
    ui->lwReferences->clear();
    const std::vector<std::string> subs = dim->References2D.getSubValues();
    for (size_t i = 0; i < subs.size(); i++) {
        if (subs[i].empty()) {
            continue;
        }
        auto item = new QListWidgetItem(QString::fromStdString(subs[i]), ui->lwReferences);
        item->setData(Qt::UserRole, int(i));
        for (auto& found : m_refound) {
            if (found.index == int(i) && found.now == subs[i]) {
                item->setForeground(QColor(0xb0, 0x70, 0x00));
                item->setToolTip(tr("Was %1: found again by its name when this panel opened. "
                                    "OK keeps it, Cancel leaves the reference as it was.")
                                     .arg(QString::fromStdString(found.was)));
            }
        }
    }
    // 3D references are listed, not reattached here: they are picked in the
    // model's view (TechDraw_DimensionRepair)
    const std::vector<App::DocumentObject*> objs = dim->References3D.getValues();
    const std::vector<std::string> subs3d = dim->References3D.getSubValues();
    for (size_t i = 0; i < objs.size() && i < subs3d.size(); i++) {
        if (!objs[i]) {
            continue;
        }
        auto item = new QListWidgetItem(
            QStringLiteral("%1: %2").arg(QString::fromUtf8(objs[i]->Label.getValue()),
                                         QString::fromStdString(subs3d[i])),
            ui->lwReferences);
        item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        item->setToolTip(tr("A 3D reference: change it with Repair Dimension"));
    }
    if (current >= 0 && current < ui->lwReferences->count()) {
        ui->lwReferences->setCurrentRow(current);
    }
    else if (!m_refound.empty()) {
        // the one taken by name, ready for Reattach
        for (int row = 0; row < ui->lwReferences->count(); row++) {
            if (ui->lwReferences->item(row)->data(Qt::UserRole).toInt()
                == m_refound.front().index) {
                ui->lwReferences->setCurrentRow(row);
                break;
            }
        }
    }
}

void TaskDimension::showMessage()
{
    auto dim = getDim();
    QString text;
    if (dim && dim->isError()) {
        const char* status = dim->getStatusString();
        text = QStringLiteral("<font color='red'>%1<br/>%2</font>")
                   .arg(tr("Recompute failed"),
                        QString::fromUtf8(status ? status : "").toHtmlEscaped());
    }
    if (!m_refound.empty()) {
        QStringList said;
        for (auto& found : m_refound) {
            said << tr("%1 is now %2").arg(QString::fromStdString(found.was),
                                           QString::fromStdString(found.now));
        }
        QString note = tr("Found again by name: %1. Its geometry is not what the reference was "
                          "made on: check it, then OK keeps it, Cancel leaves the reference as "
                          "it was, or Reattach puts it on another element.")
                           .arg(said.join(QStringLiteral("; ")));
        if (!text.isEmpty()) {
            text += QStringLiteral("<br/>");
        }
        text += QStringLiteral("<font color='#b07000'>%1</font>").arg(note.toHtmlEscaped());
    }
    ui->lMessage->setText(text);
    ui->lMessage->setVisible(!text.isEmpty());
}

void TaskDimension::onReattachToggled(bool checked)
{
    if (checked) {
        auto item = ui->lwReferences->currentItem();
        if (!item || !(item->flags() & Qt::ItemIsEnabled)) {
            QSignalBlocker block(ui->pbReattach);
            ui->pbReattach->setChecked(false);
            QMessageBox::warning(Gui::getMainWindow(), tr("No reference selected"),
                                 tr("Select the reference to reattach first."));
            return;
        }
        // the next element picked is the one taken
        Gui::Selection().clearSelection();
    }
}

void TaskDimension::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    if (msg.Type != Gui::SelectionChanges::AddSelection || !ui->pbReattach->isChecked()) {
        return;
    }
    auto dim = getDim();
    auto item = ui->lwReferences->currentItem();
    if (!dim || !item || !msg.pObjectName || !msg.pSubName) {
        return;
    }
    TechDraw::DrawViewPart* dvp = dim->getViewPart();
    std::string sub(msg.pSubName);
    std::string::size_type dot = sub.rfind('.');
    if (dot != std::string::npos) {
        sub = sub.substr(dot + 1);
    }
    const std::string kind = DrawUtil::getGeomTypeFromName(sub);
    if (!dvp || dvp->getDocument() != dim->getDocument()
        || std::string(msg.pDocName ? msg.pDocName : "") != dim->getDocument()->getName()
        || std::string(msg.pObjectName) != dvp->getNameInDocument()
        || (kind != "Edge" && kind != "Vertex")) {
        // not an element of the dimension's view: wait for one
        return;
    }
    const int index = item->data(Qt::UserRole).toInt();
    TechDraw::ReferenceVector refs = dim->getReferences2d();
    if (index < 0 || index >= int(refs.size())) {
        return;
    }
    {
        QSignalBlocker block(ui->pbReattach);
        ui->pbReattach->setChecked(false);
    }
    if (refs.at(index).getSubName() == sub) {
        return;
    }
    if (m_steps->isOn()) {
        m_steps->step();
    }
    refs.at(index).setSubName(sub);
    dim->setReferences2d(refs);
    // what was taken by name is settled for this one
    m_refound.erase(std::remove_if(m_refound.begin(), m_refound.end(),
                                   [index](const TechDraw::DrawViewDimension::NameMatch& m) {
                                       return m.index == index;
                                   }),
                    m_refound.end());
    recomputeFeature();
}

TaskDimension::~TaskDimension()
{
}

bool TaskDimension::accept()
{
    if (m_steps->isOn()) {
        // Each change was a step: the one open is committed, and the edit
        // ended -- this one, of the several there may be
        m_steps->close();
        recomputeFeature();
        m_steps->endEdit();
        return true;
    }
    Gui::Document* doc = m_dimensionVP->getDocument();
    Gui::Command::updateActive();
    Gui::Command::commitCommand();
    doc->resetEdit();

    return true;
}

bool TaskDimension::reject()
{
    // Its own steps taken back, and no other view's
    if (m_steps->reject()) {
        recomputeFeature();
        m_parent->updateView(true);
        return true;
    }
    Gui::Document* doc = m_dimensionVP->getDocument();
    doc->abortCommand();
    recomputeFeature();
    m_parent->updateView(true);
    doc->resetEdit();

    return true;
}

bool TaskDimension::holdsTransaction() const
{
    return !m_steps->isOn();
}

void TaskDimension::recomputeFeature()
{
    App::DocumentObject* objVP = m_dimensionVP->getObject();
    assert(objVP);
    objVP->getDocument()->recomputeFeature(objVP);
    fillReferences();
    showMessage();
}

void TaskDimension::onTheoreticallyExactChanged()
{
    m_parent->getDimFeat()->TheoreticalExact.setValue(ui->cbTheoreticallyExact->isChecked());
    // if TheoreticalExact disable tolerances and set them to zero
    if (ui->cbTheoreticallyExact->isChecked()) {
        ui->qsbOvertolerance->setValue(0.0);
        ui->qsbUndertolerance->setValue(0.0);
        ui->cbEqualTolerance->setDisabled(true);
        ui->qsbOvertolerance->setDisabled(true);
        ui->qsbUndertolerance->setDisabled(true);
        ui->leFormatSpecifierOverTolerance->setDisabled(true);
        ui->leFormatSpecifierUnderTolerance->setDisabled(true);
        ui->cbArbitraryTolerances->setDisabled(true);
        ui->cbArbitraryTolerances->setChecked(false);
    }
    else {
        ui->cbEqualTolerance->setDisabled(false);
        ui->qsbOvertolerance->setDisabled(false);
        ui->leFormatSpecifierOverTolerance->setDisabled(false);
        ui->cbArbitraryTolerances->setDisabled(false);
        if (!ui->cbEqualTolerance->isChecked()) {
            ui->qsbUndertolerance->setDisabled(false);
            ui->leFormatSpecifierUnderTolerance->setDisabled(false);
        }
    }
    recomputeFeature();
}

void TaskDimension::onEqualToleranceChanged()
{
    m_parent->getDimFeat()->EqualTolerance.setValue(ui->cbEqualTolerance->isChecked());
    // if EqualTolerance set negated overtolerance for untertolerance
    // then also the OverTolerance must be positive
    if (ui->cbEqualTolerance->isChecked()) {
        // if OverTolerance is negative or zero, first set it to zero
        if (ui->qsbOvertolerance->value().getValue() < 0)
            ui->qsbOvertolerance->setValue(0.0);
        ui->qsbOvertolerance->setMinimum(0.0);
        ui->qsbUndertolerance->setValue(-1.0 * ui->qsbOvertolerance->value().getValue());
        ui->qsbUndertolerance->setUnit(ui->qsbOvertolerance->value().getUnit());
        ui->qsbUndertolerance->setDisabled(true);
        ui->leFormatSpecifierUnderTolerance->setDisabled(true);
    }
    else {
        ui->qsbOvertolerance->setMinimum(-DBL_MAX);
        if (!ui->cbTheoreticallyExact->isChecked()) {
            ui->qsbUndertolerance->setDisabled(false);
            ui->leFormatSpecifierUnderTolerance->setDisabled(false);
        }
    }
    recomputeFeature();
}

void TaskDimension::onOvertoleranceChanged()
{
    m_parent->getDimFeat()->OverTolerance.setValue(ui->qsbOvertolerance->value().getValue());
    // if EqualTolerance set negated overtolerance for untertolerance
    if (ui->cbEqualTolerance->isChecked()) {
        ui->qsbUndertolerance->setValue(-1.0 * ui->qsbOvertolerance->value().getValue());
        ui->qsbUndertolerance->setUnit(ui->qsbOvertolerance->value().getUnit());
    }
    recomputeFeature();
}

void TaskDimension::onUndertoleranceChanged()
{
    m_parent->getDimFeat()->UnderTolerance.setValue(ui->qsbUndertolerance->value().getValue());
    recomputeFeature();
}

void TaskDimension::onFormatSpecifierChanged()
{
    m_parent->getDimFeat()->FormatSpec.setValue(ui->leFormatSpecifier->text().toUtf8().constData());
    recomputeFeature();
}

void TaskDimension::onArbitraryChanged()
{
    m_parent->getDimFeat()->Arbitrary.setValue(ui->cbArbitrary->isChecked());
    recomputeFeature();
}

void TaskDimension::onFormatSpecifierOverToleranceChanged()
{
//    Base::Console().Message("TD::onFormatSpecifierOverToleranceChanged()\n");
    // if (m_blockToleranceLoop) { return; }
    m_parent->getDimFeat()->FormatSpecOverTolerance.setValue(ui->leFormatSpecifierOverTolerance->text().toUtf8().constData());
    if (ui->cbArbitraryTolerances->isChecked() ) {
        // Don't do anything else if tolerance is Arbitrary
        recomputeFeature();
        return;
    }

    if (ui->cbEqualTolerance->isChecked()) {
        // the under tolerance has to match this one
        ui->leFormatSpecifierUnderTolerance->setText(ui->leFormatSpecifierOverTolerance->text());
        m_parent->getDimFeat()->FormatSpecUnderTolerance.setValue(ui->leFormatSpecifierUnderTolerance->text().toUtf8().constData());
    }
    recomputeFeature();
}

void TaskDimension::onFormatSpecifierUnderToleranceChanged()
{
//    Base::Console().Message("TD::onFormatSpecifierUnderToleranceChanged()\n");
    m_parent->getDimFeat()->FormatSpecUnderTolerance.setValue(ui->leFormatSpecifierUnderTolerance->text().toUtf8().constData());
    if (ui->cbArbitraryTolerances->isChecked() ) {
        // Don't do anything else if tolerance is Arbitrary
        recomputeFeature();
        return;
    }
    if (ui->cbEqualTolerance->isChecked()) {
        // if EqualTolerance is checked, then underTolerance is disabled, so this shouldn't happen!
        // the over tolerance has to match this one
        ui->leFormatSpecifierOverTolerance->setText(ui->leFormatSpecifierUnderTolerance->text());
        m_parent->getDimFeat()->FormatSpecOverTolerance.setValue(ui->leFormatSpecifierOverTolerance->text().toUtf8().constData());
    }
    recomputeFeature();
}

void TaskDimension::onArbitraryTolerancesChanged()
{
    m_parent->getDimFeat()->ArbitraryTolerances.setValue(ui->cbArbitraryTolerances->isChecked());
    recomputeFeature();
}

void TaskDimension::onFlipArrowheadsChanged()
{
    m_dimensionVP->FlipArrowheads.setValue(ui->cbArrowheads->isChecked());
    recomputeFeature();
}

void TaskDimension::onColorChanged()
{
    App::Color ac;
    ac.setValue<QColor>(ui->dimensionColor->color());
    m_dimensionVP->Color.setValue(ac);
    recomputeFeature();
}

void TaskDimension::onFontsizeChanged()
{
    m_dimensionVP->Fontsize.setValue(ui->qsbFontSize->value().getValue());
    recomputeFeature();
}

void TaskDimension::onDrawingStyleChanged()
{
    m_dimensionVP->StandardAndStyle.setValue(ui->comboDrawingStyle->currentIndex());
    recomputeFeature();
}

void TaskDimension::onOverrideToggled()
{
    m_parent->getDimFeat()->AngleOverride.setValue(ui->rbOverride->isChecked());
    recomputeFeature();

}

void TaskDimension::onDimAngleChanged()
{
    m_parent->getDimFeat()->LineAngle.setValue(ui->dsbDimAngle->value());
    recomputeFeature();
}

void TaskDimension::onExtAngleChanged()
{
    m_parent->getDimFeat()->ExtensionAngle.setValue(ui->dsbExtAngle->value());
    recomputeFeature();
}

void TaskDimension::onDimUseDefaultClicked()
{
    pointPair points = m_parent->getDimFeat()->getLinearPoints();
    //duplicate coordinate conversion logic from QGIViewDimension
    Base::Vector2d first2(points.first().x, -points.first().y);
    Base::Vector2d second2(points.second().x, -points.second().y);
    double lineAngle = (second2 - first2).Angle();
    ui->dsbDimAngle->setValue(lineAngle * 180.0 / M_PI);
}

void TaskDimension::onDimUseSelectionClicked()
{
    std::pair<double, bool> result = getAngleFromSelection();
    if (result.second) {
        ui->dsbDimAngle->setValue(result.first * 180.0 / M_PI);
    }
}

void TaskDimension::onExtUseDefaultClicked()
{
    pointPair points = m_parent->getDimFeat()->getLinearPoints();
    //duplicate coordinate conversion logic from QGIViewDimension
    Base::Vector2d first2(points.first().x, -points.first().y);
    Base::Vector2d second2(points.second().x, -points.second().y);
    Base::Vector2d lineDirection = second2 - first2;
    Base::Vector2d extensionDirection(-lineDirection.y, lineDirection.x);
    double extensionAngle = extensionDirection.Angle();
    ui->dsbExtAngle->setValue(extensionAngle * 180.0 / M_PI);
}
void TaskDimension::onExtUseSelectionClicked()
{
    std::pair<double, bool> result = getAngleFromSelection();
    if (result.second) {
        ui->dsbExtAngle->setValue(result.first * 180.0 / M_PI);
    }
}

std::pair<double, bool> TaskDimension::getAngleFromSelection()
{
    std::pair<double, bool> result;
    result.first = 0.0;
    result.second = true;
    std::vector<Gui::SelectionObject> selection = Gui::Selection().getSelectionEx();
    TechDraw::DrawViewPart * objFeat = nullptr;
    std::vector<std::string> SubNames;
    if (!selection.empty()) {
        objFeat = static_cast<TechDraw::DrawViewPart*> (selection.front().getObject());
        SubNames = selection.front().getSubNames();
        if (SubNames.size() == 2) {             //expecting Vertices
            std::string geomName0 = DrawUtil::getGeomTypeFromName(SubNames[0]);
            int geomIndex0 = DrawUtil::getIndexFromName(SubNames[0]);
            std::string geomName1 = DrawUtil::getGeomTypeFromName(SubNames[1]);
            int geomIndex1 = DrawUtil::getIndexFromName(SubNames[1]);
            if ((geomName0 == "Vertex") && (geomName1 == "Vertex"))  {
                TechDraw::VertexPtr v0 = objFeat->getProjVertexByIndex(geomIndex0);
                TechDraw::VertexPtr v1 = objFeat->getProjVertexByIndex(geomIndex1);
                Base::Vector2d v02(v0->point().x, -v0->point().y);
                Base::Vector2d v12(v1->point().x, -v1->point().y);
                result.first = (v12 - v02).Angle();
                return result;
            }
        } else if (SubNames.size() == 1) {      //expecting Edge
            std::string geomName0 = DrawUtil::getGeomTypeFromName(SubNames[0]);
            int geomIndex0 = DrawUtil::getIndexFromName(SubNames[0]);
            if (geomName0 == "Edge") {
                TechDraw::BaseGeomPtr edge = objFeat->getGeomByIndex(geomIndex0);
                Base::Vector2d v02(edge->getStartPoint().x, -edge->getStartPoint().y);
                Base::Vector2d v12(edge->getEndPoint().x, -edge->getEndPoint().y);
                result.first = (v12 - v02).Angle();
                return result;
            }
        }
    }

    QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Incorrect Selection"),
                                               QObject::tr("Select 2 Vertexes or 1 Edge"));
    result.second = false;
    return result;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
TaskDlgDimension::TaskDlgDimension(QGIViewDimension *parent, ViewProviderDimension *dimensionVP) :
    TaskDialog()
{
    widget  = new TaskDimension(parent, dimensionVP);
    taskbox = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("TechDraw_Dimension"), widget->windowTitle(), true, nullptr);
    taskbox->groupLayout()->addWidget(widget);
    Content.push_back(taskbox);
    // Not when each change is a step: an undo of one is no end of the
    // edit, and its own Cancel undoes
    setAutoCloseOnTransactionChange(widget->holdsTransaction());
}

TaskDlgDimension::~TaskDlgDimension()
{
}

void TaskDlgDimension::update()
{
}

//==== calls from the TaskView ===============================================================
void TaskDlgDimension::open()
{
}

void TaskDlgDimension::clicked(int i)
{
    Q_UNUSED(i);
}

bool TaskDlgDimension::accept()
{
    widget->accept();
    return true;
}

bool TaskDlgDimension::reject()
{
    widget->reject();
    return true;
}

bool TaskDlgDimension::holdsTransaction() const
{
    return widget->holdsTransaction();
}

#include "moc_TaskDimension.cpp"
