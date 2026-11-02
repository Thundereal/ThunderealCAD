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
#include <map>

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#endif

#include <App/Document.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Selection.h>
#include <Gui/SelectionObject.h>
#include <Mod/Part/App/SubShapeBinder.h>
#include <Mod/Part/Gui/ViewProviderExt.h>

#include "EdgeMarkers.h"
#include "Import.h"
#include "TaskNetwork.h"


using namespace SurfaceGui;

PROPERTY_SOURCE(SurfaceGui::ViewProviderNetwork, PartGui::ViewProviderSpline)

namespace SurfaceGui
{

void ViewProviderNetwork::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    QAction* act = menu->addAction(QObject::tr("Edit network"), receiver, member);
    act->setData(QVariant((int)ViewProvider::Default));
    PartGui::ViewProviderSpline::setupContextMenu(menu, receiver, member);
}

bool ViewProviderNetwork::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return ViewProviderSpline::setEdit(ModNum);
    }
    auto obj = static_cast<Surface::Network*>(getObject());
    Gui::TaskView::TaskDialog* dlg = Gui::Control().activeDialog();
    if (dlg) {
        // double-clicking the object being edited keeps its panel
        if (qobject_cast<TaskNetwork*>(dlg)) {
            Gui::Control().showDialog(dlg);
            return true;
        }
        return false;
    }
    Gui::Control().showDialog(new TaskNetwork(this, obj));
    return true;
}

void ViewProviderNetwork::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        // when pressing ESC make sure to close the dialog
        QTimer::singleShot(0, &Gui::Control(), qOverload<>(&Gui::ControlSingleton::closeDialog));
    }
    else {
        PartGui::ViewProviderSpline::unsetEdit(ModNum);
    }
}

QIcon ViewProviderNetwork::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Surface_Network");
}

namespace
{
PartGui::ViewProviderPartExt* partView(App::DocumentObject* obj)
{
    return dynamic_cast<PartGui::ViewProviderPartExt*>(
        Gui::Application::Instance->getViewProvider(obj));
}
}  // namespace

void ViewProviderNetwork::highlightCurves(bool on)
{
    for (const auto& objT : highlighted) {
        if (auto svp = partView(objT.getObject())) {
            svp->unsetHighlightedEdges();
        }
    }
    highlighted.clear();
    auto network = dynamic_cast<Surface::Network*>(getObject());
    if (!on || !network) {
        return;
    }

    // magenta profiles, cyan guides; with no guides the network is sorted
    // when it is built, and all of it is the profiles' colour
    const App::Color colours[2] = {App::Color(1.0f, 0.0f, 1.0f), App::Color(0.0f, 0.8f, 1.0f)};
    std::map<App::DocumentObject*, std::vector<App::Color>> edges;
    int family = 0;
    for (auto prop : {&network->Profiles, &network->Guides}) {
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            auto svp = partView(objs[i]);
            auto feature = dynamic_cast<Part::Feature*>(objs[i]);
            if (!svp || !feature) {
                continue;
            }
            auto& colors = edges[objs[i]];
            if (colors.empty()) {
                TopTools_IndexedMapOfShape map;
                TopExp::MapShapes(feature->Shape.getValue(), TopAbs_EDGE, map);
                colors.resize(map.Extent(), svp->LineColor.getValue());
            }
            const auto& sub = subs[i];
            if (sub.empty()) {
                std::fill(colors.begin(), colors.end(), colours[family]);
            }
            else if (sub.rfind("Edge", 0) == 0) {
                std::size_t idx = std::strtoul(sub.c_str() + 4, nullptr, 10);
                if (idx >= 1 && idx <= colors.size()) {
                    colors[idx - 1] = colours[family];
                }
            }
        }
        ++family;
    }
    for (auto& [obj, colors] : edges) {
        partView(obj)->setHighlightedEdges(colors);
        highlighted.insert(App::DocumentObjectT(obj));
    }
}

// ----------------------------------------------------------------------------

class NetworkPanel::Gate: public Gui::SelectionGate
{
public:
    Gate(NetworkPanel* panel)
        : panel(panel)
    {}
    ~Gate() override
    {
        panel->pick = Pick::None;
    }
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        auto network = panel->object();
        if (!network || obj == network || !obj->isDerivedFrom<Part::Feature>()) {
            return false;
        }
        std::string element(sub ? sub : "");
        switch (panel->pick) {
            case Pick::Profile:
            case Pick::Guide:
                // an edge, or an object with edges given whole
                if (element.empty()) {
                    return Part::Feature::getTopoShape(obj).countSubShapes(TopAbs_EDGE) > 0
                        && !panel->listed(obj, element);
                }
                return element.rfind("Edge", 0) == 0 && !panel->listed(obj, element);
            case Pick::Remove:
                return panel->listed(obj, element) || panel->listed(obj, "")
                    || panel->bindersListed();
            case Pick::Support:
                return element.rfind("Face", 0) == 0;
            default:
                return false;
        }
    }

private:
    NetworkPanel* panel;
};

// ----------------------------------------------------------------------------

NetworkPanel::NetworkPanel(ViewProviderNetwork* vp, Surface::Network* obj)
    : vp(vp)
    , objT(obj)
{
    setObjectName(QStringLiteral("SurfaceGui_Network"));
    setWindowTitle(tr("Network surface"));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // the curves
    auto curves = new QGroupBox(tr("Curves"), this);
    auto grid = new QGridLayout(curves);
    auto makeList = [&](const char* name, const QString& title, int column) {
        auto label = new QLabel(title, curves);
        grid->addWidget(label, 0, column);
        auto list = new QListWidget(curves);
        list->setObjectName(QLatin1String(name));
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setMinimumHeight(90);
        grid->addWidget(list, 1, column);
        connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
            lastList = list;
        });
        auto del = new QAction(tr("Remove"), list);
        del->setShortcut(QKeySequence::Delete);
        list->addAction(del);
        connect(del, &QAction::triggered, this, [this, list] {
            lastList = list;
            removeCurrent();
        });
        auto other = new QAction(tr("Move to the other family"), list);
        list->addAction(other);
        connect(other, &QAction::triggered, this, [this, list] {
            lastList = list;
            moveCurve();
        });
        list->setContextMenuPolicy(Qt::ActionsContextMenu);
        return list;
    };
    profiles = makeList("listProfiles", tr("Profiles"), 0);
    guides = makeList("listGuides", tr("Guides"), 1);
    auto makeTool = [&](const char* name, const QString& text, const QString& tip) {
        auto b = new QToolButton(curves);
        b->setObjectName(QLatin1String(name));
        b->setText(text);
        b->setToolTip(tip);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return b;
    };
    addProfile = makeTool("buttonAddProfile",
                          tr("Add profile"),
                          tr("Pick edges, or whole objects in the tree, to add to the profiles"));
    addGuide = makeTool("buttonAddGuide",
                        tr("Add guide"),
                        tr("Pick edges, or whole objects in the tree, to add to the guides"));
    remove = makeTool("buttonRemove", tr("Remove"), tr("Pick curves to take out of the network"));
    move = makeTool("buttonMove",
                    tr("Move"),
                    tr("Move the curve selected in a list to the other family"));
    for (auto b : {addProfile, addGuide, remove}) {
        b->setCheckable(true);
    }
    auto tools = new QHBoxLayout;
    for (auto b : {addProfile, addGuide, remove, move}) {
        tools->addWidget(b);
    }
    grid->addLayout(tools, 2, 0, 1, 2);
    separateButton = new QPushButton(tr("Separate the families"), curves);
    separateButton->setObjectName(QStringLiteral("buttonSeparate"));
    separateButton->setToolTip(
        tr("List the profiles and guides the curves are sorted into, edge by edge,\n"
           "so that a curve in the wrong family can be moved"));
    grid->addWidget(separateButton, 3, 0, 1, 2);
    familiesNote = new QLabel(curves);
    familiesNote->setObjectName(QStringLiteral("labelFamilies"));
    familiesNote->setWordWrap(true);
    grid->addWidget(familiesNote, 4, 0, 1, 2);
    layout->addWidget(curves);

    connect(addProfile, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Profile, -1, on);
    });
    connect(addGuide, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Guide, -1, on);
    });
    connect(remove, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Remove, -1, on);
    });
    connect(move, &QToolButton::clicked, this, &NetworkPanel::moveCurve);
    connect(separateButton, &QPushButton::clicked, this, &NetworkPanel::separate);

    // the edge conditions
    auto conditions = new QGroupBox(tr("Edge conditions"), this);
    auto cgrid = new QGridLayout(conditions);
    const char* names[4] = {"firstProfile", "lastProfile", "firstGuide", "lastGuide"};
    const QString titles[4] = {tr("First profile"),
                               tr("Last profile"),
                               tr("First guide"),
                               tr("Last guide")};
    sides = {{{{"first profile",
                &obj->FirstProfileContinuity,
                &obj->FirstProfileWeight,
                &obj->FirstProfileSupport}},
              {{"last profile",
                &obj->LastProfileContinuity,
                &obj->LastProfileWeight,
                &obj->LastProfileSupport}},
              {{"first guide",
                &obj->FirstGuideContinuity,
                &obj->FirstGuideWeight,
                &obj->FirstGuideSupport}},
              {{"last guide",
                &obj->LastGuideContinuity,
                &obj->LastGuideWeight,
                &obj->LastGuideSupport}}}};
    for (int i = 0; i < 4; ++i) {
        auto& side = sides[i];
        const QString name = QLatin1String(names[i]);
        cgrid->addWidget(new QLabel(titles[i], conditions), 2 * i, 0);
        side.continuity = new QComboBox(conditions);
        side.continuity->setObjectName(name + QLatin1String("Continuity"));
        side.continuity->addItems(
            {QStringLiteral("G0"), QStringLiteral("G1"), QStringLiteral("G2")});
        side.continuity->setToolTip(tr("G0 position, G1 tangent, G2 curvature"));
        cgrid->addWidget(side.continuity, 2 * i, 1);
        side.weight = new QDoubleSpinBox(conditions);
        side.weight->setObjectName(name + QLatin1String("Weight"));
        side.weight->setRange(Surface::WeightRange.LowerBound, Surface::WeightRange.UpperBound);
        side.weight->setSingleStep(Surface::WeightRange.StepSize);
        side.weight->setDecimals(2);
        side.weight->setToolTip(tr("How far the neighbour's direction carries into the surface"));
        cgrid->addWidget(side.weight, 2 * i, 2);
        side.pick = new QToolButton(conditions);
        side.pick->setObjectName(name + QLatin1String("Support"));
        side.pick->setText(tr("Face"));
        side.pick->setCheckable(true);
        side.pick->setToolTip(tr("Pick the face to meet; without one, the face the curve is an "
                                 "edge of"));
        cgrid->addWidget(side.pick, 2 * i, 3);
        auto clear = new QToolButton(conditions);
        clear->setObjectName(name + QLatin1String("SupportClear"));
        clear->setText(QStringLiteral("x"));
        clear->setToolTip(tr("Meet the face the curve is an edge of"));
        cgrid->addWidget(clear, 2 * i, 4);
        side.support = new QLabel(conditions);
        side.support->setObjectName(name + QLatin1String("SupportLabel"));
        cgrid->addWidget(side.support, 2 * i + 1, 1, 1, 4);

        connect(side.continuity,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                [this, i](int index) {
                    if (filling || !object()) {
                        return;
                    }
                    checkOpenCommand();
                    sides[i].props.continuity->setValue(index);
                    update();
                });
        connect(side.weight,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this,
                [this, i](double value) {
                    if (filling || !object()) {
                        return;
                    }
                    checkOpenCommand();
                    sides[i].props.weight->setValue(value);
                    update();
                });
        connect(side.pick, &QToolButton::toggled, this, [this, i](bool on) {
            startPick(Pick::Support, i, on);
        });
        connect(clear, &QToolButton::clicked, this, [this, i] {
            if (!object()) {
                return;
            }
            checkOpenCommand();
            sides[i].props.support->setValue(nullptr);
            fillSides();
            update();
        });
    }
    auto all = new QPushButton(tr("Apply to all"), conditions);
    all->setObjectName(QStringLiteral("buttonApplyAll"));
    all->setToolTip(tr("Give every side the first profile's continuity and weight"));
    cgrid->addWidget(all, 8, 0, 1, 5);
    connect(all, &QPushButton::clicked, this, &NetworkPanel::applyToAll);
    layout->addWidget(conditions);

    // the tolerances, and what was built
    auto result = new QGroupBox(tr("Fit"), this);
    auto rgrid = new QGridLayout(result);
    auto makeTolerance = [&](const char* name, const QString& title, int row, const QString& tip) {
        rgrid->addWidget(new QLabel(title, result), row, 0);
        auto spin = new QDoubleSpinBox(result);
        spin->setObjectName(QLatin1String(name));
        spin->setDecimals(6);
        spin->setRange(1e-7, 10.0);
        spin->setSingleStep(1e-4);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setToolTip(tip);
        rgrid->addWidget(spin, row, 1);
        return spin;
    };
    tolerance = makeTolerance(
        "tolerance",
        tr("Tolerance"),
        0,
        tr("How far a profile and a guide may pass each other and still cross"));
    fitTolerance = makeTolerance("fitTolerance",
                                 tr("Fit tolerance"),
                                 1,
                                 tr("How far the surface may stray from the curves"));
    for (auto spin : {tolerance, fitTolerance}) {
        connect(spin,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this,
                [this, spin](double value) {
                    auto network = object();
                    if (filling || !network) {
                        return;
                    }
                    checkOpenCommand();
                    (spin == tolerance ? network->Tolerance : network->FitTolerance)
                        .setValue(value);
                    update();
                });
    }
    readout = new QLabel(result);
    readout->setObjectName(QStringLiteral("labelReadout"));
    rgrid->addWidget(readout, 2, 0, 1, 2);
    achieved = new QLabel(result);
    achieved->setObjectName(QStringLiteral("labelAchieved"));
    achieved->setWordWrap(true);
    rgrid->addWidget(achieved, 3, 0, 1, 2);
    status = new QLabel(result);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    rgrid->addWidget(status, 4, 0, 1, 2);
    layout->addWidget(result);

    fillLists();
    fillSides();
    attachDocument(Gui::Application::Instance->getDocument(obj->getDocument()));

    // the conditions in the view too, each on its boundary (the guides run
    // along V: the first and last guide are UMin and UMax)
    using Surface::Geom::Boundary;
    const Boundary boundaries[4] = {Boundary::VMin, Boundary::VMax, Boundary::UMin, Boundary::UMax};
    markers = std::make_unique<EdgeMarkers>(obj, this);
    for (int i = 0; i < 4; ++i) {
        markers->add(titles[i],
                     sides[i].continuity,
                     sides[i].weight,
                     EdgeMarkers::onBoundary(obj, boundaries[i]));
    }
}

NetworkPanel::~NetworkPanel()
{
    if (pick != Pick::None) {
        Gui::Selection().rmvSelectionGate();
    }
}

Surface::Network* NetworkPanel::object() const
{
    return dynamic_cast<Surface::Network*>(objT.getObject());
}

void NetworkPanel::open()
{
    checkOpenCommand();
    vp->highlightCurves(true);
    Gui::Selection().clearSelection();
    update();
    markers->attach();
}

void NetworkPanel::checkOpenCommand()
{
    auto network = object();
    if (network && checkCommand && !Gui::Command::hasPendingCommand()) {
        std::string msg("Edit ");
        msg += network->Label.getValue();
        Gui::Command::openCommand(msg.c_str());
        checkCommand = false;
    }
}

void NetworkPanel::slotUndoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void NetworkPanel::slotRedoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void NetworkPanel::slotDeletedObject(const Gui::ViewProviderDocumentObject& obj)
{
    if (vp == &obj) {
        vp->highlightCurves(false);
    }
}

bool NetworkPanel::listed(App::DocumentObject* obj, const std::string& sub) const
{
    auto network = object();
    if (!network) {
        return false;
    }
    for (auto prop : {&network->Profiles, &network->Guides}) {
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            if (objs[i] == obj && subs[i] == sub) {
                return true;
            }
        }
    }
    return false;
}

bool NetworkPanel::bindersListed() const
{
    auto network = object();
    if (!network) {
        return false;
    }
    for (auto prop : {&network->Profiles, &network->Guides}) {
        for (auto obj : prop->getValues()) {
            if (obj && obj->isDerivedFrom<Part::SubShapeBinder>()) {
                return true;
            }
        }
    }
    return false;
}

void NetworkPanel::fillLists()
{
    auto network = object();
    if (!network) {
        return;
    }
    QListWidget* lists[2] = {profiles, guides};
    int family = 0;
    for (auto prop : {&network->Profiles, &network->Guides}) {
        auto list = lists[family++];
        QSignalBlocker block(list);
        list->clear();
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            QString text = QString::fromUtf8(objs[i]->Label.getValue());
            if (!subs[i].empty()) {
                text += QLatin1Char('.') + QString::fromStdString(subs[i]);
            }
            auto item = new QListWidgetItem(text, list);
            item->setData(Qt::UserRole, (int)i);
        }
    }
    bool sorted = network->Guides.getSize() == 0 && network->Profiles.getSize() > 0;
    separateButton->setEnabled(sorted);
    familiesNote->setText(sorted ? tr("No guides: the curves are sorted into the two families "
                                      "as the surface is built.")
                                 : QString());
}

void NetworkPanel::fillSides()
{
    auto network = object();
    if (!network) {
        return;
    }
    filling = true;
    for (auto& side : sides) {
        side.continuity->setCurrentIndex((int)side.props.continuity->getValue());
        side.weight->setValue(side.props.weight->getValue());
        auto support = side.props.support->getValue();
        const auto& subs = side.props.support->getSubValues();
        side.support->setText(
            support ? QString::fromUtf8(support->Label.getValue())
                    + (subs.empty() ? QString()
                                    : QLatin1Char('.') + QString::fromStdString(subs.front()))
                    : tr("the face the curve is an edge of"));
    }
    tolerance->setValue(network->Tolerance.getValue());
    fitTolerance->setValue(network->FitTolerance.getValue());
    filling = false;
}

void NetworkPanel::update()
{
    auto network = object();
    if (!network) {
        return;
    }
    network->recomputeFeature();
    vp->highlightCurves(true);
    bool valid = network->isValid();
    status->setText(valid ? QString() : QString::fromUtf8(network->getStatusString()));
    readout->setText(valid ? tr("Widest crossing %1 mm, fit within %2 mm")
                                 .arg(network->MaxGap.getValue(), 0, 'g', 3)
                                 .arg(network->MaxFitDeviation.getValue(), 0, 'g', 3)
                           : QString());
    QStringList lines;
    for (const auto& line : network->Achieved.getValues()) {
        lines << QString::fromStdString(line);
    }
    achieved->setText(valid ? lines.join(QLatin1Char('\n')) : QString());
    if (markers) {
        markers->place();
    }
}

void NetworkPanel::startPick(Pick mode, int side, bool on)
{
    if (!on) {
        if (pick == mode && (mode != Pick::Support || pickSide == side)) {
            exitPick();
        }
        return;
    }
    // one pick at a time: the others' buttons go up
    QToolButton* buttons[] = {addProfile, addGuide, remove};
    Pick modes[] = {Pick::Profile, Pick::Guide, Pick::Remove};
    for (int i = 0; i < 3; ++i) {
        if (modes[i] != mode) {
            QSignalBlocker block(buttons[i]);
            buttons[i]->setChecked(false);
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (mode != Pick::Support || i != side) {
            QSignalBlocker block(sides[i].pick);
            sides[i].pick->setChecked(false);
        }
    }
    if (pick != Pick::None) {
        Gui::Selection().rmvSelectionGate();
    }
    Gui::Selection().clearSelection();
    Gui::Selection().addSelectionGate(new Gate(this));
    // after the gate: a gate replaced resets pick as it goes
    pick = mode;
    pickSide = side;
}

void NetworkPanel::exitPick()
{
    Gui::Selection().clearSelection();
    Gui::Selection().rmvSelectionGate();
    pickSide = -1;
    for (auto b : {addProfile, addGuide, remove}) {
        QSignalBlocker block(b);
        b->setChecked(false);
    }
    for (auto& side : sides) {
        QSignalBlocker block(side.pick);
        side.pick->setChecked(false);
    }
}

void NetworkPanel::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto network = object();
    if (pick == Pick::None || !network || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    Gui::SelectionObject sel(msg);
    App::DocumentObject* obj = sel.getObject();
    if (!obj) {
        return;
    }
    std::string sub = msg.pSubName ? msg.pSubName : "";
    // as picked, unresolved: what import needs to tell the scope
    const App::SubObjectT picked = pickOf(msg);
    auto refers = [&](App::DocumentObject* o, const std::string& s) {
        return (o == obj && (s == sub || s.empty())) || refersTo(o, s, picked);
    };
    checkOpenCommand();
    switch (pick) {
        case Pick::Profile:
        case Pick::Guide: {
            auto& prop = pick == Pick::Profile ? network->Profiles : network->Guides;
            auto objs = prop.getValues();
            auto subs = prop.getSubValues();
            for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
                if (refersTo(objs[i], subs[i], picked)) {
                    // listed already, through a binder
                    return;
                }
            }
            // as a sketch takes external geometry: directly in scope, else
            // through a binder in the network's body
            auto link = importPick(network, picked);
            objs.push_back(link.getObject());
            subs.push_back(link.getSubName());
            prop.setValues(objs, subs);
            break;
        }
        case Pick::Remove:
            for (auto prop : {&network->Profiles, &network->Guides}) {
                auto objs = prop->getValues();
                auto subs = prop->getSubValues();
                for (std::size_t i = 0; i < objs.size(); ++i) {
                    if (refers(objs[i], subs[i])) {
                        objs.erase(objs.begin() + i);
                        subs.erase(subs.begin() + i);
                        prop->setValues(objs, subs);
                        break;
                    }
                }
            }
            break;
        case Pick::Support:
            if (pickSide >= 0) {
                auto link = importPick(network, picked);
                sides[pickSide].props.support->setValue(link.getObject(), {link.getSubName()});
                // one face each: the pick ends there
                QTimer::singleShot(0, this, [this] {
                    exitPick();
                });
            }
            break;
        default:
            break;
    }
    fillLists();
    fillSides();
    update();
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

void NetworkPanel::removeCurrent()
{
    auto network = object();
    if (!network || !lastList || !lastList->currentItem()) {
        return;
    }
    int i = lastList->currentItem()->data(Qt::UserRole).toInt();
    auto& prop = lastList == profiles ? network->Profiles : network->Guides;
    auto objs = prop.getValues();
    auto subs = prop.getSubValues();
    if (i < 0 || i >= (int)objs.size()) {
        return;
    }
    checkOpenCommand();
    objs.erase(objs.begin() + i);
    subs.erase(subs.begin() + i);
    prop.setValues(objs, subs);
    fillLists();
    update();
}

void NetworkPanel::moveCurve()
{
    auto network = object();
    if (!network) {
        return;
    }
    // the list with a selected curve; both, the one picked in last
    QListWidget* from = nullptr;
    for (auto list : {profiles, guides}) {
        if (!list->selectedItems().isEmpty() && (!from || list == lastList)) {
            from = list;
        }
    }
    if (!from) {
        return;
    }
    int i = from->selectedItems().front()->data(Qt::UserRole).toInt();
    auto& source = from == profiles ? network->Profiles : network->Guides;
    auto& target = from == profiles ? network->Guides : network->Profiles;
    auto objs = source.getValues();
    auto subs = source.getSubValues();
    if (i < 0 || i >= (int)objs.size()) {
        return;
    }
    checkOpenCommand();
    auto tobjs = target.getValues();
    auto tsubs = target.getSubValues();
    tobjs.push_back(objs[i]);
    tsubs.push_back(subs[i]);
    objs.erase(objs.begin() + i);
    subs.erase(subs.begin() + i);
    source.setValues(objs, subs);
    target.setValues(tobjs, tsubs);
    fillLists();
    update();
}

void NetworkPanel::separate()
{
    auto network = object();
    if (!network) {
        return;
    }
    checkOpenCommand();
    auto error = network->separateFamilies();
    if (!error.empty()) {
        status->setText(QString::fromStdString(error));
        return;
    }
    fillLists();
    update();
}

void NetworkPanel::applyToAll()
{
    if (!object()) {
        return;
    }
    checkOpenCommand();
    auto continuity = sides[0].props.continuity->getValue();
    auto weight = sides[0].props.weight->getValue();
    for (int i = 1; i < 4; ++i) {
        sides[i].props.continuity->setValue(continuity);
        sides[i].props.weight->setValue(weight);
    }
    fillSides();
    update();
}

bool NetworkPanel::accept()
{
    if (pick != Pick::None) {
        exitPick();
    }
    auto network = object();
    if (!network) {
        return true;
    }
    if (network->mustExecute()) {
        network->recomputeFeature();
    }
    if (!network->isValid()) {
        // said in the panel, not in a dialog: a dialog's event loop would
        // stop a serving process for every browser it serves
        status->setText(tr("Not built: %1").arg(QString::fromUtf8(network->getStatusString())));
        return false;
    }
    vp->highlightCurves(false);
    return true;
}

bool NetworkPanel::reject()
{
    if (pick != Pick::None) {
        exitPick();
    }
    vp->highlightCurves(false);
    return true;
}

// ----------------------------------------------------------------------------

TaskNetwork::TaskNetwork(ViewProviderNetwork* vp, Surface::Network* obj)
    : docName(obj->getDocument()->getName())
{
    widget = new NetworkPanel(vp, obj);
    auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Surface_Network"),
                                          widget->windowTitle(),
                                          true,
                                          nullptr);
    box->groupLayout()->addWidget(widget);
    Content.push_back(box);
}

void TaskNetwork::open()
{
    widget->open();
}

bool TaskNetwork::accept()
{
    if (!widget->accept()) {
        return false;
    }
    Gui::Command::commitCommand();
    resetEdit();
    return true;
}

bool TaskNetwork::reject()
{
    widget->reject();
    Gui::Command::abortCommand();
    resetEdit();
    return true;
}

void TaskNetwork::resetEdit()
{
    // the network's own document: a served one has no window, and so is
    // never the active one
    if (Gui::Application::Instance->getDocument(docName.c_str())) {
        Gui::Command::doCommand(Gui::Command::Gui,
                                "Gui.getDocument('%s').resetEdit()",
                                docName.c_str());
        Gui::Command::doCommand(Gui::Command::App,
                                "App.getDocument('%s').recompute()",
                                docName.c_str());
    }
}

}  // namespace SurfaceGui

#include "moc_TaskNetwork.cpp"
