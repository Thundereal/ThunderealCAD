/***************************************************************************
 *   Copyright (c) 2026 FreeCAD Project Association                        *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 **************************************************************************/

#include "PreCompiled.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QBoxLayout>
#include <QChildEvent>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFocusFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSizeGrip>
#include <QSpacerItem>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolBox>
#include <QWheelEvent>

#include <algorithm>

#include <App/Application.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include "Control.h"
#include <Gui/MiscParams.h>
#include "Fw/FwImage.h"
#include "Fw/FwPanelMirror.h"
#include "Fw/FwQtView.h"
#include "Fw/FwStore.h"
#include "Fw/FwWidgets.h"
#include "MainWindow.h"
#include "TaskView/TaskDialog.h"
#include "TaskView/TaskEditControl.h"
#include "TaskView/TaskView.h"

using namespace Gui;
using namespace Gui::Fw;

namespace
{
int& panelSerial()
{
    static int n = 0;
    return n;
}

int& widgetSerial()
{
    static int n = 0;
    return n;
}

int& dialogSerial()
{
    static int n = 0;
    return n;
}

/// A dialog button box's button: a push button model that says which
/// standard button it is (`QDialogButtonBox::StandardButton`), so a
/// client keys the dialog's OK, Cancel and Apply by flag.
class PanelButton : public Gui::Fw::QPushButton
{
public:
    explicit PanelButton(Widget* parent)
        : Gui::Fw::QPushButton(parent)
    {
        declare(QStringLiteral("standardButton"), 0);
    }
};

const QSet<QString>& knownSet()
{
    static const QSet<QString> set = [] {
        const QStringList names = knownClasses();
        return QSet<QString>(names.begin(), names.end());
    }();
    return set;
}

/// The class-table name a real widget's meta-object chain lands on
/// (`Gui::PrefCheckBox` -> "QCheckBox", Sketcher's ConstraintView ->
/// "QListWidget", anything else -> "QWidget" in the end).
QString tableClassOf(QWidget* w)
{
    if (qobject_cast<Gui::TaskView::TaskBox*>(w))
        return QStringLiteral("QGroupBox");
    if (qobject_cast<QToolBox*>(w))
        return QStringLiteral("QTabWidget");
    for (const QMetaObject* mo = w->metaObject(); mo; mo = mo->superClass()) {
        const QString name = QString::fromUtf8(mo->className());
        if (knownSet().contains(name))
            return name;
    }
    return QStringLiteral("QWidget");
}

/// Qt's own machinery inside a widget, not the panel's content.  A
/// window is a root of its own (M3), never a child.  A message box
/// names its text, icon and buttons `qt_msgbox*` (`qt_msgboxex_icon_label`
/// among them): those ARE the content.
bool internalChild(QWidget* c)
{
    if (c->isWindow() || qobject_cast<::QMenu*>(c) || qobject_cast<QSizeGrip*>(c)
        || qobject_cast<QFocusFrame*>(c))
        return true;
    const QString name = c->objectName();
    return name.startsWith(QLatin1String("qt_")) && !name.startsWith(QLatin1String("qt_msgbox"));
}

/// Whether a window is a top-level dialog the mirror follows (M3): a
/// `QDialog` that is its own window.
bool topLevelDialog(QWidget* w)
{
    return w && w->isWindow() && qobject_cast<::QDialog*>(w);
}

bool hasContentChildren(QWidget* w)
{
    for (QObject* o : w->children()) {
        if (!o->isWidgetType())
            continue;
        if (!internalChild(static_cast<QWidget*>(o)))
            return true;
    }
    return false;
}

/// Whether the walk goes INTO a widget: a container is walked, a leaf
/// (a control with a model, or a custom-painted widget with nothing
/// inside) is not.
bool isContainer(QWidget* w, const QString& cls)
{
    static const QSet<QString> containers = {
        QStringLiteral("QWidget"),        QStringLiteral("QFrame"),
        QStringLiteral("QGroupBox"),      QStringLiteral("QScrollArea"),
        QStringLiteral("QTabWidget"),     QStringLiteral("QStackedWidget"),
        QStringLiteral("QSplitter"),      QStringLiteral("QDialog"),
        QStringLiteral("QDialogButtonBox"), QStringLiteral("Gui::PrefCheckableGroupBox")};
    if (!containers.contains(cls))
        return false;
    if (cls == QLatin1String("QWidget") || cls == QLatin1String("QFrame"))
        return w->layout() || hasContentChildren(w);
    return true;
}

QString ptrKey(const void* p)
{
    return QString::number(reinterpret_cast<quintptr>(p), 16);
}

QString posKey(const QVariantList& pos)
{
    QStringList parts;
    for (const QVariant& v : pos)
        parts.append(v.toString());
    return parts.join(QLatin1Char(','));
}

/// Write the keys of `values` that differ from the bag as native code
/// would, WITHOUT the backend hearing it: the values come from the real
/// widget, and the bound view would only write them back.  Returns how
/// many keys went out; `writeNs` grows by the time the write took (the
/// store's fan-out included).
int writeDiff(Widget* model, const QVariantMap& values, qint64& writeNs)
{
    QVariantMap diff;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (model->property(it.key()) != it.value())
            diff.insert(it.key(), it.value());
    }
    if (diff.isEmpty())
        return 0;
    QElapsedTimer clock;
    clock.start();
    Backend* backend = model->backend();
    model->setBackend(nullptr);
    model->setProperties(diff, Source::Native);
    model->setBackend(backend);
    writeNs += clock.nsecsElapsed();
    return diff.size();
}

void replayKey(QWidget* w, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

::QDialogButtonBox* buttonBoxOf(QWidget* view)
{
    if (!view)
        return nullptr;
    for (auto ctrl : view->findChildren<Gui::TaskView::TaskEditControl*>())
        if (auto box = ctrl->findChild<::QDialogButtonBox*>())
            return box;
    return nullptr;
}
}  // namespace

/// One walk's bookkeeping: what it visited, what it made (post-order,
/// the announce order), which containers' layouts changed.
struct PanelMirror::Walk
{
    QSet<QWidget*> visited;
    QList<QPair<QWidget*, Widget*>> created;
    QList<Widget*> changed;
};

/// One task dialog up (M5): whose it is, its class, its boxes and button
/// box, and its root
struct PanelMirror::TaskRoot
{
    TaskOwner owner;
    QString dialogClass;
    QList<QPointer<QWidget>> contents;
    QPointer<::QDialogButtonBox> buttons;
    QPointer<Widget> root;
    QString rootId;
};

PanelMirror& PanelMirror::instance()
{
    static PanelMirror mirror;
    return mirror;
}

/// Every push can stop the mirror: a push that fails drops its subscriber,
/// and the last one out stops it (SceneWidgetStream::checkMirror). Torn down
/// from inside start(), start() went on and brought the mirror back up with
/// nobody subscribed; from inside rebuild() or flush(), those went on with
/// the models, the list and the tables stop() had just freed. So a stop
/// asked for while an entry point is on the stack waits for the outermost
/// to leave.
class PanelMirror::BusyScope
{
public:
    explicit BusyScope(PanelMirror& mirror)
        : _mirror(mirror)
    {
        ++_mirror._busy;
    }
    ~BusyScope()
    {
        if (--_mirror._busy == 0 && _mirror._stopPending) {
            _mirror._stopPending = false;
            _mirror.stop();
        }
    }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

private:
    PanelMirror& _mirror;
};

PanelMirror::PanelMirror()
{
    _rebuildTimer.setSingleShot(true);
    _rebuildTimer.setInterval(0);
    connect(&_rebuildTimer, &QTimer::timeout, this, &PanelMirror::rebuild);
    _flushTimer.setSingleShot(true);
    _flushTimer.setInterval(0);
    connect(&_flushTimer, &QTimer::timeout, this, &PanelMirror::flush);
    _grabTimer.setSingleShot(true);
    connect(&_grabTimer, &QTimer::timeout, this, [this]() {
        // the grabs the rate cap held back: the last paint of a burst
        // is what the client should see
        QSet<QWidget*> pending;
        pending.swap(_pendingGrabs);
        for (QWidget* w : pending)
            if (_models.contains(w))
                _dirty.insert(w);
        flush();
    });
    _pollTimer.setSingleShot(false);
    connect(&_pollTimer, &QTimer::timeout, this, &PanelMirror::poll);
    _clock.start();
    _dialogTimer.setSingleShot(true);
    _dialogTimer.setInterval(0);
    connect(&_dialogTimer, &QTimer::timeout, this, [this]() {
        // the tick after a top-level dialog's Show: its tree is complete
        // and laid out (a message box sets its layout up in showEvent)
        QList<QPointer<QWidget>> pending;
        pending.swap(_pendingDialogs);
        for (const auto& w : pending)
            if (w && w->isVisible() && !_roots.contains(w.data()))
                showDialog(w.data());
    });
    _showTimer.setSingleShot(true);
    _showTimer.setInterval(0);
    connect(&_showTimer, &QTimer::timeout, this, [this]() {
        QList<PendingShow> pending;
        pending.swap(_pendingShows);
        for (const PendingShow& p : pending) {
            // that view's dialog, shown by now -- not the one the desktop
            // shows, which a client's never is (M5)
            Gui::TaskView::TaskDialog* dlg = nullptr;
            if (getMainWindow()) {
                for (auto d : Control().shownDialogs()) {
                    if (d->owner() == p.owner)
                        dlg = d;
                }
            }
            if (!dlg)
                continue;
            QList<QWidget*> contents;
            for (const auto& c : p.contents)
                if (c)
                    contents.append(c.data());
            show(QString::fromUtf8(dlg->metaObject()->className()), contents, p.buttons.data(),
                 p.owner);
        }
    });
}

PanelMirror::~PanelMirror() = default;

bool PanelMirror::owns(const QString& id)
{
    return id == listId() || id.startsWith(QLatin1String("panel:"))
        || id.startsWith(QLatin1String("pw:")) || id.startsWith(QLatin1String("dialog:"));
}

QString PanelMirror::panelId() const
{
    return _tasks.isEmpty() ? QString() : _tasks.last()->rootId;
}

QStringList PanelMirror::panelIds() const
{
    QStringList ids;
    for (const TaskRoot* t : _tasks)
        ids.append(t->rootId);
    return ids;
}

TaskOwner PanelMirror::ownerOf(const QString& id) const
{
    auto closing = _closingOwners.constFind(id);
    if (closing != _closingOwners.constEnd())
        return closing.value();
    // up the model tree to the root it is under: a model's QObject parent
    // is its container's model, a root's the list
    for (QObject* o = Store::instance().object(id); o; o = o->parent()) {
        for (const TaskRoot* t : _tasks) {
            if (t->root == o)
                return t->owner;
        }
        for (auto it = _roots.constBegin(); it != _roots.constEnd(); ++it) {
            if (_models.value(it.key()) == o)
                return _dialogOwners.value(it.key());
        }
    }
    return TaskOwner();
}

PanelMirror::TaskRoot* PanelMirror::taskRootOf(const TaskOwner& owner)
{
    for (TaskRoot* t : _tasks) {
        if (t->owner == owner)
            return t;
    }
    return nullptr;
}

PanelMirror::TaskRoot* PanelMirror::taskRootById(const QString& id)
{
    for (TaskRoot* t : _tasks) {
        if (t->rootId == id)
            return t;
    }
    return nullptr;
}

QStringList PanelMirror::dialogIds() const
{
    QStringList ids;
    for (QWidget* w : _rootOrder)
        ids.append(_roots.value(w));
    return ids;
}

Widget* PanelMirror::modelOf(QWidget* widget) const
{
    return _models.value(widget).data();
}

void PanelMirror::start()
{
    if (_running) {
        // a subscriber arriving while a stop waits for an entry point keeps it
        _stopPending = false;
        return;
    }
    BusyScope busy(*this);
    _running = true;
    _rebuilds = 0;
    _list = new Widget;
    _list->setQtClass(QStringLiteral("QWidget"));
    _list->setInitial(QStringLiteral("objectName"), listId());
    new Layout(Layout::VBox, _list);
    Store::instance().adopt(listId(), _list);
    // the top-level dialogs (M3): every Show in the application passes
    // here, a window's is a root's
    if (qApp && !_appFiltered) {
        qApp->installEventFilter(this);
        _appFiltered = true;
    }
    for (QWidget* w : QApplication::topLevelWidgets())
        if (topLevelDialog(w) && w->isVisible())
            _pendingDialogs.append(w);
    if (!_pendingDialogs.isEmpty())
        _dialogTimer.start();
    startPoll();
    if (!getMainWindow())
        return;  // no task view to follow (a test drives `show` itself)
    _connShow = Control().signalShowDialog.connect(
        [this](QWidget* view, std::vector<QWidget*>& contents, const TaskOwner& owner) {
            onShowDialog(view, contents, owner);
        },
        fastsignals::advanced_tag {});
    _connRemove = Control().signalRemoveDialog.connect(
        [this](QWidget*, std::vector<QWidget*>&, const TaskOwner& owner) {
            onRemoveDialog(owner);
        },
        fastsignals::advanced_tag {});
    // every dialog up, each view's (M5), with the button box its page has
    for (Gui::TaskView::TaskDialog* dlg : Control().shownDialogs()) {
        QList<QWidget*> contents;
        for (QWidget* c : dlg->getDialogContent())
            contents.append(c);
        show(QString::fromUtf8(dlg->metaObject()->className()), contents,
             Gui::TaskView::TaskDialogAttorney::getButtonBox(dlg), dlg->owner());
    }
}

void PanelMirror::stop()
{
    if (!_running)
        return;
    if (_busy) {
        _stopPending = true;
        return;
    }
    // its own pushes can ask again: that one runs, finds it stopped, returns
    BusyScope busy(*this);
    _showTimer.stop();
    _pendingShows.clear();
    _connShow.disconnect();
    _connRemove.disconnect();
    _dialogTimer.stop();
    _pendingDialogs.clear();
    if (_appFiltered && qApp) {
        qApp->removeEventFilter(this);
        _appFiltered = false;
    }
    hide();
    while (!_rootOrder.isEmpty())
        closeRoot(_rootOrder.last(), true);
    _running = false;
    _rebuildTimer.stop();
    _flushTimer.stop();
    _pollTimer.stop();
    if (_list) {
        Store::instance().release(listId());
        delete _list.data();
    }
}

void PanelMirror::onShowDialog(QWidget* view, const std::vector<QWidget*>& contents,
                               const TaskOwner& owner)
{
    // the signal comes before the dialog is active and before
    // `modifyStandardButtons` and `open()` ran: walk on the next tick,
    // when the panel is what its view's user sees
    PendingShow p;
    p.owner = owner;
    for (QWidget* c : contents)
        p.contents.append(c);
    p.buttons = buttonBoxOf(view);
    _pendingShows.erase(std::remove_if(_pendingShows.begin(), _pendingShows.end(),
                                       [&owner](const PendingShow& q) { return q.owner == owner; }),
                        _pendingShows.end());
    _pendingShows.append(p);
    _showTimer.start();
}

void PanelMirror::onRemoveDialog(const TaskOwner& owner)
{
    // that view's dialog, of the several there may be (M5)
    _pendingShows.erase(std::remove_if(_pendingShows.begin(), _pendingShows.end(),
                                       [&owner](const PendingShow& q) { return q.owner == owner; }),
                        _pendingShows.end());
    hide(owner);
}

bool PanelMirror::allowed(const QString& dialogClass) const
{
    const QString pref = QString::fromStdString(MiscParams::getPanelMirror()).trimmed();
    if (pref.isEmpty() || pref == QLatin1String("all"))
        return true;
    if (pref == QLatin1String("none"))
        return false;
    for (const QString& name : pref.split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (name.trimmed() == dialogClass)
            return true;
    return false;
}

void PanelMirror::show(const QString& dialogClass, const QList<QWidget*>& contents,
                       ::QDialogButtonBox* buttons, const TaskOwner& owner)
{
    BusyScope busy(*this);
    if (!_running)
        start();
    hide(owner);
    if (!allowed(dialogClass)) {
        Base::Console().Log("PanelMirror: %s not mirrored (Preferences/Fw/PanelMirror)\n",
                            qPrintable(dialogClass));
        return;
    }
    auto t = new TaskRoot;
    t->owner = owner;
    t->dialogClass = dialogClass;
    for (QWidget* c : contents)
        if (c)
            t->contents.append(c);
    t->buttons = buttons;
    t->root = createWidget(QStringLiteral("QDialog"), _list);
    t->root->setQtClass(dialogClass);
    QString title;
    for (const auto& c : t->contents) {
        if (auto box = qobject_cast<Gui::TaskView::TaskBox*>(c.data()))
            title = box->headerText();
        else if (c)
            title = c->windowTitle();
        if (!title.isEmpty())
            break;
    }
    t->root->setInitial(QStringLiteral("windowTitle"), title);
    t->root->setInitial(QStringLiteral("visible"), true);
    t->rootId = QStringLiteral("panel:%1").arg(++panelSerial());
    Store::instance().adopt(t->rootId, t->root, false);
    const QString rootId = t->rootId;
    connect(t->root, &Widget::requested, this,
            [this, rootId](const QString& name, const QVariantList& args) {
                onPanelRequest(rootId, name, args);
            });
    _tasks.append(t);
    rebuild();
}

void PanelMirror::hide()
{
    BusyScope busy(*this);
    while (!_tasks.isEmpty())
        hideRoot(static_cast<int>(_tasks.size()) - 1);
}

void PanelMirror::hide(const TaskOwner& owner)
{
    BusyScope busy(*this);
    for (int i = 0; i < _tasks.size(); ++i) {
        if (_tasks[i]->owner == owner) {
            hideRoot(i);
            return;
        }
    }
}

void PanelMirror::hideRoot(int index)
{
    TaskRoot* t = _tasks.takeAt(index);
    Store& store = Store::instance();
    // a client's own request may have closed the dialog (its reject):
    // the close is the desktop's fact and must reach that client too
    Store::OriginScope scope(0);
    const QString id = t->rootId;
    if (t->root) {
        // the root's close implies the subtree: one message, the children
        // released without one -- to whom the open went
        _closingOwners.insert(id, t->owner);
        store.release(id, true);
        _closingOwners.remove(id);
        releaseDescendants(t->root);
        _signatures.remove(t->root);
        if (_list && _list->layout())
            _list->layout()->removeWidget(t->root);
        delete t->root.data();  // the models are its descendants
    }
    delete t;
    if (_list)
        store.notifyLayout(listId());
    idle();
    Q_EMIT hidden(id);
}

void PanelMirror::idle()
{
    if (active())
        return;
    _rebuildTimer.stop();
    _flushTimer.stop();
    _grabTimer.stop();
    _pictures.clear();
    _grabbedAt.clear();
    _grabbedAs.clear();
    _pendingGrabs.clear();
    _pictureCapTold = false;
    _dirty.clear();
}

// ---- the top-level dialogs (M3) --------------------------------------------------

void PanelMirror::scheduleDialog(QWidget* window)
{
    if (_roots.contains(window))
        return;
    // whose it is: the view being handled as it shows -- a client's
    // request, or the panel slot it ran (docs/TaskPanelPerView.md sec 6)
    if (!_dialogOwners.contains(window))
        _dialogOwners.insert(window, TaskOwner::current());
    for (const auto& w : _pendingDialogs)
        if (w == window)
            return;
    _pendingDialogs.append(window);
    if (!_dialogTimer.isActive())
        _dialogTimer.start();
}

void PanelMirror::showDialog(QWidget* window)
{
    BusyScope busy(*this);
    if (!window || !topLevelDialog(window) || _roots.contains(window))
        return;
    if (!_running)
        start();
    const QString cls = QString::fromUtf8(window->metaObject()->className());
    if (!allowed(cls)) {
        Base::Console().Log("PanelMirror: %s not mirrored (Preferences/Fw/PanelMirror)\n",
                            qPrintable(cls));
        return;
    }
    // a dialog the store already carries as a bound view's widget (a
    // guest form realized by FwQt) is in the store once
    Store& store = Store::instance();
    for (const QString& id : store.ids()) {
        if (FwQt::widgetOf(store.object(id)) == window)
            return;
    }
    _roots.insert(window, QStringLiteral("dialog:%1").arg(++dialogSerial()));
    _rootOrder.append(window);
    rebuild();
}

void PanelMirror::hideDialog(QWidget* window)
{
    BusyScope busy(*this);
    if (_roots.contains(window))
        closeRoot(window, true);
}

void PanelMirror::closeRoot(QWidget* window, bool announce)
{
    const QString id = _roots.take(window);
    _rootOrder.removeAll(window);
    const TaskOwner owner = _dialogOwners.take(window);
    if (id.isEmpty())
        return;
    _closingOwners.insert(id, owner);
    // a client's own click may have dismissed it: the close is the
    // desktop's fact and reaches that client too
    Store::OriginScope scope(0);
    Widget* model = _models.value(window).data();
    if (model && _list && _list->layout())
        _list->layout()->removeWidget(model);
    releaseModel(window, announce);
    _closingOwners.remove(id);
    if (_list)
        Store::instance().notifyLayout(listId());
    idle();
    Q_EMIT hidden(id);
}

void PanelMirror::scheduleRebuild()
{
    if (active() && !_walking && !_rebuildTimer.isActive())
        _rebuildTimer.start();
}

void PanelMirror::scheduleFlush()
{
    if (!_flushTimer.isActive())
        _flushTimer.start();
}

void PanelMirror::markDirty(QWidget* widget)
{
    // a grab renders the widget, which paints: not evidence of a change
    if (!active() || _walking || _grabbing || !_models.contains(widget))
        return;
    _dirty.insert(widget);
    scheduleFlush();
}

bool PanelMirror::eventFilter(QObject* watched, QEvent* event)
{
    if (!watched->isWidgetType())
        return QObject::eventFilter(watched, event);
    auto w = static_cast<QWidget*>(watched);
    if (!_models.contains(w)) {
        // a scroll area (an item view, a text edit) paints its viewport,
        // not itself: the viewport's paint is the view's evidence
        auto area = qobject_cast<QAbstractScrollArea*>(w->parentWidget());
        if (area && area->viewport() == w && _models.contains(area)) {
            w = area;
        }
        else {
            // the application filter (M3): a top-level dialog's Show
            // makes a root on the next tick; everything else is not ours
            if (event->type() == QEvent::Show && topLevelDialog(w))
                scheduleDialog(w);
            return QObject::eventFilter(watched, event);
        }
    }
    if (event->type() == QEvent::Hide && _roots.contains(w)) {
        // a dialog root going down (its done(), a close): the root's close
        closeRoot(w, true);
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
        case QEvent::Paint:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::EnabledChange:
        case QEvent::ToolTipChange:
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::LanguageChange:
        case QEvent::WindowTitleChange:
        case QEvent::ReadOnlyChange:
        case QEvent::PaletteChange:
            markDirty(w);
            break;
        case QEvent::ChildAdded:
        case QEvent::ChildRemoved: {
            QObject* child = static_cast<QChildEvent*>(event)->child();
            if (child && child->isWidgetType())
                scheduleRebuild();
            break;
        }
        case QEvent::LayoutRequest:
            scheduleRebuild();
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

void PanelMirror::startPoll()
{
    const int ms = static_cast<int>(MiscParams::getPanelPollMs());
    if (ms > 0)
        _pollTimer.start(ms);
    else
        _pollTimer.stop();
}

void PanelMirror::poll()
{
    BusyScope busy(*this);
    if (!_running || _walking || _models.isEmpty())
        return;
    // Everything, not just what is dirty -- the point is the widgets no
    // event marked.  writeDiff sends only what differs, so a poll over a
    // panel nothing has touched costs the reads and puts nothing on the
    // wire (docs/Sandbox.md 8.4).
    ++_stats.polls;
    // Over a snapshot, and looked up again inside the loop, for flush()'s
    // reason: a refresh writes to the store and fans out to subscribers,
    // and anything that reaches a slot from there can release a widget --
    // a live iterator into _models would not survive it.
    const QList<QWidget*> widgets = _models.keys();
    for (QWidget* w : widgets) {
        if (_dirty.contains(w))
            continue;  // a dirty widget is the flush's, and is read there
        auto it = _models.find(w);
        if (it == _models.end() || !it.value())
            continue;
        ++_stats.widgetsRead;
        // a picture nothing paints (a page out of sight) is looked at
        // here; one that is painted is grabbed when it is
        refresh(w, it.value(), false, !w->isVisible());
    }
}

void PanelMirror::flush()
{
    BusyScope busy(*this);
    QSet<QWidget*> dirty;
    dirty.swap(_dirty);
    if (dirty.isEmpty())
        return;
    ++_stats.flushes;
    for (QWidget* w : dirty) {
        auto it = _models.find(w);
        if (it != _models.end() && it.value()) {
            ++_stats.widgetsRead;
            refresh(w, it.value(), false);
        }
    }
}

QVariantMap PanelMirror::stats() const
{
    QVariantMap m;
    m.insert(QStringLiteral("flushes"), _stats.flushes);
    m.insert(QStringLiteral("polls"), _stats.polls);
    m.insert(QStringLiteral("widgetsRead"), _stats.widgetsRead);
    m.insert(QStringLiteral("keysRead"), _stats.keysRead);
    m.insert(QStringLiteral("keysWritten"), _stats.keysWritten);
    m.insert(QStringLiteral("readUs"), _stats.readNs / 1000);
    m.insert(QStringLiteral("writeUs"), _stats.writeNs / 1000);
    m.insert(QStringLiteral("rebuilds"), _stats.rebuilds);
    m.insert(QStringLiteral("walkUs"), _stats.walkNs / 1000);
    m.insert(QStringLiteral("grabs"), _stats.grabs);
    m.insert(QStringLiteral("grabUs"), _stats.grabNs / 1000);
    m.insert(QStringLiteral("models"), static_cast<qint64>(_models.size()));
    return m;
}

void PanelMirror::resetStats()
{
    _stats = Stats();
}

// ---- the walk --------------------------------------------------------------------

void PanelMirror::withoutBackends(const std::function<void()>& fn)
{
    // a model's layout is rebuilt from the real one: the bound view must
    // not apply the ops to the real layout it came from
    QList<QPair<QPointer<Widget>, Backend*>> saved;
    for (auto it = _models.constBegin(); it != _models.constEnd(); ++it) {
        if (it.value()) {
            saved.append({it.value(), it.value()->backend()});
            it.value()->setBackend(nullptr);
        }
    }
    fn();
    for (const auto& pair : saved) {
        if (pair.first)
            pair.first->setBackend(pair.second);
    }
}

void PanelMirror::rebuild()
{
    BusyScope busy(*this);
    if (!active())
        return;
    _walking = true;
    ++_rebuilds;
    ++_stats.rebuilds;
    Store& store = Store::instance();
    Store::OriginScope scope(0);  // the opens are the desktop's, whoever caused them
    QList<TaskRoot*> newTasks;
    for (TaskRoot* t : _tasks) {
        if (t->root && !_signatures.contains(t->root))
            newTasks.append(t);
    }
    Walk w;
    QElapsedTimer walkClock;
    walkClock.start();
    withoutBackends([&]() { walk(w); });
    _stats.walkNs += walkClock.nsecsElapsed();

    // the opens, referenced before referrer: the created list is in
    // post-order, and the root comes last
    QStringList newDialogs;
    for (const auto& pair : w.created) {
        const QString id = store.idOf(pair.second);
        if (!id.isEmpty())
            store.announceOpen(id);
        if (_roots.contains(pair.first))
            newDialogs.append(_roots.value(pair.first));
    }
    for (TaskRoot* t : newTasks)
        store.announceOpen(t->rootId);
    for (Widget* container : w.changed) {
        const QString id = store.idOf(container);
        if (!id.isEmpty())
            store.notifyLayout(id);
    }
    // what went: after the layout updates that stopped naming it
    QList<QWidget*> gone;
    for (auto it = _models.constBegin(); it != _models.constEnd(); ++it)
        if (!w.visited.contains(it.key()))
            gone.append(it.key());
    for (QWidget* real : gone)
        releaseModel(real, true);
    for (TaskRoot* t : newTasks)
        _list->layout()->addWidget(t->root);
    for (const auto& pair : w.created) {
        if (_roots.contains(pair.first))
            _list->layout()->addWidget(pair.second);
    }
    if (!newTasks.isEmpty() || !newDialogs.isEmpty())
        store.notifyLayout(listId());
    _walking = false;
    for (TaskRoot* t : newTasks)
        Q_EMIT shown(t->rootId);
    for (const QString& id : newDialogs)
        Q_EMIT shown(id);
    Q_EMIT rebuilt();
}

void PanelMirror::walk(Walk& w)
{
    bool isNew = false;
    // each task dialog up (M5)
    for (TaskRoot* t : _tasks) {
        Widget* root = t->root.data();
        if (!root)
            continue;
        QStringList signature;
        auto lay = new Layout(Layout::VBox);
        for (const auto& c : t->contents) {
            if (!c)
                continue;
            Widget* m = mirrorWidget(w, c.data(), root, isNew);
            lay->addWidget(m);
            signature.append(QStringLiteral("box:") + ptrKey(m));
        }
        if (t->buttons) {
            Widget* m = mirrorWidget(w, t->buttons.data(), root, isNew);
            lay->addWidget(m);
            signature.append(QStringLiteral("buttons:") + ptrKey(m));
        }
        const bool rootNew = !_signatures.contains(root);
        if (rootNew || _signatures.value(root) != signature) {
            Layout* old = root->layout();
            root->setLayout(lay);
            delete old;
            _signatures.insert(root, signature);
            if (!rootNew)
                w.changed.append(root);
        }
        else {
            delete lay;
        }
    }
    // the top-level dialogs (M3): each a root walked from its window,
    // whose real layout is the root's
    for (QWidget* window : _rootOrder)
        mirrorWidget(w, window, _list, isNew);
}

Widget* PanelMirror::newModel(QWidget* real, Widget* parentModel, bool picture)
{
    Widget* model = nullptr;
    auto box = qobject_cast<::QDialogButtonBox*>(real->parentWidget());
    auto button = qobject_cast<::QAbstractButton*>(real);
    if (box && button) {
        model = new PanelButton(parentModel);
        model->setInitial(QStringLiteral("standardButton"),
                          static_cast<int>(box->standardButton(button)));
    }
    else if (picture) {
        // a leaf the table does not know, with nothing inside: what it
        // paints, as a label's pixmap (M2)
        model = createWidget(QStringLiteral("QLabel"), parentModel);
        _pictures.insert(real);
    }
    else {
        model = createWidget(tableClassOf(real), parentModel);
    }
    // the real class name, which the DOM keys its rendering on -- except
    // a box, whose subclasses (a workbench's TaskBox-derived panel) are
    // all the one box to a client
    if (qobject_cast<Gui::TaskView::TaskBox*>(real))
        model->setQtClass(QStringLiteral("Gui::TaskView::TaskBox"));
    else
        model->setQtClass(QString::fromUtf8(real->metaObject()->className()));
    model->setInitial(QStringLiteral("objectName"), real->objectName());
    return model;
}

Widget* PanelMirror::mirrorWidget(Walk& w, QWidget* real, Widget* parentModel, bool& isNew)
{
    w.visited.insert(real);
    Widget* model = _models.value(real).data();
    isNew = !model;
    const QString cls = tableClassOf(real);
    const bool container = isContainer(real, cls);
    if (!model) {
        bool picture = !container && cls == QLatin1String("QWidget");
        if (picture && _pictures.size() >= maxPictures()) {
            if (!_pictureCapTold) {
                _pictureCapTold = true;
                Base::Console().Log("PanelMirror: more than %d picture leaves, the rest are bare\n",
                                    maxPictures());
            }
            picture = false;
        }
        model = newModel(real, parentModel, picture);
        _models.insert(real, model);
        // a dialog root carries the id minted at its show (M3)
        QString id = _roots.value(real);
        if (id.isEmpty())
            id = QStringLiteral("pw:%1").arg(++widgetSerial());
        Store::instance().adopt(id, model, false);
    }
    else if (model->parentWidget() != parentModel) {
        // moved between containers: the model tree follows, silently
        // (the real move already happened)
        model->QObject::setParent(parentModel);
    }
    if (container) {
        QStringList signature;
        Layout* old = model->layout();
        buildContent(w, real, model, signature);
        if (isNew || _signatures.value(model) != signature) {
            delete old;
            _signatures.insert(model, signature);
            if (!isNew)
                w.changed.append(model);
        }
        else {
            // unchanged: keep the layout that was sent
            Layout* fresh = model->layout();
            model->setLayout(old);
            delete fresh;
        }
    }
    if (isNew) {
        bindModel(real, model);
        refresh(real, model, true);
        watchWidget(real);
        w.created.append({real, model});
    }
    return model;
}

void PanelMirror::buildContent(Walk& w, QWidget* real, Widget* model, QStringList& signature)
{
    Layout* lay = nullptr;
    bool childIsNew = false;
    auto addPage = [&](QWidget* page, const QString& tag) {
        if (!page)
            return;
        Widget* m = mirrorWidget(w, page, model, childIsNew);
        lay->addWidget(m);
        signature.append(tag + ptrKey(m));
    };
    if (auto box = qobject_cast<::QDialogButtonBox*>(real)) {
        lay = new Layout(Layout::HBox);
        for (::QAbstractButton* b : box->buttons())
            addPage(b, QStringLiteral("button:"));
    }
    else if (auto tabs = qobject_cast<::QTabWidget*>(real)) {
        lay = new Layout(Layout::VBox);
        for (int i = 0; i < tabs->count(); ++i)
            addPage(tabs->widget(i), QStringLiteral("page:"));
    }
    else if (auto toolbox = qobject_cast<QToolBox*>(real)) {
        lay = new Layout(Layout::VBox);
        for (int i = 0; i < toolbox->count(); ++i)
            addPage(toolbox->widget(i), QStringLiteral("page:"));
    }
    else if (auto stack = qobject_cast<::QStackedWidget*>(real)) {
        lay = new Layout(Layout::VBox);
        for (int i = 0; i < stack->count(); ++i)
            addPage(stack->widget(i), QStringLiteral("page:"));
    }
    else if (auto splitter = qobject_cast<::QSplitter*>(real)) {
        lay = new Layout(splitter->orientation() == Qt::Horizontal ? Layout::HBox : Layout::VBox);
        for (int i = 0; i < splitter->count(); ++i)
            addPage(splitter->widget(i), QStringLiteral("page:"));
    }
    else if (auto scroll = qobject_cast<::QScrollArea*>(real)) {
        lay = new Layout(Layout::VBox);
        addPage(scroll->widget(), QStringLiteral("content:"));
    }
    else {
        QLayout* realLayout = nullptr;
        bool strays = true;
        if (auto box = qobject_cast<Gui::TaskView::TaskBox*>(real)) {
            realLayout = box->groupLayout();
            strays = false;  // the header and the fold machinery are chrome
        }
        else {
            realLayout = real->layout();
        }
        if (realLayout)
            lay = buildLayout(w, realLayout, model, signature);
        else
            lay = new Layout(Layout::VBox);
        if (strays) {
            // a child no layout holds (a container built by hand)
            for (QObject* o : real->children()) {
                if (!o->isWidgetType())
                    continue;
                auto c = static_cast<QWidget*>(o);
                if (internalChild(c) || w.visited.contains(c))
                    continue;
                addPage(c, QStringLiteral("child:"));
            }
        }
    }
    model->setLayout(lay);
}

Layout* PanelMirror::buildLayout(Walk& w, QLayout* real, Widget* owner, QStringList& signature)
{
    Layout::Kind kind = Layout::VBox;
    auto grid = qobject_cast<QGridLayout*>(real);
    auto form = qobject_cast<QFormLayout*>(real);
    auto box = qobject_cast<QBoxLayout*>(real);
    if (grid)
        kind = Layout::Grid;
    else if (form)
        kind = Layout::Form;
    else if (box)
        kind = (box->direction() == QBoxLayout::LeftToRight
                || box->direction() == QBoxLayout::RightToLeft)
            ? Layout::HBox
            : Layout::VBox;
    auto lay = new Layout(kind);
    lay->setObjectName(real->objectName());
    const QMargins m = real->contentsMargins();
    lay->setContentsMargins(m.left(), m.top(), m.right(), m.bottom());
    if (real->spacing() >= 0)
        lay->setSpacing(real->spacing());
    signature.append(QStringLiteral("lay:%1:%2:%3,%4,%5,%6:%7")
                         .arg(lay->className(), real->objectName())
                         .arg(m.left())
                         .arg(m.top())
                         .arg(m.right())
                         .arg(m.bottom())
                         .arg(real->spacing()));
    if (grid) {
        QVariantList colStretch, rowStretch, colMin;
        bool anyCol = false, anyRow = false, anyMin = false;
        for (int c = 0; c < grid->columnCount(); ++c) {
            colStretch.append(grid->columnStretch(c));
            colMin.append(grid->columnMinimumWidth(c));
            anyCol = anyCol || grid->columnStretch(c);
            anyMin = anyMin || grid->columnMinimumWidth(c);
        }
        for (int r = 0; r < grid->rowCount(); ++r) {
            rowStretch.append(grid->rowStretch(r));
            anyRow = anyRow || grid->rowStretch(r);
        }
        if (anyCol)
            lay->setExtra(QStringLiteral("columnStretch"), colStretch);
        if (anyRow)
            lay->setExtra(QStringLiteral("rowStretch"), rowStretch);
        if (anyMin)
            lay->setExtra(QStringLiteral("columnMinimumWidth"), colMin);
        if (grid->horizontalSpacing() != grid->verticalSpacing()) {
            lay->setExtra(QStringLiteral("horizontalSpacing"), grid->horizontalSpacing());
            lay->setExtra(QStringLiteral("verticalSpacing"), grid->verticalSpacing());
        }
        signature.append(QStringLiteral("gridx:") + posKey(colStretch) + QLatin1Char('/')
                         + posKey(rowStretch) + QLatin1Char('/') + posKey(colMin));
    }
    bool childIsNew = false;
    for (int i = 0; i < real->count(); ++i) {
        QLayoutItem* item = real->itemAt(i);
        if (!item)
            continue;
        QVariantList pos;
        if (grid) {
            int row = 0, col = 0, rs = 1, cs = 1;
            grid->getItemPosition(i, &row, &col, &rs, &cs);
            pos = QVariantList {row, col, rs, cs};
        }
        else if (form) {
            int row = 0;
            QFormLayout::ItemRole role = QFormLayout::FieldRole;
            form->getItemPosition(i, &row, &role);
            pos = QVariantList {row, static_cast<int>(role)};
        }
        const int stretch = box ? box->stretch(i) : 0;
        const int align = static_cast<int>(item->alignment());
        if (QWidget* cw = item->widget()) {
            if (internalChild(cw))
                continue;
            Widget* cm = mirrorWidget(w, cw, owner, childIsNew);
            lay->addWidget(cm, pos);
            lay->setItemStretch(lay->count() - 1, stretch);
            lay->setItemAlignment(lay->count() - 1, align);
            signature.append(QStringLiteral("w:%1@%2#%3/%4")
                                 .arg(ptrKey(cm), posKey(pos))
                                 .arg(stretch)
                                 .arg(align));
        }
        else if (QLayout* sub = item->layout()) {
            signature.append(QStringLiteral("sub@%1#%2/%3(").arg(posKey(pos)).arg(stretch).arg(align));
            Layout* sm = buildLayout(w, sub, owner, signature);
            lay->addLayout(sm, pos);
            lay->setItemStretch(lay->count() - 1, stretch);
            lay->setItemAlignment(lay->count() - 1, align);
            signature.append(QStringLiteral(")"));
        }
        else if (QSpacerItem* sp = item->spacerItem()) {
            const QSize hint = sp->sizeHint();
            const int hp = static_cast<int>(sp->sizePolicy().horizontalPolicy());
            const int vp = static_cast<int>(sp->sizePolicy().verticalPolicy());
            lay->addSpacer(hint.width(), hint.height(), pos, hp, vp);
            signature.append(QStringLiteral("sp:%1,%2,%3,%4@%5")
                                 .arg(hint.width())
                                 .arg(hint.height())
                                 .arg(hp)
                                 .arg(vp)
                                 .arg(posKey(pos)));
        }
    }
    return lay;
}

// ---- binding, watching, reading ------------------------------------------------------

void PanelMirror::bindModel(QWidget* real, Widget* model)
{
    FwQt::View* view = FwQt::View::bind(model, real);
    // an item view's rows are the panel's: reflected, not owned (M2)
    if (view && qobject_cast<::QAbstractItemView*>(real))
        view->reflectItems();
    connect(model, &Widget::propertiesChanged, this,
            [this, real, model](const QStringList& names, int source) {
                onModelWritten(real, model, names, source);
            });
    connect(model, &Widget::requested, this,
            [this, real, model](const QString& name, const QVariantList& args) {
                onModelRequest(real, model, name, args);
            });
}

void PanelMirror::watchWidget(QWidget* real)
{
    if (_watched.contains(real))
        return;
    _watched.insert(real);
    real->installEventFilter(this);
    if (auto area = qobject_cast<QAbstractScrollArea*>(real))
        area->viewport()->installEventFilter(this);
    connect(real, &QObject::destroyed, this, [this, real]() {
        _watched.remove(real);
        if (_roots.contains(real))
            closeRoot(real, true);  // a dialog deleted while up: its close
        else
            releaseModel(real, false);
        scheduleRebuild();
    });
    if (auto box = qobject_cast<Gui::TaskView::TaskBox*>(real))
        connect(box, &Gui::TaskView::TaskBox::toggledExpansion, this,
                [this, real]() { markDirty(real); });
    if (auto toolbox = qobject_cast<QToolBox*>(real))
        connect(toolbox, &QToolBox::currentChanged, this, [this, real](int) { markDirty(real); });
}

void PanelMirror::unwatchWidget(QWidget* real)
{
    if (!_watched.remove(real))
        return;
    real->removeEventFilter(this);
    if (auto area = qobject_cast<QAbstractScrollArea*>(real))
        area->viewport()->removeEventFilter(this);
    disconnect(real, nullptr, this, nullptr);
}

void PanelMirror::releaseModel(QWidget* real, bool announce)
{
    auto it = _models.find(real);
    if (it == _models.end())
        return;
    QPointer<Widget> model = it.value();
    _models.erase(it);
    _dirty.remove(real);
    forgetPicture(real);
    if (_watched.contains(real))
        unwatchWidget(real);
    if (!model)
        return;
    _signatures.remove(model.data());
    Store& store = Store::instance();
    // the descendants first, quietly: a client drops them with this one
    releaseDescendants(model.data());
    const QString id = store.idOf(model.data());
    if (!id.isEmpty())
        store.release(id, announce);
    delete model.data();
}

void PanelMirror::releaseDescendants(Widget* model)
{
    Store& store = Store::instance();
    for (Widget* child : model->findChildren<Widget*>()) {
        const QString cid = store.idOf(child);
        if (!cid.isEmpty())
            store.release(cid, false);
        _signatures.remove(child);
        QWidget* childReal = _models.key(child, nullptr);
        if (childReal) {
            _models.remove(childReal);
            _dirty.remove(childReal);
            forgetPicture(childReal);
            unwatchWidget(childReal);
        }
    }
}

QVariantMap PanelMirror::read(QWidget* real, Widget* model) const
{
    static const QSet<QString> skip = {
        QStringLiteral("visible"),     QStringLiteral("font"),        QStringLiteral("focus"),
        QStringLiteral("actions"),     QStringLiteral("watchEvents"), QStringLiteral("menu"),
        QStringLiteral("forAction"),   QStringLiteral("defaultAction"),
        QStringLiteral("standardButton")};
    QStringList keys;
    for (const QString& key : model->propertyNames())
        if (!skip.contains(key))
            keys.append(key);
    QVariantMap v = FwQt::View::readProperties(real, keys);
    v.insert(QStringLiteral("visible"), !real->isHidden());
    QVariantMap font;
    if (real->font().bold())
        font.insert(QStringLiteral("bold"), true);
    if (real->font().italic())
        font.insert(QStringLiteral("italic"), true);
    v.insert(QStringLiteral("font"), font);
    if (auto combo = qobject_cast<::QComboBox*>(real)) {
        QStringList items;
        for (int i = 0; i < combo->count(); ++i)
            items.append(combo->itemText(i));
        v.insert(QStringLiteral("items"), items);
    }
    else if (auto tabs = qobject_cast<::QTabWidget*>(real)) {
        QStringList names;
        for (int i = 0; i < tabs->count(); ++i)
            names.append(tabs->tabText(i));
        v.insert(QStringLiteral("tabs"), names);
    }
    else if (auto toolbox = qobject_cast<QToolBox*>(real)) {
        QStringList names;
        for (int i = 0; i < toolbox->count(); ++i)
            names.append(toolbox->itemText(i));
        v.insert(QStringLiteral("tabs"), names);
        v.insert(QStringLiteral("currentIndex"), toolbox->currentIndex());
    }
    else if (auto box = qobject_cast<Gui::TaskView::TaskBox*>(real)) {
        v.insert(QStringLiteral("title"), box->headerText());
        v.insert(QStringLiteral("checkable"), box->isExpandable());
        v.insert(QStringLiteral("checked"), box->isGroupVisible());
        v.insert(QStringLiteral("flat"), !box->hasHeader());
    }
    else if (auto view = qobject_cast<::QAbstractItemView*>(real)) {
        // the header, which no Q_PROPERTY carries
        if (QAbstractItemModel* m = view->model()) {
            const int cols = m->columnCount();
            QStringList labels;
            for (int c = 0; c < cols; ++c)
                labels.append(m->headerData(c, Qt::Horizontal).toString());
            v.insert(QStringLiteral("columnCount"), std::max(cols, 1));
            v.insert(QStringLiteral("columns"), labels);
        }
    }
    return v;
}

bool PanelMirror::shownInDialog(QWidget* real) const
{
    if (real->isVisible())
        return true;
    for (const TaskRoot* task : _tasks) {
        for (const auto& box : task->contents) {
            if (box && (box.data() == real || box->isAncestorOf(real)))
                return !box->isHidden() && (box.data() == real || real->isVisibleTo(box));
        }
    }
    return false;
}

QString PanelMirror::grabPicture(QWidget* real)
{
    if (!shownInDialog(real))
        return QString();
    const qint64 now = _clock.elapsed();
    const qint64 last = _grabbedAt.value(real, -grabIntervalMs());
    if (now - last < grabIntervalMs()) {
        // too soon after the last one (a blinking caret would stream at
        // the blink rate): once more when the interval is up
        _pendingGrabs.insert(real);
        if (!_grabTimer.isActive())
            _grabTimer.start(static_cast<int>(grabIntervalMs() - (now - last)));
        return QString();
    }
    _grabbedAt.insert(real, now);
    ++_grabs;
    ++_stats.grabs;
    QElapsedTimer grabClock;
    grabClock.start();
    // A hidden widget is laid out by the render itself: Qt lays out its
    // hidden ancestors as if shown first, so the size is asked after.
    _grabbing = true;
    QPixmap pixmap;
    for (int pass = 0; pass < 2; ++pass) {
        if (real->width() <= 0 || real->height() <= 0)
            break;
        if (pixmap.size() == real->size())
            break;
        // at 1x whatever the screen's ratio; the size capped
        pixmap = QPixmap(real->size());
        pixmap.fill(Qt::transparent);
        real->render(&pixmap, QPoint(), QRegion(), QWidget::DrawChildren);
    }
    _grabbing = false;
    if (pixmap.isNull() || pixmap.size() != real->size()) {
        _stats.grabNs += grabClock.nsecsElapsed();
        return QString();
    }
    if (pixmap.width() > maxPictureSide() || pixmap.height() > maxPictureSide())
        pixmap = pixmap.scaled(maxPictureSide(), maxPictureSide(), Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    // a look that finds what the last one did keeps its id, unencoded:
    // the poll looks at what nothing paints (shownInDialog)
    const QImage image = pixmap.toImage();
    const size_t sum = qHashBits(image.constBits(), static_cast<size_t>(image.sizeInBytes()),
                                 static_cast<size_t>(image.width()) * 31 + image.height());
    auto before = _grabbedAs.constFind(real);
    if (before != _grabbedAs.constEnd() && before->first == sum
        && ImageStore::instance().contains(before->second)) {
        _stats.grabNs += grabClock.nsecsElapsed();
        return before->second;
    }
    const QString id = ImageStore::instance().add(image);
    _grabbedAs.insert(real, {sum, id});
    _stats.grabNs += grabClock.nsecsElapsed();
    return id;
}

void PanelMirror::forgetPicture(QWidget* real)
{
    _pictures.remove(real);
    _grabbedAt.remove(real);
    _pendingGrabs.remove(real);
    _grabbedAs.remove(real);
}

void PanelMirror::refresh(QWidget* real, Widget* model, bool initial, bool allowGrab)
{
    // the re-read's own time: the whole refresh less the grab and the
    // write, which are counted on their own
    const qint64 grabBefore = _stats.grabNs;
    const qint64 writeBefore = _stats.writeNs;
    QElapsedTimer clock;
    clock.start();
    QVariantMap v = read(real, model);
    // what has no name to send and travels by image id (M2)
    if (_pictures.contains(real)) {
        // a poll skips this for a widget on screen: grabPicture() forces a
        // repaint, and a widget nothing is painting has no new picture to
        // give anyway
        if (allowGrab) {
            const QString id = grabPicture(real);
            if (!id.isNull())
                v.insert(QStringLiteral("pixmap"), id);
        }
    }
    else if (auto button = qobject_cast<::QAbstractButton*>(real)) {
        if (!button->icon().isNull())
            v.insert(QStringLiteral("icon"),
                     ImageStore::instance().ofIcon(button->icon(), button->iconSize()));
    }
    else if (auto label = qobject_cast<::QLabel*>(real)) {
        const QPixmap pixmap = label->pixmap();
        if (!pixmap.isNull())
            v.insert(QStringLiteral("pixmap"), ImageStore::instance().ofPixmap(pixmap));
    }
    else if (qobject_cast<::QAbstractItemView*>(real) && !initial) {
        // what no model signal carries: rows hidden by the view, a
        // tree's expansion
        if (FwQt::View* view = FwQt::View::of(model))
            view->syncReflectedRows();
    }
    if (initial) {
        for (auto it = v.constBegin(); it != v.constEnd(); ++it)
            model->setInitial(it.key(), it.value());
    }
    else {
        _stats.keysRead += v.size();
        _stats.keysWritten += writeDiff(model, v, _stats.writeNs);
        _stats.readNs += clock.nsecsElapsed() - (_stats.writeNs - writeBefore)
            - (_stats.grabNs - grabBefore);
    }
}

// ---- a client's writes and requests --------------------------------------------------

void PanelMirror::onModelWritten(QWidget* real, Widget* model, const QStringList& names,
                                 int source)
{
    Q_UNUSED(model)
    if (source != static_cast<int>(Source::Client) || _relaying)
        return;
    // a streamed client's write landed in the real widget: what the
    // widget made of it (a formatted quantity, a clamped value) must
    // reach the writer too, so the widget is re-read outside the
    // writer's origin; and the setters fire `textChanged`, not the
    // edit signals a panel connects.  The bound view hears the relayed
    // signal as the user's edit and writes the (equal) value once more
    // under the same origin: the guard stops it there.
    markDirty(real);
    _relaying = true;
    if (auto edit = qobject_cast<::QLineEdit*>(real)) {
        if (names.contains(QLatin1String("text")))
            Q_EMIT edit->textEdited(edit->text());
    }
    else if (auto combo = qobject_cast<::QComboBox*>(real)) {
        if (names.contains(QLatin1String("currentIndex")) && combo->currentIndex() >= 0)
            Q_EMIT combo->activated(combo->currentIndex());
    }
    _relaying = false;
}

void PanelMirror::onModelRequest(QWidget* real, Widget* model, const QString& name,
                                 const QVariantList& args)
{
    Q_UNUSED(model)
    if (_roots.contains(real)) {
        onDialogRequest(real, name, args);
        return;
    }
    if (name == QLatin1String("mouse") || name == QLatin1String("wheel")) {
        replayMouse(real, name, args);
    }
    else if (name == QLatin1String("click")) {
        if (auto b = qobject_cast<::QAbstractButton*>(real))
            b->click();
    }
    else if (name == QLatin1String("toggle")) {
        if (auto b = qobject_cast<::QAbstractButton*>(real))
            b->toggle();
    }
    else if (name == QLatin1String("editingFinished") || name == QLatin1String("returnPressed")) {
        replayKey(real, Qt::Key_Return);
    }
    else if (name == QLatin1String("escape")) {
        replayKey(real, Qt::Key_Escape);
    }
}

void PanelMirror::replayMouse(QWidget* real, const QString& name, const QVariantList& args)
{
    // a picture is display until here: the client's pointer, replayed
    // into the real widget as the desktop's would arrive.  `mouse`:
    // [type, x, y, button, buttons, modifiers] with type one of press,
    // release, move, dblclick, enter, leave; `wheel`: [x, y, dx, dy,
    // buttons, modifiers].  x, y are the picture's pixels.
    if (!_pictures.contains(real)) {
        Base::Console().Log("PanelMirror: %s replay ignored, %s is not a picture\n",
                            qPrintable(name), qPrintable(real->objectName()));
        return;
    }
    const bool wheel = name == QLatin1String("wheel");
    const int at = wheel ? 0 : 1;
    double x = args.value(at).toDouble();
    double y = args.value(at + 1).toDouble();
    // the grab was scaled down past the side cap: the picture's pixels
    // back to the widget's
    const int side = std::max(real->width(), real->height());
    if (side > maxPictureSide()) {
        const double factor = static_cast<double>(side) / maxPictureSide();
        x *= factor;
        y *= factor;
    }
    const QPointF local(x, y);
    const QPointF global = real->mapToGlobal(local);
    if (wheel) {
        const QPoint delta(args.value(2).toInt(), args.value(3).toInt());
        const auto buttons = static_cast<Qt::MouseButtons>(args.value(4).toInt());
        const auto mods = static_cast<Qt::KeyboardModifiers>(args.value(5).toInt());
        QWheelEvent ev(local, global, QPoint(), delta, buttons, mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(real, &ev);
        return;
    }
    const QString type = args.value(0).toString();
    const auto mods = static_cast<Qt::KeyboardModifiers>(args.value(5).toInt());
    if (type == QLatin1String("enter")) {
        QEnterEvent ev(local, local, global);
        QApplication::sendEvent(real, &ev);
        return;
    }
    if (type == QLatin1String("leave")) {
        QEvent ev(QEvent::Leave);
        QApplication::sendEvent(real, &ev);
        return;
    }
    QEvent::Type kind = QEvent::None;
    if (type == QLatin1String("press"))
        kind = QEvent::MouseButtonPress;
    else if (type == QLatin1String("release"))
        kind = QEvent::MouseButtonRelease;
    else if (type == QLatin1String("move"))
        kind = QEvent::MouseMove;
    else if (type == QLatin1String("dblclick"))
        kind = QEvent::MouseButtonDblClick;
    if (kind == QEvent::None)
        return;
    // the button that caused it (none for a move), the state after it
    auto button = static_cast<Qt::MouseButton>(
        args.value(3, static_cast<int>(Qt::LeftButton)).toInt());
    if (kind == QEvent::MouseMove)
        button = Qt::NoButton;
    Qt::MouseButtons buttons;
    if (args.size() > 4)
        buttons = static_cast<Qt::MouseButtons>(args.value(4).toInt());
    else if (kind == QEvent::MouseButtonRelease || kind == QEvent::MouseMove)
        buttons = Qt::NoButton;
    else
        buttons = button;
    QMouseEvent ev(kind, local, local, global, button, buttons, mods);
    QApplication::sendEvent(real, &ev);
}

void PanelMirror::onDialogRequest(QWidget* window, const QString& name,
                                  const QVariantList& args)
{
    // accept, reject, done and close are the bound view's (it holds the
    // real QDialog); what it does not know is the button box's buttons
    // by flag and role, as the panel root has them
    auto box = window->findChild<::QDialogButtonBox*>();
    if (!box)
        return;
    if (name == QLatin1String("clicked")) {
        ::QAbstractButton* b = box->button(
            static_cast<::QDialogButtonBox::StandardButton>(args.value(0).toInt()));
        if (b)
            b->click();
    }
    else if (name == QLatin1String("helpRequested")) {
        for (::QAbstractButton* b : box->buttons()) {
            if (box->buttonRole(b) == ::QDialogButtonBox::HelpRole) {
                b->click();
                return;
            }
        }
    }
}

void PanelMirror::onPanelRequest(const QString& rootId, const QString& name,
                                 const QVariantList& args)
{
    // the dialog of the root the request names (M5), not the one the
    // desktop shows
    TaskRoot* t = taskRootById(rootId);
    const TaskOwner owner = t ? t->owner : TaskOwner();
    ::QDialogButtonBox* box = t ? t->buttons.data() : nullptr;
    auto clickRole = [box](::QDialogButtonBox::ButtonRole role) {
        if (!box)
            return false;
        for (::QAbstractButton* b : box->buttons()) {
            if (box->buttonRole(b) == role) {
                b->click();
                return true;
            }
        }
        return false;
    };
    if (name == QLatin1String("accept")) {
        if (!clickRole(::QDialogButtonBox::AcceptRole) && getMainWindow()) {
            if (owner.isNull())
                Control().accept();
            else
                Control().accept(owner);
        }
    }
    else if (name == QLatin1String("reject")) {
        if (!clickRole(::QDialogButtonBox::RejectRole) && getMainWindow()) {
            if (owner.isNull())
                Control().reject();
            else
                Control().reject(owner);
        }
    }
    else if (name == QLatin1String("clicked")) {
        ::QAbstractButton* b = box ? box->button(static_cast<::QDialogButtonBox::StandardButton>(
                                       args.value(0).toInt()))
                                 : nullptr;
        if (b)
            b->click();
    }
    else if (name == QLatin1String("helpRequested")) {
        clickRole(::QDialogButtonBox::HelpRole);
    }
}

#include "moc_FwPanelMirror.cpp"
