// SPDX-License-Identifier: LGPL-2.1-or-later

// Library paths (docs/LibraryPaths.md): links into named folders, saved as
// the name and the path under it beside the old path.

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/LibraryPaths.h"
#include "App/PropertyLinks.h"
#include "Base/Exception.h"
#include "Base/FileInfo.h"
#include "Base/Writer.h"
#include <src/App/InitApplication.h>

namespace fs = std::filesystem;
using App::LibraryPaths;

// NOLINTBEGIN(readability-magic-numbers)

namespace
{

// The names these tests set in the settings; removed after each.
const char* const LibName = "GtLib";
const char* const StdName = "GtStd";

std::string str(const fs::path& p)
{
    return p.generic_string();
}

}  // namespace

class LibraryPathsTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        App::GetApplication().closeAllDocuments();
        clearSettings();
        _root = fs::path(Base::FileInfo::getTempPath())
            / ("libpaths-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-"
               + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(_root);
        fs::create_directories(_root / "lib" / "fasteners");
        fs::create_directories(_root / "asm");
    }

    void TearDown() override
    {
        App::GetApplication().closeAllDocuments();
        clearSettings();
        std::error_code ec;
        fs::remove_all(_root, ec);
    }

    static void clearSettings()
    {
        for (auto& v : LibraryPaths::getSystem()) {
            if (v.first.rfind("Gt", 0) == 0)
                LibraryPaths::removeSystem(v.first);
        }
    }

    static std::string ext()
    {
        return App::Document::getFileExtension(App::Document::getCurrentSchemaVersion());
    }

    /// A document at `path` holding one object, "Bolt".
    static void makePart(const fs::path& path)
    {
        auto& app = App::GetApplication();
        const std::string name = app.getUniqueDocumentName("part");
        App::Document* doc = app.newDocument(name.c_str(), "test");
        doc->addObject("App::FeaturePython", "Bolt");
        fs::create_directories(path.parent_path());
        ASSERT_TRUE(doc->saveAs(str(path).c_str()));
        app.closeDocument(name.c_str());
    }

    /// A new document, with an object holding an xlink to `target`'s Bolt.
    App::PropertyXLink* makeOwner(App::Document*& owner, const fs::path& target)
    {
        auto& app = App::GetApplication();
        App::Document* part = app.openDocument(str(target).c_str());
        EXPECT_NE(part, nullptr);
        const std::string name = app.getUniqueDocumentName("asm");
        owner = app.newDocument(name.c_str(), "test");
        auto obj = owner->addObject("App::FeaturePython", "Holder");
        auto link = dynamic_cast<App::PropertyXLink*>(
            obj->addDynamicProperty("App::PropertyXLink", "Link"));
        link->setValue(part->getObject("Bolt"));
        return link;
    }

    static App::PropertyXLink* linkOf(App::Document* doc)
    {
        auto obj = doc ? doc->getObject("Holder") : nullptr;
        return obj ? dynamic_cast<App::PropertyXLink*>(obj->getPropertyByName("Link")) : nullptr;
    }

    static std::string saved(const App::PropertyXLink* link)
    {
        Base::StringWriter writer;
        link->Save(writer);
        return writer.getString();
    }

    App::Document* reopen(const fs::path& path)
    {
        App::GetApplication().closeAllDocuments();
        return App::GetApplication().openDocument(str(path).c_str());
    }

    fs::path part() const
    {
        return _root / "lib" / "fasteners" / ("bolt." + ext());
    }

    fs::path assembly() const
    {
        return _root / "asm" / ("asm." + ext());
    }

    fs::path _root;
};

TEST_F(LibraryPathsTest, namesAndTokens)
{
    EXPECT_TRUE(LibraryPaths::isValidName("Lib"));
    EXPECT_TRUE(LibraryPaths::isValidName("Lib_2"));
    EXPECT_FALSE(LibraryPaths::isValidName("2Lib"));
    EXPECT_FALSE(LibraryPaths::isValidName("Li b"));
    EXPECT_FALSE(LibraryPaths::isValidName(""));

    std::string name, file;
    EXPECT_TRUE(LibraryPaths::parseToken("%Lib%/fasteners/bolt.FCStd", name, file));
    EXPECT_EQ(name, "Lib");
    EXPECT_EQ(file, "fasteners/bolt.FCStd");
    EXPECT_EQ(LibraryPaths::makeToken(name, file), "%Lib%/fasteners/bolt.FCStd");
    // Never climbs out of its folder.
    EXPECT_FALSE(LibraryPaths::parseToken("%Lib%/../bolt.FCStd", name, file));
    EXPECT_FALSE(LibraryPaths::parseToken("%Lib%bolt.FCStd", name, file));
    EXPECT_FALSE(LibraryPaths::parseToken("/abs/bolt.FCStd", name, file));

    EXPECT_THROW(LibraryPaths::cleanFolder("relative/dir"), Base::ValueError);
    EXPECT_EQ(LibraryPaths::cleanFolder("/a/b/../c/"), "/a/c");
}

TEST_F(LibraryPathsTest, matchTakesTheDeepestAndTheDocumentsName)
{
    LibraryPaths::Match m;
    // The deepest folder wins.
    EXPECT_TRUE(LibraryPaths::match("/l/std/bolt.FCStd", {}, {{"Lib", "/l"}, {"Std", "/l/std"}},
                                    "", m));
    EXPECT_EQ(m.name, "Std");
    EXPECT_EQ(m.file, "bolt.FCStd");
    EXPECT_TRUE(m.added);
    // Whole path components only.
    EXPECT_FALSE(LibraryPaths::match("/library/x.FCStd", {}, {{"Lib", "/lib"}}, "", m));
    // One folder under two names: the document's.
    EXPECT_TRUE(LibraryPaths::match("/l/x.FCStd", {{"Mine", "/l"}}, {{"Lib", "/l"}}, "", m));
    EXPECT_EQ(m.name, "Mine");
    EXPECT_FALSE(m.added);
    // A name the document maps elsewhere: the next free one (Q4).
    const std::string a = str(_root / "lib");
    EXPECT_TRUE(LibraryPaths::match("/b/x.FCStd", {{"Lib", a}}, {{"Lib", "/b"}}, "", m));
    EXPECT_EQ(m.name, "Lib_2");
    EXPECT_TRUE(m.renamed);
    EXPECT_TRUE(LibraryPaths::match("/b/x.FCStd", {{"Lib", a}, {"Lib_2", "/c"}},
                                    {{"Lib", "/b"}}, "", m));
    EXPECT_EQ(m.name, "Lib_3");
    // Elsewhere, where its folder is not here, it is no clash: the name.
    EXPECT_TRUE(LibraryPaths::match("/b/x.FCStd", {{"Lib", "/nowhere/lib"}}, {{"Lib", "/b"}},
                                    "", m));
    EXPECT_EQ(m.name, "Lib");
    EXPECT_FALSE(m.renamed);
    // A document inside the library links relatively (Q5).
    EXPECT_FALSE(LibraryPaths::match("/l/std/x.FCStd", {}, {{"Lib", "/l"}, {"Std", "/l/std"}},
                                     "/l/other/owner.FCStd", m));
}

TEST_F(LibraryPathsTest, resolveTakesTheDocumentsFolderFirst)
{
    makePart(part());
    const std::string lib = str(_root / "lib");
    const std::string copy = str(_root / "copy");
    fs::create_directories(_root / "copy" / "fasteners");
    fs::copy_file(part(), _root / "copy" / "fasteners" / part().filename());
    const std::string file = "fasteners/" + part().filename().string();

    LibraryPaths::Resolved r;
    EXPECT_TRUE(LibraryPaths::resolve("L", file, {{"L", lib}}, {{"L", copy}}, r));
    EXPECT_EQ(r.folder, lib);
    EXPECT_TRUE(r.fromDocument);
    EXPECT_TRUE(r.conflict);  // Q2: both hold it
    EXPECT_EQ(r.other, copy);
    EXPECT_TRUE(LibraryPaths::resolve("L", file, {{"L", str(_root / "gone")}}, {{"L", copy}}, r));
    EXPECT_EQ(r.folder, copy);
    EXPECT_FALSE(r.fromDocument);
    EXPECT_FALSE(LibraryPaths::resolve("L", file, {{"L", str(_root / "gone")}}, {}, r));
}

TEST_F(LibraryPathsTest, savedBesideTheOldPath)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));

    EXPECT_EQ(link->getLibraryName(), LibName);
    EXPECT_EQ(link->getLibraryFile(), "fasteners/" + part().filename().string());
    EXPECT_EQ(std::string(link->getFilePath()),
              std::string("%GtLib%/fasteners/") + part().filename().string());
    const std::string xml = saved(link);
    EXPECT_NE(xml.find("file=\"../lib/fasteners/" + part().filename().string() + "\""),
              std::string::npos) << xml;
    EXPECT_NE(xml.find("lib=\"GtLib\" libfile=\"fasteners/"), std::string::npos) << xml;
    // The document keeps the folder.
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "lib"));
}

TEST_F(LibraryPathsTest, openedWithTheLibraryMoved)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    App::GetApplication().closeAllDocuments();

    // Elsewhere on this machine, and the settings say so: the document's
    // folder and the old path both miss.
    fs::create_directories(_root / "moved");
    fs::rename(_root / "lib", _root / "moved" / "lib");
    LibraryPaths::setSystem(LibName, str(_root / "moved" / "lib"));

    owner = reopen(assembly());
    ASSERT_NE(owner, nullptr);
    auto link = linkOf(owner);
    ASSERT_NE(link, nullptr);
    ASSERT_NE(link->getValue(), nullptr);
    EXPECT_TRUE(LibraryPaths::samePath(link->getFullPath(),
                                       str(_root / "moved" / "lib" / "fasteners" / part().filename())));
    EXPECT_EQ(link->getLibraryFolder(), str(_root / "moved" / "lib"));
    // The map follows, in memory (Q3).
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "moved" / "lib"));
}

TEST_F(LibraryPathsTest, openedWithOnlyTheDocumentsFolder)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    // The assembly moves away from the library: the old relative path misses.
    fs::create_directories(_root / "far" / "away");
    const fs::path moved = _root / "far" / "away" / assembly().filename();
    App::GetApplication().closeAllDocuments();
    fs::rename(assembly(), moved);
    LibraryPaths::removeSystem(LibName);

    owner = reopen(moved);
    auto link = linkOf(owner);
    ASSERT_NE(link, nullptr);
    EXPECT_NE(link->getValue(), nullptr);
    EXPECT_EQ(link->getLibraryFolder(), str(_root / "lib"));
}

TEST_F(LibraryPathsTest, openedWithNeitherIsBrokenAndKeepsTheLibrary)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    App::GetApplication().closeAllDocuments();
    fs::rename(_root / "lib", _root / "gone");
    LibraryPaths::removeSystem(LibName);

    owner = reopen(assembly());
    auto link = linkOf(owner);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getValue(), nullptr);
    EXPECT_EQ(link->getLibraryName(), LibName);
    EXPECT_TRUE(link->getLibraryFolder().empty());
    // Saved again, the library stays for a machine that has it.
    ASSERT_TRUE(owner->save());
    EXPECT_NE(saved(link).find("lib=\"GtLib\""), std::string::npos);
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "lib"));
}

TEST_F(LibraryPathsTest, nestedFoldersTakeTheDeepest)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    LibraryPaths::setSystem(StdName, str(_root / "lib" / "fasteners"));
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    EXPECT_EQ(link->getLibraryName(), StdName);
    EXPECT_EQ(link->getLibraryFile(), part().filename().string());
    // Only the name used is kept.
    EXPECT_EQ(owner->LibraryPaths.getValues().count(LibName), 0u);
    EXPECT_EQ(owner->LibraryPaths.getValues().count(StdName), 1u);
}

TEST_F(LibraryPathsTest, aNameTheDocumentMapsElsewhereGetsANewOne)
{
    makePart(part());
    const fs::path other = _root / "other" / ("nut." + ext());
    makePart(other);
    LibraryPaths::setSystem(LibName, str(_root / "other"));
    App::Document* owner = nullptr;
    auto first = makeOwner(owner, other);
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    ASSERT_EQ(first->getLibraryName(), LibName);

    // The settings now name another folder GtLib; a link into it.
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* part2 = App::GetApplication().openDocument(str(part()).c_str());
    auto obj = owner->addObject("App::FeaturePython", "Holder2");
    auto second = dynamic_cast<App::PropertyXLink*>(
        obj->addDynamicProperty("App::PropertyXLink", "Link"));
    second->setValue(part2->getObject("Bolt"));
    EXPECT_EQ(second->getLibraryName(), "GtLib_2");
    EXPECT_EQ(first->getLibraryName(), LibName);
    EXPECT_EQ(owner->LibraryPaths.getValues().at("GtLib_2"), str(_root / "lib"));
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "other"));
}

TEST_F(LibraryPathsTest, anUnsavedOwnerHoldsALibraryLink)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    // Not saved, and still a library link with a file.
    EXPECT_EQ(link->getLibraryName(), LibName);
    EXPECT_FALSE(link->getFullPath().empty());
    // Saved, the old path is relative.
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    EXPECT_NE(saved(link).find("file=\"../lib/fasteners/"), std::string::npos) << saved(link);
}

TEST_F(LibraryPathsTest, aDocumentInsideTheLibraryLinksRelatively)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    // Set while the owner had no file, then saved inside the library.
    ASSERT_TRUE(owner->saveAs(str(_root / "lib" / "asm" / ("asm." + ext())).c_str()));
    EXPECT_TRUE(link->getLibraryName().empty());
    EXPECT_EQ(saved(link).find("lib="), std::string::npos);
    EXPECT_NE(saved(link).find("file=\"../fasteners/"), std::string::npos) << saved(link);
    // Set again once it has a file.
    auto bolt = link->getValue();
    link->setValue(nullptr);
    link->setValue(bolt);
    EXPECT_TRUE(link->getLibraryName().empty());
}

TEST_F(LibraryPathsTest, anOldDocumentIsConvertedAtLoad)
{
    makePart(part());
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    ASSERT_TRUE(link->getLibraryName().empty());

    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    owner = reopen(assembly());
    link = linkOf(owner);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getLibraryName(), LibName);
    ASSERT_TRUE(owner->save());
    EXPECT_NE(saved(link).find("lib=\"GtLib\""), std::string::npos);
}

TEST_F(LibraryPathsTest, aStandardFormatSaveKeepsTheLibrary)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    owner->SaveSchemaVersion.setValue(4);
    const fs::path fc = _root / "asm" / "asm.FCStd";
    ASSERT_TRUE(owner->saveAs(str(fc).c_str()));

    App::GetApplication().closeAllDocuments();
    fs::rename(_root / "lib", _root / "moved");
    LibraryPaths::setSystem(LibName, str(_root / "moved"));
    owner = reopen(fc);
    auto link = linkOf(owner);
    ASSERT_NE(link, nullptr);
    EXPECT_NE(link->getValue(), nullptr);
    EXPECT_EQ(link->getLibraryFolder(), str(_root / "moved"));
}

TEST_F(LibraryPathsTest, aCopyIntoAnotherDocumentKeepsTheLibrary)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));

    const std::string name = App::GetApplication().getUniqueDocumentName("other");
    App::Document* other = App::GetApplication().newDocument(name.c_str(), "test");
    // Elsewhere, and not beside the library: the old path would not do.
    // A copy with external links still needs the target saved once.
    fs::create_directories(_root / "x" / "y");
    ASSERT_TRUE(other->saveAs(str(_root / "x" / "y" / ("other." + ext())).c_str()));
    auto copies = other->copyObject({owner->getObject("Holder")});
    ASSERT_EQ(copies.size(), 1u);
    auto link = dynamic_cast<App::PropertyXLink*>(copies[0]->getPropertyByName("Link"));
    ASSERT_NE(link, nullptr);
    EXPECT_NE(link->getValue(), nullptr);
    EXPECT_EQ(link->getLibraryName(), LibName);
    EXPECT_EQ(other->LibraryPaths.getValues().count(LibName), 1u);
}

TEST_F(LibraryPathsTest, editingTheDocumentsFolderRemapsAsOneUndoStep)
{
    makePart(part());
    fs::create_directories(_root / "lib2" / "fasteners");
    fs::copy_file(part(), _root / "lib2" / "fasteners" / part().filename());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    auto link = makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    owner->setUndoMode(1);
    App::Document* before = link->getValue()->getDocument();

    owner->openTransaction("remap");
    owner->LibraryPaths.setValue(LibName, str(_root / "lib2"));
    owner->commitTransaction();
    ASSERT_NE(link->getValue(), nullptr);
    EXPECT_NE(link->getValue()->getDocument(), before);
    EXPECT_TRUE(LibraryPaths::samePath(link->getFullPath(),
                                       str(_root / "lib2" / "fasteners" / part().filename())));

    ASSERT_TRUE(owner->undo());
    ASSERT_NE(link->getValue(), nullptr);
    EXPECT_EQ(link->getValue()->getDocument(), before);
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "lib"));
}

TEST_F(LibraryPathsTest, aSettingChangedWithDocumentsOpen)
{
    makePart(part());
    fs::create_directories(_root / "lib2" / "fasteners");
    fs::copy_file(part(), _root / "lib2" / "fasteners" / part().filename());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    App::Document* owner = nullptr;
    makeOwner(owner, part());
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));

    // The document's folder holds the file: it wins, nothing to change.
    auto before = LibraryPaths::getSystem();
    LibraryPaths::setSystem(LibName, str(_root / "lib2"));
    EXPECT_TRUE(LibraryPaths::changed(before).empty());

    // It no longer names a folder that holds it: the setting's would.
    owner->setLibraryPathsQuietly({{LibName, str(_root / "gone")}});
    before = LibraryPaths::getSystem();
    LibraryPaths::setSystem(LibName, str(_root / "lib2"));
    auto affected = LibraryPaths::changed(before);
    ASSERT_EQ(affected.size(), 1u);
    EXPECT_EQ(affected[0].document, owner);
    EXPECT_EQ(affected[0].links, 1);
    // Left as it was till applied.
    auto link = linkOf(owner);
    EXPECT_TRUE(LibraryPaths::samePath(link->getFullPath(), str(part())));
    EXPECT_EQ(LibraryPaths::apply(*owner), 1);
    EXPECT_TRUE(LibraryPaths::samePath(link->getFullPath(),
                                       str(_root / "lib2" / "fasteners" / part().filename())));
    EXPECT_EQ(owner->LibraryPaths.getValues().at(LibName), str(_root / "lib2"));
}

TEST_F(LibraryPathsTest, aTokenSetsALink)
{
    makePart(part());
    LibraryPaths::setSystem(LibName, str(_root / "lib"));
    const std::string token = std::string("%GtLib%/fasteners/") + part().filename().string();
    EXPECT_TRUE(LibraryPaths::samePath(LibraryPaths::resolveToken(token), str(part())));

    const std::string name = App::GetApplication().getUniqueDocumentName("asm");
    App::Document* owner = App::GetApplication().newDocument(name.c_str(), "test");
    ASSERT_TRUE(owner->saveAs(str(assembly()).c_str()));
    auto obj = owner->addObject("App::FeaturePython", "Holder");
    auto link = dynamic_cast<App::PropertyXLink*>(
        obj->addDynamicProperty("App::PropertyXLink", "Link"));
    link->setValue(std::string(token), "Bolt", {});
    EXPECT_NE(link->getValue(), nullptr);
    EXPECT_EQ(link->getLibraryName(), LibName);
    EXPECT_THROW(link->setValue(std::string("%GtNone%/x.FCStd"), "Bolt", {}), Base::ValueError);
}

TEST_F(LibraryPathsTest, theMergeComparesALibraryLinkByItsLibrary)
{
    const std::string a = "<XLink file=\"../../a/lib/f/bolt.FCStd\" stamp=\"2026-10-18T01:00:00Z\" "
                          "name=\"Bolt\" lib=\"Lib\" libfile=\"f/bolt.FCStd\" sub=\"Face1\"/>";
    const std::string b = "<XLink file=\"/b/lib/f/bolt.FCStd\" stamp=\"2026-10-17T09:00:00Z\" "
                          "name=\"Bolt\" lib=\"Lib\" libfile=\"f/bolt.FCStd\" sub=\"Face1\"/>";
    EXPECT_EQ(LibraryPaths::comparableSaved(a), LibraryPaths::comparableSaved(b));
    // Another object, or another file of the library, is another link.
    std::string other = b;
    other.replace(other.find("Face1"), 5, "Face2");
    EXPECT_NE(LibraryPaths::comparableSaved(a), LibraryPaths::comparableSaved(other));
    // A link with no library is compared as it is.
    const std::string plain = "<XLink file=\"x.FCStd\" stamp=\"s\" name=\"Bolt\"/>";
    EXPECT_EQ(LibraryPaths::comparableSaved(plain), plain);
}

// NOLINTEND(readability-magic-numbers)
