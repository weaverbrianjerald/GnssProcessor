/**
 * @file test_row_trace.cpp
 * @brief The decomposition trace of a code or phase row (measurement/RowTrace.hpp), written
 *        for every row by the single-receiver model and by the network producer's row loop.
 *
 * Both builders sum the terms of one observation equation, and the trace prints the value of
 * each term next to the prediction. It is the first read when a residual of metres has to be
 * attributed to a clock, an ionosphere, a bias or an ambiguity, so a builder that stops
 * writing it leaves the producer's rows unreadable without any number moving. Covered here:
 *  - the network row loop prints one PHASE-DECOMP or CODE-DECOMP line per row, ahead of the
 *    row's own trace line, with the values of the terms the station's parameter set holds;
 *  - the single-receiver model prints one per row too;
 *  - the trace level changes no row: the innovations, variances and Jacobian are the same bit
 *    for bit at trace and at info, and nothing is written at info.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/LocalBirth.hpp"
#include "network/assembly/SharedAdmission.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/state/StationParams.hpp"
#include "support/ModelStack.hpp"
#include "support/StationBirthFixture.hpp"
#include "support/TestTempDir.hpp"

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;
using gnss_test::BirthFixture;
using gnss_test::syntheticObservation;

namespace {

/**
 * @brief Brings a log manager up at a level and captures the row logger's output in a string.
 *
 * The same pattern as the other log-capturing suites: an initialized manager in a directory of
 * the test's own, the loggers it registers dropped before and after (never @c drop_all, which
 * would leave the rest of the binary logging into an empty registry).
 */
class RowLog {
public:
    /// Initializes @p log at @p level and attaches the capture to its @c measurement_model logger.
    RowLog(LogManager& log, LogLevel level) {
        spdlog::apply_all([this](std::shared_ptr<spdlog::logger> l) { before_.insert(l->name()); });
        spdlog::drop("gnss_processor");
        spdlog::drop("measurement_model");
        log.initialize(gnss_test::tempDir() / "row_trace_logs", level);
        logger_ = log.getLogger("measurement_model");
        sink_ = std::make_shared<spdlog::sinks::ostream_sink_mt>(out_);
        logger_->sinks().push_back(sink_);
    }

    ~RowLog() {
        auto& sinks = logger_->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
        std::vector<std::string> mine;
        spdlog::apply_all([&](std::shared_ptr<spdlog::logger> l) {
            if (before_.count(l->name()) == 0) mine.push_back(l->name());
        });
        for (const std::string& name : mine) spdlog::drop(name);
        spdlog::drop("gnss_processor");
        spdlog::drop("measurement_model");
    }

    RowLog(const RowLog&) = delete;
    RowLog& operator=(const RowLog&) = delete;

    /// Everything the row logger wrote so far.
    std::string text() {
        logger_->flush();
        return out_.str();
    }

private:
    std::set<std::string>                           before_;   ///< Loggers registered before this one.
    std::ostringstream                              out_;
    std::shared_ptr<spdlog::logger>                 logger_;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
};

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

/// The index of the first line that contains @p what, or -1.
int lineWith(const std::vector<std::string>& lines, const std::string& what) {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(what) != std::string::npos) return static_cast<int>(i);
    return -1;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

/// The number of decomposition lines in a text.
int decompositionLines(const std::vector<std::string>& lines) {
    int n = 0;
    for (const std::string& l : lines)
        if (has(l, "PHASE-DECOMP ") || has(l, "CODE-DECOMP ")) ++n;
    return n;
}

/// Every row of @p em has its decomposition line in @p lines, ahead of the row's own trace
/// line, and the line's innovation is the row's.
void expectLineForEveryRow(const std::vector<std::string>& lines, const EpochMeasurement& em) {
    ASSERT_GT(em.innovations.size(), 0);
    for (Eigen::Index r = 0; r < em.innovations.size(); ++r) {
        const std::string& sat = em.satellite_ids[static_cast<std::size_t>(r)];
        const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
        const std::string kind = sig[0] == 'L' ? "PHASE-DECOMP " : "CODE-DECOMP ";
        const int d = lineWith(lines, kind + sat + " " + sig + " ");
        const int m = lineWith(lines, "Meas " + sat + "/" + sig + ":");
        ASSERT_GE(d, 0) << "no decomposition line for " << sat << " " << sig;
        ASSERT_GE(m, 0) << "no row line for " << sat << " " << sig;
        EXPECT_LT(d, m) << sat << " " << sig;
        EXPECT_TRUE(has(lines[static_cast<std::size_t>(d)],
                        fmt::format("zd_innov={:+.3f}m", em.innovations(r))))
            << lines[static_cast<std::size_t>(d)];
    }
}

/// One network station's epoch: births, admission, then the rows, with the receiver clock and
/// the ionosphere of G05 set to known linearization points.
struct StationEpochRows {
    EpochMeasurement rows;
    StationParams    set;
    std::string      text;
};

StationEpochRows networkRows(LogLevel level) {
    BirthFixture f;
    RowLog log(f.log, level);
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    networkBirthLocalStates(c, f.set, obs);
    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);
    f.set.slots[static_cast<std::size_t>(f.set.clock)].x_lin = 12.5;
    const int iono = findParam(f.set, slantIonoRef(*SatId::parse("G05")));
    EXPECT_GE(iono, 0);
    if (iono >= 0) f.set.slots[static_cast<std::size_t>(iono)].x_lin = 1.25;
    StationEpochRows out;
    out.rows = networkStationRows(c, f.set, obs);
    out.set  = f.set;
    out.text = log.text();
    return out;
}

}  // namespace

TEST(RowTraceTest, TheNetworkRowLoopPrintsTheDecompositionOfEveryRowAheadOfTheRow) {
    const StationEpochRows e = networkRows(LogLevel::Trace);
    const std::vector<std::string> lines = linesOf(e.text);
    ASSERT_EQ(e.rows.innovations.size(), 12);   // four signals of each of two GPS satellites, four of one Galileo
    EXPECT_EQ(decompositionLines(lines), 12);
    expectLineForEveryRow(lines, e.rows);

    // Six phase rows and six code rows, each line carrying the terms of the set it was built from.
    int phase = 0, code = 0;
    for (const std::string& l : lines) {
        if (has(l, "PHASE-DECOMP ")) ++phase;
        if (has(l, "CODE-DECOMP ")) ++code;
        if (has(l, "-DECOMP ")) EXPECT_TRUE(has(l, "rx_clk=+12.500")) << l;
    }
    EXPECT_EQ(phase, 6);
    EXPECT_EQ(code, 6);

    // The slant ionosphere enters with +1 on code and -1 on phase at the reference band.
    const int c1c = lineWith(lines, "CODE-DECOMP G05 C1C ");
    const int l1c = lineWith(lines, "PHASE-DECOMP G05 L1C ");
    ASSERT_GE(c1c, 0);
    ASSERT_GE(l1c, 0);
    EXPECT_TRUE(has(lines[static_cast<std::size_t>(c1c)], "iono=+1.250")) << lines[static_cast<std::size_t>(c1c)];
    EXPECT_TRUE(has(lines[static_cast<std::size_t>(l1c)], "iono=-1.250")) << lines[static_cast<std::size_t>(l1c)];

    // The float ambiguity term of a phase row is the ambiguity parameter's linearization point.
    const int amb = findParam(e.set, ambiguityRef(*SatId::parse("G05"),
                                                  *SignalId::fromObsCode(System::GPS, "L1C")));
    ASSERT_GE(amb, 0);
    EXPECT_TRUE(has(lines[static_cast<std::size_t>(l1c)],
                    fmt::format("amb={:+.3f}", slotAt(e.set, amb).x_lin)))
        << lines[static_cast<std::size_t>(l1c)];
}

TEST(RowTraceTest, TheTraceLevelChangesNoRowAndInfoLevelWritesNothing) {
    const StationEpochRows traced = networkRows(LogLevel::Trace);
    const StationEpochRows quiet  = networkRows(LogLevel::Info);

    EXPECT_FALSE(has(quiet.text, "-DECOMP "));
    EXPECT_FALSE(has(quiet.text, "Meas "));

    ASSERT_EQ(traced.rows.innovations.size(), quiet.rows.innovations.size());
    ASSERT_EQ(traced.rows.H.rows(), quiet.rows.H.rows());
    ASSERT_EQ(traced.rows.H.cols(), quiet.rows.H.cols());
    for (Eigen::Index r = 0; r < traced.rows.H.rows(); ++r) {
        EXPECT_EQ(traced.rows.innovations(r), quiet.rows.innovations(r)) << r;
        EXPECT_EQ(traced.rows.R_v(r), quiet.rows.R_v(r)) << r;
        for (Eigen::Index k = 0; k < traced.rows.H.cols(); ++k)
            EXPECT_EQ(traced.rows.H(r, k), quiet.rows.H(r, k)) << r << "," << k;
    }
    EXPECT_EQ(traced.rows.satellite_ids, quiet.rows.satellite_ids);
    EXPECT_EQ(traced.rows.signal_types, quiet.rows.signal_types);
}

TEST(RowTraceTest, TheSingleReceiverModelPrintsTheDecompositionOfEveryRow) {
    LogManager log;
    gnss_test::ModelStack stack;
    ASSERT_TRUE(stack.load(log, "row_trace", "ppp-rtk", "", "", false));
    RowLog trace(log, LogLevel::Trace);

    const std::vector<PppObservation> obs = {
        syntheticObservation("G12", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 0),
        syntheticObservation("E05", System::Galileo, {"C1C", "L1C"}, 0, 120.0, 33.0)};
    const EpochMeasurement em = stack.model->computeMeasurementUpdate(obs, stack.states->getStateVector());
    const std::vector<std::string> lines = linesOf(trace.text());
    EXPECT_GE(decompositionLines(lines), static_cast<int>(em.innovations.size()));
    expectLineForEveryRow(lines, em);
}
