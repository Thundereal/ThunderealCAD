// SPDX-License-Identifier: LGPL-2.1-or-later

// The solver-neutral model and the backend registry
// (docs/AssemblySolverInterface.md sec 4.2, 4.3, 4.8).

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/PropertyStandard.h>
#include <Base/Interpreter.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/Solver/Registry.h>
#include <src/App/InitApplication.h>

using namespace Assembly::Solver;

namespace
{

class FakeSession: public Session
{
public:
    Status load(const Model&) override
    {
        return Status::Ok;
    }
    Result solve(const SolveOptions&) override
    {
        return {};
    }
    Status beginDrag(const std::vector<Id>&, const DragOptions&) override
    {
        return Status::Unsupported;
    }
    Result dragStep(const std::vector<BodyPose>&) override
    {
        return {};
    }
    Result endDrag() override
    {
        return {};
    }
    std::vector<BodyPose> poses() const override
    {
        return {};
    }
    Diagnostics analyse() override
    {
        return {};
    }
};

class FakeBackend: public Backend
{
public:
    explicit FakeBackend(std::string n, int tag = 0)
        : n(std::move(n))
        , tag(tag)
    {}
    std::string name() const override
    {
        return n;
    }
    std::string label() const override
    {
        return n;
    }
    Capabilities capabilities() const override
    {
        Capabilities caps;
        caps.joints.set(static_cast<std::size_t>(JointKind::Revolute));
        return caps;
    }
    std::unique_ptr<Session> createSession() override
    {
        return std::make_unique<FakeSession>();
    }

    std::string n;
    int tag;
};

}  // namespace

TEST(SolverModel, idsAreIndices)  // NOLINT
{
    Model model;
    Id src = model.addSource("Part");
    Body body;
    body.source = src;
    Id b0 = model.addBody(body);
    Id b1 = model.addBody(body);
    Frame frame;
    frame.body = b1;
    Id f0 = model.addFrame(frame);
    Joint joint;
    joint.kind = JointKind::Revolute;
    joint.frameI = f0;
    Id j0 = model.addJoint(joint);

    EXPECT_EQ(src, 0u);
    EXPECT_EQ(b0, 0u);
    EXPECT_EQ(b1, 1u);
    EXPECT_EQ(model.bodies[1].id, 1u);
    EXPECT_EQ(f0, 0u);
    EXPECT_EQ(model.frames[0].body, 1u);
    EXPECT_EQ(j0, 0u);
    EXPECT_EQ(model.joints[0].frameJ, NoId);
    EXPECT_EQ(model.sourceName(src), "Part");
    EXPECT_EQ(model.sourceName(NoId), "");
}

class SolverRegistryTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import AssemblyApp");
    }

    void SetUp() override
    {
        hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Mod/Assembly"
        );
        savedPref = hGrp->GetASCII("SolverBackend", "");
        hGrp->RemoveASCII("SolverBackend");
    }

    void TearDown() override
    {
        auto& registry = Registry::instance();
        registry.remove("TestA");
        registry.remove("TestB");
        if (savedPref.empty()) {
            hGrp->RemoveASCII("SolverBackend");
        }
        else {
            hGrp->SetASCII("SolverBackend", savedPref.c_str());
        }
    }

    ParameterGrp::handle hGrp;
    std::string savedPref;
};

TEST_F(SolverRegistryTest, addFindReplaceRemove)  // NOLINT
{
    auto& registry = Registry::instance();
    const auto before = registry.names().size();
    registry.add(std::make_unique<FakeBackend>("TestA", 1));
    registry.add(std::make_unique<FakeBackend>("TestB", 2));
    ASSERT_NE(registry.find("TestA"), nullptr);
    EXPECT_EQ(registry.names().size(), before + 2);
    EXPECT_EQ(registry.names().back(), "TestB");

    // Same name: replaced in place.
    registry.add(std::make_unique<FakeBackend>("TestA", 3));
    EXPECT_EQ(registry.names().size(), before + 2);
    EXPECT_EQ(static_cast<FakeBackend*>(registry.find("TestA"))->tag, 3);

    auto session = registry.find("TestB")->createSession();
    ASSERT_TRUE(session);
    EXPECT_EQ(session->load(Model {}), Status::Ok);
    EXPECT_EQ(session->update(Model {}), Status::NeedsReload);
    EXPECT_EQ(session->exportNative("x"), Status::Unsupported);

    EXPECT_TRUE(registry.remove("TestA"));
    EXPECT_FALSE(registry.remove("TestA"));
    EXPECT_EQ(registry.find("TestA"), nullptr);
}

TEST_F(SolverRegistryTest, selectByPreferenceAndProperty)  // NOLINT
{
    auto& registry = Registry::instance();
    registry.add(std::make_unique<FakeBackend>("TestA"));
    registry.add(std::make_unique<FakeBackend>("TestB"));

    // Nothing set: the default name.
    EXPECT_EQ(Registry::requestedName(nullptr), std::string(DefaultBackend));

    hGrp->SetASCII("SolverBackend", "TestB");
    EXPECT_EQ(Registry::requestedName(nullptr), "TestB");
    EXPECT_EQ(registry.select(static_cast<const App::DocumentObject*>(nullptr))->name(), "TestB");

    // The assembly's own property wins over the preference.
    auto docName = App::GetApplication().getUniqueDocumentName("solverRegistry");
    auto* doc = App::GetApplication().newDocument(docName.c_str(), "testUser");
    auto* assembly = doc->addObject<Assembly::AssemblyObject>();
    auto* prop = static_cast<App::PropertyString*>(
        assembly->addDynamicProperty("App::PropertyString", "SolverBackend")
    );
    ASSERT_NE(prop, nullptr);
    EXPECT_EQ(Registry::requestedName(assembly), "TestB");  // empty property: preference
    prop->setValue("TestA");
    EXPECT_EQ(Registry::requestedName(assembly), "TestA");
    EXPECT_EQ(registry.select(assembly)->name(), "TestA");
    App::GetApplication().closeDocument(docName.c_str());
}

TEST_F(SolverRegistryTest, unknownNameFallsBack)  // NOLINT
{
    auto& registry = Registry::instance();
    registry.add(std::make_unique<FakeBackend>("TestA"));
    Backend* fallback = registry.select("NoSuchSolver");
    ASSERT_NE(fallback, nullptr);
    // The default backend when one is registered, else the first one.
    if (registry.find(DefaultBackend)) {
        EXPECT_EQ(fallback->name(), std::string(DefaultBackend));
    }
    else {
        EXPECT_EQ(fallback->name(), registry.names().front());
    }
    EXPECT_EQ(registry.select("TestA")->name(), "TestA");
}
