// SPDX-License-Identifier: LGPL-2.1-or-later

// What a killed or crashed process left in the user cache that holds nothing
// to recover is removed at the next start (docs/WorkerProcess.md sec 49): its
// reader folders, and its document folders that are empty. The sweep runs
// here on a folder of the test's own.

#include "gtest/gtest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <QCoreApplication>

#include "App/CacheSweep.h"
#include "Base/FileInfo.h"
#include <src/App/InitApplication.h>
#include <src/TempDirectory.h>

namespace
{

// No process has it: past the largest pid of Linux (2^22) and macOS (99999),
// and refused as such by OpenProcess on Windows
constexpr long long gonePid = 2147483000LL;

// One that runs and is not this process: init, or Windows' System
#ifdef _WIN32
constexpr long long runningPid = 4;
#else
constexpr long long runningPid = 1;
#endif

double now()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

class CacheSweepTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    std::string folder(const std::string& name, bool withFile = false)
    {
        const std::string path = dir.string() + "/" + name;
        Base::FileInfo(path).createDirectories();
        if (withFile) {
            Base::FileInfo(path + "/sub").createDirectories();
            std::ofstream(path + "/sub/file") << "content";
        }
        return path;
    }

    static std::string reader(long long pid)
    {
        return "Exe_Reader_c00945dd-e044-48f3-b04d-5b4faf146e38_" + std::to_string(pid);
    }

    static std::string document(long long pid)
    {
        return "Exe_Doc_c00945dd-e044-48f3-b04d-5b4faf146e38_a1b2c3_" + std::to_string(pid);
    }

    static bool there(const std::string& path)
    {
        return Base::FileInfo(path).exists();
    }

    App::CacheSweep::Counts sweep(double before = now() + 60.0)
    {
        return App::CacheSweep::run(dir.string(), "Exe", before);
    }

    tests::TempDirectory dir {"cachesweep"};
};

}  // namespace

TEST_F(CacheSweepTest, anEndedProcessesReaderAndEmptyDocumentGo)
{
    const auto readerFolder = folder(reader(gonePid), true);
    const auto emptyFolder = folder(document(gonePid));
    auto counts = sweep();
    EXPECT_FALSE(there(readerFolder));
    EXPECT_FALSE(there(emptyFolder));
    EXPECT_EQ(counts.readers, 1);
    EXPECT_EQ(counts.documents, 1);
    EXPECT_EQ(counts.failed, 0);
}

TEST_F(CacheSweepTest, aDocumentWithAnythingInItStays)
{
    // a transaction log, say, which the recovery reads
    const auto kept = folder(document(gonePid), true);
    auto counts = sweep();
    EXPECT_TRUE(there(kept + "/sub/file"));
    EXPECT_EQ(counts.documents, 0);
}

TEST_F(CacheSweepTest, aRunningProcessesFoldersStay)
{
    const long long own = QCoreApplication::applicationPid();
    const auto a = folder(reader(runningPid), true);
    const auto b = folder(document(runningPid));
    const auto c = folder(reader(own), true);
    const auto d = folder(document(own));
    auto counts = sweep();
    EXPECT_TRUE(there(a));
    EXPECT_TRUE(there(b));
    EXPECT_TRUE(there(c));
    EXPECT_TRUE(there(d));
    EXPECT_EQ(counts.readers + counts.documents, 0);
}

TEST_F(CacheSweepTest, aFolderChangedSinceTheStartStays)
{
    const auto a = folder(reader(gonePid), true);
    const auto b = folder(document(gonePid));
    auto counts = sweep(now() - 3600.0);
    EXPECT_TRUE(there(a));
    EXPECT_TRUE(there(b));
    EXPECT_EQ(counts.readers + counts.documents, 0);
}

TEST_F(CacheSweepTest, anotherProgramsAndOtherFoldersStay)
{
    const auto a = folder("Other_Reader_x_" + std::to_string(gonePid), true);
    const auto b = folder("Exe_Doc_no_pid_here");
    const auto c = folder("Exe_Something_" + std::to_string(gonePid));
    auto counts = sweep();
    EXPECT_TRUE(there(a));
    EXPECT_TRUE(there(b));
    EXPECT_TRUE(there(c));
    EXPECT_EQ(counts.readers + counts.documents, 0);
}

TEST_F(CacheSweepTest, aStoppedSweepRemovesNothing)
{
    const auto a = folder(reader(gonePid), true);
    auto counts = App::CacheSweep::run(dir.string(), "Exe", now() + 60.0, []() { return true; });
    EXPECT_TRUE(there(a));
    EXPECT_EQ(counts.readers, 0);
}

TEST_F(CacheSweepTest, aFolderOlderThanTheProcessOfItsPidGoes)
{
    // The pid has been reused: the process now running under it started
    // after the folder was last changed, so it did not make it
    const auto old = folder(reader(runningPid), true);
    namespace fs = std::filesystem;
    fs::last_write_time(old, fs::last_write_time(old) - std::chrono::hours(24 * 365 * 20));
    sweep();
#ifdef __APPLE__
    EXPECT_FALSE(there(old));
#else
    // Linux tells no start time, and Windows' System process none to a
    // user: the folder is kept, as for a process that may be its own
    EXPECT_TRUE(there(old));
#endif
}
