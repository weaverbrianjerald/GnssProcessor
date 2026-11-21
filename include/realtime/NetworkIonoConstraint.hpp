#pragma once

/**
 * @file NetworkIonoConstraint.hpp
 * @brief The user's corrections from the network's slant ionosphere: one correction per satellite from the network
 *        receivers' links, its weight, the per-satellite events at both edges and the between-satellite rows
 *        (@c network.closed_loop.iono_file, stage 1; the links come from @c NetworkIonoFile).
 *
 * Corrections, per epoch and constellation, from links whose integers the network holds at that epoch only:
 *  - @c pair (two receivers at the user's site): d_s = I_A - I_B; the receiver term is the median of d over three or
 *    more satellites; a satellite with |d_s - median| above test_k sigma(el) is not used (the two receivers
 *    disagree); the correction is the mean of the two, aligned by the median;
 *  - @c plane (four or more receivers around the user): a reference satellite seen by the user and integer at the
 *    most receivers; per other satellite, the receivers' between-satellite differences fitted by a plane in east and
 *    north, evaluated at the user's a priori position (the reference's correction is zero);
 *  - @c single (one receiver): its value.
 * Each correction carries an arbitrary level per constellation (a receiver term, or the reference's value); only
 * between-satellite differences are applied, so the level cancels together with the receiver terms of the network
 * receivers and of the user, and no state is added.
 *
 * Cut: a link enters only when the satellite's elevation at that network receiver is at least min_elev_deg (the
 * author's ruling of 2026-11-21: the network's slant ionosphere agrees between the two site receivers to 0.2 to 0.3 cm
 * above 30 degrees and has a tail of centimetres below); the user keeps its own mask, and its satellites without a
 * correction keep a free ionosphere.
 *
 * Weight: sigma(el), one table by elevation bin at the user from the cut up, measured at the user (the closure of the
 * corrections with the converged user's own slant states); an elevation below the first edge takes the first bin.
 * The rows of one constellation against reference r are x_s - x_r = c_s - c_r with covariance D diag(sigma^2) D^T
 * (sigma_r^2 + delta_kl sigma_s^2).
 *
 * Events, per satellite and counted at both edges (standing invariant 7):
 *  - the innovation test: per constellation, u_s = c_s - x_s over the satellites with a correction and a state
 *    (the BSSD pivot's slant state is frozen, so the states carry one common offset per constellation, which the
 *    centre absorbs), centred on their median; a satellite
 *    with |u_s - median| above test_k sqrt(sigma_s^2 + P_ss + V_m) fails, V_m = pi / (2 n) times the median of the
 *    n values sigma^2 + P (the median's own variance, so the pivot is not failed while the others are loose); two
 *    satellites: their difference is tested and both fail together;
 *  - an active satellite is released when its correction is missing, a link is not integer, the receivers disagree,
 *    the constellation has too few satellites for the combination, its links drop below the cut, or it fails the
 *    innovation test; on an innovation release its slant variance is reset (refloat), since its state and the
 *    correction conflict; on the other releases the state relaxes with its own process noise;
 *  - a satellite never released is used from its first passing epoch; a released one only after return_ep
 *    consecutive passing epochs (the return is validated); a return interrupted by a failing epoch is counted.
 * Plain value types and free functions; the real-time engine owns one @c NetworkIonoConstraint across epochs and
 * applies its rows (@c RealTimeEngine::applyNetworkIonoFile_).
 */

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "realtime/NetworkIonoFile.hpp"

namespace GnssProcessor {

/// How the receivers' values become one correction per satellite.
enum class IonoFileCombine { Pair, Plane, Single };

/// Why a satellite has no row this epoch (the release causes).
enum class IonoFileCause : std::size_t {
    Missing = 0,   ///< No link at a needed receiver, no network epoch, or no user state.
    NotInteger,    ///< A needed link's integers are not held by the network at this epoch.
    Disagree,      ///< The two site receivers disagree beyond test_k sigma.
    TooFew,        ///< Too few satellites or receivers for the combination.
    LowElevation,  ///< The needed links are below the elevation cut at the network receivers.
    Innovation,    ///< The row's innovation exceeds test_k sigma (a jump or a conflict).
    Count
};

/// Short name of a cause, for the log and the run-end line.
const char* ionoFileCauseName(IonoFileCause cause);

/// Parse a combination name ("pair", "plane", "single"); false for anything else.
bool parseIonoFileCombine(const std::string& name, IonoFileCombine& combine);

/// Settings of the corrections and the events.
struct IonoFileSettings {
    IonoFileCombine combine{ IonoFileCombine::Pair };  ///< How the receivers' values combine.
    std::vector<std::string> stations;                  ///< Network receivers read (two, one, or four or more).
    double min_elev_deg{ 30.0 };                        ///< Elevation cut at the network receiver, degrees.
    std::vector<double> sigma_elev_deg;                 ///< Lower edges of the elevation bins, degrees, increasing.
    std::vector<double> sigma_m;                        ///< Sigma of one satellite's correction per bin, metres.
    double test_k{ 4.0 };                               ///< Bound of the agreement and innovation tests, in sigma.
    int return_ep{ 4 };                                 ///< Passing epochs before a released satellite returns.

    /**
     * @brief Sigma of one satellite's correction at the user's elevation.
     * @param elevation_deg  Elevation at the user, degrees.
     * @return The bin's sigma in metres (the first bin below the first edge); negative only for an empty table.
     */
    double sigma(double elevation_deg) const;
};

/// A satellite the user offers this epoch (it has a slant state).
struct IonoFileUserSat {
    std::string sat;               ///< Satellite identifier.
    double elevation_deg{ 0.0 };   ///< Elevation at the user, degrees.
};

/// One satellite's correction this epoch.
struct IonoFileCorrection {
    std::string sat;                                ///< Satellite identifier.
    bool valid{ false };                            ///< A correction exists and passed the network-side tests.
    IonoFileCause cause{ IonoFileCause::Missing };  ///< Why not, when @c valid is false.
    double value_m{ 0.0 };                          ///< Correction, metres on GPS L1, arbitrary level per constellation.
    double sigma_m{ 0.0 };                          ///< Its sigma at the user's elevation, metres.
    double elevation_net_deg{ 0.0 };                ///< Mean elevation at the network receivers used, degrees.
};

/// East and north offsets (km) of each receiver from the user's a priori position.
using IonoFileOffsets = std::map<std::string, std::array<double, 2>>;

/**
 * @brief East and north offsets of the receivers from the user, in the user's local horizon (km).
 * @param user_ecef  The user's a priori position, metres.
 * @param receivers  Receiver -> ECEF position, metres.
 * @return Receiver -> (east, north) in km.
 */
IonoFileOffsets ionoFileOffsets(const std::array<double, 3>& user_ecef,
                                const std::map<std::string, std::array<double, 3>>& receivers);

/**
 * @brief This epoch's corrections, one per offered satellite (a pure function of its inputs).
 * @param settings  Combination, receivers and sigma table.
 * @param links  The network epoch's links (nullptr when the network run has no epoch here).
 * @param sats  The satellites the user offers.
 * @param offsets  Receivers' east and north offsets from the user (read by @c plane only).
 * @return One correction per offered satellite, in the order of @p sats.
 */
std::vector<IonoFileCorrection> ionoFileCorrections(const IonoFileSettings& settings, const NetworkIonoEpoch* links,
                                                    const std::vector<IonoFileUserSat>& sats,
                                                    const IonoFileOffsets& offsets);

/// The user's slant state of one offered satellite, as the innovation test reads it.
struct IonoFileUserState {
    double x_m{ 0.0 };     ///< Slant state, metres on GPS L1 (one common offset per constellation, set by the frozen pivot).
    double var_m2{ 0.0 };  ///< Its variance, m^2 (zero for the pivot).
    bool pivot{ false };   ///< The satellite is its constellation's BSSD pivot.
};

/// The rows of one constellation: one row per satellite against the reference.
struct IonoFileRows {
    char constellation{ 'G' };      ///< Constellation letter.
    std::string reference;          ///< Reference satellite r.
    std::vector<std::string> sats;  ///< Satellites s, one row x_s - x_r = c_s - c_r each.
};

/// One offered satellite's test and state this epoch (the per-satellite trace reads it).
struct IonoFileSatResult {
    std::string sat;          ///< Satellite identifier.
    bool tested{ false };     ///< It took part in the innovation test (a correction and a state).
    double innov_m{ 0.0 };    ///< Its innovation against the constellation's centre, metres.
    double bound_m{ 0.0 };    ///< The test bound, metres.
    bool active{ false };     ///< Its row is applied this epoch (would be, in the measure-only form).
};

/// What the engine applies this epoch.
struct IonoFileDecision {
    std::vector<IonoFileRows> rows;      ///< Rows by constellation (two or more active satellites).
    std::vector<std::string> refloat;    ///< Satellites whose slant variance is reset (innovation releases).
    std::vector<std::string> events;     ///< One short text per release, return or failed return.
    std::vector<IonoFileSatResult> sats; ///< One per offered satellite, in the order of the corrections.
};

/// Counters of one constellation, printed in the run-end line.
struct IonoFileCounters {
    long epochs_with_rows{ 0 };    ///< Epochs with at least one row applied.
    long rows{ 0 };                ///< Rows applied.
    long first_uses{ 0 };          ///< Satellites used at their first passing epoch (never released before).
    long returns_validated{ 0 };   ///< Released satellites back after return_ep passing epochs.
    long returns_failed{ 0 };      ///< Returns interrupted by a failing epoch.
    long refloats{ 0 };            ///< Slant variances reset on an innovation release.
    std::array<long, static_cast<std::size_t>(IonoFileCause::Count)> releases{};   ///< Releases by cause.
};

/**
 * @class NetworkIonoConstraint
 * @brief The per-satellite events of the file corrections across epochs, and the rows of each epoch.
 */
class NetworkIonoConstraint {
public:
    /// Set the settings (combination, receivers, table, test bound, return epochs).
    void setSettings(const IonoFileSettings& settings) { settings_ = settings; }

    /// The settings in use.
    const IonoFileSettings& settings() const { return settings_; }

    /**
     * @brief The events of one epoch and the rows to apply.
     *
     * Runs the innovation test, advances each offered satellite's state (release, return, first use), releases the
     * active satellites the user no longer offers (cause Missing), and forms one row set per constellation with two
     * or more active satellites: the reference is the constellation's BSSD pivot when it is active, otherwise the
     * active satellite with the highest elevation.
     *
     * @param corr  This epoch's corrections (ionoFileCorrections), one per offered satellite.
     * @param state  The user's slant state of each offered satellite, by satellite.
     * @param elevation  The user's elevation of each offered satellite, by satellite, degrees.
     * @return Rows, refloats and the epoch's event texts.
     */
    IonoFileDecision decide(const std::vector<IonoFileCorrection>& corr,
                            const std::map<std::string, IonoFileUserState>& state,
                            const std::map<std::string, double>& elevation);

    /// Counters by constellation letter.
    const std::map<char, IonoFileCounters>& counters() const { return counters_; }

    /// The run-end line: per constellation, rows, first uses, releases by cause, refloats and returns.
    std::string runEndLine() const;

private:
    /// One satellite's use across epochs.
    struct SatUse {
        bool active{ false };    ///< Its row is applied.
        bool released{ false };  ///< It has been released at least once (a return must be validated).
        int streak{ 0 };         ///< Consecutive passing epochs while returning.
    };

    IonoFileSettings settings_;                ///< Settings.
    std::map<std::string, SatUse> use_;        ///< Satellite -> its use.
    std::map<char, IonoFileCounters> counters_;///< Constellation -> counters.
};

}  // namespace GnssProcessor
