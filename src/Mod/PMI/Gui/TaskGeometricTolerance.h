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

#ifndef PMIGUI_TASKGEOMETRICTOLERANCE_H
#define PMIGUI_TASKGEOMETRICTOLERANCE_H

#include <vector>

#include <App/DocumentObserver.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace PMI
{
class GeometricTolerance;
}

namespace PMIGui
{

/** The feature control frame builder (docs/GDandT.md sec 10, P1): the
 * characteristics the advisor offers for the feature, the zone, the value,
 * the modifiers and the datum compartments. Each change is written to the
 * tolerance at once, so the frame in the view follows; what the check finds
 * is shown under it.
 */
class TaskGeometricTolerance: public QWidget
{
    Q_OBJECT

public:
    explicit TaskGeometricTolerance(PMI::GeometricTolerance* gt, QWidget* parent = nullptr);

    PMI::GeometricTolerance* object() const;

private:
    void fill();
    void offer();
    void apply();
    void report();

    App::DocumentObjectT _obj;
    bool _filling = false;
    std::vector<int> _chars;
    std::vector<int> _zones;

    QComboBox* _characteristic;
    QComboBox* _zone;
    QDoubleSpinBox* _value;
    QComboBox* _material;
    QCheckBox* _free;
    QCheckBox* _tangent;
    QCheckBox* _common;
    QCheckBox* _statistical;
    QCheckBox* _projected;
    QDoubleSpinBox* _projectedLength;
    QComboBox* _datum[3];
    QComboBox* _datumMaterial[3];
    QLabel* _issues;
};

class TaskDlgGeometricTolerance: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgGeometricTolerance(PMI::GeometricTolerance* gt);

    /// The tolerance it edits, or null once that is gone
    App::DocumentObject* object() const
    {
        return _obj.getObject();
    }

    bool accept() override;
    bool reject() override;
    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    TaskGeometricTolerance* _widget;
    App::DocumentObjectT _obj;
};

}  // namespace PMIGui

#endif  // PMIGUI_TASKGEOMETRICTOLERANCE_H
