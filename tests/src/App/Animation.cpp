// SPDX-License-Identifier: LGPL-2.1-or-later

// The curves of an animation track and the animation codec of the property
// types (docs/Animation.md sections 3.12, 3.13 and 4; App/AnimationCurve.h).

#include "gtest/gtest.h"

#include <cmath>

#include <QEasingCurve>

#include "App/Animation.h"
#include "App/AnimationCurve.h"
#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/PropertyGeo.h"
#include "App/PropertyStandard.h"
#include "App/PropertyUnits.h"
#include "Base/Placement.h"
#include "Base/Rotation.h"
#include <src/App/InitApplication.h>

// NOLINTBEGIN(readability-magic-numbers)

using namespace App::Anim;

namespace
{

constexpr double Pi = 3.14159265358979323846;

Key key(double t, std::vector<double> v, Interp interp = Interp::Linear)
{
    Key k;
    k.t = t;
    k.v = std::move(v);
    k.interp = interp;
    return k;
}

Curve scalarCurve(std::vector<Key> keys)
{
    Curve c;
    c.layout.add(Kind::Scalar, static_cast<int>(keys.front().v.size()));
    c.keys = std::move(keys);
    c.prepare();
    return c;
}

double at(const Curve& c, double t, int i = 0)
{
    std::vector<double> out;
    EXPECT_TRUE(c.evaluate(t, out));
    return out.at(i);
}

std::vector<double> quat(const Base::Rotation& r)
{
    std::vector<double> v;
    rotationToNumbers(r, RotationMode::Shortest, v);
    return v;
}

Base::Rotation rotZ(double deg)
{
    return Base::Rotation(Base::Vector3d(0, 0, 1), deg * Pi / 180);
}

/// The signed angle about +z of a rotation that turns about z
double angleZ(const Base::Rotation& r)
{
    Base::Vector3d v = r.multVec(Base::Vector3d(1, 0, 0));
    return std::atan2(v.y, v.x) * 180 / Pi;
}

}  // namespace

// ---------------------------------------------------------------------------
// Easing

class AnimationEasing: public ::testing::TestWithParam<int>
{};

// Every type and mode against Qt's own QEasingCurve, the old App::KeyFrame's
// easing: the enumerations line up as InQuad = 1 + type * 4 + mode
TEST_P(AnimationEasing, matchesQEasingCurve)
{
    const int index = GetParam();
    Easing e;
    e.type = static_cast<EaseType>(index / 4);
    e.mode = static_cast<EaseMode>(index % 4);
    for (auto params : {std::make_tuple(1.0, 0.3, 1.70158), std::make_tuple(1.5, 0.45, 2.5)}) {
        std::tie(e.amplitude, e.period, e.overshoot) = params;
        QEasingCurve qt(static_cast<QEasingCurve::Type>(1 + index));
        qt.setAmplitude(e.amplitude);
        qt.setPeriod(e.period);
        qt.setOvershoot(e.overshoot);
        for (int i = 0; i <= 200; ++i) {
            double u = i / 200.0;
            EXPECT_NEAR(ease(e, u), qt.valueForProgress(u), 1e-6)
                << "type " << index / 4 << " mode " << index % 4 << " u " << u
                << " amplitude " << e.amplitude;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllTypesAndModes, AnimationEasing, ::testing::Range(0, 40));

TEST(AnimationCurve, easingEndsAtTheKeys)
{
    Easing e;
    for (int type = 0; type < 10; ++type) {
        for (int mode = 0; mode < 4; ++mode) {
            e.type = static_cast<EaseType>(type);
            e.mode = static_cast<EaseMode>(mode);
            EXPECT_NEAR(ease(e, 0), 0, 1e-3) << type << " " << mode;
            EXPECT_NEAR(ease(e, 1), 1, 1e-3) << type << " " << mode;
            // clamped outside the span
            EXPECT_EQ(ease(e, -1), ease(e, 0));
            EXPECT_EQ(ease(e, 2), ease(e, 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Spans

TEST(AnimationCurve, noKeysNoValue)
{
    Curve c;
    c.layout.add(Kind::Scalar, 1);
    std::vector<double> out;
    EXPECT_FALSE(c.evaluate(0, out));
}

TEST(AnimationCurve, oneKeyEverywhere)
{
    auto c = scalarCurve({key(1, {7})});
    EXPECT_EQ(at(c, -10), 7);
    EXPECT_EQ(at(c, 1), 7);
    EXPECT_EQ(at(c, 10), 7);
}

TEST(AnimationCurve, linearAndConstant)
{
    auto c = scalarCurve({key(0, {0}), key(2, {10}, Interp::Constant), key(3, {20})});
    EXPECT_DOUBLE_EQ(at(c, 0), 0);
    EXPECT_DOUBLE_EQ(at(c, 0.5), 2.5);
    EXPECT_DOUBLE_EQ(at(c, 2), 10);
    EXPECT_DOUBLE_EQ(at(c, 2.99), 10);  // the span leaving the key at 2 holds
    EXPECT_DOUBLE_EQ(at(c, 3), 20);
}

TEST(AnimationCurve, keysAtOneTimeMergeTheLastWins)
{
    auto c = scalarCurve({key(0, {0}), key(1, {5}), key(1 + 1e-8, {6}), key(2, {0})});
    ASSERT_EQ(c.keys.size(), 3u);
    EXPECT_DOUBLE_EQ(at(c, 1), 6);
    EXPECT_EQ(c.find(1.0), 1);
    EXPECT_EQ(c.find(1.5), -1);
}

TEST(AnimationCurve, keysAreSorted)
{
    auto c = scalarCurve({key(2, {10}), key(0, {0})});
    EXPECT_DOUBLE_EQ(at(c, 1), 5);
}

TEST(AnimationCurve, easingSpan)
{
    Key k0 = key(0, {0}, Interp::Easing);
    k0.easing.type = EaseType::Quad;
    k0.easing.mode = EaseMode::In;
    auto c = scalarCurve({k0, key(1, {10})});
    EXPECT_NEAR(at(c, 0.5), 2.5, 1e-12);  // 10 * 0.5^2
}

TEST(AnimationCurve, pythonSpanUsesTheHook)
{
    auto c = scalarCurve({key(0, {0}, Interp::Python), key(1, {10})});
    c.python = [](const Key&, double u) {
        return u * u * u;
    };
    EXPECT_NEAR(at(c, 0.5), 1.25, 1e-12);
    c.python = nullptr;  // without one, linear
    EXPECT_NEAR(at(c, 0.5), 5, 1e-12);
}

// ---------------------------------------------------------------------------
// Bezier

// Flat free handles a third of the span long: x(s) = s, and y is the cubic
// with control values 0 0 1 1, that is 3 s^2 - 2 s^3
TEST(AnimationCurve, bezierClosedForm)
{
    Key k0 = key(0, {0}, Interp::Bezier);
    Key k1 = key(1, {1});
    k0.right = {HandleType::Free, 1.0 / 3, {0}};
    k1.left = {HandleType::Free, -1.0 / 3, {0}};
    auto c = scalarCurve({k0, k1});
    for (double s : {0.1, 0.25, 0.5, 0.8, 0.95}) {
        EXPECT_NEAR(at(c, s), 3 * s * s - 2 * s * s * s, 1e-9) << s;
    }
}

TEST(AnimationCurve, bezierVectorHandlesAreStraight)
{
    Key k0 = key(0, {0}, Interp::Bezier);
    Key k1 = key(2, {10});
    k0.right.type = HandleType::Vector;
    k1.left.type = HandleType::Vector;
    auto c = scalarCurve({k0, k1});
    for (double t : {0.2, 0.7, 1.0, 1.9}) {
        EXPECT_NEAR(at(c, t), 5 * t, 1e-9) << t;
    }
}

// Auto clamped: through every key, flat at the peak, never above it
TEST(AnimationCurve, bezierAutoClampedHoldsThePeak)
{
    auto c = scalarCurve({key(0, {0}, Interp::Bezier), key(1, {10}, Interp::Bezier),
                          key(2, {0}, Interp::Bezier), key(3, {5})});
    EXPECT_NEAR(at(c, 0), 0, 1e-12);
    EXPECT_NEAR(at(c, 1), 10, 1e-12);
    EXPECT_NEAR(at(c, 2), 0, 1e-12);
    for (int i = 0; i <= 100; ++i) {
        double t = i * 2.0 / 100;
        EXPECT_LE(at(c, t), 10 + 1e-9) << t;
        EXPECT_GE(at(c, t), -1e-9) << t;
    }
    // smooth across the key at 1: the slopes either side agree
    double h = 1e-5;
    EXPECT_NEAR((at(c, 1) - at(c, 1 - h)) / h, (at(c, 1 + h) - at(c, 1)) / h, 1e-2);
}

// Handles that reach past each other are shortened together, so time runs
// one way: the curve stays a function of time and still meets its keys
TEST(AnimationCurve, bezierLongHandlesStayMonotonicInTime)
{
    Key k0 = key(0, {0}, Interp::Bezier);
    Key k1 = key(1, {1});
    k0.right = {HandleType::Free, 5, {3}};
    k1.left = {HandleType::Free, -5, {-3}};
    auto c = scalarCurve({k0, k1});
    double last = -1;
    for (int i = 0; i <= 50; ++i) {
        double v = at(c, i / 50.0);
        EXPECT_GE(v, last - 1e-9);
        last = v;
    }
    EXPECT_NEAR(at(c, 1 - 1e-12), 1, 1e-6);
}

// ---------------------------------------------------------------------------
// Extrapolation

TEST(AnimationCurve, extrapolation)
{
    auto c = scalarCurve({key(0, {0}), key(1, {10}), key(2, {4})});

    EXPECT_DOUBLE_EQ(at(c, -1), 0);  // Constant both sides by default
    EXPECT_DOUBLE_EQ(at(c, 5), 4);

    c.before = Extrap::Linear;
    c.after = Extrap::Linear;
    EXPECT_DOUBLE_EQ(at(c, -1), -10);
    EXPECT_DOUBLE_EQ(at(c, 3), -2);

    c.after = Extrap::Cycle;
    EXPECT_DOUBLE_EQ(at(c, 2.5), 5);
    EXPECT_DOUBLE_EQ(at(c, 4.5), 5);

    c.after = Extrap::CycleOffset;  // each repeat starts at the last's end, +4
    EXPECT_DOUBLE_EQ(at(c, 2.5), 9);
    EXPECT_DOUBLE_EQ(at(c, 4.5), 13);

    c.after = Extrap::PingPong;
    EXPECT_DOUBLE_EQ(at(c, 2.5), 7);  // backwards: 2 - 0.5 = 1.5
    EXPECT_DOUBLE_EQ(at(c, 4.5), 5);  // forwards again

    c.before = Extrap::Cycle;
    EXPECT_DOUBLE_EQ(at(c, -0.5), 7);  // 1.5 of the cycle before
}

// ---------------------------------------------------------------------------
// Groups

TEST(AnimationCurve, layoutMergesScalars)
{
    Layout l;
    l.add(Kind::Scalar, 3);
    l.add(Kind::Scalar, 2);
    l.add(Kind::Quaternion, 99);  // sizes of the fixed kinds are their own
    l.add(Kind::Scalar, 1);
    ASSERT_EQ(l.groups.size(), 3u);
    EXPECT_EQ(l.groups[0].size, 5);
    EXPECT_EQ(l.groups[1].size, 4);
    EXPECT_EQ(l.size(), 10);
}

TEST(AnimationCurve, quaternionTakesTheShortestWay)
{
    Curve c;
    c.layout.add(Kind::Quaternion);
    c.keys = {key(0, quat(rotZ(0))), key(1, quat(rotZ(270)))};
    c.prepare();
    std::vector<double> out;
    c.evaluate(0.5, out);
    Base::Rotation r;
    rotationFromNumbers(out.data(), RotationMode::Shortest, r);
    EXPECT_NEAR(angleZ(r), -45, 1e-9);  // the 90 deg way round, not the 270
}

TEST(AnimationCurve, quaternionOfTheOtherSignIsTheSameRotation)
{
    Curve c;
    c.layout.add(Kind::Quaternion);
    auto q = quat(rotZ(40));
    auto minus = q;
    for (double& x : minus) {
        x = -x;
    }
    c.keys = {key(0, q, Interp::Bezier), key(1, minus, Interp::Bezier), key(2, q)};
    c.prepare();
    std::vector<double> out;
    for (double t : {0.3, 1.0, 1.6}) {
        c.evaluate(t, out);
        Base::Rotation r;
        rotationFromNumbers(out.data(), RotationMode::Shortest, r);
        EXPECT_NEAR(angleZ(r), 40, 1e-9) << t;
    }
}

TEST(AnimationCurve, turnsGoAllTheWayRound)
{
    Layout layout;
    layout.rotation = RotationMode::Turns;
    layout.addRotation();
    Curve c;
    c.layout = layout;
    c.keys = {key(0, {0, 0, 1, 0}), key(1, {0, 0, 1, 720})};
    c.prepare();
    std::vector<double> out;
    c.evaluate(0.25, out);
    EXPECT_NEAR(out[3], 180, 1e-9);
    c.evaluate(0.375, out);  // 270 deg: -90 about z
    Base::Rotation r;
    rotationFromNumbers(out.data(), RotationMode::Turns, r);
    EXPECT_NEAR(angleZ(r), -90, 1e-9);
}

TEST(AnimationCurve, eulerAnglesAreScalars)
{
    Layout layout;
    layout.rotation = RotationMode::Euler;
    layout.addRotation();
    ASSERT_EQ(layout.size(), 3);
    Curve c;
    c.layout = layout;
    c.keys = {key(0, {0, 0, 0}), key(1, {270, 0, 0})};
    c.prepare();
    std::vector<double> out;
    c.evaluate(0.5, out);
    EXPECT_NEAR(out[0], 135, 1e-12);
}

TEST(AnimationCurve, cycleOffsetComposesRotations)
{
    Curve c;
    c.layout.add(Kind::Quaternion);
    c.keys = {key(0, quat(rotZ(0))), key(1, quat(rotZ(90)))};
    c.after = Extrap::CycleOffset;
    c.prepare();
    std::vector<double> out;
    c.evaluate(1.5, out);
    Base::Rotation r;
    rotationFromNumbers(out.data(), RotationMode::Shortest, r);
    EXPECT_NEAR(angleZ(r), 135, 1e-9);
    c.evaluate(2.5, out);
    rotationFromNumbers(out.data(), RotationMode::Shortest, r);
    EXPECT_NEAR(angleZ(r), -135, 1e-9);  // 225
}

TEST(AnimationCurve, directionOnTheSphere)
{
    Curve c;
    c.layout.add(Kind::Direction);
    c.keys = {key(0, {1, 0, 0}), key(1, {0, 1, 0})};
    c.prepare();
    std::vector<double> out;
    c.evaluate(0.5, out);
    EXPECT_NEAR(out[0], std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(out[1], std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(out[2], 0, 1e-12);
}

TEST(AnimationCurve, colourStaysInRange)
{
    Curve c;
    c.layout.add(Kind::Colour);
    Key k0 = key(0, {0, 0, 0, 1}, Interp::Easing);
    k0.easing.type = EaseType::Back;  // overshoots past both ends
    k0.easing.mode = EaseMode::InOut;
    c.keys = {k0, key(1, {1, 1, 1, 1})};
    c.prepare();
    std::vector<double> out;
    for (int i = 0; i <= 20; ++i) {
        c.evaluate(i / 20.0, out);
        for (double x : out) {
            EXPECT_GE(x, 0);
            EXPECT_LE(x, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// The codec of the property types

class AnimationCodec: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    static std::vector<double> numbers(const App::Property& p, Layout& layout)
    {
        std::vector<double> v;
        EXPECT_TRUE(p.getAnimLayout(layout));
        EXPECT_TRUE(p.getAnimValue(layout, v));
        EXPECT_EQ(static_cast<int>(v.size()), layout.size());
        return v;
    }
};

TEST_F(AnimationCodec, placementRoundTripsInEveryMode)
{
    App::PropertyPlacement p;
    Base::Placement pla(Base::Vector3d(1, 2, 3),
                        Base::Rotation(Base::Vector3d(1, 1, 0).Normalize(), 0.7));
    for (auto mode : {RotationMode::Shortest, RotationMode::Turns, RotationMode::Euler}) {
        p.setValue(pla);
        Layout layout;
        layout.rotation = mode;
        auto v = numbers(p, layout);
        EXPECT_EQ(layout.size(), mode == RotationMode::Euler ? 6 : 7);
        p.setValue(Base::Placement());
        ASSERT_TRUE(p.setAnimValue(layout, v));
        EXPECT_TRUE(p.getValue().isSame(pla, 1e-12)) << static_cast<int>(mode);
    }
}

TEST_F(AnimationCodec, placementListRoundTrips)
{
    App::PropertyPlacementList p;
    std::vector<Base::Placement> list {Base::Placement(Base::Vector3d(1, 0, 0), rotZ(30)),
                                       Base::Placement(Base::Vector3d(0, 5, 0), rotZ(-60))};
    p.setValues(list);
    Layout layout;
    layout.rotation = RotationMode::Euler;
    auto v = numbers(p, layout);
    EXPECT_EQ(layout.size(), 12);
    p.setValues({});
    ASSERT_TRUE(p.setAnimValue(layout, v));
    ASSERT_EQ(p.getSize(), 2);
    EXPECT_TRUE(p[1].isSame(list[1], 1e-12));
}

TEST_F(AnimationCodec, directionIsOnTheSphere)
{
    App::PropertyDirection p;
    p.setValue(0, 0, 5);
    Layout layout;
    auto v = numbers(p, layout);
    ASSERT_EQ(layout.groups.size(), 1u);
    EXPECT_EQ(layout.groups[0].kind, Kind::Direction);
    EXPECT_DOUBLE_EQ(v[2], 1);
}

TEST_F(AnimationCodec, integerRoundsAndConstraintsClamp)
{
    App::PropertyInteger i;
    Layout layout;
    numbers(i, layout);
    i.setAnimValue(layout, {2.6});
    EXPECT_EQ(i.getValue(), 3);

    App::PropertyPercent percent;  // 0 to 100
    percent.setAnimValue(layout, {130.2});
    EXPECT_EQ(percent.getValue(), 100);

    App::PropertyFloatConstraint f;
    static const App::PropertyFloatConstraint::Constraints range {0.0, 10.0, 1.0};
    f.setConstraints(&range);
    f.setAnimValue(layout, {15});
    EXPECT_DOUBLE_EQ(f.getValue(), 10);
    f.setAnimValue(layout, {-1});
    EXPECT_DOUBLE_EQ(f.getValue(), 0);

    App::PropertyFloatConstraint free;  // no constraints: nothing clamped
    free.setAnimValue(layout, {-1e6});
    EXPECT_DOUBLE_EQ(free.getValue(), -1e6);

    App::PropertyQuantityConstraint q;
    static const App::PropertyQuantityConstraint::Constraints qrange {1.0, 2.0, 0.1};
    q.setConstraints(&qrange);
    q.setAnimValue(layout, {0.5});
    EXPECT_DOUBLE_EQ(q.getValue(), 1.0);
}

TEST_F(AnimationCodec, colourList)
{
    App::PropertyColorList p;
    p.setValues({Base::Color(1, 0, 0), Base::Color(0, 0, 1)});
    Layout layout;
    auto v = numbers(p, layout);
    ASSERT_EQ(layout.groups.size(), 2u);
    EXPECT_EQ(layout.groups[1].kind, Kind::Colour);
    EXPECT_DOUBLE_EQ(v[6], 1);  // the second's blue
}

TEST_F(AnimationCodec, steppedTypesHaveNoCodec)
{
    App::PropertyString s;
    App::PropertyBool b;
    Layout layout;
    EXPECT_FALSE(s.getAnimLayout(layout));
    EXPECT_FALSE(b.getAnimLayout(layout));
}

TEST_F(AnimationCodec, interpolateGoesThroughTheCodec)
{
    App::PropertyPlacement from, to, p;
    from.setValue(Base::Placement(Base::Vector3d(0, 0, 0), rotZ(0)));
    to.setValue(Base::Placement(Base::Vector3d(10, 0, 0), rotZ(90)));
    p.interpolate(from, to, 0.5f);
    EXPECT_NEAR(p.getValue().getPosition().x, 5, 1e-6);
    EXPECT_NEAR(angleZ(p.getValue().getRotation()), 45, 1e-4);

    App::PropertyFloat f0, f1, f;
    f0.setValue(1);
    f1.setValue(3);
    f.interpolate(f0, f1, 0.25f);
    EXPECT_DOUBLE_EQ(f.getValue(), 1.5);
}

TEST_F(AnimationCodec, interpolateStepsWhatHasNoCodec)
{
    App::PropertyString from, to, p;
    from.setValue("a");
    to.setValue("b");
    p.interpolate(from, to, 0.3f);
    EXPECT_STREQ(p.getValue(), "a");
    p.interpolate(from, to, 0.7f);
    EXPECT_STREQ(p.getValue(), "b");

    // lists of different sizes step too
    App::PropertyFloatList l0, l1, l;
    l0.setValues({1, 2});
    l1.setValues({3});
    l.interpolate(l0, l1, 0.9f);
    EXPECT_EQ(l.getSize(), 1);

    App::PropertyFloat other;
    EXPECT_THROW(p.interpolate(other, to, 0.5f), Base::TypeError);
}

// A track's path re-pointed as an expression's is (docs/Animation.md sec
// 3.3): a label it names objects by, an object a link replaces, an external
// object imported into the Animation's document.
class AnimationTrackPaths: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
    void SetUp() override
    {
        doc = App::GetApplication().newDocument("AnimTrackPaths", "testUser");
        a = doc->addObject("App::FeaturePython", "A");
        b = doc->addObject("App::FeaturePython", "B");
        for (auto obj : {a, b}) {
            obj->addDynamicProperty("App::PropertyFloat", "Val");
        }
        anim = static_cast<App::Animation*>(doc->addObject("App::Animation", "Anim"));
    }
    void TearDown() override
    {
        App::GetApplication().closeDocument(doc->getName());
        if (other) {
            App::GetApplication().closeDocument(other->getName());
        }
    }
    void track(const std::string& path)
    {
        const int t = anim->trackOf(anim->parsePath(path), true);
        ASSERT_GE(t, 0) << path;
        anim->setKey(t, 0.0);
    }
    std::string path() const
    {
        const auto& tracks = anim->Tracks.getValues();
        return tracks.empty() ? std::string() : tracks.front().target.toString();
    }
    App::Document* doc = nullptr;
    App::Document* other = nullptr;
    App::DocumentObject* a = nullptr;
    App::DocumentObject* b = nullptr;
    App::Animation* anim = nullptr;
};

TEST_F(AnimationTrackPaths, aLabelThePathNamesFollowsARelabel)
{
    a->Label.setValue("Alpha");
    track("<<Alpha>>.Val");
    ASSERT_EQ(path(), "<<Alpha>>.Val");
    a->Label.setValue("Beta");
    EXPECT_EQ(path(), "<<Beta>>.Val");
    EXPECT_EQ(anim->Tracks.getValues().front().target.getDocumentObject(), a);
}

TEST_F(AnimationTrackPaths, aPathByNameIsNotTouchedByARelabel)
{
    track("A.Val");
    a->Label.setValue("Gamma");
    EXPECT_EQ(path(), "A.Val");
}

TEST_F(AnimationTrackPaths, anObjectALinkReplacesIsNamed)
{
    track("A.Val");
    std::unique_ptr<App::Property> copy(anim->Tracks.CopyOnLinkReplace(anim, a, b));
    ASSERT_TRUE(copy);
    anim->Tracks.Paste(*copy);
    EXPECT_EQ(path(), "B.Val");
    // Replaced under another parent, a path not through it stays
    std::unique_ptr<App::Property> none(anim->Tracks.CopyOnLinkReplace(b, b, a));
    EXPECT_FALSE(none);
}

TEST_F(AnimationTrackPaths, anExternalObjectImportedIsNamedInTheDocument)
{
    other = App::GetApplication().newDocument("AnimTrackPathsExt", "testUser");
    auto ext = other->addObject("App::FeaturePython", "Ext");
    ext->addDynamicProperty("App::PropertyFloat", "Val");
    track("AnimTrackPathsExt#Ext.Val");
    ASSERT_EQ(anim->Tracks.getValues().front().target.getDocumentObject(), ext);
    std::map<std::string, std::string> nameMap {{ext->getExportName(true), b->getNameInDocument()}};
    std::unique_ptr<App::Property> copy(anim->Tracks.CopyOnImportExternal(nameMap));
    ASSERT_TRUE(copy);
    anim->Tracks.Paste(*copy);
    EXPECT_EQ(path(), "B.Val");
    EXPECT_EQ(anim->Tracks.getValues().front().target.getDocumentObject(), b);
    // Nothing the map names: nothing changes
    std::unique_ptr<App::Property> none(anim->Tracks.CopyOnImportExternal({}));
    EXPECT_FALSE(none);
}

// NOLINTEND(readability-magic-numbers)
