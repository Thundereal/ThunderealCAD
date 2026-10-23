// SPDX-License-Identifier: LGPL-2.1-or-later

// The Ondsel adapter (docs/AssemblySolverInterface.md sec 6.3 step 2): a Model
// becomes the ASMT model AssemblyObject used to build directly. The reference
// below is built the way AssemblyObject.cpp built it before the interface
// (names, marker paths, value strings, order), and the two ASMT texts must be
// the same byte for byte.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

#include <Base/Matrix.h>
#include <Base/Tools.h>  // M_PI, which OndselSolver's headers use
#include <Mod/Assembly/App/Solver/OndselBackend.h>

#include <OndselSolver/CREATE.h>
#include <OndselSolver/ASMTAngleJoint.h>
#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTCylSphJoint.h>
#include <OndselSolver/ASMTCylindricalJoint.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTGearJoint.h>
#include <OndselSolver/ASMTGeneralMotion.h>
#include <OndselSolver/ASMTLineInPlaneJoint.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTParallelAxesJoint.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTPerpendicularJoint.h>
#include <OndselSolver/ASMTPlanarJoint.h>
#include <OndselSolver/ASMTPointInLineJoint.h>
#include <OndselSolver/ASMTPointInPlaneJoint.h>
#include <OndselSolver/ASMTPrincipalMassMarker.h>
#include <OndselSolver/ASMTRackPinionJoint.h>
#include <OndselSolver/ASMTRevCylJoint.h>
#include <OndselSolver/ASMTRevoluteJoint.h>
#include <OndselSolver/ASMTRotationLimit.h>
#include <OndselSolver/ASMTRotationalMotion.h>
#include <OndselSolver/ASMTScrewJoint.h>
#include <OndselSolver/ASMTSimulationParameters.h>
#include <OndselSolver/ASMTSphSphJoint.h>
#include <OndselSolver/ASMTSphericalJoint.h>
#include <OndselSolver/ASMTTranslationLimit.h>
#include <OndselSolver/ASMTTranslationalJoint.h>
#include <OndselSolver/ASMTTranslationalMotion.h>

using namespace Assembly::Solver;

namespace
{

Base::Placement plc(double x, double y, double z, double angle = 0.0)
{
    return Base::Placement(
        Base::Vector3d(x, y, z),
        Base::Rotation(Base::Vector3d(1, 2, 3), angle)
    );
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string asmtText(const std::shared_ptr<MbD::ASMTAssembly>& assembly, const char* tag)
{
    auto path = std::filesystem::temp_directory_path()
        / (std::string("fc_ondsel_adapter_") + tag + ".asmt");
    assembly->outputFile(path.string());
    std::string text = readFile(path);
    std::filesystem::remove(path);
    return text;
}

// ---- the reference: AssemblyObject.cpp's own statements (makeMbdPart,
// makeMbdMarker, fixGroundedPart, handleOneSideOfJoint, makeMbdJoint).

std::shared_ptr<MbD::ASMTPart> refPart(const std::string& name, const Base::Placement& p)
{
    auto mbdPart = MbD::CREATE<MbD::ASMTPart>::With();
    mbdPart->setName(name);
    auto massMarker = MbD::CREATE<MbD::ASMTPrincipalMassMarker>::With();
    massMarker->setMass(1.0);
    massMarker->setDensity(1.0);
    massMarker->setMomentOfInertias(1.0, 1.0, 1.0);
    mbdPart->setPrincipalMassMarker(massMarker);
    Base::Vector3d pos = p.getPosition();
    mbdPart->setPosition3D(pos.x, pos.y, pos.z);
    Base::Matrix4D mat;
    p.getRotation().getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdPart->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
    return mbdPart;
}

std::shared_ptr<MbD::ASMTMarker> refMarker(const std::string& name, const Base::Placement& p)
{
    auto mbdMarker = MbD::CREATE<MbD::ASMTMarker>::With();
    mbdMarker->setName(name);
    Base::Vector3d pos = p.getPosition();
    mbdMarker->setPosition3D(pos.x, pos.y, pos.z);
    Base::Matrix4D mat;
    p.getRotation().getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdMarker->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
    return mbdMarker;
}

// One case per JointKind: the class AssemblyObject created and the value it set.
std::shared_ptr<MbD::ASMTJoint> refJoint(JointKind kind, double v, double v2)
{
    switch (kind) {
        case JointKind::Fixed:
            return MbD::CREATE<MbD::ASMTFixedJoint>::With();
        case JointKind::Revolute:
            return MbD::CREATE<MbD::ASMTRevoluteJoint>::With();
        case JointKind::Cylindrical:
            return MbD::CREATE<MbD::ASMTCylindricalJoint>::With();
        case JointKind::Prismatic:
            return MbD::CREATE<MbD::ASMTTranslationalJoint>::With();
        case JointKind::Spherical:
            return MbD::CREATE<MbD::ASMTSphericalJoint>::With();
        case JointKind::Planar: {
            auto j = MbD::CREATE<MbD::ASMTPlanarJoint>::With();
            j->offset = v;
            return j;
        }
        case JointKind::ParallelZ:
            return MbD::CREATE<MbD::ASMTParallelAxesJoint>::With();
        case JointKind::PerpendicularZ:
            return MbD::CREATE<MbD::ASMTPerpendicularJoint>::With();
        case JointKind::AngleZ: {
            auto j = MbD::CREATE<MbD::ASMTAngleJoint>::With();
            j->theIzJz = v;
            return j;
        }
        case JointKind::PointInPlane: {
            auto j = MbD::CREATE<MbD::ASMTPointInPlaneJoint>::With();
            j->offset = v;
            return j;
        }
        case JointKind::LineInPlane: {
            auto j = MbD::CREATE<MbD::ASMTLineInPlaneJoint>::With();
            j->offset = v;
            return j;
        }
        case JointKind::PointOnLine:
            return MbD::CREATE<MbD::ASMTPointInLineJoint>::With();
        case JointKind::PointPointDistance: {
            auto j = MbD::CREATE<MbD::ASMTSphSphJoint>::With();
            j->distanceIJ = v;
            return j;
        }
        case JointKind::PointLineDistance: {
            auto j = MbD::CREATE<MbD::ASMTCylSphJoint>::With();
            j->distanceIJ = v;
            return j;
        }
        case JointKind::LineLineDistance: {
            auto j = MbD::CREATE<MbD::ASMTRevCylJoint>::With();
            j->distanceIJ = v;
            return j;
        }
        case JointKind::Gear:
        case JointKind::CoaxialGear: {
            auto j = MbD::CREATE<MbD::ASMTGearJoint>::With();
            j->radiusI = v;
            j->radiusJ = v2;
            return j;
        }
        case JointKind::RackPinion: {
            auto j = MbD::CREATE<MbD::ASMTRackPinionJoint>::With();
            j->pitchRadius = v;
            return j;
        }
        case JointKind::Screw: {
            auto j = MbD::CREATE<MbD::ASMTScrewJoint>::With();
            j->pitch = v;
            return j;
        }
        default:
            return nullptr;
    }
}

// The corpus: two grounded objects, three moving bodies, a joint of every
// kind between bodies 2 and 3 (a coaxial gear with its carrier on body 4),
// both kinds of limit with one and two bounds, a driver of each kind, and a
// frame left over by a joint that was dropped after its first marker.
struct Corpus
{
    Model model;
    SimulationSpec sim;
    std::shared_ptr<MbD::ASMTAssembly> reference;
};

Corpus makeCorpus(bool withSimulation)
{
    Corpus c;
    Model& m = c.model;
    auto ref = MbD::CREATE<MbD::ASMTAssembly>::With();
    ref->setName("OndselAssembly");
    ref->setDebug(false);
    c.reference = ref;

    std::vector<std::shared_ptr<MbD::ASMTPart>> refParts;
    auto body = [&](const std::string& name, const Base::Placement& p) {
        Body b;
        b.source = m.addSource(name);
        b.placement = p;
        Id id = m.addBody(b);
        auto part = refPart(name, p);
        ref->addPart(part);
        refParts.push_back(part);
        return id;
    };
    // A marker on a body; returns the frame and its ASMT path.
    auto marker = [&](Id b, const std::string& name, const Base::Placement& p) {
        Frame f;
        f.body = b;
        f.local = p;
        f.label = m.addSource(name);
        Id id = m.addFrame(f);
        refParts[b]->addMarker(refMarker(name, p));
        return std::make_pair(id, "/OndselAssembly/" + refParts[b]->name + "/" + name);
    };
    auto ground = [&](const std::string& name, const Base::Placement& p) {
        // fixGroundedPart: the assembly marker, the part, its FixingMarker, the joint.
        Frame fi;
        fi.local = p;
        fi.label = m.addSource("marker-" + name);
        Id frameI = m.addFrame(fi);
        auto mbdMarker1 = refMarker("marker-" + name, p);
        ref->addMarker(mbdMarker1);
        Id b = body(name, p);
        m.bodies[b].grounded = true;
        auto [frameJ, pathJ] = marker(b, "FixingMarker", Base::Placement());
        Ground g;
        g.source = m.addSource(name);
        g.frameI = frameI;
        g.frameJ = frameJ;
        m.addGround(g);
        auto j = MbD::CREATE<MbD::ASMTFixedJoint>::With();
        j->setName(name);
        j->setMarkerI("/OndselAssembly/" + mbdMarker1->name);
        j->setMarkerJ(pathJ);
        ref->addJoint(j);
    };

    ground("Assembly#Origin", Base::Placement());
    ground("Assembly#Base", plc(1, 2, 3, 0.4));
    Id b2 = body("Assembly#Arm", plc(10, -4, 7, 0.3));
    Id b3 = body("Assembly#Link", plc(-3, 8, 2, -1.1));
    Id b4 = body("Assembly#Carrier", plc(0.5, 0.25, 9, 2.0));

    // A joint whose second side was bad: its first marker stays on the part.
    marker(b2, "Assembly#Dropped", plc(0, 0, 1));

    const double v = 3.25;
    const double v2 = -7.5;
    std::vector<std::shared_ptr<MbD::ASMTJoint>> refJoints;
    for (int k = 0; k < static_cast<int>(JointKind::Count_); ++k) {
        auto kind = static_cast<JointKind>(k);
        std::string name = "Assembly#Joint" + std::to_string(k);
        Joint j;
        j.kind = kind;
        j.value = v + k;
        j.value2 = v2;
        auto refJ = refJoint(kind, j.value, j.value2);
        if (kind == JointKind::CoaxialGear) {
            auto [frameK, pathK] = marker(b4, name + "-Carrier", plc(0, 0, 2, 0.1));
            j.frameK = frameK;
            std::static_pointer_cast<MbD::ASMTGearJoint>(refJ)->setMarkerK(pathK);
        }
        auto [frameI, pathI] = marker(b2, name, plc(k, 1, 0, 0.05 * k));
        auto [frameJ, pathJ] = marker(b3, name, plc(0, k, 1, -0.07 * k));
        j.source = m.addSource(name);
        j.frameI = frameI;
        j.frameJ = frameJ;
        refJ->setName(name);
        refJ->setMarkerI(pathI);
        refJ->setMarkerJ(pathJ);

        // Limits on two of them, as makeMbdJoint added them: min before max,
        // lengths before angles.
        auto addRef = [&](auto limit, const std::string& suffix, const char* type, std::string value) {
            limit->setName(name + suffix);
            limit->setMarkerI(pathI);
            limit->setMarkerJ(pathJ);
            limit->settype(type);
            limit->setlimit(value);
            limit->settol("1.0e-9");
            ref->addLimit(limit);
        };
        if (kind == JointKind::Cylindrical) {
            Limit len;
            len.source = j.source;
            len.frameI = frameI;
            len.frameJ = frameJ;
            len.kind = LimitKind::TranslationZ;
            len.min = -2.5;
            len.max = 12.125;
            m.addLimit(len);
            addRef(MbD::ASMTTranslationLimit::With(), "-LimitLenMin", "=>", std::to_string(-2.5));
            addRef(MbD::ASMTTranslationLimit::With(), "-LimitLenMax", "=<", std::to_string(12.125));
            Limit rot = len;
            rot.kind = LimitKind::RotationZ;
            rot.min.reset();
            rot.max = 45.0 * std::numbers::pi / 180.0;
            m.addLimit(rot);
            addRef(MbD::ASMTRotationLimit::With(), "-LimitRotMax", "=<", std::to_string(45.0) + "*pi/180.0");
        }
        if (kind == JointKind::Revolute) {
            Limit rot;
            rot.source = j.source;
            rot.frameI = frameI;
            rot.frameJ = frameJ;
            rot.kind = LimitKind::RotationZ;
            rot.min = -30.0 * std::numbers::pi / 180.0;
            rot.max = 110.5 * std::numbers::pi / 180.0;
            m.addLimit(rot);
            addRef(MbD::ASMTRotationLimit::With(), "-LimitRotMin", "=>", std::to_string(-30.0) + "*pi/180.0");
            addRef(MbD::ASMTRotationLimit::With(), "-LimitRotMax", "=<", std::to_string(110.5) + "*pi/180.0");
        }

        // Drivers on the Cylindrical (both, then the angular one too, as
        // makeMbdJoint did) and on the Prismatic.
        if (kind == JointKind::Cylindrical) {
            Driver both;
            both.source = j.source;
            both.frameI = frameI;
            both.frameJ = frameJ;
            both.kind = DriverKind::Both;
            both.rotation.text = "2*pi*time";
            both.translation.text = "10*time + 1.25";
            m.addDriver(both);
            auto gm = MbD::CREATE<MbD::ASMTGeneralMotion>::With();
            gm->setName(name + "-ScrewMotion");
            gm->setMarkerI(pathI);
            gm->setMarkerJ(pathJ);
            gm->rIJI->atiput(2, "10*time + 1.25");
            gm->angIJJ->atiput(2, "2*pi*time");
            ref->addMotion(gm);
            Driver ang = both;
            ang.kind = DriverKind::RotationZ;
            ang.translation.text.clear();
            m.addDriver(ang);
            auto rm = MbD::CREATE<MbD::ASMTRotationalMotion>::With();
            rm->setName(name + "-AngularMotion");
            rm->setMarkerI(pathI);
            rm->setMarkerJ(pathJ);
            rm->setRotationZ("2*pi*time");
            ref->addMotion(rm);
        }
        if (kind == JointKind::Prismatic) {
            Driver lin;
            lin.source = j.source;
            lin.frameI = frameI;
            lin.frameJ = frameJ;
            lin.kind = DriverKind::TranslationZ;
            lin.translation.text = "sin(time)";
            m.addDriver(lin);
            auto tm = MbD::CREATE<MbD::ASMTTranslationalMotion>::With();
            tm->setName(name + "-LinearMotion");
            tm->setMarkerI(pathI);
            tm->setMarkerJ(pathJ);
            tm->setTranslationZ("sin(time)");
            ref->addMotion(tm);
        }

        m.addJoint(j);
        refJoints.push_back(refJ);
    }
    for (auto& j : refJoints) {
        ref->addJoint(j);
    }

    if (withSimulation) {
        c.sim.tStart = 0.0;
        c.sim.tEnd = 2.5;
        c.sim.tOutput = 0.04;
        c.sim.errorTolerance = 1e-7;
        auto mbdSim = ref->simulationParameters;
        mbdSim->settstart(0.0);
        mbdSim->settend(2.5);
        mbdSim->sethout(0.04);
        mbdSim->sethmin(1.0e-9);
        mbdSim->sethmax(1.0);
        mbdSim->seterrorTol(1e-7);
    }
    return c;
}

}  // namespace

TEST(OndselAdapter, sameAsmtAsTheDirectBuild)  // NOLINT
{
    Corpus c = makeCorpus(false);
    auto asmt = Ondsel::toAsmt(c.model);
    ASSERT_TRUE(asmt.assembly);
    ASSERT_EQ(asmt.parts.size(), c.model.bodies.size());
    const std::string expected = asmtText(c.reference, "ref");
    const std::string actual = asmtText(asmt.assembly, "new");
    EXPECT_GT(expected.size(), 1000u);
    EXPECT_EQ(actual, expected);
}

TEST(OndselAdapter, sameAsmtWithSimulationParameters)  // NOLINT
{
    Corpus c = makeCorpus(true);
    auto asmt = Ondsel::toAsmt(c.model, &c.sim);
    EXPECT_EQ(asmtText(asmt.assembly, "simnew"), asmtText(c.reference, "simref"));
}

TEST(OndselAdapter, limitValueStrings)  // NOLINT
{
    EXPECT_EQ(Ondsel::limitValueString(LimitKind::TranslationZ, 12.5), "12.500000");
    // Degrees survive the trip through radians to the six decimals printed.
    for (double deg : {0.0, 1.0, 30.0, 45.0, 90.0, -179.999999, 0.1, 359.5, 1e-6}) {
        EXPECT_EQ(
            Ondsel::limitValueString(LimitKind::RotationZ, deg * std::numbers::pi / 180.0),
            std::to_string(deg) + "*pi/180.0"
        ) << deg;
    }
}

TEST(OndselAdapter, sessionSolvesAndExports)  // NOLINT
{
    // A part hanging from a grounded one by a revolute joint, started off it.
    Model m;
    Frame gi;
    gi.label = m.addSource("marker-G");
    Id frameGround = m.addFrame(gi);
    Body g;
    g.source = m.addSource("G");
    g.grounded = true;
    Id bg = m.addBody(g);
    Frame fix;
    fix.body = bg;
    fix.label = m.addSource("FixingMarker");
    Id frameFix = m.addFrame(fix);
    m.addGround({NoId, m.addSource("G"), frameGround, frameFix});
    Body b;
    b.source = m.addSource("B");
    b.placement = plc(3, 1, 2, 0.2);
    Id bb = m.addBody(b);
    Frame fi;
    fi.body = bg;
    fi.local = plc(0, 0, 10);
    fi.label = m.addSource("J");
    Frame fj = fi;
    fj.body = bb;
    fj.local = Base::Placement();
    Joint j;
    j.kind = JointKind::Revolute;
    j.source = m.addSource("J");
    j.frameI = m.addFrame(fi);
    j.frameJ = m.addFrame(fj);
    m.addJoint(j);

    OndselBackend backend;
    EXPECT_EQ(backend.name(), "Ondsel");
    EXPECT_EQ(backend.capabilities().nativeFormat, "asmt");
    // createSession() reads a preference; this test runs without the application.
    auto session = std::make_unique<OndselSession>(false);
    ASSERT_EQ(session->load(m), Status::Ok);

    auto path = std::filesystem::temp_directory_path() / "fc_ondsel_session.asmt";
    ASSERT_EQ(session->exportNative(path.string()), Status::Ok);
    EXPECT_EQ(readFile(path), asmtText(Ondsel::toAsmt(m).assembly, "session"));
    std::filesystem::remove(path);

    Result r = session->solve({});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    ASSERT_EQ(r.moved.size(), 1u);
    EXPECT_EQ(r.moved[0].body, bb);
    // The revolute puts B's frame on G's: B's origin at (0, 0, 10).
    EXPECT_NEAR((r.moved[0].placement.getPosition() - Base::Vector3d(0, 0, 10)).Length(), 0.0, 1e-9);
}

namespace
{

// G grounded, B hanging from it by a revolute joint (twice, if 'redundant').
Model pendulum(Id& bg, Id& bb, bool redundant)
{
    Model m;
    Frame gi;
    gi.label = m.addSource("marker-G");
    Id frameGround = m.addFrame(gi);
    Body g;
    g.source = m.addSource("G");
    g.grounded = true;
    bg = m.addBody(g);
    Frame fix;
    fix.body = bg;
    fix.label = m.addSource("FixingMarker");
    Id frameFix = m.addFrame(fix);
    m.addGround({NoId, m.addSource("G"), frameGround, frameFix});
    Body b;
    b.source = m.addSource("B");
    b.placement = plc(3, 1, 2, 0.2);
    bb = m.addBody(b);
    for (int k = 0; k < (redundant ? 2 : 1); ++k) {
        const std::string name = k == 0 ? "J" : "J2";
        Frame fi;
        fi.body = bg;
        fi.local = plc(0, 0, 10);
        fi.label = m.addSource(name);
        Frame fj = fi;
        fj.body = bb;
        fj.local = Base::Placement();
        Joint j;
        j.kind = JointKind::Revolute;
        j.source = m.addSource(name);
        j.frameI = m.addFrame(fi);
        j.frameJ = m.addFrame(fj);
        m.addJoint(j);
    }
    return m;
}

}  // namespace

TEST(OndselAdapter, sessionDragsAndAnalyses)  // NOLINT
{
    Id bg = NoId;
    Id bb = NoId;
    Model m = pendulum(bg, bb, false);
    auto session = std::make_unique<OndselSession>(false);
    ASSERT_EQ(session->load(m), Status::Ok);
    // Nothing solved, nothing known.
    EXPECT_EQ(session->analyse().known, 0u);
    ASSERT_EQ(session->solve({}).status, Status::Ok);

    auto poses = session->poses();
    ASSERT_EQ(poses.size(), 2u);
    EXPECT_EQ(poses[bb].body, bb);
    EXPECT_NEAR((poses[bb].placement.getPosition() - Base::Vector3d(0, 0, 10)).Length(), 0.0, 1e-9);

    // 6 x (1 + 2 bodies) - 6 (ground) - 5 (revolute).
    Diagnostics d = session->analyse();
    EXPECT_TRUE(d.known & Diagnostics::KnownDof);
    EXPECT_EQ(d.dof, 7);
    EXPECT_TRUE(d.redundant.empty());

    // A drag weight alone is a warm update; any other change needs a load.
    Model weighted = m;
    weighted.bodies[bb].dragWeight = 10.0;
    EXPECT_EQ(session->update(weighted), Status::Ok);
    Model moved = m;
    moved.joints[0].value = 1.0;
    EXPECT_EQ(session->update(moved), Status::NeedsReload);

    // Drag B a quarter turn about the joint axis: it follows.
    ASSERT_EQ(session->beginDrag({bb}, {}), Status::Ok);
    Base::Placement target(
        Base::Vector3d(0, 0, 10),
        Base::Rotation(Base::Vector3d(0, 0, 1), std::numbers::pi / 2)
    );
    Result r = session->dragStep({{bb, target}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_TRUE(session->poses()[bb].placement.isSame(target, 1e-6));
    EXPECT_EQ(session->endDrag().status, Status::Ok);
    EXPECT_TRUE(session->poses()[bb].placement.isSame(target, 1e-6));
}

TEST(OndselAdapter, sessionReportsRedundantJoints)  // NOLINT
{
    Id bg = NoId;
    Id bb = NoId;
    Model m = pendulum(bg, bb, true);
    auto session = std::make_unique<OndselSession>(false);
    ASSERT_EQ(session->load(m), Status::Ok);
    ASSERT_EQ(session->solve({}).status, Status::Ok);
    Diagnostics d = session->analyse();
    // The second revolute repeats the first: five equations go, whichever
    // joint they are taken from, and nothing conflicts.
    ASSERT_FALSE(d.redundant.empty());
    for (Id id : d.redundant) {
        EXPECT_LT(id, 2u);
    }
    EXPECT_TRUE(d.conflicting.empty());
    EXPECT_EQ(d.dof, 7);
}

namespace
{

struct Recorder: TrajectorySink
{
    std::vector<double> times;
    std::vector<std::vector<BodyPose>> frames;
    bool frame(double time, const std::vector<BodyPose>& poses) override
    {
        times.push_back(time);
        frames.push_back(poses);
        return true;
    }
};

// TestSolverGateSimulation.simulation_doc as a model: a revolute driven in
// angle, a slider in length, a cylindrical in both (with the angular driver
// repeated, as makeMbdJoint does).
Model gateSimulation()
{
    Model m;
    Frame gi;
    gi.label = m.addSource("marker-G");
    Id frameGround = m.addFrame(gi);
    Body g;
    g.source = m.addSource("G");
    g.grounded = true;
    Id bg = m.addBody(g);
    Frame fix;
    fix.body = bg;
    fix.label = m.addSource("FixingMarker");
    Id frameFix = m.addFrame(fix);
    m.addGround({NoId, m.addSource("G"), frameGround, frameFix});

    auto body = [&](const char* name, double x) {
        Body b;
        b.source = m.addSource(name);
        b.placement = Base::Placement(Base::Vector3d(x, 0, 20), Base::Rotation());
        return m.addBody(b);
    };
    auto joint = [&](const char* name, JointKind kind, Id other, Base::Vector3d at) {
        Frame fi;
        fi.body = bg;
        fi.local = Base::Placement(at, Base::Rotation());
        fi.label = m.addSource(name);
        Frame fj;
        fj.body = other;
        fj.local = Base::Placement(Base::Vector3d(5, 5, 0), Base::Rotation());
        fj.label = m.addSource(name);
        Joint j;
        j.kind = kind;
        j.source = m.addSource(name);
        j.frameI = m.addFrame(fi);
        j.frameJ = m.addFrame(fj);
        m.addJoint(j);
        return j;
    };
    auto driver = [&](const Joint& j, DriverKind kind, const char* rotation, const char* translation) {
        Driver d;
        d.source = j.source;
        d.frameI = j.frameI;
        d.frameJ = j.frameJ;
        d.kind = kind;
        d.rotation.text = rotation;
        d.translation.text = translation;
        d.rotation.tree = FormulaTree::parse(rotation);
        d.translation.tree = FormulaTree::parse(translation);
        m.addDriver(d);
    };
    Joint rev = joint("Rev", JointKind::Revolute, body("A", 0), Base::Vector3d(5, 5, 20));
    Joint sli = joint("Sli", JointKind::Prismatic, body("B", 30), Base::Vector3d(15, 5, 20));
    Joint cyl = joint("Cyl", JointKind::Cylindrical, body("C", -30), Base::Vector3d(2, 15, 20));
    driver(rev, DriverKind::RotationZ, "36*time", "");
    driver(sli, DriverKind::TranslationZ, "", "5*time + 0");
    driver(cyl, DriverKind::Both, "90*time", "2*sin(time)");
    driver(cyl, DriverKind::RotationZ, "90*time", "");
    return m;
}

}  // namespace

// The default kinematic run (simulateByPositionSolves, sec 4.6) against
// OndselSolver's runKINEMATIC on the same model: the same frames, the same
// poses to the solver's tolerance.
TEST(OndselAdapter, positionSolvesFollowTheKinematicRun)  // NOLINT
{
    const Model m = gateSimulation();
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.1;
    spec.errorTolerance = 1.0e-6;

    auto kinematic = std::make_unique<OndselSession>(false);
    ASSERT_EQ(kinematic->load(m), Status::Ok);
    Recorder expected;
    ASSERT_EQ(kinematic->simulate(spec, expected), Status::Ok);

    auto stepped = std::make_unique<OndselSession>(false);
    Recorder actual;
    ASSERT_EQ(simulateByPositionSolves(*stepped, m, spec, actual), Status::Ok);

    // runKINEMATIC reports the start twice: the assembled pose, then the
    // first output time. The position solves report each output time once.
    ASSERT_EQ(expected.frames.size(), 12u);
    ASSERT_EQ(expected.times[0], expected.times[1]);
    expected.times.erase(expected.times.begin());
    expected.frames.erase(expected.frames.begin());
    ASSERT_EQ(actual.frames.size(), expected.frames.size());
    ASSERT_EQ(actual.frames.size(), 11u);
    double worst = 0.0;
    for (std::size_t k = 0; k < actual.frames.size(); ++k) {
        SCOPED_TRACE(k);
        EXPECT_NEAR(actual.times[k], expected.times[k], 1e-9);
        ASSERT_EQ(actual.frames[k].size(), expected.frames[k].size());
        for (std::size_t i = 0; i < actual.frames[k].size(); ++i) {
            const Base::Placement& a = actual.frames[k][i].placement;
            const Base::Placement& e = expected.frames[k][i].placement;
            const double dp = (a.getPosition() - e.getPosition()).Length();
            Base::Vector3d axis;
            double angle = 0.0;
            (a.getRotation().inverse() * e.getRotation()).getValue(axis, angle);
            const double da = std::abs(std::remainder(angle, 2 * std::numbers::pi));
            worst = std::max({worst, dp, da});
            EXPECT_LT(dp, 1e-6) << "body " << i;
            EXPECT_LT(da, 1e-8) << "body " << i;
        }
    }
    // The driven bodies did move: the frames are not all the start.
    const auto& last = actual.frames.back();
    EXPECT_GT((last[3].placement.getPosition() - m.bodies[3].placement.getPosition()).Length(), 1.0);
    std::ostringstream worstText;
    worstText << worst;
    RecordProperty("worst", worstText.str());
}

// A formula the evaluator rejects stops the run before any solve.
TEST(OndselAdapter, positionSolvesRefuseABadFormula)  // NOLINT
{
    Model m = gateSimulation();
    m.drivers[0].rotation.text = "36*time)";
    m.drivers[0].rotation.tree.reset();
    auto session = std::make_unique<OndselSession>(false);
    Recorder sink;
    EXPECT_EQ(simulateByPositionSolves(*session, m, SimulationSpec {}, sink), Status::Failed);
    EXPECT_TRUE(sink.frames.empty());
}
