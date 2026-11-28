// SPDX-License-Identifier: LGPL-2.1-or-later

// The temp guard of the C++ tests: FC_TEST_TEMP_GUARD set to "report" gives the
// process a temporary folder of its own -- fc-guard-<pid> in the one it was
// given, TMPDIR (TEMP and TMP on Windows) and FREECAD_USER_TEMP pointed there
// before any test runs -- and lists what is left in it when the tests are
// done; "fail" also fails the run naming it. An empty folder is removed. Unset,
// nothing is changed. A folder per process because ctest runs each test as a
// process of its own, several at a time: a shared folder would put one test's
// leftovers on another's account.

#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace
{

namespace fs = std::filesystem;

void setVariable(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

class TempGuard: public ::testing::Environment
{
public:
    void SetUp() override
    {
        const char* mode = std::getenv("FC_TEST_TEMP_GUARD");
        if (!mode || !*mode) {
            return;
        }
        m_mode = mode;
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        if (ec) {
            return;
        }
        m_dir = base / ("fc-guard-" + std::to_string(int(getpid())));
        fs::create_directories(m_dir, ec);
        if (ec) {
            m_dir.clear();
            return;
        }
        const std::string dir = m_dir.string();
#ifdef _WIN32
        setVariable("TEMP", dir);
        setVariable("TMP", dir);
#else
        setVariable("TMPDIR", dir);
#endif
        setVariable("FREECAD_USER_TEMP", dir);
    }

    void TearDown() override
    {
        if (m_dir.empty()) {
            return;
        }
        std::error_code ec;
        std::vector<std::string> left;
        for (const auto& entry : fs::directory_iterator(m_dir, ec)) {
            left.push_back(entry.path().filename().string());
        }
        if (left.empty()) {
            fs::remove(m_dir, ec);
            return;
        }
        // the tests that ran: under ctest, one (a process each)
        const auto* unit = ::testing::UnitTest::GetInstance();
        std::string ran;
        int count = 0;
        for (int i = 0; i < unit->total_test_suite_count(); ++i) {
            const auto* suite = unit->GetTestSuite(i);
            for (int j = 0; j < suite->total_test_count(); ++j) {
                const auto* info = suite->GetTestInfo(j);
                if (!info->should_run()) {
                    continue;
                }
                if (++count <= 3) {
                    ran += std::string(ran.empty() ? "" : ", ") + suite->name() + "."
                        + info->name();
                }
            }
        }
        if (count > 3) {
            ran += " and " + std::to_string(count - 3) + " more";
        }
        std::cerr << "TEMP GUARD (" << m_mode << "): " << left.size() << " left in " << m_dir.string()
                  << " by " << ran << "\n";
        for (const auto& name : left) {
            std::cerr << "TEMP GUARD left: " << name << "\n";
        }
        if (m_mode == "fail") {
            ADD_FAILURE() << "the tests left " << left.size() << " entries in " << m_dir.string();
        }
    }

private:
    std::string m_mode;
    fs::path m_dir;
};

[[maybe_unused]] const auto* const registered =
    ::testing::AddGlobalTestEnvironment(new TempGuard);

}  // namespace
