// SPDX-License-Identifier: LGPL-2.1-or-later

// Two processes on one file (docs/TransactionLogJournal.md sec 29.7, 29.8): the
// stamp a save compares, the lock it holds, the marker of who has it open.

#include "gtest/gtest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#ifndef _WIN32
# include <sys/wait.h>
# include <unistd.h>
#endif
#ifdef __linux__
# include <sys/resource.h>
#endif

#include <QString>

#include "App/FileElsewhere.h"
#include "Base/FileInfo.h"
#include <src/App/InitApplication.h>

namespace fs = std::filesystem;
using App::DiskStamp;
namespace FileOpeners = App::FileOpeners;
using App::SaveLock;

namespace
{

class FileElsewhereTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _root = fs::path(Base::FileInfo::getTempPath())
            / ("elsewhere-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed())
               + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(_root);
        fs::create_directories(_root);
        _file = (_root / "part.TStd").string();
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(_root, ec);
    }

    static void write(const std::string& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    fs::path _root;
    std::string _file;
};

}   // namespace

TEST_F(FileElsewhereTest, theStampTellsAWriteFromNone)
{
    EXPECT_FALSE(DiskStamp::of(_file).valid());
    write(_file, "one");
    const DiskStamp first = DiskStamp::of(_file);
    ASSERT_TRUE(first.valid());
    EXPECT_TRUE(DiskStamp::of(_file).sameAs(first));

    // A save writes a new file and renames it over the old one.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(_file + ".tmp", "two");
    fs::rename(_file + ".tmp", _file);
    const DiskStamp second = DiskStamp::of(_file);
    EXPECT_FALSE(second.sameAs(first));

    // And a write in place, of the same size, is seen by its time.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(_file, "TWO");
    EXPECT_FALSE(DiskStamp::of(_file).sameAs(second));
}

TEST_F(FileElsewhereTest, aSaveLockIsHeldAgainstAnother)
{
    write(_file, "x");
    SaveLock mine(_file);
    ASSERT_TRUE(mine.lock(0));
    EXPECT_TRUE(mine.isLocked());
    EXPECT_TRUE(fs::exists(SaveLock::lockPath(_file)));
    EXPECT_EQ(fs::path(SaveLock::lockPath(_file)).filename().string(), ".~part.TStd.lock");

    SaveLock theirs(_file);
    std::string holder;
    EXPECT_FALSE(theirs.lock(50, &holder));
    EXPECT_NE(holder.find("pid"), std::string::npos) << holder;

    mine.unlock();
    EXPECT_TRUE(theirs.lock(0));
    EXPECT_FALSE(mine.lock(0));
}

#ifndef _WIN32
TEST_F(FileElsewhereTest, aSaveLockIsHeldAgainstAnotherProcess)
{
    write(_file, "x");
    // The lock is fcntl's, which only another process sees.
    int taken[2];
    int done[2];
    ASSERT_EQ(pipe(taken), 0);
    ASSERT_EQ(pipe(done), 0);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        SaveLock theirs(_file);
        const char ok = theirs.lock(0) ? 'y' : 'n';
        (void)!::write(taken[1], &ok, 1);
        char c = 0;
        (void)!::read(done[0], &c, 1);
        _exit(0);
    }
    char ok = 0;
    ASSERT_EQ(::read(taken[0], &ok, 1), 1);
    ASSERT_EQ(ok, 'y');

    SaveLock mine(_file);
    std::string holder;
    EXPECT_FALSE(mine.lock(100, &holder));
    EXPECT_EQ(holder.rfind("another process", 0), 0u) << holder;

    // Let go there, taken here while it waits.
    const char c = 'x';
    (void)!::write(done[1], &c, 1);
    EXPECT_TRUE(mine.lock(5000));
    int status = 0;
    waitpid(child, &status, 0);
    for (int fd : {taken[0], taken[1], done[0], done[1]})
        ::close(fd);
}

TEST_F(FileElsewhereTest, aLockLeftByAGoneProcessIsTakenOver)
{
    write(_file, "x");
    // A process that took the lock and died holding it: the system let it
    // go, and the file it leaves is no lock.
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        SaveLock left(_file);
        _exit(left.lock(0) ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    ASSERT_EQ(WEXITSTATUS(status), 0);
    ASSERT_TRUE(fs::exists(SaveLock::lockPath(_file)));

    SaveLock mine(_file);
    EXPECT_TRUE(mine.lock(0));
}
#endif

TEST_F(FileElsewhereTest, theOpenMarkerSaysWhoElseHasTheFile)
{
    write(_file, "x");
    EXPECT_TRUE(FileOpeners::add(_file).empty());
    EXPECT_TRUE(FileOpeners::others(_file).empty());
    const std::string marker = FileOpeners::markerPath(_file);
    EXPECT_EQ(fs::path(marker).filename().string(), ".~part.TStd.open");
    ASSERT_TRUE(fs::exists(marker));

    // Another machine's process, seen now; one of another machine not seen
    // for 40 days (FileOpenerExpiryDays, 30: taken as gone, sec 39.3); and
    // a process of this one that is gone.
    const auto now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    {
        std::ofstream out(marker, std::ios::app);
        out.precision(17);
        out << "elsewhere-host 4242 " << now << '\n';
        out << "stale-host 4343 " << now - 40 * 86400.0 << '\n';
    }
    std::string here;
    {
        std::ifstream in(marker);
        in >> here;
    }
    {
        std::ofstream out(marker, std::ios::app);
#ifndef _WIN32
        const pid_t child = fork();
        if (child == 0)
            _exit(0);
        int status = 0;
        waitpid(child, &status, 0);
        out << here << ' ' << child << " 1700000000\n";
#endif
    }
    const auto others = FileOpeners::others(_file);
    ASSERT_EQ(others.size(), 1u);
    EXPECT_EQ(others[0].host, "elsewhere-host");
    EXPECT_EQ(others[0].pid, 4242);
    EXPECT_NE(FileOpeners::describe(others[0]).find("elsewhere-host (pid 4242) since"),
              std::string::npos);

    // Closed here: the other's line stays; with the last, the marker goes.
    FileOpeners::remove(_file);
    ASSERT_TRUE(fs::exists(marker));
    EXPECT_EQ(FileOpeners::others(_file).size(), 1u);
    write(marker, "");
    FileOpeners::add(_file);
    {
        SaveLock lock(_file);
        ASSERT_TRUE(lock.lock(0));
    }
    ASSERT_TRUE(fs::exists(SaveLock::lockPath(_file)));
    FileOpeners::remove(_file);
    EXPECT_FALSE(fs::exists(marker));
    EXPECT_FALSE(fs::exists(SaveLock::lockPath(_file)));
}

namespace
{

std::string readAll(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}   // namespace

// A folder save goes in whole (docs/TransactionLogJournal.md sec 38).
TEST_F(FileElsewhereTest, aFolderSaveMovesInOnlyWhatChanged)
{
    const fs::path folder = _root / "Doc";
    fs::create_directories(folder);
    write((folder / "Document.xml").string(), "doc one");
    write((folder / "Same.xml").string(), "same");
    write((folder / "Gone.xml").string(), "gone");
    const DiskStamp same = DiskStamp::of((folder / "Same.xml").string());

    const fs::path stage = App::FolderSave::freshStage(folder.string());
    EXPECT_EQ(stage, fs::path(App::FolderSave::stagePath(folder.string())));
    EXPECT_TRUE(App::FolderSave::pending(folder.string()));
    fs::create_directories(stage / "blobs");
    write((stage / "Document.xml").string(), "doc two");
    write((stage / "Same.xml").string(), "same");
    write((stage / "blobs" / "New.brp").string(), "new");

    App::FolderSave::commit(folder.string(), {"Gone.xml"});
    EXPECT_FALSE(App::FolderSave::pending(folder.string()));
    EXPECT_FALSE(fs::exists(stage));
    EXPECT_EQ(readAll(folder / "Document.xml"), "doc two");
    EXPECT_EQ(readAll(folder / "blobs" / "New.brp"), "new");
    EXPECT_FALSE(fs::exists(folder / "Gone.xml"));
    // Not moved, not touched: the same file as before.
    EXPECT_TRUE(DiskStamp::of((folder / "Same.xml").string()).sameAs(same));
}

#ifdef __linux__
// The files of a save are flushed several at a time, on threads. A process
// that can have no more threads -- at its limit of them, or of address
// space for their stacks -- saves all the same, on the thread it is on.
// The thread that could not be made was an exception: with none made yet,
// as here, the save failed of it; with some made it went past a vector of
// threads still running, and the process was gone ("terminate called
// without an active exception"), in the middle of a save.
TEST_F(FileElsewhereTest, aFolderSaveGoesInWhenNoThreadCanBeMade)
{
    if (geteuid() == 0) {
        GTEST_SKIP() << "root is not held to the limit of processes";
    }
    const fs::path folder = _root / "Doc";
    fs::create_directories(folder);
    const fs::path stage = App::FolderSave::freshStage(folder.string());
    const int count = 64;   // enough for a pool of threads
    for (int i = 0; i < count; ++i) {
        write((stage / ("File" + std::to_string(i) + ".brp")).string(), std::to_string(i));
    }

    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // No more threads for this user than there are: the next one fails.
        rlimit limit {};
        getrlimit(RLIMIT_NPROC, &limit);
        limit.rlim_cur = 1;
        if (setrlimit(RLIMIT_NPROC, &limit) != 0) {
            _exit(3);
        }
        try {
            App::FolderSave::commit(folder.string(), {});
        }
        catch (...) {
            _exit(2);
        }
        for (int i = 0; i < count; ++i) {
            if (readAll(folder / ("File" + std::to_string(i) + ".brp")) != std::to_string(i)) {
                _exit(1);
            }
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)) << "the save ended the process, signal "
                                   << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

TEST_F(FileElsewhereTest, aStageWithoutItsRecordIsThrownAway)
{
    const fs::path folder = _root / "Doc";
    fs::create_directories(folder);
    write((folder / "Document.xml").string(), "doc one");
    const fs::path stage = App::FolderSave::freshStage(folder.string());
    write((stage / "Document.xml").string(), "doc tw");   // cut short

    EXPECT_EQ(App::FolderSave::recover(folder.string()), -1);
    EXPECT_FALSE(fs::exists(stage));
    EXPECT_EQ(readAll(folder / "Document.xml"), "doc one");
    EXPECT_EQ(App::FolderSave::recover(folder.string()), 0);
}

TEST_F(FileElsewhereTest, aRecordedStageIsMovedInEvenHalfMovedAlready)
{
    const fs::path folder = _root / "Doc";
    fs::create_directories(folder);
    write((folder / "Document.xml").string(), "doc one");
    write((folder / "A.xml").string(), "a one");
    write((folder / "Gone.xml").string(), "gone");
    const fs::path stage = App::FolderSave::freshStage(folder.string());
    write((stage / "Document.xml").string(), "doc two");
    write((stage / "B.xml").string(), "b two");
    write((stage / ".~commit").string(), "FreeCAD folder save 1\nremove Gone.xml\n");
    // A.xml was moved in before the save was cut short.
    write((folder / "A.xml").string(), "a two");

    EXPECT_EQ(App::FolderSave::recover(folder.string()), 1);
    EXPECT_FALSE(fs::exists(stage));
    EXPECT_EQ(readAll(folder / "Document.xml"), "doc two");
    EXPECT_EQ(readAll(folder / "A.xml"), "a two");
    EXPECT_EQ(readAll(folder / "B.xml"), "b two");
    EXPECT_FALSE(fs::exists(folder / "Gone.xml"));
}
