// SPDX-License-Identifier: LGPL-2.1-or-later

// A rename or a delete refused for a moment -- something outside has the file
// open: a file monitor, an antivirus, a slow share -- is waited out, for as
// long as such holds have lasted (docs/WorkerProcess.md sec 47). The holds are
// made by the test hook, an operation's attempts refused as the case needs.

#include "gtest/gtest.h"

#include "Base/FileInfo.h"
#include <src/TempDirectory.h>

#include <chrono>
#include <fstream>
#include <string>

namespace
{

double since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

class FileRetryTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        limit = Base::FileInfo::retryLimit();
        Base::FileInfo::setRetryLimit(10.0);
        Base::FileInfo::setRetryFault({});
    }

    void TearDown() override
    {
        Base::FileInfo::setRetryFault({});
        Base::FileInfo::setRetryLimit(limit);
    }

    std::string file(const char* name)
    {
        std::string path = dir.string() + "/" + name;
        std::ofstream(path) << "content";
        return path;
    }

    std::string target(const char* name)
    {
        return dir.string() + "/" + name;
    }

    double limit = 0;
    tests::TempDirectory dir {"fileretry"};
};

}  // namespace

TEST_F(FileRetryTest, aMomentaryHoldIsWaitedOut)
{
    int left = 3;
    Base::FileInfo::setRetryFault([&left](const char* op) {
        return std::string(op) == "rename" && left-- > 0 ? 1 : 0;
    });
    Base::FileInfo info(file("a"));
    EXPECT_TRUE(info.renameFile(target("b").c_str()));
    EXPECT_TRUE(Base::FileInfo(target("b")).exists());
    EXPECT_FALSE(Base::FileInfo(target("a")).exists());
    auto stats = Base::FileInfo::retryStats();
    EXPECT_EQ(stats.retried, 1);
    EXPECT_EQ(stats.recovered, 1);
    EXPECT_EQ(stats.failed, 0);
    EXPECT_GT(stats.longest, 0.0);
}

TEST_F(FileRetryTest, anotherRefusalFailsAtOnce)
{
    Base::FileInfo::setRetryFault([](const char*) { return 2; });
    const auto start = std::chrono::steady_clock::now();
    Base::FileInfo info(file("a"));
    EXPECT_FALSE(info.renameFile(target("b").c_str()));
    EXPECT_LT(since(start), 0.05);
    EXPECT_TRUE(Base::FileInfo(target("a")).exists());
    EXPECT_EQ(Base::FileInfo::retryStats().retried, 0);
}

TEST_F(FileRetryTest, aLastingHoldFailsAtTheLimit)
{
    Base::FileInfo::setRetryLimit(0.2);
    Base::FileInfo::setRetryFault([](const char*) { return 1; });
    const auto start = std::chrono::steady_clock::now();
    Base::FileInfo info(file("a"));
    EXPECT_FALSE(info.renameFile(target("b").c_str()));
    const double took = since(start);
    EXPECT_GE(took, 0.19);
    EXPECT_LT(took, 1.0);
    auto stats = Base::FileInfo::retryStats();
    EXPECT_EQ(stats.failed, 1);
    EXPECT_EQ(stats.recovered, 0);
}

TEST_F(FileRetryTest, aZeroLimitTriesOnce)
{
    Base::FileInfo::setRetryLimit(0.0);
    Base::FileInfo::setRetryFault([](const char*) { return 1; });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(Base::FileInfo(file("a")).deleteFile());
    EXPECT_LT(since(start), 0.05);
}

TEST_F(FileRetryTest, theWaitGrowsWithTheLongestHold)
{
    // a second until a hold is seen
    EXPECT_DOUBLE_EQ(Base::FileInfo::retryStats().budget, 1.0);
    // one of 0.3 s: four times that
    const auto start = std::chrono::steady_clock::now();
    Base::FileInfo::setRetryFault([start](const char*) { return since(start) < 0.3 ? 1 : 0; });
    Base::FileInfo info(file("a"));
    EXPECT_TRUE(info.renameFile(target("b").c_str()));
    auto stats = Base::FileInfo::retryStats();
    EXPECT_GE(stats.longest, 0.29);
    EXPECT_NEAR(stats.budget, 4.0 * stats.longest, 1e-9);
    // never past the limit
    Base::FileInfo::setRetryLimit(0.5);
    EXPECT_DOUBLE_EQ(Base::FileInfo::retryStats().budget, 0.5);
}

TEST_F(FileRetryTest, aDeleteIsWaitedOutToo)
{
    int left = 2;
    Base::FileInfo::setRetryFault([&left](const char* op) {
        return std::string(op) == "delete" && left-- > 0 ? 1 : 0;
    });
    const std::string path = file("a");
    EXPECT_TRUE(Base::FileInfo(path).deleteFile());
    EXPECT_FALSE(Base::FileInfo(path).exists());
    EXPECT_EQ(Base::FileInfo::retryStats().recovered, 1);
}
