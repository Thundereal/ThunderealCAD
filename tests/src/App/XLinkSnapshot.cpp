// SPDX-License-Identifier: LGPL-2.1-or-later

// A copy of links into a document never saved (docs/LibraryPaths.md sec 12):
// written as a snapshot of that document in its transient directory, and
// pasted to the document while it is open, else to the snapshot.

#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/LibraryPaths.h"
#include "App/MergeDocuments.h"
#include "App/PropertyLinks.h"
#include <src/App/InitApplication.h>

namespace fs = std::filesystem;

class XLinkSnapshotTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        auto& app = App::GetApplication();
        app.closeAllDocuments();
        _unsaved = app.newDocument("SnapUnsaved", "test");
        _unsaved->addObject("App::FeaturePython", "Bolt");
        _source = app.newDocument("SnapSource", "test");
        _holder = _source->addObject("App::FeaturePython", "Holder");
        linkOf(_holder)->setValue(_unsaved->getObject("Bolt"));
    }

    void TearDown() override
    {
        App::GetApplication().closeAllDocuments();
    }

    static App::PropertyXLink* linkOf(App::DocumentObject* obj)
    {
        auto prop = obj->getPropertyByName("Link");
        if (!prop)
            prop = obj->addDynamicProperty("App::PropertyXLink", "Link");
        return dynamic_cast<App::PropertyXLink*>(prop);
    }

    /// `_holder` copied, the snapshots written as the clipboard's copy does
    /// when `write`.
    std::string copy(bool write)
    {
        std::ostringstream out;
        App::PropertyXLink::SnapshotScope scope({_holder}, write);
        _source->exportObjects({_holder}, out);
        return out.str();
    }

    static App::PropertyXLink* paste(App::Document* doc, const std::string& data)
    {
        std::istringstream in(data);
        App::MergeDocuments md(doc);
        auto objs = md.importObjects(in);
        return objs.empty() ? nullptr : linkOf(objs.front());
    }

    App::Document* _unsaved {};
    App::Document* _source {};
    App::DocumentObject* _holder {};
};

TEST_F(XLinkSnapshotTest, theClipboardsCopyWritesTheSnapshotAndPastesToTheDocument)
{
    const std::string path = App::PropertyXLink::getSnapshotPath(_unsaved);
    // Compared in one spelling: the snapshot's path is Qt's, with forward
    // slashes, the transient directory the native one (backslashes on Windows).
    const auto generic = [](const std::string& p) {
        return fs::u8path(p).lexically_normal().generic_u8string();
    };
    EXPECT_EQ(generic(path).rfind(generic(_unsaved->TransientDir.getStrValue()), 0), 0U)
        << path << " not in " << _unsaved->TransientDir.getStrValue();
    const std::string data = copy(true);
    EXPECT_TRUE(fs::exists(fs::u8path(path))) << path;

    auto dst = App::GetApplication().newDocument("SnapTarget", "test");
    auto link = paste(dst, data);
    ASSERT_NE(link, nullptr);
    // Open: the document itself, not its snapshot.
    EXPECT_EQ(link->getValue(), _unsaved->getObject("Bolt"));
}

TEST_F(XLinkSnapshotTest, aCopyPastedAtOnceWritesNoSnapshot)
{
    const std::string path = App::PropertyXLink::getSnapshotPath(_unsaved);
    const std::string data = copy(false);
    EXPECT_FALSE(fs::exists(fs::u8path(path))) << path;
    auto dst = App::GetApplication().newDocument("SnapTarget", "test");
    auto link = paste(dst, data);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getValue(), _unsaved->getObject("Bolt"));

    // As Document::copyObject does, which refused it before.
    auto copied = dst->copyObject({_holder});
    ASSERT_EQ(copied.size(), 1U);
    EXPECT_EQ(linkOf(copied.front())->getValue(), _unsaved->getObject("Bolt"));
    EXPECT_FALSE(fs::exists(fs::u8path(path))) << path;
}

TEST_F(XLinkSnapshotTest, withTheDocumentGoneThePasteOpensTheSnapshotOnce)
{
    const std::string path = App::PropertyXLink::getSnapshotPath(_unsaved);
    const std::string data = copy(true);
    ASSERT_TRUE(fs::exists(fs::u8path(path))) << path;

    // Closed, the document takes its transient directory with it: the
    // snapshot put back where it was, as a copy kept elsewhere would find it.
    const fs::path keptDir = fs::temp_directory_path()
        / ("snapkeep-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(keptDir);
    fs::copy_file(fs::u8path(path), keptDir / "snapshot", fs::copy_options::overwrite_existing);
    const std::string uid = _unsaved->Uid.getValueStr();
    App::GetApplication().closeDocument(_unsaved->getName());
    _unsaved = nullptr;
    fs::create_directories(fs::u8path(path).parent_path());
    fs::copy_file(keptDir / "snapshot", fs::u8path(path), fs::copy_options::overwrite_existing);

    auto dst = App::GetApplication().newDocument("SnapTarget", "test");
    auto link = paste(dst, data);
    ASSERT_NE(link, nullptr);
    auto bolt = link->getValue();
    ASSERT_NE(bolt, nullptr);
    // Opened as a document never saved, with the Uid of the one it is of.
    EXPECT_FALSE(bolt->getDocument()->isSaved()) << bolt->getDocument()->getFileName();
    EXPECT_EQ(bolt->getDocument()->Uid.getValueStr(), uid);
    // Linked to as to any document never saved: one the save of the
    // document linking to it asks for first.
    const auto deps = dst->getDependentDocuments();
    EXPECT_NE(std::find(deps.begin(), deps.end(), bolt->getDocument()), deps.end());

    // Pasted again, the snapshot's file gone: the one open is taken, not
    // opened twice, and needs no file.
    std::error_code ec;
    fs::remove_all(fs::u8path(path).parent_path(), ec);
    const auto count = App::GetApplication().getDocuments().size();
    auto again = paste(dst, data);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->getValue(), bolt);
    EXPECT_EQ(App::GetApplication().getDocuments().size(), count);

    App::GetApplication().closeAllDocuments();
    fs::remove_all(fs::u8path(path).parent_path().parent_path(), ec);
    fs::remove_all(keptDir, ec);
}

TEST_F(XLinkSnapshotTest, aDocumentNeverSavedIsADependency)
{
    // A link into it has no DocInfo; the documents depended on list it all
    // the same, so a save asks to save it first.
    auto deps = _source->getDependentDocuments();
    EXPECT_NE(std::find(deps.begin(), deps.end(), _unsaved), deps.end());
    auto in = App::PropertyXLink::getDocumentInList(_unsaved);
    EXPECT_EQ(in[_unsaved].count(_source), 1U);
}

TEST_F(XLinkSnapshotTest, aLinkToAnObjectLeftOutIsLocalInItsOwnDocument)
{
    // The copy's own document never saved, its link to an object of it
    // that is not copied.
    auto other = _source->addObject("App::FeaturePython", "Other");
    linkOf(_holder)->setValue(other);
    const std::string data = copy(true);
    EXPECT_TRUE(fs::exists(fs::u8path(App::PropertyXLink::getSnapshotPath(_source))));

    auto link = paste(_source, data);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getValue(), other);

    auto dst = App::GetApplication().newDocument("SnapTarget", "test");
    link = paste(dst, data);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getValue(), other);
}

TEST_F(XLinkSnapshotTest, aSnapshotsOwnLinksNameTheirSnapshots)
{
    // The document linked to links on, into another never saved.
    auto further = App::GetApplication().newDocument("SnapFurther", "test");
    auto nut = further->addObject("App::FeaturePython", "Nut");
    auto bolt = _unsaved->getObject("Bolt");
    linkOf(bolt)->setValue(nut);
    copy(true);
    const std::string path = App::PropertyXLink::getSnapshotPath(_unsaved);
    ASSERT_TRUE(fs::exists(fs::u8path(path)));
    EXPECT_TRUE(fs::exists(fs::u8path(App::PropertyXLink::getSnapshotPath(further))));

    // The snapshot opened: its link goes to the document still open.
    auto snap = App::GetApplication().openDocument(path.c_str());
    ASSERT_NE(snap, nullptr);
    ASSERT_NE(snap, _unsaved);
    auto snapBolt = snap->getObject("Bolt");
    ASSERT_NE(snapBolt, nullptr);
    EXPECT_EQ(linkOf(snapBolt)->getValue(), nut);
}
