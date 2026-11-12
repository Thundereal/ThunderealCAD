// SPDX-License-Identifier: LGPL-2.1-or-later

// What a property write does -- opens an undo step, is recorded in one,
// changes the file, marks the document modified, is keyed by Record -- is
// said in one place, the table of App/WriteScope.cpp. These pin it, row by
// row, so a change to a cell is a change made on purpose.

#include "gtest/gtest.h"

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/FeatureTest.h"
#include "App/WriteScope.h"
#include <src/App/InitApplication.h>

// NOLINTBEGIN(readability-magic-numbers)

namespace
{
namespace C = App::WriteCause;
namespace E = App::WriteEffect;
const unsigned All = E::Step | E::Undo | E::Dirty | E::Modified | E::Key;

App::WriteTraits traits(const char* which, bool logOn = true)
{
    App::WriteTraits t;
    t.logOn = logOn;
    for (const char* c = which; *c; ++c) {
        switch (*c) {
            case 'v':
                t.view = true;
                break;
            case 'm':
                t.viewMirror = true;
                break;
            case 'u':
                t.unsaved = true;
                break;
            case 'n':
                t.noModify = true;
                break;
            case 'l':
                t.legacy = true;
                break;
            case 't':
                t.touch = true;
                break;
            case 'U':
                t.viewUndo = true;
                break;
            default:
                ADD_FAILURE() << "unknown trait " << *c;
        }
    }
    return t;
}

unsigned fx(unsigned causes, const char* which, bool logOn = true)
{
    return App::writeEffectsOf(causes, traits(which, logOn));
}
}  // namespace

TEST(WriteEffects, anEditOfASavedPropertyDoesEverything)
{
    EXPECT_EQ(fx(C::Edit, ""), All);
    EXPECT_EQ(fx(C::Edit, "v"), All);
    EXPECT_EQ(fx(C::Edit, "m"), All);
}

TEST(WriteEffects, byWhatIsWritten)
{
    // NoModify: recorded in an open step and written by a save, but opens
    // nothing and marks nothing
    EXPECT_EQ(fx(C::Edit, "n"), E::Undo | E::Dirty | E::Key);
    EXPECT_EQ(fx(C::Edit, "vn"), E::Undo | E::Dirty | E::Key);
    // Legacy: the write to the value it names is what opens a step
    EXPECT_EQ(fx(C::Edit, "l"), E::Undo | E::Dirty | E::Modified | E::Key);
    // Never saved: no change to the file
    EXPECT_EQ(fx(C::Edit, "u"), E::Step | E::Undo | E::Modified | E::Key);
}

TEST(WriteEffects, aViewProvidersPropertyOpensAStepByTheLogOrTheSetting)
{
    // Log on: every saved one, whatever ViewObjectTransaction says
    EXPECT_EQ(fx(C::Edit, "v", true), All);
    EXPECT_EQ(fx(C::Edit, "vU", true), All);
    EXPECT_EQ(fx(C::Edit, "vu", true), E::Undo | E::Modified | E::Key);
    // Log off: the setting
    EXPECT_EQ(fx(C::Edit, "v", false), All & ~E::Step);
    EXPECT_EQ(fx(C::Edit, "vU", false), All);
    EXPECT_EQ(fx(C::Edit, "vuU", false), All & ~E::Dirty);
    // An object's own is not the setting's business
    EXPECT_EQ(fx(C::Edit, "", false), All);
    EXPECT_EQ(fx(C::Edit, "u", false), All & ~E::Dirty);
}

TEST(WriteEffects, aViewFittedToTheModelIsNoEdit)
{
    // Recorded in a step that is open, so undo stays exact, and saved
    EXPECT_EQ(fx(C::ViewFit, "v"), E::Undo | E::Dirty | E::Key);
    // The mirror on the object marks nothing either
    EXPECT_EQ(fx(C::ViewFit, "m"), All & ~E::Modified);
    // What the fit writes on an object is an edit like any other
    EXPECT_EQ(fx(C::ViewFit, ""), All);
    // Log off: the scope changes nothing about steps
    EXPECT_EQ(fx(C::ViewFit, "vU", false), All & ~E::Modified);
}

TEST(WriteEffects, bookkeepingIsInNoStep)
{
    EXPECT_EQ(fx(C::Bookkeeping, ""), E::Dirty | E::Modified | E::Key);
    EXPECT_EQ(fx(C::Bookkeeping, "u"), E::Modified | E::Key);
}

TEST(WriteEffects, whatTheDocumentPutsBackOrDerivesIsNotKeyed)
{
    for (unsigned cause : {C::Transacting, C::Restoring, C::Recompute}) {
        EXPECT_EQ(fx(cause, ""), All & ~E::Key) << cause;
        EXPECT_EQ(fx(cause, "v"), All & ~E::Key) << cause;
    }
}

TEST(WriteEffects, anAnimationsWriteDoesNothing)
{
    for (const char* which : {"", "v", "m", "u", "n", "l", "vu", "t"}) {
        EXPECT_EQ(fx(C::Animation, which), 0U) << which;
        EXPECT_EQ(fx(C::Animation | C::ViewFit, which), 0U) << which;
        EXPECT_EQ(fx(C::Animation | C::Recompute, which), 0U) << which;
    }
}

TEST(WriteEffects, aDeferredLoadsValueIsNoChange)
{
    EXPECT_EQ(fx(C::DeferredLoad, ""), 0U);
    EXPECT_EQ(fx(C::DeferredLoad, "u"), 0U);
}

TEST(WriteEffects, aTouchIsAnEditButLeavesTheObjectsFileAlone)
{
    // The touch state is saved in the document's own file, which every
    // save writes; a folder save keeps the object's (Document.py's
    // FolderSaveCases count on it: a recompute touches and purges all it
    // passes)
    EXPECT_EQ(fx(C::Edit, "t"), unsigned(E::Modified));
    EXPECT_EQ(fx(C::Recompute, "t"), unsigned(E::Modified));
    EXPECT_EQ(fx(C::ViewFit, "t"), unsigned(E::Modified));
}

TEST(WriteScope, scopesAddUpAndAnEditStartsOver)
{
    EXPECT_EQ(App::WriteScope::causes(), 0U);
    {
        App::WriteScope animated(C::Animation);
        EXPECT_EQ(App::WriteScope::causes(), unsigned(C::Animation));
        {
            App::WriteScope fit(C::ViewFit);
            EXPECT_EQ(App::WriteScope::causes(), C::Animation | C::ViewFit);
            {
                App::WriteScope edit(C::Edit);
                EXPECT_EQ(App::WriteScope::causes(), 0U);
                {
                    App::WriteScope fit2(C::ViewFit);
                    EXPECT_EQ(App::WriteScope::causes(), unsigned(C::ViewFit));
                }
                EXPECT_EQ(App::WriteScope::causes(), 0U);
            }
            EXPECT_EQ(App::WriteScope::causes(), C::Animation | C::ViewFit);
        }
        EXPECT_EQ(App::WriteScope::causes(), unsigned(C::Animation));
    }
    EXPECT_EQ(App::WriteScope::causes(), 0U);
}

class WriteScopeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _obj = dynamic_cast<App::FeatureTest*>(_doc->addObject("App::FeatureTest", "A"));
        ASSERT_NE(_obj, nullptr);
        _doc->commitTransaction();
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc()
    {
        return _doc;
    }

    App::FeatureTest* obj()
    {
        return _obj;
    }

private:
    std::string _docName;
    App::Document* _doc {};
    App::FeatureTest* _obj {};
};

TEST_F(WriteScopeTest, theDocumentClassifiesAWriteFromItsScopesAndTheProperty)
{
    EXPECT_EQ(doc()->writeEffects(obj()->Integer), All);
    {
        App::WriteScope animated(C::Animation);
        EXPECT_EQ(doc()->writeEffects(obj()->Integer), 0U);
        EXPECT_EQ(doc()->touchEffects(), 0U);
        App::WriteScope edit(C::Edit);
        EXPECT_EQ(doc()->writeEffects(obj()->Integer), All);
    }
    {
        App::WriteScope fit(C::ViewFit);
        EXPECT_EQ(doc()->writeEffects(obj()->Integer), All);
        EXPECT_EQ(doc()->writeEffects(obj()->ViewObject) & E::Modified, 0U);
    }
    obj()->Integer.setStatus(App::Property::NoModify, true);
    EXPECT_EQ(doc()->writeEffects(obj()->Integer), E::Undo | E::Dirty | E::Key);
    obj()->Integer.setStatus(App::Property::NoModify, false);
    obj()->Integer.setStatus(App::Property::Transient, true);
    EXPECT_EQ(doc()->writeEffects(obj()->Integer), All & ~E::Dirty);
}

TEST_F(WriteScopeTest, anEditIsSaidOnceByTheDocument)
{
    int edits = 0;
    auto conn = doc()->signalEdited.connect([&](const App::Document&) { ++edits; });
    obj()->Integer.setValue(3);
    EXPECT_GT(edits, 0);
    edits = 0;
    obj()->purgeTouched();
    EXPECT_EQ(edits, 1);
    edits = 0;
    obj()->touch();
    EXPECT_EQ(edits, 1);
    obj()->purgeTouched();
    edits = 0;
    {
        // Played: neither the write nor the touch it makes is an edit
        App::WriteScope animated(C::Animation);
        obj()->Integer.setValue(4);
        obj()->touch();
        obj()->purgeTouched();
        EXPECT_EQ(edits, 0);
    }
    obj()->Integer.setStatus(App::Property::NoModify, true);
    obj()->purgeTouched();
    edits = 0;
    obj()->Integer.setValue(5);
    // NoModify: no edit, though it marks its object touched as it goes
    EXPECT_EQ(edits, 0);
    conn.disconnect();
}

TEST_F(WriteScopeTest, anAnimatedWriteIsInNoStepAndAnEditInsideItIs)
{
    doc()->setUndoMode(1);
    ASSERT_FALSE(doc()->hasPendingTransaction());
    {
        App::WriteScope animated(C::Animation);
        obj()->Integer.setValue(7);
        EXPECT_FALSE(doc()->hasPendingTransaction());
        App::WriteScope edit(C::Edit);
        doc()->openTransaction("edit");
        obj()->Integer.setValue(8);
    }
    doc()->commitTransaction();
    ASSERT_TRUE(doc()->undo());
    // The step holds the edit alone: undone to what the animation wrote
    EXPECT_EQ(obj()->Integer.getValue(), 7);
}

// NOLINTEND(readability-magic-numbers)
