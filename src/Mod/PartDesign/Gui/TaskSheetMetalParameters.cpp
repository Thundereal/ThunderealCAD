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
#include <cstring>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#endif

#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTransform.h>
#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <App/ElementNamingUtils.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/Inventor/SoFCSwitch.h>
#include <Gui/ViewerContext.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Gui/QuantitySpinBox.h>
#include <App/PropertyLinks.h>
#include <Mod/Part/App/Part2DObject.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/Feature.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>
#include <Mod/PartDesign/App/FeatureSheetMetalRule.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>
#include <Mod/PartDesign/App/SolidBody.h>

#include "TaskSheetMetalParameters.h"
#include "ViewProviderAddSub.h"

using namespace PartDesignGui;

namespace
{

/// Elements of one kind of the feature built on
class ElementGate: public Gui::SelectionGate
{
public:
    ElementGate(App::DocumentObject* base, std::string element)
        : base(base)
        , element(std::move(element))
    {}
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        return obj && obj == base && sub && std::string(sub).rfind(element, 0) == 0;
    }

private:
    App::DocumentObject* base;
    std::string element;
};

/// Whose the flat's hides are among an edit session's swaps: before the
/// edit's own "PartDesign.Shown", so they win where both name an occurrence
constexpr const char* FlatSwaps = "PartDesign.Flat";

QString number(double v)
{
    return QString::number(v, 'g', 6);
}

/// The sketches of obj's body it could take as a profile: none built on it
std::vector<App::DocumentObject*> sketchesFor(App::DocumentObject* obj)
{
    std::vector<App::DocumentObject*> res;
    auto body = obj ? PartDesign::Body::findBodyOf(obj) : nullptr;
    if (!body) {
        return res;
    }
    const auto after = obj->getInListRecursive();
    for (auto child : body->Group.getValues()) {
        if (child->isDerivedFrom<Part::Part2DObject>()
            && std::find(after.begin(), after.end(), child) == after.end()) {
            res.push_back(child);
        }
    }
    return res;
}

}  // namespace

/// A name as words, as the panel says it: MaterialSide is "Material side",
/// Gap1 "Gap 1"
QString PartDesignGui::sheetMetalWords(const char* name)
{
    const QString s = QString::fromLatin1(name);
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        const bool upper = c.isUpper() && (i + 1 >= s.size() || !s[i + 1].isUpper());
        if (i > 0 && (upper || (c.isDigit() && !s[i - 1].isDigit()))) {
            out += QLatin1Char(' ');
            out += c.toLower();
        }
        else {
            out += c;
        }
    }
    return QCoreApplication::translate("PartDesignGui::TaskSheetMetalParameters",
                                       out.toUtf8().constData());
}

TaskSheetMetalParameters::TaskSheetMetalParameters(ViewProvider* vp,
                                                   const std::string& pixmap,
                                                   const QString& title,
                                                   const std::vector<const char*>& values,
                                                   const char* refs,
                                                   const char* element)
    : TaskSheetMetalParameters(vp,
                               pixmap,
                               title,
                               values,
                               refs ? std::vector<SheetMetalReferences> {{refs, element}}
                                    : std::vector<SheetMetalReferences> {})
{}

TaskSheetMetalParameters::TaskSheetMetalParameters(
    ViewProvider* vp,
    const std::string& pixmap,
    const QString& title,
    const std::vector<const char*>& values,
    const std::vector<SheetMetalReferences>& lists)
    : TaskFeatureParameters(vp, nullptr, pixmap, title)
    , Gui::SelectionObserver(true, Gui::ResolveMode::OldStyleElement)
    , names(values)
{
    proxy = new QWidget(this);
    proxy->setObjectName(QStringLiteral("TaskSheetMetalParameters"));
    auto layout = new QVBoxLayout(proxy);

    auto grid = new QGridLayout;
    int row = 0;
    for (const SheetMetalReferences& refs : lists) {
        const int index = static_cast<int>(refLists.size());
        RefList entry {refs};
        const bool face = std::strcmp(refs.element, "Face") == 0;
        const QString kind = refs.single ? (face ? tr("a face") : tr("an edge"))
                                         : (face ? tr("faces") : tr("edges"));
        // several lists each said by name, its buttons named after it; one
        // alone as it always was
        const QString suffix = lists.size() > 1 ? QString::fromLatin1(refs.property) : QString();
        if (lists.size() > 1) {
            grid->addWidget(new QLabel(sheetMetalWords(refs.property), proxy), row++, 0, 1, 2);
        }
        entry.list = new QListWidget(proxy);
        entry.list->setObjectName(QStringLiteral("list") + QString::fromLatin1(refs.property));
        entry.list->setMaximumHeight(refs.single ? 28 : 110);
        if (App::Property* prop = property(refs.property)) {
            entry.list->setToolTip(QString::fromUtf8(prop->getDocumentation()));
        }
        auto remove = new QAction(tr("Remove"), entry.list);
        remove->setShortcut(QKeySequence::Delete);
        entry.list->addAction(remove);
        entry.list->setContextMenuPolicy(Qt::ActionsContextMenu);
        connect(remove, &QAction::triggered, this, [this, index] {
            removeCurrent(index);
        });
        grid->addWidget(entry.list, row++, 0, 1, 2);
        entry.add = new QToolButton(proxy);
        entry.add->setObjectName(QStringLiteral("buttonAdd") + suffix);
        entry.add->setText(refs.single ? tr("Pick") : tr("Add"));
        entry.add->setCheckable(true);
        entry.add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        entry.add->setToolTip(tr("Pick %1 in the view").arg(kind));
        connect(entry.add, &QToolButton::toggled, this, [this, index](bool on) {
            startPick(index, on);
        });
        grid->addWidget(entry.add, row, 0);
        auto removeButton = new QToolButton(proxy);
        removeButton->setObjectName(QStringLiteral("buttonRemove") + suffix);
        removeButton->setText(tr("Remove"));
        removeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        removeButton->setToolTip(tr("Take the one selected in the list out"));
        connect(removeButton, &QToolButton::clicked, this, [this, index] {
            removeCurrent(index);
        });
        grid->addWidget(removeButton, row++, 1);
        refLists.push_back(entry);
    }
    for (const char* name : names) {
        addValue(grid, row++, name);
    }
    layout->addLayout(grid);

    // what was found again by its name when the panel opened (findAgain())
    refoundNote = new QLabel(proxy);
    refoundNote->setObjectName(QStringLiteral("labelRefound"));
    refoundNote->setWordWrap(true);
    refoundNote->setStyleSheet(QStringLiteral("color: #b07000"));
    refoundNote->hide();
    layout->addWidget(refoundNote);

    rule = new QLabel(proxy);
    rule->setObjectName(QStringLiteral("labelRule"));
    rule->setWordWrap(true);
    layout->addWidget(rule);
    status = new QLabel(proxy);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(status);
    // what the feature did of its own accord (a flange's Notice)
    notice = new QLabel(proxy);
    notice->setObjectName(QStringLiteral("labelNotice"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    // the feature folded, laid flat, or both (docs/SheetMetal.md sec 12)
    auto viewRow = new QHBoxLayout;
    auto viewLabel = new QLabel(tr("View"), proxy);
    sheetView = new QComboBox(proxy);
    sheetView->setObjectName(QStringLiteral("comboSheetView"));
    sheetView->addItems({tr("Folded"), tr("Flat"), tr("Both")});
    sheetView->setToolTip(tr("Draw the feature folded, laid flat, or both: the flat moved off "
                             "the part by the sheet's flat object's placement, else beside it"));
    viewLabel->setBuddy(sheetView);
    viewRow->addWidget(viewLabel);
    viewRow->addWidget(sheetView, 1);
    layout->addLayout(viewRow);
    connect(sheetView, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        showSheetView();
    });
    flatStatus = new QLabel(proxy);
    flatStatus->setObjectName(QStringLiteral("labelFlatStatus"));
    flatStatus->setWordWrap(true);
    flatStatus->setStyleSheet(QStringLiteral("color: #d03030"));
    layout->addWidget(flatStatus);

    groupLayout()->addWidget(proxy);
    // what is wrong said after any recompute, not only the panel's own: one
    // that fails tells nothing of the object itself
    if (auto obj = vp ? vp->getObject() : nullptr) {
        recomputed = obj->getDocument()->signalRecomputed.connect(
            [this](const App::Document&, const std::vector<App::DocumentObject*>&) {
                QTimer::singleShot(0, this, [this] {
                    // what a recompute made read only, too
                    for (const auto& entry : boxes) {
                        showValue(entry.first);
                    }
                    showStatus();
                    // the flat laid again, as the feature is now
                    if (sheetView && sheetView->currentIndex() > 0) {
                        showSheetView();
                    }
                });
            });
        // a value set elsewhere -- the property editor, Python -- shown here
        changed = obj->getDocument()->signalChangedObject.connect(
            [this, obj](const App::DocumentObject& o, const App::Property& prop) {
                if (&o != obj || !prop.getName()) {
                    return;
                }
                if (boxes.count(prop.getName())) {
                    showValue(prop.getName());
                }
                else if (std::any_of(refLists.begin(), refLists.end(), [&](const RefList& r) {
                             return std::strcmp(prop.getName(), r.refs.property) == 0;
                         })) {
                    fillList();
                }
            });
    }
    refresh();
    // once the panel is up: what it finds is selected in the view
    QTimer::singleShot(0, this, [this] {
        findAgain();
    });
}

void TaskSheetMetalParameters::findAgain()
{
    auto obj = vp ? vp->getObject() : nullptr;
    if (!obj || !refound.empty()) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < static_cast<int>(refLists.size()); ++i) {
        auto list = references(i);
        App::DocumentObject* base = list ? list->getValue() : nullptr;
        if (!base) {
            continue;
        }
        const Part::TopoShape shape = Part::Feature::getTopoShape(base);
        const auto shadows = list->getShadowSubs();
        std::vector<std::string> subs = list->getSubValues(false);
        if (shape.isNull() || shadows.size() != subs.size()) {
            continue;
        }
        const std::string element = refLists[i].refs.element;
        bool touched = false;
        for (std::size_t k = 0; k < subs.size(); ++k) {
            // by its name: one with none is not looked for
            const std::string& ref = shadows[k].first;
            if (ref.empty() || !shape.getSubShape(ref.c_str(), true).IsNull()) {
                continue;
            }
            std::vector<std::string> candidates;
            for (const auto& related : Part::Feature::getRelatedElements(base, ref.c_str())) {
                std::string name;
                related.index.appendToStringBuffer(name);
                if (name.rfind(element, 0) == 0
                    && std::find(subs.begin(), subs.end(), name) == subs.end()
                    && std::find(candidates.begin(), candidates.end(), name)
                           == candidates.end()) {
                    candidates.push_back(name);
                }
            }
            // one answer, or it is the user's to say
            if (candidates.size() != 1) {
                continue;
            }
            std::string was = subs[k];
            if (was.rfind(Data::missingPrefix(), 0) == 0) {
                was.erase(0, Data::missingPrefix().size());
            }
            refound.push_back({i, was, candidates.front()});
            subs[k] = candidates.front();
            touched = true;
        }
        if (touched) {
            if (!changed) {
                setupTransaction(tr("find references again"));
            }
            changed = true;
            list->setValue(base, subs);
        }
    }
    if (!changed) {
        return;
    }
    fillList();
    recomputeFeature();
    // and lit in the view, on the feature built on
    Gui::Selection().clearSelection();
    for (const Refound& found : refound) {
        auto list = references(found.list);
        App::DocumentObject* base = list ? list->getValue() : nullptr;
        if (base && base->getDocument()) {
            Gui::Selection().addSelection(base->getDocument()->getName(),
                                          base->getNameInDocument(),
                                          found.now.c_str());
        }
    }
}

TaskSheetMetalParameters::~TaskSheetMetalParameters()
{
    if (picking >= 0) {
        Gui::Selection().rmvSelectionGate();
    }
    hideFlat();
}

void TaskSheetMetalParameters::hideFlat()
{
    auto root = vp ? vp->getRoot() : nullptr;
    if (root && flatRoot && root->findChild(flatRoot) >= 0) {
        root->removeChild(flatRoot);
    }
    if (auto gdoc = Gui::Application::Instance->getDocument(sessionDoc.c_str())) {
        if (flatRoot && gdoc->editingRoot()->hasSessionNode(flatRoot)) {
            gdoc->editingRoot()->removeSessionNode(flatRoot);
        }
    }
    restoreChildren();
    sessionDoc.clear();
    if (sheetView) {
        sheetView->setProperty("flatDrawn", false);
        sheetView->setProperty("drawnAlone", false);
    }
}

void TaskSheetMetalParameters::restoreChildren()
{
    if (auto gdoc = Gui::Application::Instance->getDocument(sessionDoc.c_str())) {
        for (auto& objT : hiddenInSession) {
            if (auto obj = objT.getObject()) {
                gdoc->setEditVisibility(FlatSwaps, obj, objT.getSubName().c_str(), -1);
            }
        }
    }
    hiddenInSession.clear();
    for (auto& [objT, children] : keptChildren) {
        auto view = Gui::Application::Instance->getViewProvider(objT.getObject());
        auto modeSwitch = view ? dynamic_cast<SoFCSwitch*>(view->getModeSwitch()) : nullptr;
        if (view && view->getModeSwitch()) {
            view->getModeSwitch()->whichChild = children.first;
        }
        if (modeSwitch) {
            modeSwitch->defaultChild = children.second;
        }
    }
    keptChildren.clear();
    // the edit's preview back, as it was
    if (keptPreview) {
        keptPreview = false;
        if (auto addSub = Base::freecad_dynamic_cast<ViewProviderAddSub>(vp)) {
            addSub->setPreviewDisplayMode(true, keptOccurrence);
        }
    }
}

void TaskSheetMetalParameters::closeFlat()
{
    closing = true;
    hideFlat();
}

void TaskSheetMetalParameters::showSheetView()
{
    const int mode = sheetView && !closing ? sheetView->currentIndex() : 0;
    auto feature = vp ? Base::freecad_dynamic_cast<Part::Feature>(vp->getObject()) : nullptr;
    flatStatus->clear();
    if (mode == 0 || !feature) {
        hideFlat();
        return;
    }
    auto body = PartDesign::Body::findBodyOf(feature);
    auto solid = body ? body->getSolidBody(feature) : nullptr;
    Part::TopoShape shape;
    std::map<std::string, std::string> directions;
    try {
        if (!solid) {
            THROWM(Base::ValueError, "The feature is of no sheet");
        }
        const PartDesign::SheetFlat& flat = solid->flatOf(feature);
        shape = flat.shape;
        directions = flat.directions;
    }
    catch (const Base::Exception& e) {
        flatStatus->setText(tr("It does not lie flat: %1").arg(QString::fromUtf8(e.what())));
    }
    catch (const Standard_Failure& e) {
        flatStatus->setText(
            tr("It does not lie flat: %1").arg(QString::fromUtf8(e.GetMessageString())));
    }
    if (shape.isNull()) {
        hideFlat();
        return;
    }
    if (!flatView) {
        // drawn as the sheet's flat object is, under the feature's view
        flatView = std::make_unique<ViewProviderSheetMetalFlatPattern>();
        // drawn whatever the feature's Visibility, which it does not own
        flatView->forceUpdate();
        flatView->setStatus(Gui::SecondaryView, true);
        flatView->attach(feature);
        // a display mode, as the document gives the views it holds
        flatView->DisplayMode.setValue("Flat Lines");
        flatView->setDisplayMode("Flat Lines");
        flatView->show();
        flatOffset = new SoTransform;
        flatRoot = new SoSeparator;
        auto pick = new SoPickStyle;
        pick->style = SoPickStyle::UNPICKABLE;
        flatRoot->addChild(pick);
        flatRoot->addChild(flatOffset);
        flatRoot->addChild(flatView->getRoot());
    }
    if (auto flatObj = solid->flatPattern()) {
        if (auto look = Base::freecad_dynamic_cast<ViewProviderSheetMetalFlatPattern>(
                Gui::Application::Instance->getViewProvider(flatObj))) {
            flatView->copyLook(*look);
        }
    }
    flatView->showFlat(shape, directions);
    // shown once it has a shape to show; placed by the feature's view it is
    // drawn under, not again by its own
    flatView->show();
    if (auto transform = flatView->getRoot()->getNumChildren() > 0
            ? dynamic_cast<SoTransform*>(flatView->getRoot()->getChild(0))
            : nullptr) {
        transform->setToDefaults();
    }

    // Both: off the folded part by the flat object's placement, else beside it
    Base::Vector3d move;
    if (mode == 2) {
        auto flatObj = solid->flatPattern();
        const Base::Placement placement =
            flatObj ? flatObj->Placement.getValue() : Base::Placement();
        if (!placement.isIdentity()) {
            move = placement.getPosition();
        }
        else {
            const Base::BoundBox3d folded = feature->Shape.getShape().getBoundBox();
            const Base::BoundBox3d laid = shape.getBoundBox();
            if (folded.IsValid() && laid.IsValid()) {
                const double gap = std::max(10.0, 0.2 * laid.LengthX());
                move.x = folded.MaxX - feature->Placement.getValue().getPosition().x
                    - laid.MinX + gap;
            }
        }
    }
    flatOffset->translation.setValue(static_cast<float>(move.x),
                                     static_cast<float>(move.y),
                                     static_cast<float>(move.z));
    // In an edit session, a node of the session's, where the edit draws
    // what it shows beside its own geometry (the PartDesign preview hangs
    // there too), placed as the edited occurrence: the session's own
    // visibility table can then hide the feature without hiding this.
    // Without one, under the feature's view.
    auto gdoc = Gui::Application::Instance->editDocument();
    Gui::ViewProviderDocumentObject* parentVp = nullptr;
    std::string subname;
    if (gdoc) {
        gdoc->getInEdit(&parentVp, &subname);
    }
    Base::Matrix4D world;
    const bool inSession = gdoc && gdoc->editingViewer() && parentVp
        && gdoc->canSetEditVisibility()
        && parentVp->getObject()->getSubObject(subname.c_str(), nullptr, &world) == feature;
    auto root = vp->getRoot();
    if (inSession) {
        if (root->findChild(flatRoot) >= 0) {
            root->removeChild(flatRoot);
        }
        Gui::EditingRoot* editing = gdoc->editingRoot();
        if (editing->hasSessionNode(flatRoot)) {
            editing->setSessionNodeTransform(flatRoot, world);
        }
        else {
            editing->addSessionNode(flatRoot, world);
        }
        sessionDoc = gdoc->getDocument()->getName();
    }
    else if (root->findChild(flatRoot) < 0) {
        root->addChild(flatRoot);
    }
    // Flat: nothing of the body drawn but the flat -- not the feature, nor
    // what its edit shows with it (the feature it is built on) -- by their
    // views' switches, their visibility untouched
    if (mode == 1) {
        // the edit's preview (the base drawn, the tool tinted over it) taken
        // away first: the session's views draw it, not the views' switches
        auto addSub = Base::freecad_dynamic_cast<ViewProviderAddSub>(vp);
        if (addSub && (addSub->isPreviewMode() || addSub->isPreviewInSession())
            && !keptPreview) {
            keptOccurrence = addSub->previewOccurrenceOf();
            keptPreview = true;
            addSub->setPreviewDisplayMode(false);
        }
        if (inSession) {
            // hidden in the session's views: the body's members and the
            // feature, by an owner whose entries come before the edit's own
            // ("PartDesign.Shown", which shows the feature)
            if (hiddenInSession.empty()) {
                const App::SubObjectT occurrence(parentVp->getObject(), subname.c_str());
                const App::SubObjectT bodyT = occurrence.getParent();
                std::vector<App::DocumentObject*> members = body->Group.getValues();
                if (std::find(members.begin(), members.end(), feature) == members.end()) {
                    members.push_back(feature);
                }
                for (auto obj : members) {
                    const App::SubObjectT objT = bodyT.getChild(obj);
                    if (gdoc->setEditVisibility(FlatSwaps, objT.getObject(),
                                                objT.getSubName().c_str(), 0)) {
                        hiddenInSession.push_back(objT);
                    }
                }
            }
        }
        else if (keptChildren.empty()) {
            std::vector<App::DocumentObject*> members = body->Group.getValues();
            if (std::find(members.begin(), members.end(), feature) == members.end()) {
                members.push_back(feature);
            }
            for (auto obj : members) {
                auto view = Gui::Application::Instance->getViewProvider(obj);
                if (!view || !view->getModeSwitch() || !view->isShow()) {
                    continue;
                }
                // and its default child, which a drawing on top (the edited
                // feature's) draws by
                auto modeSwitch = dynamic_cast<SoFCSwitch*>(view->getModeSwitch());
                keptChildren.emplace_back(
                    obj,
                    std::make_pair(view->getModeSwitch()->whichChild.getValue(),
                                   modeSwitch ? modeSwitch->defaultChild.getValue() : -1));
                view->getModeSwitch()->whichChild = SO_SWITCH_NONE;
                if (modeSwitch) {
                    modeSwitch->defaultChild = SO_SWITCH_NONE;
                }
            }
        }
    }
    else {
        restoreChildren();
    }
    // what is drawn, as a check reads it: the flat, and nothing else of the body
    sheetView->setProperty("flatDrawn", true);
    sheetView->setProperty("drawnAlone", mode == 1);
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
    const QString label = sheetMetalWords(name);
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
    else if (auto count = dynamic_cast<App::PropertyInteger*>(prop)) {
        auto spin = new QSpinBox(proxy);
        auto constrained = dynamic_cast<App::PropertyIntegerConstraint*>(count);
        auto range = constrained ? constrained->getConstraints() : nullptr;
        spin->setRange(range ? static_cast<int>(range->LowerBound) : 0,
                       range ? static_cast<int>(range->UpperBound) : 1000000);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, name](int v) {
            if (auto i = dynamic_cast<App::PropertyInteger*>(property(name))) {
                setupTransaction(tr(name));
                i->setValue(v);
                recomputeFeature();
            }
        });
        box = spin;
    }
    else if (auto choice = dynamic_cast<App::PropertyEnumeration*>(prop)) {
        auto combo = new QComboBox(proxy);
        for (const auto& item : choice->getEnumVector()) {
            combo->addItem(sheetMetalWords(item.c_str()));
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
    else if (dynamic_cast<App::PropertyLinkSub*>(prop)) {
        // a sketch of the body, or none: the items are filled by showValue()
        auto combo = new QComboBox(proxy);
        connect(combo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                [this, name, combo](int i) {
                    auto link = dynamic_cast<App::PropertyLinkSub*>(property(name));
                    auto obj = vp ? vp->getObject() : nullptr;
                    if (!link || !obj) {
                        return;
                    }
                    const QByteArray chosen = combo->itemData(i).toByteArray();
                    setupTransaction(tr(name));
                    link->setValue(chosen.isEmpty()
                                       ? nullptr
                                       : obj->getDocument()->getObject(chosen.constData()));
                    recomputeFeature();
                });
        box = combo;
    }
    if (!box) {
        return;
    }
    box->setObjectName(QString::fromLatin1(name));
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

void TaskSheetMetalParameters::showValue(const std::string& name)
{
    auto it = boxes.find(name);
    App::Property* prop = property(name.c_str());
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
    else if (auto i = dynamic_cast<App::PropertyInteger*>(prop)) {
        auto spin = static_cast<QSpinBox*>(box);
        if (spin->value() != i->getValue()) {
            spin->setValue(static_cast<int>(i->getValue()));
        }
    }
    else if (auto e = dynamic_cast<App::PropertyEnumeration*>(prop)) {
        auto combo = static_cast<QComboBox*>(box);
        if (combo->currentIndex() != static_cast<int>(e->getValue())) {
            combo->setCurrentIndex(static_cast<int>(e->getValue()));
        }
    }
    else if (auto b = dynamic_cast<App::PropertyBool*>(prop)) {
        auto check = static_cast<QCheckBox*>(box);
        if (check->isChecked() != b->getValue()) {
            check->setChecked(b->getValue());
        }
    }
    else if (auto link = dynamic_cast<App::PropertyLinkSub*>(prop)) {
        // the body's sketches as they are now, the linked one current
        auto combo = static_cast<QComboBox*>(box);
        combo->clear();
        combo->addItem(tr("None"), QByteArray());
        for (auto sketch : sketchesFor(vp ? vp->getObject() : nullptr)) {
            combo->addItem(QString::fromUtf8(sketch->Label.getValue()),
                           QByteArray(sketch->getNameInDocument()));
        }
        App::DocumentObject* linked = link->getValue();
        const int at =
            linked ? combo->findData(QByteArray(linked->getNameInDocument())) : 0;
        if (at < 0) {
            // linked, but not one of the body's sketches: shown all the same
            combo->addItem(QString::fromUtf8(linked->Label.getValue()),
                           QByteArray(linked->getNameInDocument()));
            combo->setCurrentIndex(combo->count() - 1);
        }
        else {
            combo->setCurrentIndex(at);
        }
    }
}

void TaskSheetMetalParameters::refresh()
{
    for (const auto& entry : boxes) {
        showValue(entry.first);
    }
    fillList();
    showStatus();
}

App::PropertyLinkSub* TaskSheetMetalParameters::references(int index) const
{
    return index >= 0 && index < static_cast<int>(refLists.size())
        ? dynamic_cast<App::PropertyLinkSub*>(property(refLists[index].refs.property))
        : nullptr;
}

App::DocumentObject* TaskSheetMetalParameters::builtOn() const
{
    auto feature = vp ? dynamic_cast<PartDesign::Feature*>(vp->getObject()) : nullptr;
    return feature ? feature->BaseFeature.getValue() : nullptr;
}

void TaskSheetMetalParameters::fillList()
{
    for (int i = 0; i < static_cast<int>(refLists.size()); ++i) {
        auto list = references(i);
        QListWidget* widget = refLists[i].list;
        if (!list) {
            continue;
        }
        QSignalBlocker block(widget);
        widget->clear();
        for (const auto& sub : list->getSubValues()) {
            widget->addItem(QString::fromStdString(sub));
        }
    }
    // those found again by name when the panel opened, while they are
    // listed: marked, each saying what it was, and all said in a line
    QStringList said;
    for (const Refound& found : refound) {
        QListWidget* widget = refLists[found.list].list;
        for (int row = 0; row < widget->count(); ++row) {
            QListWidgetItem* item = widget->item(row);
            if (item->text().toStdString() != found.now) {
                continue;
            }
            item->setForeground(QColor(0xb0, 0x70, 0x00));
            item->setToolTip(tr("Was %1: found again by its name when this panel opened. "
                                "It is lit in the view; OK keeps it, Cancel leaves the "
                                "reference as it was.")
                                 .arg(QString::fromStdString(found.was)));
            said << tr("%1 is now %2")
                        .arg(QString::fromStdString(found.was),
                             QString::fromStdString(found.now));
            break;
        }
    }
    if (refoundNote) {
        refoundNote->setVisible(!said.isEmpty());
        refoundNote->setText(
            said.isEmpty()
                ? QString()
                : tr("Found again by name: %1. Lit in the view and marked in the list: check, "
                     "then OK keeps it, Cancel leaves the reference as it was.")
                      .arg(said.join(QStringLiteral(", "))));
    }
}

void TaskSheetMetalParameters::showStatus()
{
    auto obj = vp ? vp->getObject() : nullptr;
    status->setText(!obj || obj->isValid() ? QString() : QString::fromUtf8(obj->getStatusString()));
    auto said = obj ? dynamic_cast<App::PropertyString*>(obj->getPropertyByName("Notice")) : nullptr;
    notice->setText(said ? QString::fromUtf8(said->getValue()) : QString());
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

void TaskSheetMetalParameters::startPick(int index, bool on)
{
    if (!references(index) || (on ? picking == index : picking != index)) {
        return;
    }
    if (picking >= 0) {
        // one list picks at a time: the other's Add let up
        Gui::Selection().rmvSelectionGate();
        const int was = picking;
        picking = -1;
        if (was != index) {
            QSignalBlocker block(refLists[was].add);
            refLists[was].add->setChecked(false);
        }
    }
    if (on) {
        Gui::Selection().clearSelection();
        Gui::Selection().addSelectionGate(
            new ElementGate(builtOn(), refLists[index].refs.element));
        picking = index;
    }
}

void TaskSheetMetalParameters::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto list = references(picking);
    if (!list || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    const SheetMetalReferences& refs = refLists[picking].refs;
    const std::string element = refs.element;
    App::DocumentObject* base = builtOn();
    App::DocumentObject* obj = msg.Object.getObject();
    const std::string sub = msg.pSubName ? msg.pSubName : "";
    if (!base || obj != base || sub.rfind(element, 0) != 0) {
        return;
    }
    auto subs = list->getSubValues();
    if (refs.single || std::find(subs.begin(), subs.end(), sub) == subs.end()) {
        setupTransaction(element == "Face" ? tr("add a face") : tr("add an edge"));
        if (refs.single) {
            subs.clear();
        }
        subs.push_back(sub);
        list->setValue(base, subs);
        fillList();
        recomputeFeature();
    }
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

void TaskSheetMetalParameters::removeCurrent(int index)
{
    auto list = references(index);
    const int row = list ? refLists[index].list->currentRow() : -1;
    if (!list || row < 0) {
        return;
    }
    auto subs = list->getSubValues();
    if (row >= static_cast<int>(subs.size())) {
        return;
    }
    setupTransaction(std::strcmp(refLists[index].refs.element, "Face") == 0
                         ? tr("remove a face")
                         : tr("remove an edge"));
    subs.erase(subs.begin() + row);
    list->setValue(list->getValue(), subs);
    fillList();
    recomputeFeature();
}

TaskDlgSheetMetalParameters::TaskDlgSheetMetalParameters(ViewProvider* vp,
                                                         TaskSheetMetalParameters* panel)
    : TaskDlgFeatureParameters(vp)
    , panel(panel)
{
    Content.push_back(panel);
}

bool TaskDlgSheetMetalParameters::accept()
{
    // the feature drawn folded again once the panel closes, which outlives
    // the dialog's end
    if (panel) {
        panel->closeFlat();
    }
    return TaskDlgFeatureParameters::accept();
}

bool TaskDlgSheetMetalParameters::reject()
{
    if (panel) {
        panel->closeFlat();
    }
    return TaskDlgFeatureParameters::reject();
}

#include "moc_TaskSheetMetalParameters.cpp"
