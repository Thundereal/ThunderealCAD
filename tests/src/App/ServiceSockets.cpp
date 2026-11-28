// SPDX-License-Identifier: LGPL-2.1-or-later

// The sockets service helpers were met at, left by processes that are gone,
// are swept by App::ServiceWorkers::removeStaleSockets(): only sockets named as
// the service names them, only of pids that are not alive and not this
// process's. On Windows the server is a named pipe, which leaves nothing on
// disk; there is nothing to plant, and this file tests nothing there.

#include "gtest/gtest.h"

#ifndef _WIN32

#include <App/ServiceWorkers.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

namespace fs = std::filesystem;

/// A Unix socket file at `path`, bound and closed: what a server that never
/// closed leaves behind
bool plantSocket(const fs::path& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const std::string p = path.string();
    if (p.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return bound;
}

/// A pid no process has: a child's, after it has ended and been reaped
long deadPid()
{
    pid_t child = ::fork();
    if (child == 0) {
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return static_cast<long>(child);
}

std::string socketName(long pid)
{
    return "thundereal-service-" + std::to_string(pid) + "-0-0123abcd";
}

class ServiceSockets: public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Short: a socket's path is held to about 100 bytes (sun_path), and
        // a per-user temporary folder on macOS is half of that already
        char tmpl[] = "/tmp/fcss-XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir = tmpl;
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path dir;
};

}  // namespace

TEST_F(ServiceSockets, aGoneProcessSocketIsRemovedALiveOneIsKept)
{
    const fs::path dead = dir / socketName(deadPid());
    const fs::path parent = dir / socketName(static_cast<long>(::getppid()));
    const fs::path own = dir / socketName(static_cast<long>(::getpid()));
    ASSERT_TRUE(plantSocket(dead));
    ASSERT_TRUE(plantSocket(parent));
    ASSERT_TRUE(plantSocket(own));

    EXPECT_EQ(App::ServiceWorkers::removeStaleSockets(dir.string()), 1);
    EXPECT_FALSE(fs::exists(dead));
    EXPECT_TRUE(fs::exists(parent));
    EXPECT_TRUE(fs::exists(own));
}

TEST_F(ServiceSockets, onlyOurSocketsAreTouched)
{
    const long gone = deadPid();
    // the name, but a plain file: not a socket the service left
    const fs::path file = dir / socketName(gone);
    std::ofstream(file) << "kept";
    // a socket of a dead pid, but not the service's name
    const fs::path other = dir / ("other-service-" + std::to_string(gone) + "-0-0123abcd");
    ASSERT_TRUE(plantSocket(other));
    const fs::path nearly = dir / ("thundereal-service-" + std::to_string(gone) + "-0-NOTHEX12");
    ASSERT_TRUE(plantSocket(nearly));

    EXPECT_EQ(App::ServiceWorkers::removeStaleSockets(dir.string()), 0);
    EXPECT_TRUE(fs::exists(file));
    EXPECT_TRUE(fs::exists(other));
    EXPECT_TRUE(fs::exists(nearly));
}

TEST_F(ServiceSockets, noMoreThanTheLimitIsLookedAt)
{
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(plantSocket(dir / socketName(deadPid())));
    }
    EXPECT_EQ(App::ServiceWorkers::removeStaleSockets(dir.string(), 2), 2);
    EXPECT_EQ(App::ServiceWorkers::removeStaleSockets(dir.string()), 3);
}

#endif
