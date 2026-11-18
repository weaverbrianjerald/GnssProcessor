/**
 * @file LiveRecord.hpp
 * @brief The live record (@c network.trace_live_record -> @c network_live.jsonl): one JSON
 *        line per processed epoch, flushed at each line, which the live monitor
 *        (`scripts/netview/netview.py live`) follows while the engine runs.
 *
 * Purpose.  The dashboard reads a finished run's traces, hundreds of megabytes per 6 h and
 * written through buffered streams.  A live monitor needs a small record that is complete
 * when each epoch ends and that a running service can afford with the heavy traces off.
 * This record is that summary: how the epoch ended, the post-fit sigma0 of code and phase,
 * the receivers in the solution and the satellites with a clock, per receiver its post-fit
 * code and phase RMS, its satellites, the robust scale of its normalized code residuals, its
 * largest normalized residual, its wet delay and its clock sigma, per satellite its post-fit
 * code RMS, its receivers, its emitted clock and its discontinuity count, the held-out user's
 * position error and every event of the epoch; with the integer fixing armed, per family,
 * receiver and satellite the links that are not tree links and how many of them are held at
 * whole numbers.  About 8 KB per epoch for 56 receivers.  The monitor classifies the events
 * (cycle slip, quality test, alarm) from their tiers, as the dashboard does, so the
 * classification has one owner.
 *
 * Four notes and one observer build an epoch's line:
 *   - NetworkRunTraces::emitEpochStatus notes how the epoch ended (every processed epoch,
 *     a skipped one included, reaches one disposition site);
 *   - NetworkRunTraces::emitEvent notes each event;
 *   - networkRingFixDecide notes the integer fixing's links, with network.ring_fix armed: each
 *     link with a float this epoch and whether it is held in this epoch's solve (the state
 *     network_ring_fix.csv calls "held"; network/ar/RingFixSeams.cpp);
 *   - networkEmitStore notes the held-out user's position error;
 *   - the observer @ref GnssProcessor::networkLiveRecord "networkLiveRecord" (slot StashTail,
 *     network/driver/EpochObservers.hpp) adds the receivers and the satellites and writes the
 *     line.
 * An epoch that ends before StashTail (a skip) has no observer call: its line is written with
 * what was noted when the first note of a later epoch arrives, or at the run end.  A note
 * that arrives for an epoch whose line is already written starts a second line of that epoch,
 * which the monitor merges (no writer does so today).
 *
 * Format, one JSON object per line (absent values are @c null; a key with nothing to say is
 * left out):
 * @code
 *   {"kind":"header","format":"netview-live/1","stations":[{"name":"ABMF00GLP","lat":16.26232,
 *    "lon":-61.52753},...],"rx_fields":[...],"sat_fields":[...]}
 *   {"t":[2419,345600],"disp":"emitted","npres":56,"nclk":61,"s0":[0.288,0.175],
 *    "user":[0.0123,0.0041,-0.0062,0.0098],"wall_ms":812,
 *    "rx":[[0.2511,0.00102,12,0.291,4.12,0.1432,0.0081],null,...],
 *    "sat":{"E01":[0.2310,40,1234.5678,3],...},
 *    "fam":{"E|L1X+L5X":[212,180],"G|L1C+L2W":[431,402]},
 *    "ev":[["T1_if_slip","slip","G05","ABMF00GLP","reset","detail",1.23,0.5],...]}
 * @endcode
 * The header comes first, written once at setup by networkLiveRecordOpen; @c rx holds one
 * entry per header station, in the header's order, @c null for a station not in the solve;
 * @c sat is keyed by satellite id.  The field lists are @ref kLiveStationFields and
 * @ref kLiveSatelliteFields.  With the integer fixing armed (network.ring_fix.enable) both
 * lists end with @ref kLiveHoldFields: every receiver and satellite entry then ends with its
 * links and its held links (both @c null on an epoch the fixing noted nothing, as at ignition
 * before the chain's record is armed), and @c fam gives the two counts per family.  Without
 * the fixing the header and the lines carry neither.
 *
 * Measure-only.  Nothing the solve or the wire reads is written; the residuals are formed
 * from the solved state exactly as the post-fit pass forms them (network/diag/FrameDiag.cpp),
 * so the record's values equal the post-fit traces' values of the same run.  Off by default:
 * no stream, no file, no line, and every other output is byte-identical.
 */
#pragma once

#include "math/TimeUtils.hpp"   // GnssTime

#include <Eigen/Core>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochStatusRow;          // network/driver/RunTypes.hpp
struct NetStation;              // network/driver/RunTypes.hpp
struct StationAssembly;         // network/driver/RunTypes.hpp
struct NetworkSolutionEpoch;    // products/NetworkProductEmitter.hpp
class  NetworkProductEmitter;   // products/NetworkProductEmitter.hpp
class  NetworkEstimator;        // network/solve/Estimator.hpp

/// The value of a quantity a line has no number for (written as JSON @c null).
inline constexpr double kLiveNoValue = std::numeric_limits<double>::quiet_NaN();

/// The header's names of a receiver entry's values, in their order in a line's @c rx entry.
inline constexpr std::array<const char*, 7> kLiveStationFields = {
    "code_rms_m", "phase_rms_m", "n_sats", "code_scale", "max_norm", "zwd_m", "clk_sd_m"};

/// The header's names of a satellite entry's values, in their order in a line's @c sat entry.
inline constexpr std::array<const char*, 4> kLiveSatelliteFields = {
    "code_rms_m", "n_rx", "clk_m", "disc"};

/// The names both field lists end with when the integer fixing is armed: the entry's links that
/// are not tree links and have a float this epoch, and how many of them are held at whole
/// numbers in this epoch's solve.
inline constexpr std::array<const char*, 2> kLiveHoldFields = {"fix_links", "fix_held"};

/// The integer fixing's links of one family, receiver or satellite in an epoch.
struct LiveHoldCount {
    int links{0};   ///< Links that are not tree links, with a float this epoch.
    int held{0};    ///< Of them, the links held at whole numbers in this epoch's solve.
};

/**
 * @brief The integer fixing's links of an epoch, counted by family, receiver and satellite:
 *        the counts the dashboard takes from network_chain_frame.csv (the links) and
 *        network_ring_fix.csv (their state).
 */
struct LiveHolds {
    std::map<std::string, LiveHoldCount> families;   ///< By family ("G|L1C+L2W": system letter, band-1 and band-2 signals).
    std::map<int, LiveHoldCount>         stations;   ///< By roster index (the header's order).
    std::map<std::string, LiveHoldCount> sats;       ///< By satellite id.

    /**
     * @brief Counts one link in its family, its receiver and its satellite.
     * @param family  The link's family ("G|L1C+L2W").
     * @param station Its receiver's roster index.
     * @param sat     Its satellite ("G05").
     * @param held    Whether it is held at whole numbers in this epoch's solve.
     */
    void add(const std::string& family, int station, const std::string& sat, bool held);
};

/// A receiver of the header: its name and its geodetic position (degrees).
struct LiveStation {
    std::string name;      ///< The station's name (its roster identifier).
    double lat_deg{0.0};   ///< Geodetic latitude (degrees).
    double lon_deg{0.0};   ///< Longitude (degrees).
};

/// One receiver's values in an epoch's line (@ref kLiveNoValue: no value this epoch).
struct LiveStationValues {
    bool   present{false};                 ///< In this epoch's solve; a station that is not is written as @c null.
    double code_rms_m{kLiveNoValue};       ///< RMS of the post-fit code residuals (m).
    double phase_rms_m{kLiveNoValue};      ///< RMS of the post-fit phase residuals (m).
    int    n_sats{0};                      ///< Distinct satellites with a post-fit code row.
    double code_scale{kLiveNoValue};       ///< 1.4826 x the median of |v| sqrt(w) over its code rows (1 = as weighted).
    double max_norm{kLiveNoValue};         ///< Largest |v| sqrt(w) over its code and phase rows.
    double zwd_m{kLiveNoValue};            ///< Wet delay: linearization point plus the reconstructed correction (m).
    double clk_sd_m{kLiveNoValue};         ///< Receiver clock sigma given the shared states, sqrt of diag(N_rr^-1) (m).
};

/// One satellite's values in an epoch's line.
struct LiveSatelliteValues {
    std::string sat;                       ///< RINEX satellite id ("G05").
    double    code_rms_m{kLiveNoValue};    ///< RMS of its post-fit code residuals over every receiver (m).
    int       n_rx{0};                     ///< Distinct receivers with a post-fit code row of it.
    double    clk_m{kLiveNoValue};         ///< Its emitted clock (m); no value on an epoch that emitted none.
    long long disc{-1};                    ///< Sum of the emitter's discontinuity counters (clock, phase and code biases); -1 when not emitted.
};

/// The values the observer adds to an epoch's line.
struct LiveEpochValues {
    double s0_code{kLiveNoValue};                ///< 1.4826 x the median of |v| sqrt(w) over every code row.
    double s0_phase{kLiveNoValue};               ///< The same over every phase row.
    std::vector<LiveStationValues>   stations;   ///< One per header station, in the header's order.
    std::vector<LiveSatelliteValues> satellites; ///< In satellite-id order.
};

/// One event of the epoch, as NetworkRunTraces::emitEvent received it.
struct LiveEvent {
    std::string tier;                  ///< The emitting layer (T1_if_slip, T3_raim, CHAIN, ...).
    std::string event;                 ///< What happened.
    std::string sat;                   ///< Its satellite, or empty.
    std::string station;               ///< Its station, or empty.
    std::string action;                ///< What was done.
    std::string detail;                ///< Free text, cut to @ref kLiveDetailBytes.
    std::optional<double> statistic;   ///< Its statistic, when in scope.
    std::optional<double> threshold;   ///< Its threshold, when in scope.
};

/// The longest event detail a line keeps (bytes, cut on a UTF-8 character boundary).
inline constexpr std::size_t kLiveDetailBytes = 200;

/// An epoch's line while it is built.
struct LiveEpochLine {
    GnssTime t;                                 ///< The epoch.
    std::string disposition;                    ///< How it ended (empty: not noted).
    std::optional<int> n_present;               ///< Stations in the solve, from the status row.
    std::optional<int> n_clock_sats;            ///< Registered satellite clocks, from the status row.
    std::optional<std::array<double, 4>> user;  ///< Held-out user error: 3D, east, north, up (m).
    std::vector<LiveEvent> events;              ///< The events noted for the epoch.
    std::optional<LiveHolds> holds;             ///< The integer fixing's links (absent: not noted this epoch).
    std::optional<LiveEpochValues> values;      ///< The observer's part (absent on a skipped epoch).
    std::optional<double> wall_ms;              ///< Wall time since the previous observer-written line (ms).
};

/**
 * @brief Quotes a string for JSON: the quote, the backslash and the control characters are
 *        escaped; other bytes (UTF-8) pass through.
 * @param s The text.
 * @return The quoted JSON string.
 */
std::string liveJsonString(std::string_view s);

/**
 * @brief Cuts a text to at most @p max_bytes bytes without splitting a UTF-8 character.
 * @param s         The text.
 * @param max_bytes The most bytes kept.
 * @return The text, shortened when longer.
 */
std::string liveCutUtf8(std::string_view s, std::size_t max_bytes);

/**
 * @brief The header line (no newline): the format, the receivers and the field lists.
 * @param stations    The network's receivers, in the order of every line's @c rx entries.
 * @param hold_fields Whether both field lists end with @ref kLiveHoldFields (the integer
 *                    fixing armed).
 * @return The JSON object.
 */
std::string formatLiveHeader(const std::vector<LiveStation>& stations, bool hold_fields);

/**
 * @brief An epoch's line (no newline).
 * @param line        The epoch's notes and the observer's values.
 * @param hold_fields Whether the header declared @ref kLiveHoldFields: every receiver and
 *                    satellite entry then ends with its two counts, @c null when the fixing
 *                    noted nothing this epoch.
 * @return The JSON object.
 */
std::string formatLiveLine(const LiveEpochLine& line, bool hold_fields);

/**
 * @brief The header's receivers: each station's name and its geodetic latitude and longitude.
 * @param stations The run's stations.
 * @return One entry per station, in the run's order.
 */
std::vector<LiveStation> liveStationsOf(const std::vector<NetStation>& stations);

/**
 * @brief The live record's stream and the epoch line under construction.  Owned by
 *        NetworkRunTraces (`traces.live_record`); opened, with its header written, by
 *        @ref GnssProcessor::networkLiveRecordOpen "networkLiveRecordOpen".
 *
 * Every member function is a no-op while the stream is closed, so the notes cost one test
 * on a run without the record.  The notes and the observer run on the epoch loop's thread.
 */
struct NetworkLiveRecord {
    /**
     * @brief Opens the stream (truncating) and writes the header line.
     * @param path        The file (`<log dir>/network_live.jsonl`).
     * @param stations    The header's receivers.
     * @param hold_fields Whether the header declares @ref kLiveHoldFields (network.ring_fix.enable).
     * @param log         Logger for the opened / failed line.
     */
    void open(const std::filesystem::path& path, const std::vector<LiveStation>& stations, bool hold_fields,
              const std::shared_ptr<spdlog::logger>& log);

    /// @brief Whether the record is armed and its stream open.
    /// @return True when lines are written.
    bool on() const { return out.is_open(); }

    /**
     * @brief Notes how an epoch ended (from the epoch-status disposition site).
     * @param t           The epoch.
     * @param disposition The disposition ("emitted", "sync_skip", ...).
     * @param r           The status row; its station and clock counts are kept.
     */
    void noteStatus(const GnssTime& t, const char* disposition, const EpochStatusRow& r);

    /**
     * @brief Notes one event of an epoch (the arguments of NetworkRunTraces::emitEvent).
     * @param t         The epoch.
     * @param tier      The emitting layer.
     * @param event     What happened.
     * @param sat       Its satellite, or empty.
     * @param station   Its station, or empty.
     * @param statistic Its statistic, when in scope.
     * @param threshold Its threshold, when in scope.
     * @param action    What was done.
     * @param detail    Free text.
     */
    void noteEvent(const GnssTime& t, const char* tier, const char* event, const std::string& sat,
                   const std::string& station, std::optional<double> statistic,
                   std::optional<double> threshold, const char* action, const std::string& detail);

    /**
     * @brief Notes the held-out user's position error of an epoch.
     * @param t       The epoch.
     * @param err3d_m The 3D error (m).
     * @param enu_m   The east, north and up errors (m).
     */
    void noteUser(const GnssTime& t, double err3d_m, const Eigen::Vector3d& enu_m);

    /**
     * @brief Notes the integer fixing's links of an epoch (from networkRingFixDecide).
     * @param t     The epoch.
     * @param holds The links with a float this epoch, counted by family, receiver and satellite.
     */
    void noteHolds(const GnssTime& t, LiveHolds holds);

    /**
     * @brief Adds the observer's values to the epoch's line and writes it (flushed).  A
     *        satellite the fixing counted that neither a residual nor the products named gets
     *        an entry with its counts alone.
     * @param t      The epoch.
     * @param values The receivers, the satellites and the network sigma0.
     */
    void writeEpoch(const GnssTime& t, LiveEpochValues values);

    /// @brief Writes a line still pending (a skipped last epoch) and closes the stream.
    void close();

    std::ofstream out;                       ///< The stream (closed unless armed).
    bool hold_fields{false};                 ///< The header declared kLiveHoldFields (the integer fixing armed).
    std::optional<LiveEpochLine> pending;    ///< The line of the epoch being noted.
    std::optional<std::chrono::steady_clock::time_point> last_epoch_write;  ///< When the observer last wrote a line.
    long long lines_written{0};              ///< Epoch lines written (the header not counted).

private:
    /// The line of epoch @p t: a pending line of another epoch is written first.
    LiveEpochLine& lineFor(const GnssTime& t);
    /// Writes one epoch line and flushes the stream.
    void writeLine(const LiveEpochLine& line);
};

/**
 * @brief Setup builder: opens the live record and writes its header when
 *        @c network.trace_live_record is armed.  Called once from runNetwork, after the
 *        trace streams open (the header needs the station roster, which openAll lacks).
 * @param run The run (written: its trace set's live record).
 */
void networkLiveRecordOpen(NetworkRun& run);

/**
 * @brief Observer (slot StashTail): the receivers' and satellites' values of the epoch, from
 *        the solved state and the prepared solution, and the epoch's line written.
 *
 * Per station in the solve, the post-fit residuals v = y - A_s x_s - A_r x_r with x_r the
 * station's reconstruction (the post-fit pass's construction); the code and phase rows by
 * their row class.  The wet delay and the clock sigma come from the station's columns through
 * its parameter set and h2local, as the wet-delay trace finds them.  The satellite clocks and
 * the discontinuity counters are read only on an emitted epoch.
 * @param run The run (written: the live record's line and stream).
 * @param ep  The epoch, read only.
 */
void networkLiveRecord(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
