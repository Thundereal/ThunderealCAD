// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <App/Animation.h>
#include <App/Application.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <Base/Console.h>

#include "AnimationTimeline.h"
#include "Application.h"
#include "BitmapFactory.h"
#include "Command.h"
#include "DockWindowManager.h"
#include "MainWindow.h"
#include "ViewProviderDocumentObject.h"
#include "propertyeditor/PropertyEditor.h"

FC_LOG_LEVEL_INIT("Animation", true, true)

using namespace Gui;
using namespace Gui::DockWnd;

// --------------------------------------------------------------------------
// Which Animation, and the property editor's marks

namespace
{

/// Sets a flag for its life and puts back what it was
struct Holding
{
    bool& flag;
    bool old;
    explicit Holding(bool& f)
        : flag(f)
        , old(f)
    {
        flag = true;
    }
    ~Holding()
    {
        flag = old;
    }
};

/// The Animation chosen for each document's timeline
std::map<std::string, App::DocumentObjectT>& chosen()
{
    static std::map<std::string, App::DocumentObjectT> map;
    return map;
}

QPointer<AnimationTimeline> theTimeline;

}  // namespace

App::Animation* Gui::activeAnimation(App::Document* doc)
{
    if (!doc) {
        doc = App::GetApplication().getActiveDocument();
    }
    if (!doc) {
        return nullptr;
    }
    auto it = chosen().find(doc->getName());
    if (it != chosen().end()) {
        if (auto anim = freecad_dynamic_cast<App::Animation>(it->second.getObject())) {
            if (anim->isAttachedToDocument()) {
                return anim;
            }
        }
    }
    App::Animation* first = nullptr;
    for (auto obj : doc->getObjectsOfType(App::Animation::getClassTypeId())) {
        auto anim = static_cast<App::Animation*>(obj);
        if (anim->Active.getValue()) {
            return anim;
        }
        if (!first) {
            first = anim;
        }
    }
    return first;
}

void Gui::chooseAnimation(App::Animation* anim)
{
    if (!anim || !anim->getDocument()) {
        return;
    }
    chosen()[anim->getDocument()->getName()] = App::DocumentObjectT(anim);
    if (theTimeline) {
        QMetaObject::invokeMethod(theTimeline, [] {
            if (theTimeline) {
                theTimeline->onUpdate();
            }
        }, Qt::QueuedConnection);
    }
}

std::string Gui::animationPath(const App::Property& prop, const App::ObjectIdentifier* sub)
{
    auto container = prop.getContainer();
    if (!container || !prop.getName()) {
        return {};
    }
    std::string res;
    if (auto obj = freecad_dynamic_cast<App::DocumentObject>(container)) {
        if (!obj->isAttachedToDocument()) {
            return {};
        }
        res = obj->getNameInDocument();
    }
    else if (auto vp = freecad_dynamic_cast<ViewProviderDocumentObject>(container)) {
        auto obj = vp->getObject();
        if (!obj || !obj->isAttachedToDocument()) {
            return {};
        }
        res = std::string(obj->getNameInDocument()) + ".ViewObject";
    }
    else {
        return {};
    }
    res += ".";
    res += prop.getName();
    if (sub && sub->numSubComponents() > 1) {
        res += sub->getSubPathStr();
    }
    return res;
}

QColor Gui::animationKeyMark(const App::Property& prop, const App::ObjectIdentifier* sub,
                             bool* driven)
{
    if (driven) {
        *driven = false;
    }
    auto container = prop.getContainer();
    App::DocumentObject* obj = freecad_dynamic_cast<App::DocumentObject>(container);
    if (!obj) {
        if (auto vp = freecad_dynamic_cast<ViewProviderDocumentObject>(container)) {
            obj = vp->getObject();
        }
    }
    if (!obj || !obj->isAttachedToDocument() || !prop.getName()) {
        return {};
    }
    auto anim = activeAnimation(obj->getDocument());
    if (!anim) {
        return {};
    }
    // Cheap first: a track naming the property at all
    bool any = false;
    for (auto& t : anim->Tracks.getValues()) {
        const std::string& name = t.target.getPropertyName();
        if (name == prop.getName() || name == "ViewObject") {
            any = true;
            break;
        }
    }
    if (!any) {
        return {};
    }
    std::string path = animationPath(prop, sub);
    if (path.empty()) {
        return {};
    }
    try {
        auto target = anim->parsePath(path);
        if (driven) {
            // Written by the keys and by something else: an expression, a
            // solver (docs/Animation.md 3.11)
            if (auto track = anim->Tracks.findTrack(target)) {
                auto state = anim->state(track->id);
                *driven = state == App::AnimationTrackState::Driven
                    || state == App::AnimationTrackState::Solved;
            }
        }
        switch (anim->keyState(target)) {
        case App::Animation::KeyState::Keyed:
            return {0xf2, 0xd0, 0x4a};  // a key here: yellow
        case App::Animation::KeyState::Animated:
            return {0xa6, 0xd8, 0x8a};  // animated: green
        case App::Animation::KeyState::Changed:
            return {0xf2, 0x9c, 0x46};  // changed, not keyed: orange
        default:
            break;
        }
    }
    catch (Base::Exception&) {
    }
    return {};
}

// --------------------------------------------------------------------------
// AnimationPlayer

AnimationPlayer* AnimationPlayer::instance()
{
    static AnimationPlayer* player = new AnimationPlayer;
    return player;
}

AnimationPlayer::AnimationPlayer()
{
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &AnimationPlayer::tick);
}

App::Animation* AnimationPlayer::animation() const
{
    return freecad_dynamic_cast<App::Animation>(anim.getObject());
}

void AnimationPlayer::play(App::Animation* a)
{
    if (!a) {
        return;
    }
    if (timer.isActive() && animation() != a) {
        pause();
    }
    anim = App::DocumentObjectT(a);
    double start = a->Start.getValue();
    double end = a->endTime();
    startTime = a->Time.getValue();
    // Once and at the end (or outside the range): from the start again
    if (startTime < start - App::Anim::TimeEpsilon
        || (a->Loop.getValue() == 0 && startTime >= end - App::Anim::TimeEpsilon)) {
        startTime = start;
    }
    a->activate();
    if (std::fabs(a->Time.getValue() - startTime) > App::Anim::TimeEpsilon) {
        a->Time.setValue(startTime);
    }
    double rate = std::max(1.0, a->FrameRate.getValue());
    timer.start(std::max(1, static_cast<int>(std::lround(1000.0 / rate))));
    clock.start();
    Q_EMIT stateChanged();
}

void AnimationPlayer::pause()
{
    timer.stop();
    Q_EMIT stateChanged();
}

void AnimationPlayer::stop()
{
    timer.stop();
    if (auto a = animation()) {
        if (a->Active.getValue()) {
            a->Active.setValue(false);  // the rest values back
        }
        else {
            a->deactivate(true);
        }
    }
    Q_EMIT stateChanged();
}

void AnimationPlayer::tick()
{
    auto a = animation();
    if (!a || !a->isAttachedToDocument()) {
        pause();
        return;
    }
    double start = a->Start.getValue();
    double end = a->endTime();
    double length = end - start;
    if (length <= App::Anim::TimeEpsilon) {
        pause();
        return;
    }
    // By the wall clock: a frame that takes long is skipped past, not
    // played late
    double t = startTime + clock.elapsed() / 1000.0 * a->Speed.getValue();
    bool done = false;
    // A presentation: playing stops at the start of an On click strip, and
    // Play goes on from there (docs/Animation.md sec 7)
    double prev = a->Time.getValue();
    for (double cp : a->clickPoints()) {
        if (prev < cp - App::Anim::TimeEpsilon && t >= cp - App::Anim::TimeEpsilon) {
            t = cp;
            a->Time.setValue(t);
            pause();
            return;
        }
    }
    switch (a->Loop.getValue()) {
    case 1:  // Loop
        if (t > end) {
            t = start + std::fmod(t - start, length);
        }
        break;
    case 2: {  // PingPong
        double u = (t - start) / length;
        double turn = std::floor(u);
        double frac = u - turn;
        t = (static_cast<long>(turn) % 2) ? end - frac * length : start + frac * length;
        break;
    }
    default:  // Once
        if (t >= end) {
            t = end;
            done = true;
        }
        break;
    }
    try {
        a->Time.setValue(t);
    }
    catch (Base::Exception& e) {
        e.ReportException();
        done = true;
    }
    if (done) {
        pause();
    }
}

// --------------------------------------------------------------------------
// AnimationKeysView: the time ruler and a row of keys per track

namespace Gui::DockWnd
{

class AnimationKeysView: public QWidget
{
public:
    explicit AnimationKeysView(AnimationTimeline* owner)
        : QWidget(owner)
        , owner(owner)
    {
        setObjectName(QStringLiteral("AnimationKeys"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(rulerHeight + 3 * rowHeight);
    }

    struct Key
    {
        int id;
        double t;
        App::Anim::Interp interp;
    };
    struct Row
    {
        int track;
        QString label;
        QString tip;
        int depth;
        App::AnimationTrackState state;
        bool mute;
        std::vector<Key> keys;
        bool strip = false;     ///< a strip: a bar from begin to end
        double begin = 0;
        double end = 0;
        QString barText;
    };

    void rebuild()
    {
        rows.clear();
        auto anim = owner->animation();
        if (!anim) {
            selected.clear();
            update();
            return;
        }
        std::vector<const App::AnimationTrack*> tracks;
        for (auto& t : anim->Tracks.getValues()) {
            tracks.push_back(&t);
        }
        // A part under its whole: by path, which puts Box.Placement before
        // Box.Placement.Base.z; otherwise the timeline's order
        // Strips last, in their order, which is what places them
        std::stable_sort(tracks.begin(), tracks.end(), [](auto a, auto b) {
            bool sa = a->kind == "Strip";
            bool sb = b->kind == "Strip";
            if (sa || sb) {
                return !sa && sb;
            }
            return a->pathString() < b->pathString();
        });
        std::map<int, App::Animation::StripSpan> spans;
        for (auto& span : anim->stripSpans()) {
            spans[span.track] = span;
        }
        std::set<std::pair<int, int>> still;
        std::vector<std::string> wholes;
        for (auto t : tracks) {
            Row row;
            row.track = t->id;
            std::string path = t->pathString();
            row.depth = 0;
            for (auto& w : wholes) {
                if (path.compare(0, w.size() + 1, w + ".") == 0) {
                    row.depth = 1;
                    row.label = QStringLiteral("    ") + QString::fromUtf8(path.substr(w.size()).c_str());
                    break;
                }
            }
            if (row.depth == 0) {
                wholes.push_back(path);
                row.label = QString::fromUtf8(path.c_str());
            }
            if (t->kind != "Property") {
                if (auto handler = App::AnimationTrackHandler::get(t->kind)) {
                    row.label = QString::fromUtf8(handler->describe(*t).c_str());
                }
            }
            if (!t->label.empty()) {
                row.label = QString::fromUtf8(t->label.c_str());
            }
            row.state = anim->state(t->id);
            row.mute = t->mute;
            row.tip = QString::fromUtf8(path.c_str());
            switch (row.state) {
            case App::AnimationTrackState::Driven:
                row.tip += tr2("\nDriven: an expression sets this property too. With Recompute "
                               "Touched or Full the expression's value shows.");
                break;
            case App::AnimationTrackState::Solved:
                row.tip += tr2("\nSolved: an Assembly places this part too.");
                break;
            case App::AnimationTrackState::Unresolved:
                row.tip += tr2("\nUnresolved: the object or the property is gone. The track is "
                               "kept and skipped.");
                break;
            case App::AnimationTrackState::Mismatch:
                row.tip += tr2("\nMismatch: the property has another type or size now.");
                break;
            default:
                break;
            }
            for (auto& k : t->keys) {
                row.keys.push_back({k.id, k.key.t, k.key.interp});
                if (selected.count({t->id, k.id})) {
                    still.insert({t->id, k.id});
                }
            }
            if (t->kind == "Strip") {
                row.strip = true;
                auto it = spans.find(t->id);
                auto child = it != spans.end() ? it->second.child : nullptr;
                row.label = tr2("Strip: %1").arg(
                    QString::fromUtf8(child ? child->Label.getValue() : t->strip.child.c_str()));
                if (it != spans.end()) {
                    row.begin = it->second.start;
                    row.end = it->second.end;
                }
                QString text = QString::fromUtf8(child ? child->Label.getValue() : "");
                const auto& st = t->strip;
                if (st.reverse) {
                    text += tr2(", reversed");
                }
                if (st.repeat > 1) {
                    text += tr2(", x%1").arg(st.repeat);
                }
                if (st.scale != 1.0) {
                    text += tr2(", at %1 speed").arg(1.0 / st.scale, 0, 'g', 3);
                }
                if (st.blend == App::AnimationStrip::Add) {
                    text += tr2(", added");
                }
                if (st.start == App::AnimationStrip::Click) {
                    text += tr2(", on click");
                }
                row.barText = text;
                row.tip = tr2("Strip: %1\nStarts %2, %3 to %4 s").arg(
                    QString::fromUtf8(t->strip.child.c_str()),
                    QString::fromUtf8(App::toString(st.start)),
                    QString::number(row.begin, 'g', 4), QString::number(row.end, 'g', 4));
            }
            rows.push_back(std::move(row));
        }
        selected = std::move(still);
        if (!zoomed) {
            fitRange();
        }
        setMinimumHeight(rulerHeight + std::max<int>(3, static_cast<int>(rows.size()) + 1) * rowHeight);
        update();
    }

    void fitRange()
    {
        auto anim = owner->animation();
        if (!anim) {
            return;
        }
        double start = std::min(0.0, anim->Start.getValue());
        double end = std::max(anim->endTime(), start + 1.0);
        double margin = (end - start) * 0.04;
        view0 = start - margin;
        view1 = end + margin;
    }

    static QString tr2(const char* text)
    {
        return AnimationTimeline::tr(text);
    }

    int gutter() const
    {
        return std::min(240, std::max(140, width() / 4));
    }
    double scale() const
    {
        return (width() - gutter() - 8) / std::max(1e-6, view1 - view0);
    }
    double xOf(double t) const
    {
        return gutter() + (t - view0) * scale();
    }
    double tOf(double x) const
    {
        return view0 + (x - gutter()) / scale();
    }
    int rowAt(int y) const
    {
        if (y < rulerHeight) {
            return -1;
        }
        int i = (y - rulerHeight) / rowHeight;
        return i < static_cast<int>(rows.size()) ? i : -1;
    }

    double snap(double t, Qt::KeyboardModifiers mods) const
    {
        auto anim = owner->animation();
        if (!anim || (mods & Qt::AltModifier)) {
            return t;
        }
        double rate = anim->FrameRate.getValue();
        return rate > 0 ? std::round(t * rate) / rate : t;
    }

    /// The key under `pos`, or {-1, -1}
    std::pair<int, int> keyAt(const QPoint& pos) const
    {
        int r = rowAt(pos.y());
        if (r < 0 || pos.x() < gutter()) {
            return {-1, -1};
        }
        for (auto& k : rows[r].keys) {
            if (std::fabs(xOf(k.t) - pos.x()) <= keySize) {
                return {rows[r].track, k.id};
            }
        }
        return {-1, -1};
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        p.fillRect(rect(), pal.base());
        auto anim = owner->animation();
        int g = gutter();
        // The play range
        if (anim) {
            double s = xOf(anim->Start.getValue());
            double e = xOf(anim->endTime());
            QColor range = pal.highlight().color();
            range.setAlpha(28);
            p.fillRect(QRectF(s, 0, e - s, height()), range);
        }
        // The ruler
        p.fillRect(QRect(0, 0, width(), rulerHeight), pal.window());
        p.setPen(pal.text().color());
        double step = rulerStep();
        double first = std::ceil(view0 / step) * step;
        QFont font = p.font();
        font.setPointSizeF(font.pointSizeF() * 0.9);
        p.setFont(font);
        for (double t = first; t <= view1; t += step) {
            double x = xOf(t);
            if (x < g) {
                continue;
            }
            p.setPen(pal.mid().color());
            p.drawLine(QPointF(x, rulerHeight), QPointF(x, height()));
            p.setPen(pal.text().color());
            p.drawLine(QPointF(x, rulerHeight - 6), QPointF(x, rulerHeight));
            p.drawText(QPointF(x + 3, rulerHeight - 7),
                       QString::number(t, 'g', 4) + QStringLiteral(" s"));
        }
        // The rows
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            int y = rulerHeight + static_cast<int>(i) * rowHeight;
            if (i % 2) {
                p.fillRect(QRect(0, y, width(), rowHeight), pal.alternateBase());
            }
            QColor text = pal.text().color();
            QFont f = font;
            switch (row.state) {
            case App::AnimationTrackState::Driven:
            case App::AnimationTrackState::Solved:
                text = QColor(0xe0, 0x80, 0x20);  // warned: written, and something else writes it
                break;
            case App::AnimationTrackState::Unresolved:
                text = pal.mid().color();
                f.setItalic(true);
                break;
            case App::AnimationTrackState::Mismatch:
                text = QColor(0xcc, 0x33, 0x33);
                break;
            default:
                break;
            }
            if (row.mute) {
                text = pal.mid().color();
                f.setStrikeOut(true);
            }
            p.setFont(f);
            if (curves && row.track == curveTrack) {
                QColor hl = pal.highlight().color();
                hl.setAlpha(60);
                p.fillRect(QRect(0, y, g, rowHeight), hl);
            }
            p.setPen(text);
            p.drawText(QRect(6, y, g - 10, rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(row.label, Qt::ElideLeft, g - 12));
            p.setFont(font);
            if (curves) {
                continue;
            }
            if (row.strip && row.end > row.begin) {
                double shift = (stripDrag && dragTrack == row.track) ? dragDt : 0.0;
                QRectF bar(xOf(row.begin + shift), y + 3, (row.end - row.begin) * scale(),
                           rowHeight - 6);
                QColor fill(0x8e, 0x6c, 0xc8);
                if (row.mute) {
                    fill.setAlpha(90);
                }
                p.setPen(QPen(fill.darker(140), 1));
                p.setBrush(fill);
                p.drawRoundedRect(bar, 4, 4);
                p.setPen(Qt::white);
                p.drawText(bar.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           p.fontMetrics().elidedText(row.barText, Qt::ElideRight,
                                                      std::max(0, int(bar.width()) - 10)));
            }
            for (auto& k : row.keys) {
                bool sel = selected.count({row.track, k.id}) > 0;
                double t = k.t + (sel && dragging ? dragDt : 0.0);
                double x = xOf(t);
                if (x < g - keySize || x > width() + keySize) {
                    continue;
                }
                drawKey(p, QPointF(x, y + rowHeight / 2.0), k.interp, sel, row.mute);
            }
        }
        if (curves) {
            paintCurves(p);
        }
        // The gutter's edge, and the time
        p.setPen(pal.mid().color());
        p.drawLine(g, 0, g, height());
        if (anim) {
            double x = xOf(anim->Time.getValue());
            if (x >= g) {
                p.setPen(QPen(QColor(0xd0, 0x30, 0x30), 2));
                p.drawLine(QPointF(x, 0), QPointF(x, height()));
                QString label = QString::number(anim->Time.getValue(), 'f', 3) + QStringLiteral(" s");
                QRectF box(x + 3, 2, p.fontMetrics().horizontalAdvance(label) + 8, rulerHeight - 6);
                p.fillRect(box, QColor(0xd0, 0x30, 0x30));
                p.setPen(Qt::white);
                p.drawText(box, Qt::AlignCenter, label);
            }
        }
        if (rows.empty()) {
            p.setPen(pal.mid().color());
            p.drawText(QRect(g, rulerHeight, width() - g, height() - rulerHeight),
                       Qt::AlignCenter,
                       anim ? tr2("No tracks yet: right-click a property and Insert key, or "
                                  "turn Record on and move something")
                            : QString());
        }
    }

    double rulerStep() const
    {
        double perPixel = 1.0 / scale();
        double want = perPixel * 70;
        double base = std::pow(10.0, std::floor(std::log10(want)));
        for (double m : {1.0, 2.0, 5.0, 10.0}) {
            if (base * m >= want) {
                return base * m;
            }
        }
        return base * 10;
    }

    void drawKey(QPainter& p, QPointF c, App::Anim::Interp interp, bool sel, bool mute)
    {
        QColor fill;
        switch (interp) {
        case App::Anim::Interp::Constant:
            fill = QColor(0x90, 0x90, 0x90);
            break;
        case App::Anim::Interp::Bezier:
            fill = QColor(0x5c, 0xb8, 0x5c);
            break;
        case App::Anim::Interp::Easing:
            fill = QColor(0x4a, 0x90, 0xe2);
            break;
        default:
            fill = QColor(0xf0, 0xf0, 0xf0);
            break;
        }
        if (mute) {
            fill.setAlpha(90);
        }
        QPainterPath path;
        double s = keySize;
        path.moveTo(c.x(), c.y() - s);
        path.lineTo(c.x() + s, c.y());
        path.lineTo(c.x(), c.y() + s);
        path.lineTo(c.x() - s, c.y());
        path.closeSubpath();
        p.fillPath(path, sel ? QColor(0xff, 0xb0, 0x20) : fill);
        p.setPen(QPen(sel ? QColor(0x80, 0x40, 0x00) : QColor(0x30, 0x30, 0x30), 1));
        p.drawPath(path);
    }

    std::vector<App::AnimationKeyRef> selectedRefs() const
    {
        std::vector<App::AnimationKeyRef> refs;
        for (auto& s : selected) {
            refs.push_back({s.first, s.second});
        }
        return refs;
    }

    void scrubTo(int x, Qt::KeyboardModifiers mods)
    {
        auto anim = owner->animation();
        if (!anim) {
            return;
        }
        double t = snap(tOf(x), mods);
        if (std::fabs(anim->Time.getValue() - t) > App::Anim::TimeEpsilon) {
            anim->Time.setValue(t);
        }
        else {
            anim->activate();
        }
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        setFocus();
        QPoint pos = ev->pos();
        if (ev->button() != Qt::LeftButton) {
            return;
        }
        pressPos = pos;
        if (curves) {
            if (curvePress(ev)) {
                return;
            }
            // Anywhere else on the curves: the time
            mode = Scrub;
            scrubTo(pos.x(), ev->modifiers());
            return;
        }
        if (pos.y() < rulerHeight) {
            mode = Scrub;
            scrubTo(pos.x(), ev->modifiers());
            return;
        }
        int stripRow = rowAt(pos.y());
        if (stripRow >= 0 && rows[stripRow].strip && pos.x() >= gutter()
            && pos.x() >= xOf(rows[stripRow].begin) && pos.x() <= xOf(rows[stripRow].end)) {
            stripDrag = true;
            dragTrack = rows[stripRow].track;
            dragFrom = rows[stripRow].begin;
            dragDt = 0;
            dragging = false;
            mode = Drag;
            return;
        }
        auto key = keyAt(pos);
        if (key.first >= 0) {
            bool add = ev->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
            if (add) {
                if (!selected.erase(key)) {
                    selected.insert(key);
                }
            }
            else if (!selected.count(key)) {
                selected.clear();
                selected.insert(key);
            }
            mode = selected.count(key) ? Drag : None;
            dragDt = 0;
            dragging = false;
            if (auto anim = owner->animation()) {
                if (auto t = anim->Tracks.getTrack(key.first)) {
                    int i = t->findId(key.second);
                    dragFrom = i >= 0 ? t->keys[i].key.t : 0.0;
                }
            }
            update();
            return;
        }
        if (pos.x() >= gutter()) {
            selected.clear();
            mode = Scrub;
            scrubTo(pos.x(), ev->modifiers());
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* ev) override
    {
        if (curves && grab.key >= 0) {
            curveMove(ev);
            return;
        }
        if (mode == Scrub) {
            scrubTo(ev->pos().x(), ev->modifiers());
        }
        else if (mode == Drag) {
            if (!dragging && std::abs(ev->pos().x() - pressPos.x()) < 3) {
                return;
            }
            dragging = true;
            // The key pressed lands on a frame; the others keep their offsets
            double to = snap(dragFrom + (ev->pos().x() - pressPos.x()) / scale(), ev->modifiers());
            dragDt = to - dragFrom;
            update();
        }
        else {
            auto key = keyAt(ev->pos());
            setCursor(key.first >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
            int r = rowAt(ev->pos().y());
            setToolTip(r >= 0 && ev->pos().x() < gutter() ? rows[r].tip : QString());
        }
    }

    void mouseReleaseEvent(QMouseEvent* ev) override
    {
        if (curves && grab.key >= 0) {
            curveRelease(ev);
            return;
        }
        if (stripDrag) {
            auto anim = owner->animation();
            auto t = anim ? anim->Tracks.getTrack(dragTrack) : nullptr;
            if (t && dragging && std::fabs(dragDt) > App::Anim::TimeEpsilon) {
                App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Move strip"));
                App::AnimationTrack o = *t;
                o.strip.offset += dragDt;
                anim->Tracks.setTrackOptions(dragTrack, o);
            }
            stripDrag = false;
            mode = None;
            dragging = false;
            dragDt = 0;
            update();
            return;
        }
        if (mode == Drag && dragging && std::fabs(dragDt) > App::Anim::TimeEpsilon) {
            if (auto anim = owner->animation()) {
                App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Move keys"));
                try {
                    anim->Tracks.moveKeys(selectedRefs(), dragDt);
                }
                catch (Base::Exception& e) {
                    e.ReportException();
                }
            }
        }
        mode = None;
        dragging = false;
        dragDt = 0;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent* ev) override
    {
        auto key = keyAt(ev->pos());
        auto anim = owner->animation();
        if (key.first < 0 || !anim) {
            return;
        }
        if (auto t = anim->Tracks.getTrack(key.first)) {
            int i = t->findId(key.second);
            if (i >= 0) {
                if (std::fabs(anim->Time.getValue() - t->keys[i].key.t) > App::Anim::TimeEpsilon) {
                    anim->Time.setValue(t->keys[i].key.t);
                }
                else {
                    anim->activate();
                }
            }
        }
    }

    void wheelEvent(QWheelEvent* ev) override
    {
        double delta = ev->angleDelta().y() / 120.0;
        if (delta == 0) {
            delta = ev->angleDelta().x() / 120.0;
        }
        if (ev->modifiers() & Qt::ShiftModifier) {
            double dt = (view1 - view0) * 0.1 * delta;
            view0 -= dt;
            view1 -= dt;
        }
        else {
            double at = tOf(ev->position().x());
            double f = std::pow(0.85, delta);
            view0 = at - (at - view0) * f;
            view1 = at + (view1 - at) * f;
        }
        zoomed = true;
        update();
        ev->accept();
    }

    void keyPressEvent(QKeyEvent* ev) override
    {
        auto anim = owner->animation();
        if (!anim) {
            return;
        }
        if ((ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) && !selected.empty()) {
            deleteSelected();
            return;
        }
        if (ev->key() == Qt::Key_Home) {
            zoomed = false;
            fitRange();
            update();
            return;
        }
        QWidget::keyPressEvent(ev);
    }

    void deleteSelected()
    {
        auto anim = owner->animation();
        if (!anim || selected.empty()) {
            return;
        }
        App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Delete keys"));
        try {
            for (auto& s : std::set<std::pair<int, int>>(selected)) {
                anim->Tracks.removeKey(s.first, s.second);
            }
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        selected.clear();
    }

    void contextMenuEvent(QContextMenuEvent* ev) override
    {
        auto anim = owner->animation();
        if (!anim) {
            return;
        }
        QMenu menu;
        int r = rowAt(ev->pos().y());
        if (ev->pos().x() < gutter() || (r >= 0 && rows[r].strip)) {
            if (r < 0) {
                return;
            }
            trackMenu(menu, rows[r].track);
        }
        else {
            auto key = keyAt(ev->pos());
            if (key.first >= 0 && !selected.count(key)) {
                selected.clear();
                selected.insert(key);
                update();
            }
            if (selected.empty()) {
                return;
            }
            keyMenu(menu);
        }
        menu.exec(ev->globalPos());
    }

    void keyMenu(QMenu& menu)
    {
        auto anim = owner->animation();
        using namespace App::Anim;
        auto setInterp = [this, anim](Interp interp, const Easing* easing) {
            App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Set interpolation"));
            try {
                anim->Tracks.setInterp(selectedRefs(), interp, easing);
            }
            catch (Base::Exception& e) {
                e.ReportException();
            }
        };
        QMenu* interp = menu.addMenu(tr2("Interpolation"));
        interp->addAction(tr2("Constant"), [=] { setInterp(Interp::Constant, nullptr); });
        interp->addAction(tr2("Linear"), [=] { setInterp(Interp::Linear, nullptr); });
        interp->addAction(tr2("Bezier"), [=] { setInterp(Interp::Bezier, nullptr); });
        // The easing of the first key selected, its type or mode changed
        Easing current;
        if (auto t = anim->Tracks.getTrack(selected.begin()->first)) {
            int i = t->findId(selected.begin()->second);
            if (i >= 0) {
                current = t->keys[i].key.easing;
            }
        }
        QMenu* easing = menu.addMenu(tr2("Easing"));
        for (int i = 0; i <= static_cast<int>(EaseType::Bounce); ++i) {
            auto type = static_cast<EaseType>(i);
            easing->addAction(QString::fromUtf8(nameOf(type)), [=] {
                Easing e = current;
                e.type = type;
                setInterp(Interp::Easing, &e);
            });
        }
        QMenu* mode = menu.addMenu(tr2("Easing mode"));
        for (int i = 0; i <= static_cast<int>(EaseMode::OutIn); ++i) {
            auto m = static_cast<EaseMode>(i);
            auto action = mode->addAction(QString::fromUtf8(nameOf(m)), [=] {
                Easing e = current;
                e.mode = m;
                setInterp(Interp::Easing, &e);
            });
            action->setCheckable(true);
            action->setChecked(current.mode == m);
        }
        menu.addSeparator();
        menu.addAction(tr2("Go to key"), [this, anim] {
            if (auto t = anim->Tracks.getTrack(selected.begin()->first)) {
                int i = t->findId(selected.begin()->second);
                if (i >= 0) {
                    anim->Time.setValue(t->keys[i].key.t);
                }
            }
        });
        menu.addAction(BitmapFactory().iconFromTheme("edit-delete"),
                       tr2("Delete keys"), [this] { deleteSelected(); });
    }

    void stripMenu(QMenu& menu, int trackId)
    {
        auto anim = owner->animation();
        auto track = anim->Tracks.getTrack(trackId);
        auto change = [anim, trackId](const char* name, auto&& edit) {
            auto t = anim->Tracks.getTrack(trackId);
            if (!t) {
                return;
            }
            App::AutoTransaction guard(name);
            try {
                App::AnimationTrack o = *t;
                edit(o.strip, o);
                anim->Tracks.setTrackOptions(trackId, o);
            }
            catch (Base::Exception& e) {
                e.ReportException();
            }
        };
        const auto& st = track->strip;
        QMenu* start = menu.addMenu(tr2("Starts"));
        const std::pair<App::AnimationStrip::Start, QString> starts[] = {
            {App::AnimationStrip::After, tr2("After the strip before")},
            {App::AnimationStrip::With, tr2("With the strip before")},
            {App::AnimationStrip::Click, tr2("On click: playing stops here")},
            {App::AnimationStrip::At, tr2("At a time of its own")}};
        for (auto& s : starts) {
            auto mode = s.first;
            auto a = start->addAction(s.second, [=] {
                change(QT_TRANSLATE_NOOP("Command", "Strip start"),
                       [=](App::AnimationStrip& o, App::AnimationTrack&) {
                           // Where it lies now, kept: At takes the time as its offset
                           if (mode == App::AnimationStrip::At) {
                               for (auto& span : anim->stripSpans()) {
                                   if (span.track == trackId) {
                                       o.offset = span.start;
                                   }
                               }
                           }
                           else if (o.start == App::AnimationStrip::At) {
                               o.offset = 0;
                           }
                           o.start = mode;
                       });
            });
            a->setCheckable(true);
            a->setChecked(st.start == mode);
        }
        QMenu* blend = menu.addMenu(tr2("Blend"));
        for (auto b : {App::AnimationStrip::Replace, App::AnimationStrip::Add}) {
            auto a = blend->addAction(
                b == App::AnimationStrip::Add ? tr2("Add: on top of what is below")
                                              : tr2("Replace"),
                [=] {
                    change(QT_TRANSLATE_NOOP("Command", "Strip blend"),
                           [=](App::AnimationStrip& o, App::AnimationTrack&) { o.blend = b; });
                });
            a->setCheckable(true);
            a->setChecked(st.blend == b);
        }
        QMenu* repeat = menu.addMenu(tr2("Repeat"));
        for (int n : {1, 2, 3, 4, 5, 10}) {
            auto a = repeat->addAction(QString::number(n), [=] {
                change(QT_TRANSLATE_NOOP("Command", "Strip repeat"),
                       [=](App::AnimationStrip& o, App::AnimationTrack&) { o.repeat = n; });
            });
            a->setCheckable(true);
            a->setChecked(st.repeat == n);
        }
        QMenu* speed = menu.addMenu(tr2("Speed"));
        for (double s : {0.25, 0.5, 1.0, 2.0, 4.0}) {
            auto a = speed->addAction(QStringLiteral("%1 x").arg(s), [=] {
                change(QT_TRANSLATE_NOOP("Command", "Strip speed"),
                       [=](App::AnimationStrip& o, App::AnimationTrack&) { o.scale = 1.0 / s; });
            });
            a->setCheckable(true);
            a->setChecked(std::fabs(st.scale - 1.0 / s) < 1e-9);
        }
        auto rev = menu.addAction(tr2("Reverse"), [=] {
            change(QT_TRANSLATE_NOOP("Command", "Strip reverse"),
                   [](App::AnimationStrip& o, App::AnimationTrack&) { o.reverse = !o.reverse; });
        });
        rev->setCheckable(true);
        rev->setChecked(st.reverse);
        auto mute = menu.addAction(tr2("Mute"), [=] {
            change(QT_TRANSLATE_NOOP("Command", "Mute strip"),
                   [](App::AnimationStrip&, App::AnimationTrack& o) { o.mute = !o.mute; });
        });
        mute->setCheckable(true);
        mute->setChecked(track->mute);
        menu.addSeparator();
        menu.addAction(BitmapFactory().iconFromTheme("edit-delete"), tr2("Remove strip"), [=] {
            App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Remove strip"));
            anim->Tracks.removeTrack(trackId);
        });
    }

    void trackMenu(QMenu& menu, int trackId)
    {
        auto anim = owner->animation();
        auto track = anim->Tracks.getTrack(trackId);
        if (!track) {
            return;
        }
        if (track->kind == "Strip") {
            stripMenu(menu, trackId);
            return;
        }
        using namespace App::Anim;
        auto change = [anim, trackId](const char* name, auto&& edit) {
            auto t = anim->Tracks.getTrack(trackId);
            if (!t) {
                return;
            }
            App::AutoTransaction guard(name);
            try {
                App::AnimationTrack o = *t;
                edit(o);
                anim->Tracks.setTrackOptions(trackId, o);
            }
            catch (Base::Exception& e) {
                e.ReportException();
            }
        };
        auto mute = menu.addAction(tr2("Mute"), [=] {
            change(QT_TRANSLATE_NOOP("Command", "Mute track"),
                   [](App::AnimationTrack& o) { o.mute = !o.mute; });
        });
        mute->setCheckable(true);
        mute->setChecked(track->mute);
        auto extrap = [&](const QString& title, bool after) {
            QMenu* sub = menu.addMenu(title);
            for (int i = 0; i <= static_cast<int>(Extrap::PingPong); ++i) {
                auto e = static_cast<Extrap>(i);
                auto action = sub->addAction(QString::fromUtf8(nameOf(e)), [=] {
                    change(QT_TRANSLATE_NOOP("Command", "Set extrapolation"),
                           [=](App::AnimationTrack& o) { (after ? o.after : o.before) = e; });
                });
                action->setCheckable(true);
                action->setChecked((after ? track->after : track->before) == e);
            }
        };
        extrap(tr2("Before the first key"), false);
        extrap(tr2("After the last key"), true);
        bool rotation = false;
        for (auto& g : track->layout.groups) {
            rotation = rotation || g.kind == Kind::Quaternion;
        }
        rotation = rotation || track->rotation != RotationMode::Shortest;
        if (rotation) {
            QMenu* sub = menu.addMenu(tr2("Rotation"));
            for (int i = 0; i <= static_cast<int>(RotationMode::Euler); ++i) {
                auto m = static_cast<RotationMode>(i);
                auto action = sub->addAction(QString::fromUtf8(nameOf(m)), [=] {
                    App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Set rotation mode"));
                    try {
                        anim->setRotationMode(trackId, m);
                    }
                    catch (Base::Exception& e) {
                        e.ReportException();
                    }
                });
                action->setCheckable(true);
                action->setChecked(track->rotation == m);
            }
        }
        menu.addSeparator();
        menu.addAction(tr2("Key at the current time"), [=] {
            App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Insert key"));
            try {
                auto t = anim->Tracks.getTrack(trackId);
                if (t->kind == "Property") {
                    anim->keyNow(t->target);
                }
                else {
                    anim->setKey(trackId, anim->Time.getValue());
                }
            }
            catch (Base::Exception& e) {
                e.ReportException();
            }
        });
        menu.addAction(BitmapFactory().iconFromTheme("edit-delete"), tr2("Remove track"), [=] {
            App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Remove track"));
            anim->Tracks.removeTrack(trackId);
        });
    }

    // ----------------------------------------------------------------------
    // The curve editor (docs/Animation.md phase 4): the chosen track's
    // numbers over time, its keys and, where a span is Bezier, their handles

    const App::AnimationTrack* currentTrack() const
    {
        auto anim = owner->animation();
        return anim && curveTrack >= 0 ? anim->Tracks.getTrack(curveTrack) : nullptr;
    }

    /// The numbers drawn, and of those which take a handle of their own
    void components(const App::AnimationTrack& track, std::vector<int>& shown,
                    std::vector<bool>& editable) const
    {
        shown.clear();
        editable.clear();
        int i = 0;
        for (auto& g : track.layout.groups) {
            for (int j = 0; j < g.size; ++j, ++i) {
                shown.push_back(i);
                editable.push_back(g.kind == App::Anim::Kind::Scalar
                                   || g.kind == App::Anim::Kind::Colour);
            }
        }
    }

    /// The track as drawn: dragged, a copy with the drag applied
    App::AnimationTrack shownTrack(const App::AnimationTrack& track) const
    {
        App::AnimationTrack t = track;
        if (grab.key < 0 || !grabMoved) {
            return t;
        }
        int i = t.findId(grab.key);
        if (i < 0) {
            return t;
        }
        auto& k = t.keys[i].key;
        if (grab.side == 0) {
            k.t += grabDt;
            if (grab.comp >= 0 && grab.comp < static_cast<int>(k.v.size())) {
                k.v[grab.comp] += grabDv;
            }
            std::stable_sort(t.keys.begin(), t.keys.end(), [](auto& a, auto& b) {
                return a.key.t < b.key.t;
            });
        }
        else {
            k.left = grabLeft;
            k.right = grabRight;
        }
        return t;
    }

    double curveTop() const
    {
        return rulerHeight + 12.0;
    }
    double curveBottom() const
    {
        return std::max(curveTop() + 20.0, height() - 12.0);
    }
    double yOfValue(double v) const
    {
        return curveBottom() - (v - vmin) / (vmax - vmin) * (curveBottom() - curveTop());
    }
    double valuePerPixel() const
    {
        return (vmax - vmin) / (curveBottom() - curveTop());
    }

    void fitValues(const App::Anim::Curve& curve, const std::vector<int>& shown)
    {
        double lo = 1e300;
        double hi = -1e300;
        std::vector<double> nums;
        for (int x = gutter(); x < width(); x += 4) {
            if (curve.evaluate(tOf(x), nums)) {
                for (int c : shown) {
                    lo = std::min(lo, nums[c]);
                    hi = std::max(hi, nums[c]);
                }
            }
        }
        for (auto& k : curve.keys) {
            for (int c : shown) {
                lo = std::min(lo, k.v[c]);
                hi = std::max(hi, k.v[c]);
            }
        }
        if (lo > hi) {
            lo = 0;
            hi = 1;
        }
        double pad = std::max((hi - lo) * 0.1, 1e-6);
        vmin = lo - pad;
        vmax = hi + pad;
    }

    static QColor componentColor(int c)
    {
        static const QColor colors[] = {{0xd0, 0x40, 0x40}, {0x40, 0xa0, 0x40},
                                        {0x40, 0x70, 0xd0}, {0xc0, 0x80, 0x20},
                                        {0x90, 0x50, 0xc0}, {0x20, 0xa0, 0xa0},
                                        {0x80, 0x80, 0x80}};
        return colors[c % 7];
    }

    void paintCurves(QPainter& p)
    {
        auto track0 = currentTrack();
        int g = gutter();
        QRect area(g + 1, rulerHeight, width() - g - 1, height() - rulerHeight);
        p.fillRect(area, palette().base());
        if (!track0 || track0->stepped() || track0->keys.empty()) {
            p.setPen(palette().mid().color());
            p.drawText(area, Qt::AlignCenter,
                       track0 && track0->stepped()
                           ? tr2("A stepped track has no curve")
                           : tr2("Click a track's name to see its curves"));
            return;
        }
        App::AnimationTrack track = shownTrack(*track0);
        App::Anim::Curve curve;
        track.makeCurve(curve);
        std::vector<int> shown;
        std::vector<bool> editable;
        components(track, shown, editable);
        if (!grabMoved) {
            fitValues(curve, shown);
        }
        p.save();
        p.setClipRect(area);
        // Value grid
        double span = vmax - vmin;
        double step = std::pow(10.0, std::floor(std::log10(span / 4)));
        for (double m : {1.0, 2.0, 5.0}) {
            if (span / (step * m) <= 6) {
                step *= m;
                break;
            }
        }
        p.setPen(palette().mid().color());
        for (double v = std::ceil(vmin / step) * step; v <= vmax; v += step) {
            double y = yOfValue(v);
            p.drawLine(QPointF(g, y), QPointF(width(), y));
            p.drawText(QPointF(g + 4, y - 2), QString::number(v, 'g', 5));
        }
        // The curves
        std::vector<double> nums;
        for (std::size_t s = 0; s < shown.size(); ++s) {
            int c = shown[s];
            QPainterPath path;
            bool first = true;
            for (int x = g; x <= width(); x += 2) {
                if (!curve.evaluate(tOf(x), nums)) {
                    continue;
                }
                QPointF pt(x, yOfValue(nums[c]));
                if (first) {
                    path.moveTo(pt);
                    first = false;
                }
                else {
                    path.lineTo(pt);
                }
            }
            QColor col = componentColor(c);
            if (!editable[s]) {
                col.setAlpha(110);
            }
            p.setPen(QPen(col, editable[s] ? 1.6 : 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        // Keys and handles
        for (std::size_t i = 0; i < curve.keys.size(); ++i) {
            auto& k = curve.keys[i];
            bool bezierOut = k.interp == App::Anim::Interp::Bezier && i + 1 < curve.keys.size();
            bool bezierIn = i > 0 && curve.keys[i - 1].interp == App::Anim::Interp::Bezier;
            int id = track.keys[i].id;
            for (std::size_t s = 0; s < shown.size(); ++s) {
                int c = shown[s];
                QPointF kp(xOf(k.t), yOfValue(k.v[c]));
                if (editable[s]) {
                    auto handle = [&](const App::Anim::Handle& h, bool on) {
                        if (!on || c >= static_cast<int>(h.dv.size())) {
                            return;
                        }
                        QPointF hp(xOf(k.t + h.dt), yOfValue(k.v[c] + h.dv[c]));
                        p.setPen(QPen(palette().text().color(), 1));
                        p.drawLine(kp, hp);
                        p.setBrush(componentColor(c));
                        p.drawRect(QRectF(hp.x() - 3, hp.y() - 3, 6, 6));
                    };
                    handle(k.left, bezierIn);
                    handle(k.right, bezierOut);
                }
                bool sel = grab.key == id && grab.comp == c;
                p.setPen(QPen(QColor(0x30, 0x30, 0x30), 1));
                p.setBrush(sel ? QColor(0xff, 0xb0, 0x20) : componentColor(c));
                p.drawEllipse(kp, editable[s] ? 4.5 : 3.0, editable[s] ? 4.5 : 3.0);
            }
        }
        p.restore();
    }

    /// A press in curve mode: true when it was the curve editor's
    bool curvePress(QMouseEvent* ev)
    {
        QPoint pos = ev->pos();
        grab = Grab();
        grabMoved = false;
        if (pos.x() < gutter()) {
            int r = rowAt(pos.y());
            if (r >= 0) {
                curveTrack = rows[r].track;
                update();
            }
            return true;
        }
        if (pos.y() < rulerHeight) {
            return false;  // scrub as ever
        }
        auto track = currentTrack();
        if (!track || track->stepped() || track->keys.empty()) {
            return false;
        }
        App::Anim::Curve curve;
        track->makeCurve(curve);
        std::vector<int> shown;
        std::vector<bool> editable;
        components(*track, shown, editable);
        auto near = [&](QPointF a) {
            return std::hypot(a.x() - pos.x(), a.y() - pos.y()) <= 6.0;
        };
        // Handles first: they lie over keys at small zooms
        for (std::size_t i = 0; i < curve.keys.size(); ++i) {
            auto& k = curve.keys[i];
            bool bezierOut = k.interp == App::Anim::Interp::Bezier && i + 1 < curve.keys.size();
            bool bezierIn = i > 0 && curve.keys[i - 1].interp == App::Anim::Interp::Bezier;
            for (std::size_t s = 0; s < shown.size(); ++s) {
                int c = shown[s];
                if (!editable[s]) {
                    continue;
                }
                for (int side : {-1, 1}) {
                    auto& h = side < 0 ? k.left : k.right;
                    if (!(side < 0 ? bezierIn : bezierOut) || c >= static_cast<int>(h.dv.size())) {
                        continue;
                    }
                    if (near(QPointF(xOf(k.t + h.dt), yOfValue(k.v[c] + h.dv[c])))) {
                        grab = {track->keys[i].id, side, c};
                        grabLeft = k.left;
                        grabRight = k.right;
                        grabFrom = side < 0 ? QPointF(k.left.dt, k.left.dv[c])
                                            : QPointF(k.right.dt, k.right.dv[c]);
                        return true;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < curve.keys.size(); ++i) {
            auto& k = curve.keys[i];
            for (std::size_t s = 0; s < shown.size(); ++s) {
                int c = shown[s];
                if (near(QPointF(xOf(k.t), yOfValue(k.v[c])))) {
                    grab = {track->keys[i].id, 0, editable[s] ? c : -1};
                    grabFrom = QPointF(k.t, k.v[c]);
                    grabDt = 0;
                    grabDv = 0;
                    update();
                    return true;
                }
            }
        }
        return false;  // the time, as a click anywhere else
    }

    void curveMove(QMouseEvent* ev)
    {
        QPoint d = ev->pos() - pressPos;
        if (!grabMoved && std::abs(d.x()) + std::abs(d.y()) < 3) {
            return;
        }
        grabMoved = true;
        double dt = d.x() / scale();
        double dv = -d.y() * valuePerPixel();
        if (grab.side == 0) {
            grabDt = snap(grabFrom.x() + dt, ev->modifiers()) - grabFrom.x();
            grabDv = grab.comp >= 0 ? dv : 0.0;
            if (ev->modifiers() & Qt::ShiftModifier) {
                grabDt = 0;  // in value only
            }
        }
        else {
            // The handle dragged becomes Free; Alt mirrors it on the other side
            auto& h = grab.side < 0 ? grabLeft : grabRight;
            h.type = App::Anim::HandleType::Free;
            h.dt = grab.side < 0 ? std::min(-1e-4, grabFrom.x() + dt)
                                 : std::max(1e-4, grabFrom.x() + dt);
            h.dv[grab.comp] = grabFrom.y() + dv;
            if (ev->modifiers() & Qt::AltModifier) {
                auto& o = grab.side < 0 ? grabRight : grabLeft;
                o.type = App::Anim::HandleType::Free;
                o.dt = -h.dt;
                if (grab.comp < static_cast<int>(o.dv.size())) {
                    o.dv[grab.comp] = -h.dv[grab.comp];
                }
            }
        }
        update();
    }

    void curveRelease(QMouseEvent*)
    {
        auto anim = owner->animation();
        auto track = currentTrack();
        Grab g = grab;
        bool moved = grabMoved;
        grab = Grab();
        grabMoved = false;
        if (!anim || !track || !moved) {
            update();
            return;
        }
        try {
            if (g.side == 0) {
                App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Move key"));
                if (std::fabs(grabDt) > App::Anim::TimeEpsilon) {
                    anim->Tracks.moveKeys({{track->id, g.key}}, grabDt);
                }
                track = anim->Tracks.getTrack(curveTrack);
                int i = track ? track->findId(g.key) : -1;
                if (i >= 0 && g.comp >= 0 && grabDv != 0) {
                    App::AnimationKey k = track->keys[i];
                    k.key.v[g.comp] += grabDv;
                    anim->Tracks.setKey(track->id, std::move(k));
                }
            }
            else {
                App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Move handle"));
                anim->Tracks.setHandles({track->id, g.key}, grabLeft, grabRight);
            }
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        update();
    }

    struct Grab
    {
        int key = -1;
        int side = 0;  ///< 0 the key, -1 its left handle, 1 its right
        int comp = -1;
    } grab;
    bool curves = false;
    int curveTrack = -1;
    double vmin = 0;
    double vmax = 1;
    bool grabMoved = false;
    QPointF grabFrom;
    double grabDt = 0;
    double grabDv = 0;
    App::Anim::Handle grabLeft;
    App::Anim::Handle grabRight;

    AnimationTimeline* owner;
    std::vector<Row> rows;
    std::set<std::pair<int, int>> selected;
    double view0 = -0.1;
    double view1 = 5.1;
    bool zoomed = false;
    enum Mode
    {
        None,
        Scrub,
        Drag
    } mode = None;
    QPoint pressPos;
    double dragFrom = 0;
    double dragDt = 0;
    bool dragging = false;
    bool stripDrag = false;
    int dragTrack = -1;
    static constexpr int rulerHeight = 22;
    static constexpr int rowHeight = 20;
    static constexpr double keySize = 5.5;
};

}  // namespace Gui::DockWnd

// --------------------------------------------------------------------------
// AnimationTimeline

AnimationTimeline::AnimationTimeline(QWidget* parent)
    : DockWindow(nullptr, parent)
{
    theTimeline = this;
    setWindowTitle(tr("Animation"));
    setWindowIcon(BitmapFactory().pixmap("Animation"));

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    auto bar = new QHBoxLayout;
    bar->setSpacing(2);
    layout->addLayout(bar);

    auto cmdButton = [&](const char* cmd, const char* icon, const QString& tip) {
        auto b = new QToolButton(this);
        b->setIcon(BitmapFactory().iconFromTheme(icon));
        b->setIconSize(QSize(24, 24));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, [cmd] {
            Application::Instance->commandManager().runCommandByName(cmd);
        });
        bar->addWidget(b);
        return b;
    };

    animations = new QComboBox(this);
    animations->setMinimumWidth(120);
    animations->setToolTip(tr("The Animation shown"));
    bar->addWidget(animations);
    cmdButton("Std_AnimationNew", "AnimationNew", tr("New animation"));
    bar->addSpacing(8);
    cmdButton("Std_AnimationPrev", "AnimationPrev", tr("Previous key"));
    playButton = cmdButton("Std_AnimationPlay", "AnimationPlay", tr("Play"));
    cmdButton("Std_AnimationStop", "AnimationStop", tr("Stop: put the design back"));
    cmdButton("Std_AnimationNext", "AnimationNext", tr("Next key"));
    bar->addSpacing(8);
    recordButton = cmdButton("Std_AnimationRecord", "AnimationRecord",
                             tr("Record: key every property changed"));
    recordButton->setCheckable(true);
    cmdButton("Std_AnimationNewFrame", "AnimationNewFrame",
              tr("Key the selected objects' placements at the current time"));
    cmdButton("Std_AnimationKeyCamera", "camera-photo",
              tr("Key the 3D view's camera at the current time"));
    bar->addSpacing(8);
    curvesButton = new QToolButton(this);
    curvesButton->setText(tr("Curves"));
    curvesButton->setCheckable(true);
    curvesButton->setToolTip(tr("Show the curves of the track chosen (click its name): drag a "
                                "key in time and value, Shift in value only; drag a Bezier "
                                "handle, Alt to mirror it"));
    bar->addWidget(curvesButton);
    connect(curvesButton, &QToolButton::toggled, this, [this](bool on) {
        keys->curves = on;
        if (on && keys->curveTrack < 0) {
            if (!keys->selected.empty()) {
                keys->curveTrack = keys->selected.begin()->first;
            }
            else if (!keys->rows.empty()) {
                keys->curveTrack = keys->rows.front().track;
            }
        }
        keys->update();
    });
    bar->addSpacing(8);

    auto spin = [&](const QString& label, const QString& tip, double min, double max,
                    double step, int decimals, const QString& suffix) {
        if (!label.isEmpty()) {
            auto l = new QLabel(label, this);
            bar->addWidget(l);
        }
        auto box = new QDoubleSpinBox(this);
        box->setRange(min, max);
        box->setSingleStep(step);
        box->setDecimals(decimals);
        box->setSuffix(suffix);
        box->setToolTip(tip);
        box->setKeyboardTracking(false);
        bar->addWidget(box);
        return box;
    };
    timeBox = spin(tr("Time"), tr("The time shown"), -1e6, 1e6, 0.1, 3, QStringLiteral(" s"));
    startBox = spin(tr("Range"), tr("Where playing starts"), -1e6, 1e6, 0.5, 2,
                    QStringLiteral(" s"));
    endBox = spin(QStringLiteral("-"), tr("Where playing ends; 0 for the last key"), 0, 1e6, 0.5, 2,
                  QStringLiteral(" s"));
    rateBox = spin(QString(), tr("Frames per second"), 1, 240, 1, 0, QStringLiteral(" fps"));
    speedBox = spin(QString(), tr("Playing speed"), 0.01, 100, 0.25, 2, QStringLiteral(" x"));
    loopBox = new QComboBox(this);
    loopBox->addItems({tr("Once"), tr("Loop"), tr("Ping-pong")});
    loopBox->setToolTip(tr("What playing does at the end"));
    bar->addWidget(loopBox);
    recomputeBox = new QComboBox(this);
    recomputeBox->addItems({tr("Recompute touched"), tr("View only"), tr("Recompute all")});
    recomputeBox->setToolTip(tr("Each frame: recompute what it touched, update the view only, or "
                                "recompute the whole document"));
    bar->addWidget(recomputeBox);
    bar->addStretch();

    emptyLabel = new QLabel(tr("No animation in this document: press New animation to make one."),
                            this);
    emptyLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(emptyLabel);
    keys = new AnimationKeysView(this);
    layout->addWidget(keys, 1);

    auto onValue = [this](auto&& apply) {
        return [this, apply](double v) {
            if (updating) {
                return;
            }
            if (auto anim = animation()) {
                apply(anim, v);
            }
        };
    };
    connect(timeBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            onValue([](App::Animation* a, double v) {
                if (std::fabs(a->Time.getValue() - v) > App::Anim::TimeEpsilon) {
                    a->Time.setValue(v);
                }
                else {
                    a->activate();
                }
            }));
    auto edit = [](App::Animation* a, App::Property& prop, auto&& set) {
        App::AutoTransaction guard(QT_TRANSLATE_NOOP("Command", "Edit animation"));
        (void)a;
        (void)prop;
        set();
    };
    connect(startBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            onValue([edit](App::Animation* a, double v) {
                edit(a, a->Start, [&] { a->Start.setValue(v); });
            }));
    connect(endBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            onValue([edit](App::Animation* a, double v) {
                edit(a, a->End, [&] { a->End.setValue(v); });
            }));
    connect(rateBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            onValue([edit](App::Animation* a, double v) {
                edit(a, a->FrameRate, [&] { a->FrameRate.setValue(v); });
            }));
    connect(speedBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            onValue([edit](App::Animation* a, double v) {
                edit(a, a->Speed, [&] { a->Speed.setValue(v); });
            }));
    connect(loopBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, edit](int i) {
        if (auto a = animation(); a && !updating) {
            edit(a, a->Loop, [&] { a->Loop.setValue(i); });
        }
    });
    connect(recomputeBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, edit](int i) {
                if (auto a = animation(); a && !updating) {
                    edit(a, a->Recompute, [&] { a->Recompute.setValue(i); });
                }
            });
    connect(animations, qOverload<int>(&QComboBox::activated), this,
            &AnimationTimeline::onAnimationChosen);
    connect(AnimationPlayer::instance(), &AnimationPlayer::stateChanged, this,
            &AnimationTimeline::refreshValues);

    refreshTimer.setSingleShot(true);
    refreshTimer.setInterval(0);
    connect(&refreshTimer, &QTimer::timeout, this, &AnimationTimeline::refresh);

    auto& app = App::GetApplication();
    conns.emplace_back(app.signalChangedObject.connect(
        [this](const App::DocumentObject& obj, const App::Property& prop) {
            onSlotChangedObject(obj, prop);
        }));
    conns.emplace_back(app.signalNewObject.connect([this](const App::DocumentObject& obj) {
        if (obj.isDerivedFrom(App::Animation::getClassTypeId())) {
            refreshTimer.start();
        }
    }));
    conns.emplace_back(app.signalDeletedObject.connect([this](const App::DocumentObject&) {
        refreshTimer.start();
    }));
    conns.emplace_back(app.signalUndo.connect([this] { refreshTimer.start(); }));
    conns.emplace_back(app.signalRedo.connect([this] { refreshTimer.start(); }));
    conns.emplace_back(Application::Instance->signalActiveDocument.connect(
        [this](const Gui::Document&) { refreshTimer.start(); }));
    conns.emplace_back(app.signalDeleteDocument.connect([this](const App::Document& doc) {
        chosen().erase(doc.getName());
        refreshTimer.start();
    }));
    refresh();
}

AnimationTimeline::~AnimationTimeline() = default;

AnimationTimeline* AnimationTimeline::instance()
{
    return theTimeline;
}

void AnimationTimeline::showTimeline()
{
    // The dock is named after its widget's object name, not the name it is
    // registered by
    auto mgr = DockWindowManager::instance();
    if (auto w = mgr->getDockWindow("Animation")) {
        mgr->activate(w);
    }
}

App::Animation* AnimationTimeline::animation() const
{
    return activeAnimation();
}

void AnimationTimeline::onSlotChangedObject(const App::DocumentObject& obj,
                                            const App::Property& prop)
{
    auto anim = animation();
    if (&obj != anim) {
        // An animated object's change can change a track's state; a value
        // drawn in the property editor changes with its key mark
        return;
    }
    if (&prop == &anim->Time) {
        refreshValues();
        keys->update();
        // The property editor's key marks follow the time
        for (auto editor : getMainWindow()->findChildren<PropertyEditor::PropertyEditor*>()) {
            editor->viewport()->update();
        }
        return;
    }
    refreshTimer.start();
}

void AnimationTimeline::onAnimationChosen(int index)
{
    auto doc = App::GetApplication().getActiveDocument();
    if (!doc || index < 0) {
        return;
    }
    auto obj = doc->getObject(animations->itemData(index).toByteArray().constData());
    if (auto anim = freecad_dynamic_cast<App::Animation>(obj)) {
        chooseAnimation(anim);
        keys->zoomed = false;
        refresh();
    }
}

void AnimationTimeline::refreshAnimations()
{
    Holding hold(updating);
    animations->clear();
    auto doc = App::GetApplication().getActiveDocument();
    auto current = animation();
    if (!doc) {
        return;
    }
    for (auto obj : doc->getObjectsOfType(App::Animation::getClassTypeId())) {
        animations->addItem(BitmapFactory().iconFromTheme("Animation"),
                            QString::fromUtf8(obj->Label.getValue()),
                            QByteArray(obj->getNameInDocument()));
        if (obj == current) {
            animations->setCurrentIndex(animations->count() - 1);
        }
    }
}

void AnimationTimeline::refreshValues()
{
    Holding hold(updating);
    auto anim = animation();
    bool on = anim != nullptr;
    for (QWidget* w : std::initializer_list<QWidget*> {timeBox, startBox, endBox, rateBox, speedBox,
                                                      loopBox, recomputeBox}) {
        w->setEnabled(on);
    }
    emptyLabel->setVisible(!on);
    keys->setVisible(on);
    if (!anim) {
        return;
    }
    timeBox->setValue(anim->Time.getValue());
    startBox->setValue(anim->Start.getValue());
    endBox->setValue(anim->End.getValue());
    rateBox->setValue(anim->FrameRate.getValue());
    speedBox->setValue(anim->Speed.getValue());
    loopBox->setCurrentIndex(static_cast<int>(anim->Loop.getValue()));
    recomputeBox->setCurrentIndex(static_cast<int>(anim->Recompute.getValue()));
    recordButton->setChecked(anim->Recording.getValue());
    auto player = AnimationPlayer::instance();
    bool playing = player->isPlaying() && player->animation() == anim;
    playButton->setIcon(BitmapFactory().iconFromTheme(playing ? "AnimationPause" : "AnimationPlay"));
    playButton->setToolTip(playing ? tr("Pause: keep the pose") : tr("Play"));
}

void AnimationTimeline::refresh()
{
    refreshAnimations();
    refreshValues();
    keys->rebuild();
}

#include "moc_AnimationTimeline.cpp"
