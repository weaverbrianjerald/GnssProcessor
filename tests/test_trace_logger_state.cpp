/**
 * @file test_trace_logger_state.cpp
 * @brief TraceLogger::logState in the baseline modes, where the filter has no
 *        absolute zenith wet delay state.
 *
 * Before the fix logState required an absolute tropo index, which a baseline
 * StateManager never allocates (getTropoIndex() is max()), so a baseline run
 * wrote a header-only state_trace.csv and a warning every epoch.  The rows are
 * now written: the base tropo_m / tropo_std_m pair is NaN, as std_at writes any
 * absent state, and the differential ZWD, when estimated, has its own dynamic
 * column pair dzwd_m / dzwd_std_m, appended like the clock and gradient columns.
 * A PPP state trace is unchanged (the guard case).
 */

#include <gtest/gtest.h>

#include "logging/TraceLogger.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

class TraceLoggerStateTraceTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    std::filesystem::path dir;

    void SetUp() override {
        dir = std::filesystem::temp_directory_path()
            / (std::string("trace_logger_state_")
               + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// Load @p json as the configuration.
    void load(const std::string& json) {
        const auto path = dir.string() + ".json";
        {
            std::ofstream out(path);
            out << json;
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
    }

    /// A GPS Baseline-Single configuration, with or without the differential ZWD.
    void loadBaseline(bool differential_tropo) {
        load(std::string(R"({
            "processing": {"mode": "Baseline-Single", "active_systems": ["GPS"]},
            "baseline_fixture": "Custom",
            "baseline": {
                "reference": {"name": "REF0", "fixed": true,
                              "position_ecef": [6378137.0, 0.0, 0.0]},
                "rover": {"name": "ROV0"},
                "estimate_differential_tropo": )")
             + (differential_tropo ? "true" : "false") + R"(,
                "differential_tropo_init_std_m": 0.1
            }
        })");
    }

    /// Write two epochs of @p states and return the file's lines.
    std::vector<std::string> writeTwoEpochs(const StateManager& states) {
        LoggingConfig lc;
        lc.enable_state_trace = true;
        {
            TraceLogger trace(log, dir, /*is_ppp_rtk=*/false, lc);
            EXPECT_TRUE(trace.isInitialized());
            trace.logState(GnssTime{2418, 1000.0}, states);
            trace.logState(GnssTime{2418, 1030.0}, states);
            trace.flush();
        }
        std::ifstream in(dir / "state_trace.csv");
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    }

    static std::vector<std::string> fields(const std::string& line) {
        std::vector<std::string> out;
        std::stringstream ss(line);
        for (std::string f; std::getline(ss, f, ',');) out.push_back(f);
        return out;
    }

    /// The field of @p row under the header column @p name ("" when absent).
    static std::string column(const std::vector<std::string>& header,
                              const std::vector<std::string>& row, const std::string& name) {
        for (size_t i = 0; i < header.size() && i < row.size(); ++i)
            if (header[i] == name) return row[i];
        return {};
    }

    static bool hasColumn(const std::vector<std::string>& header, const std::string& name) {
        for (const auto& h : header) if (h == name) return true;
        return false;
    }
};

TEST_F(TraceLoggerStateTraceTest, BaselineWithDifferentialTropoWritesRowsWithNanTropoAndTheDzwd) {
    loadBaseline(true);
    StateManager states(5, log, config);
    ASSERT_TRUE(states.hasDifferentialTropo());
    ASSERT_GE(states.getTropoIndex(), static_cast<size_t>(states.getStateSize()));
    Eigen::VectorXd x = states.getStateVector();
    x.head<3>() = Eigen::Vector3d(-2493304.7, -4655215.1, 3565497.6);
    x(static_cast<Eigen::Index>(states.getDifferentialTropoIndex())) = 0.0123;
    states.setStateVector(x);

    const auto lines = writeTwoEpochs(states);
    ASSERT_EQ(lines.size(), 3u);  // header and two rows
    const auto header = fields(lines[0]);
    ASSERT_TRUE(hasColumn(header, "dzwd_m"));
    ASSERT_TRUE(hasColumn(header, "dzwd_std_m"));
    for (size_t r = 1; r < lines.size(); ++r) {
        const auto row = fields(lines[r]);
        ASSERT_EQ(row.size(), header.size()) << lines[r];
        EXPECT_EQ(column(header, row, "x_m"), "-2493304.700000");
        EXPECT_EQ(column(header, row, "tropo_m"), "nan");
        EXPECT_EQ(column(header, row, "tropo_std_m"), "nan");
        EXPECT_EQ(column(header, row, "dzwd_m"), "0.012300");
        EXPECT_EQ(column(header, row, "dzwd_std_m"), "0.100000");
    }
    EXPECT_EQ(fields(lines[2])[1], "1030.000");
}

TEST_F(TraceLoggerStateTraceTest, BaselineWithoutDifferentialTropoWritesRowsWithNanTropo) {
    loadBaseline(false);
    StateManager states(5, log, config);
    ASSERT_FALSE(states.hasDifferentialTropo());

    const auto lines = writeTwoEpochs(states);
    ASSERT_EQ(lines.size(), 3u);
    const auto header = fields(lines[0]);
    EXPECT_FALSE(hasColumn(header, "dzwd_m"));
    EXPECT_EQ(header.back(), "tropo_std_m");
    for (size_t r = 1; r < lines.size(); ++r) {
        const auto row = fields(lines[r]);
        ASSERT_EQ(row.size(), header.size()) << lines[r];
        EXPECT_EQ(column(header, row, "tropo_m"), "nan");
        EXPECT_EQ(column(header, row, "tropo_std_m"), "nan");
    }
}

TEST_F(TraceLoggerStateTraceTest, PppWritesTheTropoStateAndNoDzwdColumns) {
    load(R"({"processing": {"mode": "ppp", "active_systems": ["GPS"]}})");
    StateManager states(5, log, config);
    ASSERT_LT(states.getTropoIndex(), static_cast<size_t>(states.getStateSize()));
    Eigen::VectorXd x = states.getStateVector();
    x(static_cast<Eigen::Index>(states.getTropoIndex())) = 0.25;
    states.setStateVector(x);

    const auto lines = writeTwoEpochs(states);
    ASSERT_EQ(lines.size(), 3u);
    const auto header = fields(lines[0]);
    EXPECT_FALSE(hasColumn(header, "dzwd_m"));
    const auto row = fields(lines[1]);
    ASSERT_EQ(row.size(), header.size());
    EXPECT_EQ(column(header, row, "tropo_m"), "0.250000");
    EXPECT_NE(column(header, row, "tropo_std_m"), "nan");
}

} // namespace
