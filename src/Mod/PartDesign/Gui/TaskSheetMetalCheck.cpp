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

#include <functional>
#include <numbers>
#include <sstream>

#ifndef _PreComp_
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#endif

#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Selection.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>
#include <Mod/PartDesign/App/FeatureSheetMetalRule.h>
#include <Mod/PartDesign/App/SheetMetalCheck.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>
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

/** The editor of a bend's K or allowance: the plain number, not the source
 * the cell shows beside it. What is typed goes to the panel, which writes it
 * to the feature and checks again -- queued, as that refills the table the
 * editor is closing in.
 */
class BendValueDelegate: public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(int, int, const QString&)> edited;

    QWidget* createEditor(QWidget* parent,
                          const QStyleOptionViewItem& /*option*/,
                          const QModelIndex& /*index*/) const override
    {
        auto edit = new QLineEdit(parent);
        edit->setObjectName(QStringLiteral("editBendValue"));
        edit->setPlaceholderText(QObject::tr("the rule's"));
        return edit;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        static_cast<QLineEdit*>(editor)->setText(index.data(Qt::UserRole).toString());
    }
    void setModelData(QWidget* editor,
                      QAbstractItemModel* /*model*/,
                      const QModelIndex& index) const override
    {
        const QString text = static_cast<QLineEdit*>(editor)->text();
        if (text == index.data(Qt::UserRole).toString() || !edited) {
            return;
        }
        auto edited = this->edited;
        const int row = index.row();
        const int column = index.column();
        // the delegate as context: gone with the panel, the call is too
        auto context = const_cast<BendValueDelegate*>(this);
        QTimer::singleShot(0, context, [edited, row, column, text] {
            edited(row, column, text);
        });
    }
};

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
                         "card, the feature's own, or the bend's own. Pick one to select its\n"
                         "faces. Double-click a K or an allowance to give the bend its own;\n"
                         "clear it to give the bend back to the rule"));
    bends->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    auto delegate = new BendValueDelegate(bends);
    delegate->edited = [this](int row, int column, const QString& text) {
        setBendValue(row, column, text);
    };
    bends->setItemDelegateForColumn(KColumn, delegate);
    bends->setItemDelegateForColumn(AllowanceColumn, delegate);
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
    bendRows.clear();
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
            auto item = new QTableWidgetItem(cells[c]);
            if (c == KColumn || c == AllowanceColumn) {
                // what the editor starts from: the bend's own K, or nothing
                // where it takes another's
                if (bend.kFactorSource == "bend") {
                    item->setData(Qt::UserRole,
                                  c == KColumn ? number(bend.kFactor, 6)
                                               : number(bend.allowance, 6));
                }
            }
            else {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            bends->setItem(row, c, item);
        }
        bendRows.push_back(bend);
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

void TaskSheetMetalCheck::setBendValue(int row, int column, const QString& text)
{
    auto obj = target.getObject();
    if (!obj || row < 0 || row >= static_cast<int>(bendRows.size())) {
        return;
    }
    const PartDesign::SheetBendRow bend = bendRows[row];
    auto maker = obj->getDocument()->getObject(bend.feature.c_str());
    auto ext = maker ? maker->getExtensionByType<PartDesign::SheetMetalExtension>(true) : nullptr;
    if (!ext) {
        return;
    }
    std::ostringstream cmd;
    cmd.precision(17);
    cmd << "setBendKFactor('" << bend.name << "'";
    const QString value = text.trimmed();
    if (!value.isEmpty()) {
        bool ok = false;
        double k = QLocale().toDouble(value, &ok);
        if (!ok) {
            k = value.toDouble(&ok);
        }
        if (!ok) {
            summary->setText(tr("'%1' is not a number.").arg(value));
            return;
        }
        if (column == AllowanceColumn) {
            // the allowance is angle x (R + K T): the K that makes it
            auto rule = ext->getRule();
            const double angle = bend.angle * std::numbers::pi / 180;
            const double thickness = rule ? rule->Thickness.getValue() : 0;
            if (angle <= 0 || thickness <= 0) {
                return;
            }
            k = (k / angle - bend.radius) / thickness;
        }
        cmd << ", " << k;
    }
    cmd << ")";

    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Bend K factor"));
    try {
        FCMD_OBJ_CMD(maker, cmd.str());
        obj->getDocument()->recompute();
        Gui::Command::commitCommand();
    }
    catch (const Base::Exception& e) {
        Gui::Command::abortCommand();
        check();
        summary->setText(QString::fromUtf8(e.what()));
        return;
    }
    check();
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
