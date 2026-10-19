// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#ifndef GUI_ONVIEWVALUE_H
#define GUI_ONVIEWVALUE_H

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <fastsignals/signal.h>

#include <Base/Matrix.h>
#include <Base/Vector3D.h>
#include <FCGlobal.h>

class QKeyEvent;
class QMenu;
class QWidget;
class SoNodeSensor;

namespace App
{
class Document;
}

namespace Gui
{

class EditableDatumLabel;
class Gizmo;
class GizmoContainer;
class ViewProvider;
class QuantitySpinBox;
class ViewerContext;

/** The values of a feature in edit, shown and typed in the 3D view.
 *
 * docs/OnViewValueEditing.md, sections 3.1 and 3.2. Each value is held by a
 * QuantitySpinBox -- the panel's own, or a hidden one -- which brings its
 * unit, its limits, its expression and the code that writes the property; a
 * handle (a LinearGizmo, or a RotationGizmo for an angle) may drive the same
 * box. The set adds a label showing
 * the value, and the ACTIVE value: made so by a click on its handle or its
 * label, or by Tab in the view; its handle is highlighted, and while it is
 * active a key typed in the view goes to it:
 *   - a digit, a sign or a decimal point opens its label for typing with it;
 *   - Enter opens its label with the value selected;
 *   - Tab and Shift+Tab move to the next and the previous value;
 *   - Escape leaves no value active.
 *   - Space opens the active handle's menu, as a right click on it does;
 *     with no active value, the edit's menu (the menu button's, entry 173).
 * In the open label (entry 136, the reporter 2026-10-15 10:07 and 10:13):
 * Enter commits (the box gets the value, and with it the property) and the
 * label stays open; Escape drops what was typed since the last Enter and
 * closes it; Tab commits, closes it and makes the next value active; the
 * focus going elsewhere commits and closes it. An open label is one undo
 * step, as a drag of a handle is; Esc or another mouse button during a drag
 * cancels it, the value back to where it began (stepBegun/stepEnded).
 *
 * A value may have several handles -- a fillet's default radius has one on
 * every edge that takes it; making one active highlights them all. A label is
 * either always shown at the top middle of the view (a feature's default
 * value, which stands for no single place) or shown beside its active handle
 * only. A right click on a handle opens the value's menu, and the pointer over
 * it shows a tool tip saying so.
 */
class GuiExport OnViewValueSet: public QObject
{
    Q_OBJECT

public:
    enum class Place
    {
        /// always shown, at the top middle of the view
        TopMiddle,
        /// shown beside its handle while that one is active
        AtHandle,
    };

    /// Fills the menu of a handle of a value
    using MenuProvider = std::function<void(QMenu& menu, Gizmo* handle)>;
    /** Adds to the menu of every set, after the handles' switch and before
     * the settings -- what a module has to say of any edit of its features
     * (PartDesign's "Show preview", docs/OnViewValueEditing.md 8.2)
     */
    using MenuExtension = std::function<void(OnViewValueSet& set, QMenu& menu)>;

    explicit OnViewValueSet(QObject* parent = nullptr);
    ~OnViewValueSet() override;

    /** Add a value; returns its index, which is also its place in the Tab
     * order. \a title leads the label's text ("Radius 2 mm"). A handle is
     * to be added before its container makes it (GizmoContainer::
     * appendGizmo): its menu and tool tip are set up here.
     */
    int addValue(QuantitySpinBox* box, const QString& title, Place place,
                 Gizmo* handle = nullptr);
    /// One more handle of the value at \a index
    void addHandle(int index, Gizmo* handle);
    /// Let go of the handles of the value at \a index, which the caller
    /// deletes after; the value stays
    void clearHandles(int index);
    /// Where a handle stands, in world coordinates: its label goes beside it
    void setHandleAnchor(Gizmo* handle, const Base::Vector3d& world);
    /** Every handle's anchor where the handle stands: its place in its
     * container's frame, which \a toWorld turns into the world's
     * (GizmoContainer::getWorldMatrix). For a feature whose handles stand
     * where their labels go.
     */
    void placeAnchors(const Base::Matrix4D& toWorld);
    /// What the menu of the value at \a index offers
    void setMenu(int index, MenuProvider provider);
    /** The value at \a index is not used by the feature now -- a pad's second
     * length while it has one side: no label, passed over by Tab, and not
     * the active one. Its handles are the feature's to hide.
     */
    void setHidden(int index, bool hidden);
    /** Whether a drag cancelled by Escape or another button writes the
     * value back (the default): the handle puts its box back silently, and
     * the box's signal is given again so that its owner writes the
     * property. Off for an owner that rolls the drag back itself, from
     * stepEnded(true) -- the fillet's held undo step.
     */
    void setCancelWritesBack(bool on)
    {
        cancelWritesBack = on;
    }
    /** The group of the value at \a index, 0 for none: while a value is
     * active, the handles of every value of its group are highlighted too
     * (docs/OnViewValueEditing.md 6.1)
     */
    void setGroup(int index, int group);
    /** Keep the first \a count values, drop the rest -- with their labels,
     * and with the hold on their handles, which the caller deletes after.
     */
    void truncate(int count);
    int count() const;
    /// The value \a handle belongs to, or -1
    int valueOf(const Gizmo* handle) const;

    /// Open the menu of \a handle, of the active handle when null; at \a at
    /// when given, else at the pointer, or at the handle
    void openMenu(Gizmo* handle = nullptr, bool atPointer = false,
                  std::optional<QPoint> at = std::nullopt);
    /// The menu button's click: the menu below the button
    void openMenuBelowButton();
    /** The edit's menu (docs/OnViewValueEditing.md 8.2), one for the right
     * click on a handle, Space and the menu button: the value's own entries
     * when \a handle or the active value has them, "Show handles", what the
     * modules add (addMenuExtension), and the drag "Settings" sub-menu.
     */
    void buildMenu(QMenu& menu, Gizmo* handle);
    static void addMenuExtension(MenuExtension extension);
    /** A key of a view without a widget -- a browser's mirror viewer --
     * offered to the set shown in \a viewer, as the view widget's filter
     * takes the desktop's (docs/OnViewValueEditing.md sec 9.2 #3): Tab and
     * Shift+Tab move the active value, Escape cancels a drag or leaves no
     * value active, Enter and a digit (\a text) open the active value's
     * label for typing. True when taken, its release too.
     */
    static bool takeViewerKey(ViewerContext* viewer, int key,
                              Qt::KeyboardModifiers modifiers, bool press,
                              const QString& text = QString());
    /** The menu of the handle the pointer is over in \a viewer, built into
     * \a menu -- a browser's right click on a handle (sec 9.2 #4), as the
     * view widget's filter opens it on the desktop. False when no set in
     * \a viewer has a handle under the pointer.
     */
    static bool buildViewerMenu(ViewerContext* viewer, QMenu& menu);
    /// What a client covers of \a viewer changed (ViewerContext::
    /// coveredRects): the labels of the sets shown there placed again
    static void viewCoverChanged(ViewerContext* viewer);
    /** A feature of \a doc was recomputed, \a failed or not. During a drag
     * of a handle -- and for the recompute of its last value -- a failure
     * writes the dragged value back to the last that worked, inside the
     * drag's undo step (the user, 2026-10-19)
     */
    static void featureRecomputed(const App::Document* doc, bool failed);
    /** A left press or release in \a viewer, a view with no widget, at
     * \a pos, ahead of the scene: a click on no handle leaves no value
     * active, as on the desktop (entry 159)
     */
    static void viewerLeftButton(ViewerContext* viewer, bool press, const QPoint& pos);
    /** Whether the handles are shown in this set's view (8.5: the view's
     * own, not a preference). Hidden, the values still take Tab, typing and
     * the menu, and an active value's label stands at the top middle.
     */
    bool areHandlesShown() const
    {
        return handlesShown;
    }
    void setHandlesShown(bool shown);
    /// The handle made active last, if its value is still the active one
    Gizmo* getActiveHandle() const;

    /// Show the values in \a view; null takes them out of the view it had.
    void attach(ViewerContext* view);
    /** Show the values in the view \a vp is edited in, place the handles'
     * anchors from \a container (placeAnchors) when there is one, and
     * restate the labels. A panel calls it once made and again after the
     * events of the moment, as the view is told of the edit after the panel
     * is made.
     */
    void attachToEdit(const ViewProvider* vp, const GizmoContainer* container);
    ViewerContext* getView() const;

    /// Restate every label from its box -- after a recompute, say.
    void refresh();

    /// Make the value at \a index the active one; -1 for none. \a handle,
    /// one of its handles, is the one its label goes beside.
    void setActive(int index, Gizmo* handle = nullptr);
    int getActive() const;

    /** Open the label of the value at \a index for typing. \a first, a key
     * typed in the view, is given to it, replacing the selected value.
     */
    bool startTyping(int index, QKeyEvent* first = nullptr);
    /// The index of the value whose label is open, or -1
    int getTyping() const;

    /// Where the default value's label is put: this far below the view's top.
    static constexpr int TopMargin = 28;
    /// Where a handle's label is put: this far right of and above the handle
    static constexpr int HandleOffset = 24;

Q_SIGNALS:
    /// The view's camera moved: a handle that depends on it may move too.
    void cameraChanged();
    void activeChanged(int index);
    /** A change that is one undo step however many writes it makes begins:
     * a drag of a handle, or a label opened for typing (entry 136). Ended
     * by stepEnded(); \a abort when a drag was cancelled, its changes to
     * be rolled back.
     */
    void stepBegun();
    void stepEnded(bool abort);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry;

    void makeLabels();
    void dropLabels();
    void placeLabels();
    void showLabels();
    void updateLabel(int index);
    void highlight();
    void setupHandle(int index, Gizmo* handle);
    void onCamera();
    void onLabelClicked(EditableDatumLabel* label);
    void commit(int index, double value);
    void endTyping(bool keep);
    /// The keys of an open label's box, ahead of the label's own handling
    bool takeLabelKey(QKeyEvent* event);
    /// Make the next or the previous value active, as Tab does
    void moveActive(bool back);
    /// What is typed in the open label, written to its box if it is a value
    /// and not the box's already
    void writeTyped();
    void focusView();
    bool takeKey(QKeyEvent* event, bool overrideOnly);
    bool takeMirrorKey(int key, Qt::KeyboardModifiers modifiers, bool press,
                       const QString& text);
    /// featureRecomputed() for this set's drag
    void dragRecomputed(bool failed);
    /// The view, or null once it is gone: a panel can outlive its view -- a
    /// closed document takes its views before the panel's deferred delete
    ViewerContext* liveView() const;
    /// The "Settings" sub-menu: the drag settings as widgets, each written
    /// to its preference as it changes (entry 171)
    void addSettingsMenu(QMenu& menu);
    /// The menu button's place, at the left of the first top-middle label
    void placeMenuButton(int middle, int labelWidth);
    /// The menu button's side: a label's height
    int menuButtonSide() const;
    /// The middle of the part of the view no overlay covers, in the view
    /// widget's coordinates; the view's own middle when too little is clear
    int clearMiddle(int needed) const;

    std::vector<std::unique_ptr<Entry>> entries;
    /// "Show handles": the values' handles shown in this view
    /// (Gizmo::setHiddenInView)
    bool handlesShown = true;
    /// The always shown menu button (entry 173), on a view with widgets
    QPointer<QWidget> menuButton;
    QMetaObject::Connection coverConnection;
    ViewerContext* view = nullptr;
    /// The view's own handle (ViewerContext::lifetime()), expired with it
    std::weak_ptr<const void> viewLife;
    QPointer<QWidget> keyWidget;
    SoNodeSensor* cameraSensor = nullptr;
    fastsignals::scoped_connection connCameraReplaced;
    std::vector<QMetaObject::Connection> typingConnections;
    /// Keys taken on their press, whose release is taken too: the view acts
    /// on some releases (Escape leaves the edit, ViewProvider::eventCallback)
    std::vector<int> takenKeys;
    int active = -1;
    bool cancelWritesBack = true;
    Gizmo* activeHandle = nullptr;
    int typing = -1;
    /// The handle being dragged, between its drag's start and end
    Gizmo* dragHandle = nullptr;
    /// The box the drag in progress changes, the last value of it that
    /// recomputed, and whether a finished drag's last recompute is still to
    /// come (featureRecomputed())
    QPointer<QuantitySpinBox> dragBox;
    double dragGood = 0.0;
    bool dragSettling = false;
    /// The value whose handle the pointer is over, -1 for none: its handles
    /// and its group's lit as a shape under the pointer is (entry 144)
    int preselected = -1;
    /// A mouse button that cancelled a drag: its release is taken too
    int cancelButton = 0;
    /// The handle a right press was over: its release opens its menu. Taken
    /// at the view widget, ahead of the navigation, which opens the view's
    /// own menu on a right click in some styles before the scene sees it.
    Gizmo* rightPressHandle = nullptr;
    /// A left press on nothing: its release, unmoved, leaves no value
    /// active (entry 159); setActive() counts, so a value made active in
    /// between keeps it
    bool clickMayClear = false;
    QPoint clickPos;
    int clickActivations = 0;
    int activations = 0;
};

}  // namespace Gui

#endif  // GUI_ONVIEWVALUE_H
