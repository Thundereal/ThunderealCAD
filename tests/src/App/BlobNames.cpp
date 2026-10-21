// SPDX-License-Identifier: LGPL-2.1-or-later

/* Neutral names in the transient directory (docs/HandsOnQueue.md entry 206).
 *
 * A close deletes the transient directory, and a ransomware heuristic guards
 * the delete of a file with an image or document extension: the user's
 * company's Apex ended the program over `blobs/<uuid>.png`. Every file the
 * store writes there now ends in ".blob", with the type before the suffix
 * and in the blob's own record; an old-style name from a running session's
 * directory, or a file of the user's, still answers with its own extension.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/FileBlobManager.h>
#include <App/PropertyFile.h>
#include <Base/FileInfo.h>

#include <src/App/InitApplication.h>

// In namespace App because the type system reads a registered name as
// <module>::<class> and tries to import the module for anything else.
namespace App
{
/// An object carrying nothing but an included file.
class TestNeutralNameFeature: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(TestNeutralNameFeature);

public:
    TestNeutralNameFeature()
    {
        ADD_PROPERTY_TYPE(File, (""), "Test", App::Prop_None, "an included file");
    }
    App::PropertyFileIncluded File;
    const char* getViewProviderName() const override
    {
        return "";
    }
};
}  // namespace App

PROPERTY_SOURCE(App::TestNeutralNameFeature, App::DocumentObject)

namespace
{

using App::TestNeutralNameFeature;

class BlobNamesTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        TestNeutralNameFeature::init();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("neutral");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _obj = static_cast<TestNeutralNameFeature*>(
            _doc->addObject("App::TestNeutralNameFeature", "obj"));
    }

    void TearDown() override
    {
        if (_doc) {
            App::GetApplication().closeDocument(_doc->getName());
            _doc = nullptr;
        }
        for (const auto& path : _scratch) {
            Base::FileInfo fi(path);
            if (fi.exists()) {
                try {
                    fi.deleteFile();
                }
                catch (...) {
                }
            }
        }
        _scratch.clear();
    }

    /// A scratch file holding \a content, cleaned up with the test.
    std::string writeScratch(const std::string& content, const char* suffix)
    {
        std::string path = Base::FileInfo::getTempFileName();
        path += suffix;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << content;
        }
        _scratch.push_back(path);
        return path;
    }

    static bool endsWith(const std::string& s, const std::string& tail)
    {
        return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    }

    std::string _docName;
    App::Document* _doc = nullptr;
    TestNeutralNameFeature* _obj = nullptr;
    std::vector<std::string> _scratch;
};

TEST(BlobNames, theStoredExtensionSitsBeforeTheNeutralSuffix)
{
    using App::FileBlobManager;
    EXPECT_EQ(FileBlobManager::storedExtension("C:/cache/blobs/a1b2.png.blob"), "png");
    EXPECT_EQ(FileBlobManager::storedExtension("/cache/blobs/a1b2.svg.blob"), "svg");
    // An old-style name, from a directory a running session made before
    EXPECT_EQ(FileBlobManager::storedExtension("/cache/blobs/a1b2.png"), "png");
    // A file of the user's, with its own extension
    EXPECT_EQ(FileBlobManager::storedExtension("/home/me/photo.JPG"), "JPG");
    // No type at all
    EXPECT_EQ(FileBlobManager::storedExtension("/cache/blobs/a1b2.blob"), "");
    EXPECT_EQ(FileBlobManager::storedExtension("/cache/blobs/a1b2"), "");
    EXPECT_EQ(FileBlobManager::neutralName("Box.Image.png"), "Box.Image.png.blob");
}

TEST_F(BlobNamesTest, anIncludedFileIsStoredUnderANeutralName)
{
    const std::string src = writeScratch("not really a png", ".png");
    _obj->File.setValue(src.c_str());
    const std::string stored = _obj->File.getValue();
    EXPECT_TRUE(endsWith(stored, ".png.blob")) << stored;
    // In the document's transient directory, under blobs/. TransientDir
    // mixes separators on Windows, the stored path does not: compare with
    // forward slashes
    auto slashes = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    const std::string transient = slashes(_doc->TransientDir.getValue());
    EXPECT_EQ(slashes(stored).compare(0, transient.size(), transient), 0)
        << stored << " not under " << transient;
    EXPECT_NE(stored.find("/blobs/"), std::string::npos) << stored;
    // The path's own last extension is the neutral one; the type is read
    // from before it
    EXPECT_EQ(Base::FileInfo(stored).extension(), "blob");
    EXPECT_EQ(App::FileBlobManager::storedExtension(stored), "png");
    EXPECT_TRUE(Base::FileInfo(stored).exists());
}

TEST_F(BlobNamesTest, aBlobKnowsItsTypeWhateverItsPathEndsIn)
{
    App::FileBlobManager& manager = _doc->getFileBlobManager();
    // Made by the store: the new-style name
    App::FileBlobHandle made = manager.adoptBytes("bytes", "png");
    ASSERT_TRUE(made);
    EXPECT_EQ(made->extension(), "png");
    EXPECT_TRUE(made->hasExtension("png"));
    EXPECT_TRUE(made->hasExtension("PNG"));
    EXPECT_FALSE(made->hasExtension("blob"));
    EXPECT_TRUE(endsWith(made->path(), ".png.blob")) << made->path();

    // An old-style file, as a directory from before the suffix holds it: a
    // writable scratch in the transient directory, named <x>.jpg, adopted
    // with no extension given -- the type comes off the name before any
    // suffix, and there is none
    const std::string old = _doc->TransientDir.getStrValue() + "/blobs/old-style.jpg";
    Base::FileInfo(_doc->TransientDir.getStrValue() + "/blobs").createDirectories();
    {
        std::ofstream out(old, std::ios::binary | std::ios::trunc);
        out << "jpeg bytes";
    }
    App::FileBlobHandle adopted = manager.adoptFile(old.c_str());
    ASSERT_TRUE(adopted);
    EXPECT_EQ(adopted->extension(), "jpg");
    EXPECT_TRUE(adopted->hasExtension("jpg"));
    // Adopted, it was renamed into the store's own naming
    EXPECT_TRUE(endsWith(adopted->path(), ".jpg.blob")) << adopted->path();
}

TEST_F(BlobNamesTest, aSourceInTheStoresNamingKeepsItsType)
{
    // Another property's blob: <uuid>.png.blob, handed on as a path
    const std::string src = writeScratch("first bytes", ".png");
    _obj->File.setValue(src.c_str());
    auto other = static_cast<TestNeutralNameFeature*>(
        _doc->addObject("App::TestNeutralNameFeature", "other"));
    other->File.setValue(_obj->File.getValue());
    EXPECT_EQ(other->File.blobExtension(), "png");
    ASSERT_TRUE(other->File.getBlob());
    EXPECT_EQ(other->File.getBlob()->extension(), "png");

    // A capture written outside the store as <name>.png.blob, the way
    // TechDraw's shaded underlay does
    const std::string capture = writeScratch("captured bytes", ".png.blob");
    auto third = static_cast<TestNeutralNameFeature*>(
        _doc->addObject("App::TestNeutralNameFeature", "third"));
    third->File.setValue(capture.c_str());
    EXPECT_EQ(third->File.blobExtension(), "png");
    ASSERT_TRUE(third->File.getBlob());
    EXPECT_EQ(third->File.getBlob()->extension(), "png");
    EXPECT_TRUE(endsWith(third->File.getValue(), ".png.blob")) << third->File.getValue();
}

}  // namespace
