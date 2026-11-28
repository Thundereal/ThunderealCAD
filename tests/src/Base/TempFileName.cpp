// SPDX-License-Identifier: LGPL-2.1-or-later

// Base::FileInfo::getTempFileName() gives a name it could make, or fails with a
// reason: never a name it did not get. A folder it cannot make a file in is
// the failure every platform can show; GetTempFileNameW's other one, every
// name of the prefix taken (65535 per prefix and folder), is shown on Windows
// with a folder filled beforehand.

#include "gtest/gtest.h"

#include "Base/Exception.h"
#include "Base/FileInfo.h"
#include <src/TempDirectory.h>

#include <filesystem>
#include <string>

TEST(TempFileName, aFreshNameInTheFolder)
{
    tests::TempDirectory dir {"tempfilename"};
    const std::string first = Base::FileInfo::getTempFileName(nullptr, dir.string().c_str());
    const std::string second = Base::FileInfo::getTempFileName(nullptr, dir.string().c_str());
    const auto folder = std::filesystem::weakly_canonical(dir.path());
    EXPECT_EQ(std::filesystem::weakly_canonical(std::filesystem::path(first).parent_path()),
              folder);
    // a name, not a file: what made it to hold the name has gone again
    EXPECT_FALSE(std::filesystem::exists(first));
    EXPECT_NE(first, second);
}

TEST(TempFileName, aFolderItCannotWriteToIsAFailure)
{
    tests::TempDirectory dir {"tempfilename"};
    const std::string missing = (dir.path() / "not-there" / "nor-this").string();
    // before: on Windows an uninitialised buffer, elsewhere the template
    // ".../fileXXXXXX" itself, given as the name
    EXPECT_THROW(Base::FileInfo::getTempFileName(nullptr, missing.c_str()), Base::FileException);
    EXPECT_FALSE(std::filesystem::exists(missing));
}
