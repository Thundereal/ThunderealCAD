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
#include <array>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#endif

#include <App/Document.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/SelectionObject.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Surface/App/FeatureBlendSurface.h>
#include <Mod/Surface/App/FeaturePipeShell.h>
#include <Mod/Surface/App/FeatureTwoRailSweep.h>

#include "EndEdit.h"
#include "TaskTools.h"


using namespace SurfaceGui;

PROPERTY_SOURCE(SurfaceGui::ViewProviderBlendSurface, PartGui::ViewProviderSpline)
PROPERTY_SOURCE(SurfaceGui::ViewProviderTwoRailSweep, PartGui::ViewProviderSpline)
PROPERTY_SOURCE(SurfaceGui::ViewProviderPipeShell, PartGui::ViewProviderPart)

namespace SurfaceGui
{

namespace
{

bool kindTakes(PickKind kind, App::DocumentObject* obj, const std::string& sub)
{
    if (!obj->isDerivedFrom<Part::Feature>()) {
        return false;
    }
    bool edge = sub.rfind("Edge", 0) == 0, face = sub.rfind("Face", 0) == 0;
    switch (kind) {
        case PickKind::Edges:
            return edge;
        case PickKind::Faces:
            return face;
        case PickKind::EdgesOrWhole:
            return edge || sub.empty();
        case PickKind::Any:
            return true;
    }
    return false;
}

QString linkText(App::DocumentObject* obj, const std::string& sub)
{
    if (!obj) {
        return QObject::tr("(none)");
    }
    QString text = QString::fromUtf8(obj->Label.getValue());
    if (!sub.empty()) {
        text += QLatin1Char('.') + QString::fromStdString(sub);
    }
    return text;
}

QToolButton* toolButton(QWidget* parent, const QString& name, const QString& text, bool checkable)
{
    auto b = new QToolButton(parent);
    b->setObjectName(name);
    b->setText(text);
    b->setCheckable(checkable);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

}  // namespace

// -- ToolPanel ---------------------------------------------------------------

class ToolPanel::Gate: public Gui::SelectionGate
{
public:
    explicit Gate(ToolPanel* panel)
        : panel(panel)
    {}
    ~Gate() override
    {
        panel->gateOn = false;
    }
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        return panel->allows(obj, sub ? sub : "");
    }

private:
    ToolPanel* panel;
};

ToolPanel::ToolPanel(App::DocumentObject* obj, const char* name, const QString& title)
    : objT(obj)
{
    setObjectName(QLatin1String(name));
    setWindowTitle(title);
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    status = new QLabel(this);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);
    attachDocument(Gui::Application::Instance->getDocument(obj->getDocument()));
}

ToolPanel::~ToolPanel()
{
    if (gateOn) {
        Gui::Selection().rmvSelectionGate();
    }
}

EdgeMarkers* ToolPanel::edgeMarkers()
{
    if (!markers && object()) {
        markers = std::make_unique<EdgeMarkers>(object(), this);
    }
    return markers.get();
}

App::DocumentObject* ToolPanel::object() const
{
    return objT.getObject();
}

void ToolPanel::addFill(std::function<void()> fill)
{
    fills.push_back(std::move(fill));
}

void ToolPanel::refill()
{
    filling = true;
    for (const auto& f : fills) {
        f();
    }
    filling = false;
}

bool ToolPanel::allows(App::DocumentObject* obj, const std::string& sub) const
{
    return obj && obj != object() && pickAccept && pickAccept(obj, sub);
}

void ToolPanel::startPick(QToolButton* button, Accept accept, Take take)
{
    if (pickButton && pickButton != button) {
        QSignalBlocker block(pickButton);
        pickButton->setChecked(false);
    }
    if (gateOn) {
        Gui::Selection().rmvSelectionGate();
    }
    pickButton = button;
    pickAccept = std::move(accept);
    pickTake = std::move(take);
    Gui::Selection().clearSelection();
    Gui::Selection().addSelectionGate(new Gate(this));
    gateOn = true;
}

void ToolPanel::endPick()
{
    if (gateOn) {
        Gui::Selection().rmvSelectionGate();
    }
    if (pickButton) {
        QSignalBlocker block(pickButton);
        pickButton->setChecked(false);
    }
    pickButton = nullptr;
    pickAccept = nullptr;
    pickTake = nullptr;
}

void ToolPanel::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    if (!gateOn || !pickTake || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    Gui::SelectionObject sel(msg);
    App::DocumentObject* obj = sel.getObject();
    std::string sub = msg.pSubName ? msg.pSubName : "";
    if (!allows(obj, sub)) {
        return;
    }
    checkOpenCommand();
    auto take = pickTake;
    take(obj, sub);
    refill();
    changed();
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

void ToolPanel::checkOpenCommand()
{
    auto obj = object();
    if (obj && checkCommand && !Gui::Command::hasPendingCommand()) {
        std::string msg("Edit ");
        msg += obj->Label.getValue();
        Gui::Command::openCommand(msg.c_str());
        checkCommand = false;
    }
}

void ToolPanel::slotUndoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void ToolPanel::slotRedoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void ToolPanel::changed()
{
    auto obj = object();
    if (!obj) {
        return;
    }
    obj->recomputeFeature();
    status->setText(obj->isValid() ? QString() : QString::fromUtf8(obj->getStatusString()));
    if (markers) {
        markers->place();
    }
}

void ToolPanel::open()
{
    checkOpenCommand();
    Gui::Selection().clearSelection();
    refill();
    changed();
    if (markers) {
        markers->attach();
    }
}

bool ToolPanel::accept()
{
    endPick();
    auto obj = object();
    if (!obj) {
        return true;
    }
    obj->recomputeFeature();
    if (!obj->isValid()) {
        // said here, not in a dialog: a dialog's event loop would stop a
        // serving process for every browser it serves
        status->setText(tr("Not built: %1").arg(QString::fromUtf8(obj->getStatusString())));
        return false;
    }
    return true;
}

bool ToolPanel::reject()
{
    endPick();
    return true;
}

// -- TaskTool ----------------------------------------------------------------

TaskTool::TaskTool(ToolPanel* panel, const char* icon)
    : docName(panel->object()->getDocument()->getName())
    , panel(panel)
{
    auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap(icon),
                                          panel->windowTitle(),
                                          true,
                                          nullptr);
    box->groupLayout()->addWidget(panel);
    Content.push_back(box);
}

void TaskTool::open()
{
    panel->open();
}

bool TaskTool::accept()
{
    if (!panel->accept()) {
        return false;
    }
    Gui::Command::commitCommand();
    endEdit(docName);
    return true;
}

bool TaskTool::reject()
{
    panel->reject();
    Gui::Command::abortCommand();
    endEdit(docName);
    return true;
}

// -- the fields --------------------------------------------------------------

QWidget* pickList(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyLinkSubList* prop,
                  PickKind kind)
{
    auto w = new QWidget(panel);
    auto grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(new QLabel(title, w), 0, 0, 1, 2);
    auto list = new QListWidget(w);
    list->setObjectName(QLatin1String(name));
    list->setMaximumHeight(90);
    grid->addWidget(list, 1, 0, 1, 2);
    const QString n = QLatin1String(name);
    auto add = toolButton(w, n + QLatin1String("Add"), QObject::tr("Add"), true);
    auto remove = toolButton(w, n + QLatin1String("Remove"), QObject::tr("Remove"), true);
    grid->addWidget(add, 2, 0);
    grid->addWidget(remove, 2, 1);
    auto listed = [prop](App::DocumentObject* obj, const std::string& sub) {
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            if (objs[i] == obj && subs[i] == sub) {
                return (int)i;
            }
        }
        return -1;
    };
    panel->addFill([list, prop] {
        list->clear();
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            list->addItem(linkText(objs[i], subs[i]));
        }
    });
    QObject::connect(add, &QToolButton::toggled, panel, [=](bool on) {
        if (!on) {
            panel->endPick();
            return;
        }
        panel->startPick(
            add,
            [=](App::DocumentObject* obj, const std::string& sub) {
                return kindTakes(kind, obj, sub) && listed(obj, sub) < 0;
            },
            [=](App::DocumentObject* obj, const std::string& sub) {
                auto objs = prop->getValues();
                auto subs = prop->getSubValues();
                objs.push_back(obj);
                subs.push_back(sub);
                prop->setValues(objs, subs);
            });
    });
    QObject::connect(remove, &QToolButton::toggled, panel, [=](bool on) {
        if (!on) {
            panel->endPick();
            return;
        }
        panel->startPick(
            remove,
            [=](App::DocumentObject* obj, const std::string& sub) {
                return listed(obj, sub) >= 0 || listed(obj, "") >= 0;
            },
            [=](App::DocumentObject* obj, const std::string& sub) {
                int i = listed(obj, sub);
                if (i < 0) {
                    i = listed(obj, "");
                }
                auto objs = prop->getValues();
                auto subs = prop->getSubValues();
                if (i >= 0) {
                    objs.erase(objs.begin() + i);
                    subs.erase(subs.begin() + i);
                    prop->setValues(objs, subs);
                }
            });
    });
    panel->body()->insertWidget(panel->body()->count() - 1, w);
    return w;
}

QWidget* pickOne(ToolPanel* panel,
                 const QString& title,
                 const char* name,
                 App::PropertyLinkSub* prop,
                 PickKind kind)
{
    auto w = new QWidget(panel);
    auto row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(title, w));
    auto label = new QLabel(w);
    label->setObjectName(QLatin1String(name) + QLatin1String("Label"));
    row->addWidget(label, 1);
    auto pick = toolButton(w, QLatin1String(name), QObject::tr("Pick"), true);
    pick->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    row->addWidget(pick);
    panel->addFill([label, prop] {
        const auto& subs = prop->getSubValues();
        label->setText(linkText(prop->getValue(), subs.empty() ? std::string() : subs.front()));
    });
    QObject::connect(pick, &QToolButton::toggled, panel, [=](bool on) {
        if (!on) {
            panel->endPick();
            return;
        }
        panel->startPick(
            pick,
            [=](App::DocumentObject* obj, const std::string& sub) {
                return kindTakes(kind, obj, sub);
            },
            [=](App::DocumentObject* obj, const std::string& sub) {
                prop->setValue(obj, {sub});
                // one each: the pick ends there
                QTimer::singleShot(0, panel, [panel] {
                    panel->endPick();
                });
            });
    });
    panel->body()->insertWidget(panel->body()->count() - 1, w);
    return w;
}

QWidget* conditionRow(ToolPanel* panel,
                      const QString& title,
                      const char* name,
                      const Surface::ConditionProperties& props,
                      EdgeMarkers::Where where)
{
    auto w = new QWidget(panel);
    auto grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    const QString n = QLatin1String(name);
    grid->addWidget(new QLabel(title, w), 0, 0);
    auto continuity = new QComboBox(w);
    continuity->setObjectName(n + QLatin1String("Continuity"));
    continuity->addItems({QStringLiteral("G0"), QStringLiteral("G1"), QStringLiteral("G2")});
    grid->addWidget(continuity, 0, 1);
    auto weight = new QDoubleSpinBox(w);
    weight->setObjectName(n + QLatin1String("Weight"));
    weight->setRange(Surface::WeightRange.LowerBound, Surface::WeightRange.UpperBound);
    weight->setSingleStep(Surface::WeightRange.StepSize);
    weight->setDecimals(2);
    grid->addWidget(weight, 0, 2);
    auto pick = toolButton(w, n + QLatin1String("Support"), QObject::tr("Face"), true);
    pick->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    grid->addWidget(pick, 0, 3);
    auto clear = toolButton(w, n + QLatin1String("SupportClear"), QStringLiteral("x"), false);
    clear->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    grid->addWidget(clear, 0, 4);
    auto support = new QLabel(w);
    support->setObjectName(n + QLatin1String("SupportLabel"));
    grid->addWidget(support, 1, 1, 1, 4);
    panel->addFill([=] {
        continuity->setCurrentIndex(int(props.continuity->getValue()));
        weight->setValue(props.weight->getValue());
        auto obj = props.support->getValue();
        const auto& subs = props.support->getSubValues();
        support->setText(obj ? linkText(obj, subs.empty() ? std::string() : subs.front())
                             : QObject::tr("the face the edge is an edge of"));
    });
    QObject::connect(continuity,
                     qOverload<int>(&QComboBox::currentIndexChanged),
                     panel,
                     [=](int index) {
                         if (panel->filling) {
                             return;
                         }
                         panel->checkOpenCommand();
                         props.continuity->setValue(index);
                         panel->changed();
                     });
    QObject::connect(weight,
                     qOverload<double>(&QDoubleSpinBox::valueChanged),
                     panel,
                     [=](double value) {
                         if (panel->filling) {
                             return;
                         }
                         panel->checkOpenCommand();
                         props.weight->setValue(value);
                         panel->changed();
                     });
    QObject::connect(pick, &QToolButton::toggled, panel, [=](bool on) {
        if (!on) {
            panel->endPick();
            return;
        }
        panel->startPick(
            pick,
            [](App::DocumentObject* obj, const std::string& sub) {
                return kindTakes(PickKind::Faces, obj, sub);
            },
            [=](App::DocumentObject* obj, const std::string& sub) {
                props.support->setValue(obj, {sub});
                QTimer::singleShot(0, panel, [panel] {
                    panel->endPick();
                });
            });
    });
    QObject::connect(clear, &QToolButton::clicked, panel, [=] {
        panel->checkOpenCommand();
        props.support->setValue(nullptr);
        panel->refill();
        panel->changed();
    });
    if (where) {
        if (auto markers = panel->edgeMarkers()) {
            markers->add(title, continuity, weight, std::move(where));
        }
    }
    panel->body()->insertWidget(panel->body()->count() - 1, w);
    return w;
}

namespace
{
/// A label and a field in a row, the field's change written and recomputed
template<typename W>
QWidget* field(ToolPanel* panel, const QString& title, W* widget)
{
    auto w = new QWidget(panel);
    auto row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(title, w));
    widget->setParent(w);
    row->addWidget(widget, 1);
    panel->body()->insertWidget(panel->body()->count() - 1, w);
    return w;
}
}  // namespace

QWidget* enumField(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyEnumeration* p)
{
    auto combo = new QComboBox;
    combo->setObjectName(QLatin1String(name));
    for (const auto& e : p->getEnumVector()) {
        combo->addItem(QString::fromStdString(e));
    }
    panel->addFill([=] {
        combo->setCurrentIndex(int(p->getValue()));
    });
    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), panel, [=](int i) {
        if (panel->filling) {
            return;
        }
        panel->checkOpenCommand();
        p->setValue(long(i));
        panel->changed();
    });
    return field(panel, title, combo);
}

QWidget* intField(ToolPanel* panel,
                 const QString& title,
                 const char* name,
                 App::PropertyInteger* p)
{
    auto spin = new QSpinBox;
    spin->setObjectName(QLatin1String(name));
    spin->setRange(0, 1000);
    if (auto c = dynamic_cast<App::PropertyIntegerConstraint*>(p)) {
        if (auto cs = c->getConstraints()) {
            spin->setRange(int(cs->LowerBound), int(cs->UpperBound));
        }
    }
    panel->addFill([=] {
        spin->setValue(int(p->getValue()));
    });
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), panel, [=](int v) {
        if (panel->filling) {
            return;
        }
        panel->checkOpenCommand();
        p->setValue(long(v));
        panel->changed();
    });
    return field(panel, title, spin);
}

QWidget* boolField(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyBool* p)
{
    auto check = new QCheckBox(title);
    check->setObjectName(QLatin1String(name));
    panel->addFill([=] {
        check->setChecked(p->getValue());
    });
    QObject::connect(check, &QCheckBox::toggled, panel, [=](bool on) {
        if (panel->filling) {
            return;
        }
        panel->checkOpenCommand();
        p->setValue(on);
        panel->changed();
    });
    return field(panel, QString(), check);
}

QWidget* vectorField(ToolPanel* panel,
                    const QString& title,
                    const char* name,
                    App::PropertyVector* p)
{
    auto w = new QWidget(panel);
    auto row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(title, w));
    std::array<QDoubleSpinBox*, 3> spins;
    const char* axes[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        spins[i] = new QDoubleSpinBox(w);
        spins[i]->setObjectName(QLatin1String(name) + QLatin1String(axes[i]));
        spins[i]->setRange(-1e6, 1e6);
        spins[i]->setDecimals(3);
        row->addWidget(spins[i]);
    }
    panel->addFill([=] {
        const auto& v = p->getValue();
        spins[0]->setValue(v.x);
        spins[1]->setValue(v.y);
        spins[2]->setValue(v.z);
    });
    for (auto s : spins) {
        QObject::connect(s,
                         qOverload<double>(&QDoubleSpinBox::valueChanged),
                         panel,
                         [=](double) {
            if (panel->filling) {
                return;
            }
            panel->checkOpenCommand();
            p->setValue(Base::Vector3d(spins[0]->value(), spins[1]->value(), spins[2]->value()));
            panel->changed();
        });
    }
    panel->body()->insertWidget(panel->body()->count() - 1, w);
    return w;
}

// -- the panels --------------------------------------------------------------

namespace
{

ToolPanel* blendPanel(Surface::BlendSurface* obj)
{
    auto p = new ToolPanel(obj, "SurfaceGui_BlendSurface", QObject::tr("Blend surface"));
    pickOne(p, QObject::tr("First edge"), "pickEdge1", &obj->Edge1, PickKind::Edges);
    conditionRow(p,
                 QObject::tr("First side"),
                 "side1",
                 {"first edge", &obj->Side1Continuity, &obj->Side1Weight, &obj->Side1Support},
                 EdgeMarkers::onEdge(obj, &obj->Edge1));
    pickOne(p, QObject::tr("Second edge"), "pickEdge2", &obj->Edge2, PickKind::Edges);
    conditionRow(p,
                 QObject::tr("Second side"),
                 "side2",
                 {"second edge", &obj->Side2Continuity, &obj->Side2Weight, &obj->Side2Support},
                 EdgeMarkers::onEdge(obj, &obj->Edge2));
    intField(p, QObject::tr("Curves across"), "samples", &obj->Samples);
    return p;
}

ToolPanel* sweepPanel(Surface::TwoRailSweep* obj)
{
    auto p = new ToolPanel(obj, "SurfaceGui_TwoRailSweep", QObject::tr("Two-rail sweep"));
    pickOne(p, QObject::tr("First rail"), "pickRail1", &obj->Rail1, PickKind::Edges);
    pickOne(p, QObject::tr("Second rail"), "pickRail2", &obj->Rail2, PickKind::Edges);
    pickList(p, QObject::tr("Sections"), "listSections", &obj->Sections, PickKind::Edges);
    intField(p, QObject::tr("Places along the rails"), "stations", &obj->Stations);
    return p;
}

ToolPanel* pipePanel(Surface::PipeShell* obj)
{
    auto p = new ToolPanel(obj, "SurfaceGui_PipeShell", QObject::tr("Pipeshell"));
    pickList(p, QObject::tr("Spine"), "listSpine", &obj->Spine, PickKind::EdgesOrWhole);
    pickList(p, QObject::tr("Profiles"), "listProfiles", &obj->Profiles, PickKind::Any);
    enumField(p, QObject::tr("Mode"), "mode", &obj->Mode);
    vectorField(p, QObject::tr("Direction"), "direction", &obj->Direction);
    pickList(p,
             QObject::tr("Auxiliary spine"),
             "listAuxiliary",
             &obj->AuxiliarySpine,
             PickKind::EdgesOrWhole);
    boolField(p,
              QObject::tr("Same share of length along both spines"),
              "curvilinear",
              &obj->CurvilinearEquivalence);
    enumField(p, QObject::tr("Contact"), "contact", &obj->AuxiliaryContact);
    pickList(p, QObject::tr("Support faces"), "listSupport", &obj->Support, PickKind::Faces);
    enumField(p, QObject::tr("Transition"), "transition", &obj->Transition);
    boolField(p, QObject::tr("Move the profiles to the spine"), "withContact", &obj->WithContact);
    boolField(p,
              QObject::tr("Turn the profiles square to it"),
              "withCorrection",
              &obj->WithCorrection);
    boolField(p, QObject::tr("Solid"), "solid", &obj->Solid);
    return p;
}

/// The edit of `vp`'s object: its panel, or the one already open kept
template<typename Make>
bool openPanel(Gui::ViewProviderDocumentObject* vp, const char* icon, Make make)
{
    if (auto dlg = Gui::Control().activeDialog()) {
        if (qobject_cast<TaskTool*>(dlg)) {
            Gui::Control().showDialog(dlg);
            return true;
        }
        return false;
    }
    Gui::Control().showDialog(new TaskTool(make(), icon));
    return true;
}

void closePanel()
{
    QTimer::singleShot(0, &Gui::Control(), qOverload<>(&Gui::ControlSingleton::closeDialog));
}

}  // namespace

bool ViewProviderBlendSurface::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return ViewProviderSpline::setEdit(ModNum);
    }
    auto obj = static_cast<Surface::BlendSurface*>(getObject());
    return openPanel(this, "Surface_BlendSurface", [obj] {
        return blendPanel(obj);
    });
}

void ViewProviderBlendSurface::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        closePanel();
    }
    else {
        ViewProviderSpline::unsetEdit(ModNum);
    }
}

QIcon ViewProviderBlendSurface::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Surface_BlendSurface");
}

bool ViewProviderTwoRailSweep::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return ViewProviderSpline::setEdit(ModNum);
    }
    auto obj = static_cast<Surface::TwoRailSweep*>(getObject());
    return openPanel(this, "Surface_TwoRailSweep", [obj] {
        return sweepPanel(obj);
    });
}

void ViewProviderTwoRailSweep::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        closePanel();
    }
    else {
        ViewProviderSpline::unsetEdit(ModNum);
    }
}

QIcon ViewProviderTwoRailSweep::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Surface_TwoRailSweep");
}

bool ViewProviderPipeShell::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    auto obj = static_cast<Surface::PipeShell*>(getObject());
    return openPanel(this, "Surface_PipeShell", [obj] {
        return pipePanel(obj);
    });
}

void ViewProviderPipeShell::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        closePanel();
    }
    else {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
    }
}

QIcon ViewProviderPipeShell::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Surface_PipeShell");
}

}  // namespace SurfaceGui

#include "moc_TaskTools.cpp"
