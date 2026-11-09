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

#include <QPointer>
#include <QAction>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#endif

#include <App/Document.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Selection.h>
#include <Gui/SelectionObject.h>
#include <Mod/Surface/App/Geom/Trim.h>

#include "TaskTrim.h"


using namespace SurfaceGui;

PROPERTY_SOURCE(SurfaceGui::ViewProviderTrim, PartGui::ViewProviderPart)

namespace SurfaceGui
{

void ViewProviderTrim::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    QAction* act = menu->addAction(QObject::tr("Edit trim"), receiver, member);
    act->setData(QVariant((int)ViewProvider::Default));
    PartGui::ViewProviderPart::setupContextMenu(menu, receiver, member);
}

bool ViewProviderTrim::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    auto obj = static_cast<Surface::Trim*>(getObject());
    if (Gui::TaskView::TaskDialog* dlg = Gui::Control().activeDialog()) {
        if (qobject_cast<TaskTrim*>(dlg)) {
            Gui::Control().showDialog(dlg);
            return true;
        }
        return false;
    }
    Gui::Control().showDialog(new TaskTrim(this, obj));
    return true;
}

void ViewProviderTrim::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        QTimer::singleShot(0, &Gui::Control(), qOverload<>(&Gui::ControlSingleton::closeDialog));
    }
    else {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
    }
}

QIcon ViewProviderTrim::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Surface_Trim");
}

// ----------------------------------------------------------------------------

class TrimPanel::Gate: public Gui::SelectionGate
{
public:
    explicit Gate(TrimPanel* panel)
        : panel(panel)
    {}
    ~Gate() override
    {
        // The panel can be gone first: a served session's own selection
        // deletes the gate after the panel closed (ASan, heap-use-after-free).
        if (panel)
            panel->pick = Pick::None;
    }
    bool allow(App::Document*, App::DocumentObject* obj, const char* sub) override
    {
        if (!panel)
            return false;
        auto trim = panel->object();
        if (!trim || !obj->isDerivedFrom<Part::Feature>()) {
            return false;
        }
        std::string element(sub ? sub : "");
        bool face = element.rfind("Face", 0) == 0;
        switch (panel->pick) {
            case Pick::Keep:
                // a piece of the cut, shown whole while picking
                return obj == trim && face;
            case Pick::Base:
                return obj != trim && (face || element.empty());
            case Pick::Tool:
                return obj != trim
                    && (face || element.empty() || element.rfind("Edge", 0) == 0);
            case Pick::Remove:
                for (auto prop : {&trim->Base, &trim->Tools}) {
                    const auto& objs = prop->getValues();
                    const auto& subs = prop->getSubValues();
                    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
                        if (objs[i] == obj && (subs[i] == element || subs[i].empty())) {
                            return true;
                        }
                    }
                }
                return false;
            default:
                return false;
        }
    }

private:
    QPointer<TrimPanel> panel;
};

// ----------------------------------------------------------------------------

TrimPanel::TrimPanel(ViewProviderTrim* vp, Surface::Trim* obj)
    : vp(vp)
    , objT(obj)
{
    setObjectName(QStringLiteral("SurfaceGui_Trim"));
    setWindowTitle(tr("Trim"));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto cut = new QGroupBox(tr("Cut"), this);
    auto grid = new QGridLayout(cut);
    grid->addWidget(new QLabel(tr("Faces"), cut), 0, 0);
    grid->addWidget(new QLabel(tr("Cut by"), cut), 0, 1);
    bases = new QListWidget(cut);
    bases->setObjectName(QStringLiteral("listBase"));
    tools = new QListWidget(cut);
    tools->setObjectName(QStringLiteral("listTools"));
    for (auto list : {bases, tools}) {
        list->setMinimumHeight(60);
    }
    grid->addWidget(bases, 1, 0);
    grid->addWidget(tools, 1, 1);
    auto makeTool = [&](const char* name, const QString& text, const QString& tip) {
        auto b = new QToolButton(cut);
        b->setObjectName(QLatin1String(name));
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return b;
    };
    addBase = makeTool("buttonAddBase", tr("Add face"), tr("Pick faces to cut"));
    addTool = makeTool("buttonAddTool",
                       tr("Add tool"),
                       tr("Pick curves or surfaces to cut with, or whole objects in the tree"));
    remove = makeTool("buttonRemove", tr("Remove"), tr("Pick what to take out"));
    auto row = new QHBoxLayout;
    for (auto b : {addBase, addTool, remove}) {
        row->addWidget(b);
    }
    grid->addLayout(row, 2, 0, 1, 2);
    auto dirRow = new QHBoxLayout;
    dirRow->addWidget(new QLabel(tr("Direction"), cut));
    const char* axes[3] = {"directionX", "directionY", "directionZ"};
    for (int i = 0; i < 3; ++i) {
        direction[i] = new QDoubleSpinBox(cut);
        direction[i]->setObjectName(QLatin1String(axes[i]));
        direction[i]->setRange(-1e6, 1e6);
        direction[i]->setDecimals(3);
        direction[i]->setToolTip(tr("Along what curves are laid onto the faces; all 0, along "
                                    "the faces' normals"));
        dirRow->addWidget(direction[i]);
    }
    grid->addLayout(dirRow, 3, 0, 1, 2);
    mutual = new QCheckBox(tr("Cut the tools' surfaces too"), cut);
    mutual->setObjectName(QStringLiteral("checkMutual"));
    grid->addWidget(mutual, 4, 0, 1, 2);
    layout->addWidget(cut);

    auto sides = new QGroupBox(tr("Kept"), this);
    auto sgrid = new QGridLayout(sides);
    keep = new QToolButton(sides);
    keep->setObjectName(QStringLiteral("buttonKeep"));
    keep->setText(tr("Pick sides to keep"));
    keep->setToolTip(tr("Every piece of the cut is shown: click one to keep it, or to let it "
                        "go again"));
    keep->setCheckable(true);
    keep->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sgrid->addWidget(keep, 0, 0);
    clear = new QToolButton(sides);
    clear->setObjectName(QStringLiteral("buttonClear"));
    clear->setText(tr("Clear"));
    sgrid->addWidget(clear, 0, 1);
    kept = new QLabel(sides);
    kept->setObjectName(QStringLiteral("labelKept"));
    sgrid->addWidget(kept, 1, 0, 1, 2);
    status = new QLabel(sides);
    status->setObjectName(QStringLiteral("labelStatus"));
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color: #d03030"));
    sgrid->addWidget(status, 2, 0, 1, 2);
    layout->addWidget(sides);

    connect(addBase, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Base, on);
    });
    connect(addTool, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Tool, on);
    });
    connect(remove, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Remove, on);
    });
    connect(keep, &QToolButton::toggled, this, [this](bool on) {
        startPick(Pick::Keep, on);
    });
    connect(clear, &QToolButton::clicked, this, [this] {
        auto trim = object();
        if (!trim) {
            return;
        }
        checkOpenCommand();
        trim->KeepPoints.setValues(std::vector<Base::Vector3d>());
        trim->KeepElements.setValues(std::vector<std::string>());
        update();
    });
    for (auto spin : direction) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
            auto trim = object();
            if (filling || !trim) {
                return;
            }
            checkOpenCommand();
            trim->Direction.setValue(Base::Vector3d(direction[0]->value(),
                                                    direction[1]->value(),
                                                    direction[2]->value()));
            update();
        });
    }
    connect(mutual, &QCheckBox::toggled, this, [this](bool on) {
        auto trim = object();
        if (filling || !trim) {
            return;
        }
        checkOpenCommand();
        trim->Mutual.setValue(on);
        update();
    });

    fill();
    attachDocument(Gui::Application::Instance->getDocument(obj->getDocument()));
}

TrimPanel::~TrimPanel()
{
    if (pick != Pick::None) {
        Gui::Selection().rmvSelectionGate();
    }
}

Surface::Trim* TrimPanel::object() const
{
    return dynamic_cast<Surface::Trim*>(objT.getObject());
}

void TrimPanel::open()
{
    checkOpenCommand();
    Gui::Selection().clearSelection();
    // nothing kept yet: the pick is what there is to do
    auto trim = object();
    if (trim && trim->KeepPoints.getSize() == 0) {
        keep->setChecked(true);
    }
    else {
        update();
    }
}

void TrimPanel::checkOpenCommand()
{
    auto trim = object();
    if (trim && checkCommand && !Gui::Command::hasPendingCommand()) {
        std::string msg("Edit ");
        msg += trim->Label.getValue();
        Gui::Command::openCommand(msg.c_str());
        checkCommand = false;
    }
}

void TrimPanel::slotUndoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void TrimPanel::slotRedoDocument(const Gui::Document&)
{
    checkCommand = true;
}

void TrimPanel::fill()
{
    auto trim = object();
    if (!trim) {
        return;
    }
    filling = true;
    QListWidget* lists[2] = {bases, tools};
    int k = 0;
    for (auto prop : {&trim->Base, &trim->Tools}) {
        auto list = lists[k++];
        list->clear();
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            QString text = QString::fromUtf8(objs[i]->Label.getValue());
            if (!subs[i].empty()) {
                text += QLatin1Char('.') + QString::fromStdString(subs[i]);
            }
            list->addItem(text);
        }
    }
    const auto& d = trim->Direction.getValue();
    direction[0]->setValue(d.x);
    direction[1]->setValue(d.y);
    direction[2]->setValue(d.z);
    mutual->setChecked(trim->Mutual.getValue());
    filling = false;
}

void TrimPanel::update()
{
    auto trim = object();
    if (!trim) {
        return;
    }
    trim->ShowPieces.setValue(pick == Pick::Keep);
    trim->recomputeFeature();
    bool valid = trim->isValid();
    status->setText(valid ? QString() : QString::fromUtf8(trim->getStatusString()));

    // the pieces kept, of every piece; while picking, coloured
    const auto& shape = trim->Shape.getShape();
    std::vector<TopoDS_Face> faces;
    for (int i = 1; valid && pick == Pick::Keep && i <= (int)shape.countSubShapes(TopAbs_FACE);
         ++i) {
        faces.push_back(TopoDS::Face(shape.getSubShape(TopAbs_FACE, i)));
    }
    std::vector<int> nearest;
    for (const auto& p : trim->KeepPoints.getValues()) {
        int i = faces.empty()
            ? -1
            : Surface::Geom::nearestPiece(faces, gp_Pnt(p.x, p.y, p.z), 1e-7);
        if (i >= 0 && std::find(nearest.begin(), nearest.end(), i) == nearest.end()) {
            nearest.push_back(i);
        }
    }
    if (pick == Pick::Keep && !faces.empty()) {
        std::vector<App::Color> colors(faces.size(), vp->ShapeColor.getValue());
        for (int i : nearest) {
            colors[i] = App::Color(0.2f, 0.8f, 0.3f);
        }
        vp->setHighlightedFaces(colors);
        kept->setText(tr("%1 of %2 pieces kept").arg(nearest.size()).arg(faces.size()));
    }
    else {
        vp->unsetHighlightedFaces();
        kept->setText(tr("%1 kept of %2 pieces")
                          .arg(trim->KeepPoints.getSize())
                          .arg(trim->Pieces.getValue()));
    }
}

void TrimPanel::toggleKept(int index, const Base::Vector3d& point)
{
    auto trim = object();
    if (!trim) {
        return;
    }
    const auto& shape = trim->Shape.getShape();
    std::vector<TopoDS_Face> faces;
    for (int i = 1; i <= (int)shape.countSubShapes(TopAbs_FACE); ++i) {
        faces.push_back(TopoDS::Face(shape.getSubShape(TopAbs_FACE, i)));
    }
    auto points = trim->KeepPoints.getValues();
    auto names = trim->KeepElements.getValues();
    names.resize(points.size());
    bool found = false;
    for (std::size_t k = points.size(); k-- > 0;) {
        const auto& p = points[k];
        if (Surface::Geom::nearestPiece(faces, gp_Pnt(p.x, p.y, p.z), 1e-7) == index) {
            points.erase(points.begin() + k);
            names.erase(names.begin() + k);
            found = true;
        }
    }
    if (!found) {
        points.push_back(point);
        names.push_back(shape.getMappedName(Data::IndexedName::fromConst("Face", index + 1))
                            .toString());
    }
    trim->KeepPoints.setValues(points);
    trim->KeepElements.setValues(names);
}

void TrimPanel::startPick(Pick mode, bool on)
{
    if (!on) {
        if (pick == mode) {
            exitPick();
            update();
        }
        return;
    }
    QToolButton* buttons[] = {addBase, addTool, remove, keep};
    Pick modes[] = {Pick::Base, Pick::Tool, Pick::Remove, Pick::Keep};
    for (int i = 0; i < 4; ++i) {
        if (modes[i] != mode) {
            QSignalBlocker block(buttons[i]);
            buttons[i]->setChecked(false);
        }
    }
    if (pick != Pick::None) {
        Gui::Selection().rmvSelectionGate();
    }
    Gui::Selection().clearSelection();
    Gui::Selection().addSelectionGate(new Gate(this));
    pick = mode;
    update();
}

void TrimPanel::exitPick()
{
    Gui::Selection().clearSelection();
    Gui::Selection().rmvSelectionGate();
    for (auto b : {addBase, addTool, remove, keep}) {
        QSignalBlocker block(b);
        b->setChecked(false);
    }
}

void TrimPanel::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    auto trim = object();
    if (pick == Pick::None || !trim || msg.Type != Gui::SelectionChanges::AddSelection) {
        return;
    }
    Gui::SelectionObject sel(msg);
    App::DocumentObject* obj = sel.getObject();
    if (!obj) {
        return;
    }
    std::string sub = msg.pSubName ? msg.pSubName : "";
    checkOpenCommand();
    switch (pick) {
        case Pick::Keep:
            if (sub.rfind("Face", 0) == 0) {
                toggleKept(std::atoi(sub.c_str() + 4) - 1, Base::Vector3d(msg.x, msg.y, msg.z));
            }
            break;
        case Pick::Base:
        case Pick::Tool: {
            auto& prop = pick == Pick::Base ? trim->Base : trim->Tools;
            auto objs = prop.getValues();
            auto subs = prop.getSubValues();
            objs.push_back(obj);
            subs.push_back(sub);
            prop.setValues(objs, subs);
            break;
        }
        case Pick::Remove:
            for (auto prop : {&trim->Base, &trim->Tools}) {
                auto objs = prop->getValues();
                auto subs = prop->getSubValues();
                for (std::size_t i = 0; i < objs.size(); ++i) {
                    if (objs[i] == obj && (subs[i] == sub || subs[i].empty())) {
                        objs.erase(objs.begin() + i);
                        subs.erase(subs.begin() + i);
                        prop->setValues(objs, subs);
                        break;
                    }
                }
            }
            break;
        default:
            break;
    }
    fill();
    update();
    QTimer::singleShot(50, this, [] {
        Gui::Selection().clearSelection();
    });
}

bool TrimPanel::accept()
{
    auto trim = object();
    if (pick != Pick::None) {
        exitPick();
    }
    if (!trim) {
        return true;
    }
    trim->ShowPieces.setValue(false);
    trim->recomputeFeature();
    vp->unsetHighlightedFaces();
    if (!trim->isValid()) {
        // said here, not in a dialog
        status->setText(tr("Not built: %1").arg(QString::fromUtf8(trim->getStatusString())));
        return false;
    }
    return true;
}

bool TrimPanel::reject()
{
    if (pick != Pick::None) {
        exitPick();
    }
    if (auto trim = object()) {
        trim->ShowPieces.setValue(false);
    }
    vp->unsetHighlightedFaces();
    return true;
}

// ----------------------------------------------------------------------------

TaskTrim::TaskTrim(ViewProviderTrim* vp, Surface::Trim* obj)
    : docName(obj->getDocument()->getName())
{
    widget = new TrimPanel(vp, obj);
    auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Surface_Trim"),
                                          widget->windowTitle(),
                                          true,
                                          nullptr);
    box->groupLayout()->addWidget(widget);
    Content.push_back(box);
}

void TaskTrim::open()
{
    widget->open();
}

bool TaskTrim::accept()
{
    if (!widget->accept()) {
        return false;
    }
    Gui::Command::commitCommand();
    resetEdit();
    return true;
}

bool TaskTrim::reject()
{
    widget->reject();
    Gui::Command::abortCommand();
    resetEdit();
    return true;
}

void TaskTrim::resetEdit()
{
    // the trim's own document: a served one is never the active one
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

#include "moc_TaskTrim.cpp"
