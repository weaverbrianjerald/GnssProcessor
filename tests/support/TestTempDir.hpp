#pragma once
/**
 * @file TestTempDir.hpp
 * @brief Scratch directories for unit tests that are unique per process and per test, so two
 *        runs of the same test executable at the same time never write the same path.
 *
 * A test that needs a file or directory outside the repository takes it from here, not from
 * `std::filesystem::temp_directory_path()`: that directory is shared by every process of the
 * user, and a fixed name inside it is a race as soon as a second suite run starts.
 *
 * Layout:
 * @code
 *   <temp>/gnss_tests_<pid>/                 the process root (removed after the last test)
 *   <temp>/gnss_tests_<pid>/<Suite.Test>/    the directory tempDir() returns inside a test
 *   <temp>/gnss_tests_<pid>/<Suite>_suite/   inside SetUpTestSuite / TearDownTestSuite
 * @endcode
 *
 * Parameterized names are flattened (any character outside [A-Za-z0-9_.-] becomes '_') and a
 * name past 80 characters is cut and suffixed with a hash, which keeps a nested path under the
 * Windows path limit.  A file name inside the directory needs no pid or test prefix of its own.
 *
 * The process root is removed by a global environment after the last test.  A process that
 * crashes leaves its root behind, named by its pid.
 */

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace gnss_test {
namespace detail {

/** @brief The process root path; empty until the first tempDir() call creates it. */
inline std::filesystem::path& processRootPath() {
    static std::filesystem::path root;
    return root;
}

/** @brief Remove the process root if it was created.  Errors (open files) are ignored. */
inline void removeProcessRoot() {
    const std::filesystem::path& root = processRootPath();
    if (root.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

/** @brief Global environment whose TearDown removes the process root after the last test. */
struct ProcessRootCleanup : ::testing::Environment {
    void TearDown() override { removeProcessRoot(); }
};

/** @brief Registers ProcessRootCleanup once per process, from whichever TU includes this header. */
inline const bool kProcessRootCleanupRegistered = [] {
    ::testing::AddGlobalTestEnvironment(new ProcessRootCleanup);
    return true;
}();

/** @brief Directory-name form of @p raw: filesystem-safe, at most 80 characters plus a hash. */
inline std::string flatten(std::string raw) {
    for (char& c : raw) {
        const bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-';
        if (!keep) c = '_';
    }
    constexpr std::size_t kMax = 80;
    if (raw.size() > kMax) {
        const std::size_t h = std::hash<std::string>{}(raw) & 0xFFFFFFFFu;
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08zx", h);
        raw = raw.substr(0, kMax) + "_" + hex;
    }
    return raw;
}

}  // namespace detail

/**
 * @brief The scratch directory of the running test (created, empty on first use).
 *
 * Inside a test body this is `<temp>/gnss_tests_<pid>/<Suite.Test>`; inside a suite-level
 * hook it is `<temp>/gnss_tests_<pid>/<Suite>_suite`; outside both it is the process root.
 * Repeated calls in one test return the same directory.
 *
 * @return An existing directory private to this process and this test.
 */
inline std::filesystem::path tempDir() {
    namespace fs = std::filesystem;
    fs::path& root = detail::processRootPath();
    std::error_code ec;
    if (root.empty()) {
#ifdef _WIN32
        const long pid = static_cast<long>(::_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        fs::path candidate = fs::temp_directory_path() / ("gnss_tests_" + std::to_string(pid));
        fs::remove_all(candidate, ec);   // left by an earlier process that had the same pid
        root = candidate;
    }

    std::string leaf;
    const ::testing::UnitTest* ut = ::testing::UnitTest::GetInstance();
    if (const ::testing::TestInfo* ti = ut->current_test_info()) {
        leaf = detail::flatten(std::string(ti->test_suite_name()) + "." + ti->name());
    } else if (const ::testing::TestSuite* ts = ut->current_test_suite()) {
        leaf = detail::flatten(std::string(ts->name())) + "_suite";
    }
    const fs::path dir = leaf.empty() ? root : root / leaf;
    fs::create_directories(dir, ec);
    return dir;
}

/**
 * @brief A path for @p name inside tempDir().
 * @param name File or directory name (not created).
 * @return `tempDir() / name`.
 */
inline std::filesystem::path tempPath(const std::string& name) { return tempDir() / name; }

}  // namespace gnss_test
