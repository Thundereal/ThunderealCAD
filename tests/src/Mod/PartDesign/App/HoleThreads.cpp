// SPDX-License-Identifier: LGPL-2.1-or-later

// A hole's cosmetic threads are made once and kept until a property of the
// hole changes (docs/WorkerProcess.md sec 63): the view asks for them for
// every feature after the hole, at every change of their shapes.

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <Base/Interpreter.h>
#include <Mod/PartDesign/App/FeatureHole.h>

#include <src/App/InitApplication.h>

namespace
{

class HoleThreadsTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import Part, PartDesign, Sketcher");
    }

    static void run(const std::string& code)
    {
        try {
            Base::Interpreter().runString(code.c_str());
        }
        catch (const Base::Exception& e) {
            FAIL() << e.what();
        }
    }

    void SetUp() override
    {
        _name = App::GetApplication().getUniqueDocumentName("holeThreads");
        _doc = App::GetApplication().newDocument(_name.c_str(), "testUser");
        run("import FreeCAD as App, Part\n"
             "doc = App.getDocument('" + _name + "')\n"
             "body = doc.addObject('PartDesign::Body', 'Body')\n"
             "box = body.newObject('PartDesign::AdditiveBox', 'Box')\n"
             "box.Length = box.Width = box.Height = 10\n"
            "doc.recompute()\n"
            "if box.Shape.isNull(): raise RuntimeError('box: %s %s' % (box.State, box.getStatusString()))\n");
        run("sketch = body.newObject('Sketcher::SketchObject', 'Sketch')\n"
             "sketch.AttachmentSupport = (box, ['Face6'])\n");
        run("sketch.MapMode = 'FlatFace'\n"
             "sketch.addGeometry(Part.Circle(App.Vector(3, 3, 0), App.Vector(0, 0, 1), 1))\n"
             "sketch.addGeometry(Part.Circle(App.Vector(7, 6, 0), App.Vector(0, 0, 1), 1))\n"
             "doc.recompute()\n");
        run("hole = body.newObject('PartDesign::Hole', 'Hole')\n"
             "hole.Profile = sketch\n"
             "hole.Threaded = True\n"
             "hole.ThreadType = 'ISOMetricProfile'\n"
             "hole.ThreadSize = 'M6'\n"
             "hole.ModelThread = False\n"
             "hole.CosmeticThread = True\n"
             "hole.DepthType = 'Dimension'\n"
             "hole.Depth = 5\n"
             "hole.ThreadDepthType = 'Hole Depth'\n"
             "doc.recompute()\n");
        _hole = dynamic_cast<PartDesign::Hole*>(_doc->getObject("Hole"));
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_name.c_str());
    }

    std::string _name;
    App::Document* _doc {};
    PartDesign::Hole* _hole {};
};

}  // namespace

TEST_F(HoleThreadsTest, madeOnceKeptUntilTheHoleChanges)
{
    ASSERT_NE(_hole, nullptr);
    ASSERT_FALSE(_hole->isError());
    const auto made = _hole->getCosmeticThreads();
    ASSERT_EQ(made.size(), 2u);
    EXPECT_DOUBLE_EQ(made[0].radius, _hole->Diameter.getValue() / 2);
    EXPECT_DOUBLE_EQ(made[0].pitch, 1.0);
    EXPECT_DOUBLE_EQ(made[0].length, 5.0);

    // Asked again, the same
    const auto again = _hole->getCosmeticThreads();
    ASSERT_EQ(again.size(), made.size());
    for (size_t i = 0; i < made.size(); ++i) {
        EXPECT_EQ(again[i].origin, made[i].origin);
        EXPECT_EQ(again[i].direction, made[i].direction);
        EXPECT_EQ(again[i].radius, made[i].radius);
    }

    // A property of the hole changed, not yet recomputed: the threads as it says
    _hole->Depth.setValue(3.0);
    ASSERT_EQ(_hole->getCosmeticThreads().size(), 2u);
    EXPECT_DOUBLE_EQ(_hole->getCosmeticThreads()[0].length, 3.0);
    _hole->CosmeticThread.setValue(false);
    EXPECT_TRUE(_hole->getCosmeticThreads().empty());
    _hole->CosmeticThread.setValue(true);
    EXPECT_EQ(_hole->getCosmeticThreads().size(), 2u);

    // The profile edited and the hole made again: one hole, one thread
    run("doc = App.getDocument('" + _name + "')\n"
        "doc.Sketch.delGeometry(1)\n"
        "doc.recompute()\n");
    EXPECT_EQ(_hole->getCosmeticThreads().size(), 1u);
}
