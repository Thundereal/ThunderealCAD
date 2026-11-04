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
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#endif

#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Selection.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>
#include <Mod/PartDesign/App/SheetMetalCheck.h>
#include <Mod/PartDesign/App/SolidBody.h>

#include "TaskSheetMetalCheck.h"

using namespace PartDesignGui;

namespace
{

QString number(double v, int decimals = 3)
{
    return QString::number(v, 'f', decimals);
}

/// A value with where it comes from, as the table shows it: "1.000 (rule)"
QString sourced(const QString& value, const std::string& source)
{
    return source.empty() ? value
                          : QStringLiteral("%1 (%2)").arg(value, QString::fromStdString(source));
}

}  // namespace

TaskSheetMetalCheck::TaskSheetMetalCheck(App::DocumentObject* obj)
    : TaskBox(Gui::BitmapFactory().pixmap("PartDesign_SheetMetalCheck"),
              tr("Check sheet"),
              true,
              nullptr)
    , target(obj)
{
    // the faces named are the checked part's: the solid's tip for the solid
    // or its flat object
    auto solid = Base::freecad_dynamic_cast<PartDesign::SolidBody>(obj);
    if (auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(obj)) {
        solid = flat->getSheet();
    }
    part = solid ? solid->Tip.getValue() : obj;

    auto proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskSheetMetalCheck"));
    auto layout = new QVBoxLayout(proxy);

    layout->addWidget(new QLabel(tr("Bends"), proxy));
    bends = new QTableWidget(proxy);
    bends->setObjectName(QStringLiteral("tableBends"));
    bends->setColumnCount(7);
    bends->setHorizontalHeaderLabels({tr("Bend"),
                                      tr("Feature"),
                                      tr("Angle"),
                                      tr("Radius"),
                                      tr("K"),
                                      tr("Allowance"),
                                      tr("Direction")});
    bends->setToolTip(tr("Each bend, its values and where each comes from: the rule, its\n"
                         "card, or the feature's own. Pick one to select its faces"));
    bends->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bends->setSelectionBehavior(QAbstractItemView::SelectRows);
    bends->setSelectionMode(QAbstractItemView::SingleSelection);
    bends->verticalHeader()->setVisible(false);
    bends->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    connect(bends, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = bends->currentRow();
        if (row >= 0 && row < static_cast<int>(bendMarks.size())) {
            select(bendMarks[row]);
        }
    });
    layout->addWidget(bends);

    layout->addWidget(new QLabel(tr("Problems"), proxy));
    problems = new QListWidget(proxy);
    problems->setObjectName(QStringLiteral("listProblems"));
    problems->setToolTip(tr("What is wrong with the flat. Pick one to select what it is about"));
    problems->setWordWrap(true);
    connect(problems, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < static_cast<int>(problemMarks.size())) {
            select(problemMarks[row]);
        }
    });
    layout->addWidget(problems);

    summary = new QLabel(proxy);
    summary->setObjectName(QStringLiteral("labelSummary"));
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto again = new QPushButton(tr("Check Again"), proxy);
    again->setObjectName(QStringLiteral("buttonCheckAgain"));
    again->setToolTip(tr("Check the sheet as it is now"));
    connect(again, &QPushButton::clicked, this, &TaskSheetMetalCheck::check);
    layout->addWidget(again);

    groupLayout()->addWidget(proxy);
    check();
}

void TaskSheetMetalCheck::check()
{
    bends->setRowCount(0);
    problems->clear();
    bendMarks.clear();
    problemMarks.clear();

    auto obj = target.getObject();
    if (!obj) {
        summary->setText(tr("The object checked is gone."));
        return;
    }
    PartDesign::SheetCheck found;
    try {
        found = PartDesign::checkSheet(obj);
    }
    catch (const Base::Exception& e) {
        summary->setText(QString::fromUtf8(e.what()));
        return;
    }
    catch (const Standard_Failure& e) {
        summary->setText(QString::fromUtf8(e.GetMessageString()));
        return;
    }

    auto doc = obj->getDocument();
    bends->setRowCount(static_cast<int>(found.bends.size()));
    for (std::size_t i = 0; i < found.bends.size(); ++i) {
        const auto& bend = found.bends[i];
        auto maker = doc->getObject(bend.feature.c_str());
        const int row = static_cast<int>(i);
        const QString cells[] = {
            QString::fromStdString(bend.name),
            maker ? QString::fromUtf8(maker->Label.getValue()) : QString(),
            number(bend.angle, 2) + QStringLiteral(" deg"),
            sourced(number(bend.radius), bend.radiusSource),
            sourced(number(bend.kFactor), bend.kFactorSource),
            sourced(number(bend.allowance), bend.method + ", " + bend.methodSource),
            QString::fromStdString(bend.direction),
        };
        for (int c = 0; c < 7; ++c) {
            bends->setItem(row, c, new QTableWidgetItem(cells[c]));
        }
        bendMarks.push_back({bend.feature, bend.elements, bend.flatElements});
    }
    for (const auto& problem : found.problems) {
        problems->addItem(QString::fromStdString(problem.message));
        problemMarks.push_back({problem.feature, problem.elements, problem.flatElements});
    }
    if (found.problems.empty()) {
        summary->setText(tr("%n bend(s), no problem found.", "", static_cast<int>(found.bends.size())));
    }
    else {
        summary->setText(tr("%n bend(s), %1 problem(s) found.", "", static_cast<int>(found.bends.size()))
                             .arg(found.problems.size()));
    }
}

void TaskSheetMetalCheck::select(const Marks& marks)
{
    auto obj = part.getObject();
    if (!obj) {
        return;
    }
    const char* doc = obj->getDocument()->getName();
    Gui::Selection().clearSelection();
    if (marks.elements.empty()) {
        // of a feature, not of faces: the feature
        auto feature = obj->getDocument()->getObject(marks.feature.c_str());
        if (feature) {
            Gui::Selection().addSelection(doc, feature->getNameInDocument());
        }
        return;
    }
    // The faces as they are drawn: on the sheet's flat object, where it is
    // shown with the part's flat (the solid's tip's, laid as it is now), on
    // the folded part, or both
    auto body = PartDesign::Body::findBodyOf(obj);
    auto solid = body ? body->getSolidBody(obj) : nullptr;
    auto flatObj = solid ? solid->flatPattern() : nullptr;
    const bool flat = flatObj && flatObj->Visibility.getValue() && solid->Tip.getValue() == obj
        && flatObj->Status.getStrValue().empty() && !marks.flatElements.empty();
    const bool folded = obj->Visibility.getValue() || !flat;
    if (folded) {
        for (const auto& element : marks.elements) {
            Gui::Selection().addSelection(doc, obj->getNameInDocument(), element.c_str());
        }
    }
    if (flat) {
        // FlatFace3 of the part's flat is the flat object's Face3
        for (const auto& element : marks.flatElements) {
            const char* name = PartDesign::SolidBody::flatSubName(element.c_str());
            if (name && *name) {
                Gui::Selection().addSelection(doc, flatObj->getNameInDocument(), name);
            }
        }
    }
}

TaskDlgSheetMetalCheck::TaskDlgSheetMetalCheck(App::DocumentObject* obj)
{
    Content.push_back(new TaskSheetMetalCheck(obj));
}

#include "moc_TaskSheetMetalCheck.cpp"
