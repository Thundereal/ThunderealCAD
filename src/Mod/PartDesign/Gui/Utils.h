/***************************************************************************
 *   Copyright (C) 2015 Alexander Golubev (Fat-Zer) <fatzer2@gmail.com>    *
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

#ifndef UTILS_H_CS5LK2ZQ
#define UTILS_H_CS5LK2ZQ

#include <functional>
#include <set>
#include <string>
#include <vector>
#include <QColor>
#include <QStringList>

#include <Base/Vector3D.h>

#include "Workbench.h"
#include <Gui/ActiveObjectList.h>

class QTreeWidget;
class QTreeWidgetItem;
class QByteArray;
class QGridLayout;
class QCheckBox;
class QComboBox;
class QMenu;
class QObject;
class QWidget;

/** \file PartDesign/Gui/Utils.h
 *  This file contains some utility function used over PartDesignGui module
 */
namespace PartDesign {
    class Body;
    class DressUp;
    class Feature;
}

namespace Part {
    class TopoShape;
}

namespace Gui {
    class GizmoContainer;
    namespace TaskView {
        class TaskDialog;
    }
}

namespace App {
    class Document;
    class DocumentObject;
    class Part;
    class PropertyLinkSub;
}

namespace Sketcher {
    class SketchObject;
}

namespace PartDesignGui {

/// Activate edit mode of the given object;  mode is the view provider's
/// edit mode, 0 for its task panel
bool setEdit(App::DocumentObject *obj, App::DocumentObject *container=0,
             const char *key=PDBODYKEY, int mode=0);

/// Call before editing object to setup visibilities
void beforeEdit(App::DocumentObject *editingObj);

/** Shows a PartDesign task dialog (Gui::Control().showDialog()), its
 * widgets then able to shrink with the panel (fitTaskWidgets())
 */
void showTaskDialog(Gui::TaskView::TaskDialog *dlg);
/** Gives the buttons, check boxes, spin boxes, combo boxes and labels under
 * \a root a minimum width of a few letters, where Qt's would be their whole
 * text (a check box), the style's (75 px a push button on Windows) or the
 * widest value (144 px a quantity spin box), so that a task panel narrows
 * with its dock. A wider one never; a label's own minimum stays (one set to
 * line up a column of them). A container whose policy keeps it at its
 * preferred width or more is made able to shrink.
 */
void fitTaskWidgets(QWidget *root);

/// Show the given object on top
void showObjectOnTop(const App::SubObjectT &objT);
void hideObjectOnTop(const App::SubObjectT &objT);
void highlightObjectOnTop(const App::SubObjectT &objT);
void selectObjectOnTop(const App::SubObjectT &objT, bool multiselect=false);
void unselectObjectOnTop(const App::SubObjectT &objT);
void toggleShowOnTop(Gui::ViewProviderDocumentObject *vp,
                     App::SubObjectT &last,
                     const char *prop,
                     bool init = false);
void toggleShowOnTop(Gui::ViewProviderDocumentObject *vp,
                     std::vector<App::SubObjectT> &last,
                     const char *prop,
                     bool init = false);

/** A reference a panel found again by its name's history when it was
 * opened (Part::Feature::getRelatedElements()): the element the feature it
 * is of has no more, as it was called, and the one that answers for it.
 * Ruled 2026-11-11: taken in the panel, never in a recompute, and shown --
 * marked where the panel lists it, selected in the view, said in a line --
 * not taken silently; OK keeps it, Cancel leaves the reference missing.
 */
struct RefoundReference
{
    App::DocumentObject *object = nullptr;
    std::string was;
    std::string now;
};
/// The colour such a reference is marked in where it is listed
QColor refoundColor();
/// What its row's, its cell's or its label's tip says
QString refoundTip(const std::string &was);
/// What a panel's line says of those it found, empty for none
QString refoundNote(const std::vector<RefoundReference> &found);
/// Their elements selected in the view, on the features they are of: in
/// place of the selection, or added to it
void selectRefound(const std::vector<RefoundReference> &found, bool add = false);
/** The elements that answer for a reference \a obj has no more, by its
 * mapped name: none of \a used. A list's reference takes them all -- an
 * edge a feature cut in two is its two halves; one that holds a single
 * element (\a single) takes the answer only where there is one.
 */
std::vector<std::string> refoundAnswers(App::DocumentObject *obj, const std::string &mapped,
                                        const std::set<std::string> &used, bool single);

/** Fill a panel's list with a property's references. Filled for the first
 * time (not refresh), a reference whose element the base has no more is
 * looked up by its name's history and what answers taken in its place, all
 * of it (refoundAnswers()); with no answer it is listed as missing. What
 * was taken is told in refound, for the panel to show. Returns whether the
 * property was changed.
 */
bool populateGeometryReferences(QTreeWidget *treeWidget, App::PropertyLinkSub &prop, bool refresh,
                                std::vector<RefoundReference> *refound = nullptr);
QByteArray getGeometryItemText(QTreeWidgetItem *item);
QByteArray getGeometryItemReference(QTreeWidgetItem *item);
void setGeometryItemText(QTreeWidgetItem *item, const std::string &);
void setGeometryItemReference(QTreeWidgetItem *item, const std::string &);

/** @name A feature's operations in a handle's menu
 * (docs/OnViewValueEditing.md 3.3, entry 170): each sets the panel's own
 * field after the menu has closed, as the recompute it starts moves the
 * handles, and only while \a context lives
 */
//@{
/// A check box of the panel as a checkable entry
void addToggleAction(QMenu &menu, const QString &title, bool checked, bool enabled,
                     QObject *context, std::function<void(bool)> set);
/// A combo box's choices as a sub-menu, the current one checked
void addChoicesMenu(QMenu &menu, const QString &title, const QStringList &items, int current,
                    QObject *context, std::function<void(int)> choose);
/// The texts of a combo box's choices
QStringList comboItems(const QComboBox *combo);
//@}

/// Import an external feature into the body of the host feature using SubShapeBinder
App::SubObjectT importExternalObject(const App::SubObjectT &feature,
                                     bool report = true,
                                     bool wholeObject = true,
                                     bool noSubElement = false);
/// Import a feature with sub-element (Wire or Face) using SubShapeBinder
App::SubObjectT importExternalElement(App::SubObjectT feature, bool report=true);

/// Import multiple features with sub-elements using a single SubShapeBinder
bool importExternalElements(App::PropertyLinkSub &prop,
                            const std::vector<App::SubObjectT> &sobjs,
                            bool report=true);

/// Return active body or show a warning message
PartDesign::Body *getBody(bool messageIfNot, bool autoActivate=true, bool assertModern=true,
        App::DocumentObject **topParent=nullptr, std::string *subname=nullptr);

PartDesign::Body *getBody(App::SubObjectT &sobjT, 
        bool messageIfNot=true, bool autoActivate=true, bool assertModern=true);

/// Display a dialog to select or create a Body object when none is active
PartDesign::Body * needActiveBodyMessage(App::Document *doc,
                                         const QString& infoText=QString());

/**
 * Set given body active, and return pointer to it.
 * \param body the pointer to the body
 * \param doc the pointer to the document in question
 * \param topParent and
 * \param subname to be passed under certain circumstances
 *        (currently only subshapebinder)
 */
PartDesign::Body * makeBodyActive(App::DocumentObject *body, App::Document *doc,
                                  App::DocumentObject **topParent=nullptr,
                                  std::string *subname=nullptr);

/// Display error when there are existing Body objects, but none are active
void needActiveBodyError();

/// Create a Body object in doc, set it active, and return pointer to it
PartDesign::Body * makeBody(App::Document *doc);

/**
 * Finds a body for the given feature. And shows a message if not found
 * Also unlike Body::findBodyFor it checks if the active body has the feature first.
 */
PartDesign::Body *getBodyFor(const App::DocumentObject*, bool messageIfNot,
                             bool autoActivate=true, bool assertModern=true,
                             App::DocumentObject **topParent=nullptr, std::string *subname=nullptr);

App::Part        *getPartFor(const App::DocumentObject*, bool messageIfNot);
App::Part        *getActivePart(App::DocumentObject **topParent=0, std::string *subname=0);

/// Fix sketch support after moving a free sketch into a body
void fixSketchSupport(Sketcher::SketchObject* sketch);

/**
 * Returns true if document has any non-PartDesign objects that links to the given object.
 * If respectGroups is true don't count links from App::GeoFeatureGroup-derived objects (default is false)
 */
bool isAnyNonPartDesignLinksTo ( PartDesign::Feature *feature, bool respectGroups=false );

/// Relink all nonPartDesign features to the body instead of the given partDesign Feature
void relinkToBody ( PartDesign::Feature *feature );

/// Check if feature is dependent on anything except movable sketches and datums
bool isFeatureMovable(App::DocumentObject* feature);
/// Collect dependencies of the features during the move. Dependencies should only be dependent on origin
std::vector<App::DocumentObject*> collectMovableDependencies(std::vector<App::DocumentObject*>& features);
/// Relink sketches and datums to target body's origin
void relinkToOrigin(App::DocumentObject* feature, PartDesign::Body* body);

PartDesign::Body *queryCommandOverride();

void initMonitor();

QCheckBox *hookPropertyBool(App::DocumentObject *obj,
                                   const char *propName,
                                   QWidget *widget,
                                   const char *label);

class MonitorProxy: public QObject
{
    Q_OBJECT

protected Q_SLOTS:
    void onPreview(bool);
    void onPreviewTransparency(bool);
    void onEditTimer();
};

QGridLayout *addTaskCheckBox(Gui::ViewProviderDocumentObject *vp, QWidget * widget, int index = 0);

/** Where a dress-up's handle for \a edge of \a baseShape stands, its root,
 * in the gizmos' frame; false for none (the edge's middle is taken then)
 */
using EdgeHandleRoot = std::function<bool(Part::TopoShape& edge, Part::TopoShape& baseShape,
                                          Base::Vector3d& at)>;
/** A red cross in \a container's marks (Gui::GizmoContainer::getMarks) for
 * each edge \a dressUp failed on, at its handle's root, and for each edge it
 * no longer finds, on the face last made for it (docs/HandsOnQueue.md entry
 * 180): fixed size, on top, not picked. Clears what it showed before; none
 * without a container or a dress-up.
 */
void showFailureMarks(Gui::GizmoContainer* container, PartDesign::DressUp* dressUp,
                      const EdgeHandleRoot& root = {});

void fitViewWithDelay(int);

} /* PartDesignGui */

#endif /* end of include guard: UTILS_H_CS5LK2ZQ */
