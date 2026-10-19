#pragma once
/**
 * @file PivotElection.hpp
 * @brief The clock pivot's election by predicted station coverage (network.pivot_election).
 *
 * The network pins one satellite clock per constellation, the pivot, and every other clock is
 * estimated against it. The pivot is only a gauge, but the satellite chosen carries the
 * weight of the pin: a pivot that few stations track, that the broadcast flags unhealthy, or
 * that enters the Earth's shadow soon, holds the whole constellation's clock level on a weak
 * and short-lived tie. The legacy rule picks the lexically first registered satellite.
 *
 * This election applies at two moments only: the first epoch of a constellation, and a forced
 * handover (the incumbent pivot left the registered set or the broadcast flags it
 * unhealthy). It ranks the registered, healthy satellites by how many roster stations the
 * orbit product predicts above the elevation mask over a look-ahead horizon, and passes over
 * a satellite that enters the Earth's umbral shadow inside that horizon. Whenever an incumbent
 * pivot is registered and healthy it stays, so a run that never loses its pivot never hands
 * over.
 *
 * Everything the ranking reads is available before the epoch: the orbit product, the roster
 * coordinates and the Sun. It reads no observation. The ranking is a pure function of its
 * inputs (the sums are integer counts, ties go to the lexically first satellite), so the
 * preview before the rows and the solve glue, which both elect, always agree.
 *
 * Layout: the ranking itself (@ref rankPivotCandidates) is a free function over plain
 * scores, the predictor (@ref NetworkPivotCoverage) turns an orbit product, a roster and a
 * Sun function into those scores, and @ref networkPivotCoverageArm builds the predictor once
 * per run from the product cache.
 */

#include <Eigen/Dense>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class LogManager;
struct NetworkRun;

/// The settings of the coverage election.
struct PivotCoverageParams {
    double horizon_s{7200.0};         ///< Look-ahead of the coverage count and the shadow test (s).
    double step_s{300.0};             ///< Sample step (s); the samples are t, t + step, ..., up to t + horizon.
    double elevation_mask_deg{10.0};  ///< A station counts a satellite from this elevation up.
    double shadow_scale{1.0};         ///< Earth-radius multiplier of the cylindrical umbra test (1 = geometric umbra).

    /// @return The number of samples of the horizon, the first at the election epoch.
    std::size_t sampleCount() const;
};

/// One candidate satellite's predicted standing over the horizon.
struct PivotCandidateScore {
    std::string sat;               ///< Satellite id ("G14").
    bool        has_orbit{false};  ///< The orbit product describes the satellite at the election epoch.
    long long   station_samples{0};  ///< Count of (station, sample) pairs with the satellite at or above the mask.
    double      umbra_in_s{-1.0};  ///< Seconds from the election epoch to the first sample inside the umbra; negative when none inside the horizon.

    /// @return True when the satellite is inside the umbra at some sample of the horizon.
    bool entersUmbra() const { return umbra_in_s >= 0.0; }
};

/// The outcome of one election.
struct PivotRanking {
    std::string constellation;   ///< Constellation key ("G").
    double      t_gps_s{0.0};    ///< Election epoch, continuous GPS seconds.
    std::size_t samples{0};      ///< Samples of the horizon (the divisor of the mean station count).
    std::size_t stations{0};     ///< Roster stations counted.
    std::string sat;             ///< The elected satellite; empty when no candidate has an orbit.
    bool        all_shadowed{false};  ///< Every candidate with an orbit enters the umbra inside the horizon.
    std::vector<PivotCandidateScore> table;   ///< The candidates in the order given.

    /// @param s A candidate's score.
    /// @return The mean number of roster stations tracking the satellite over the horizon.
    double meanStations(const PivotCandidateScore& s) const;
    /// @return The score of the elected satellite, or null when none was elected.
    const PivotCandidateScore* winner() const;
    /// @return One line naming the pick, its mean station count, the runner-up and what was passed over.
    std::string describe() const;
};

/**
 * @brief Rank candidate pivots by predicted coverage, passing over those that enter the umbra.
 *
 * Among the candidates with an orbit, those that stay out of the umbra for the whole horizon
 * come first and the one with the largest @c station_samples wins. When every candidate
 * enters the umbra, the one that enters it last wins, then the larger @c station_samples.
 * A tie goes to the lexically first satellite. A candidate without an orbit is never elected.
 *
 * @param[in,out] ranking Its @c table is filled by the caller; this sets @c sat and
 *                        @c all_shadowed.
 */
void rankPivotCandidates(PivotRanking& ranking);

/// One election recorded for the run-end table.
struct PivotElectionRecord {
    PivotRanking ranking;   ///< The election that made the pick.
};

/**
 * @brief Predicts roster coverage and umbra entry of candidate pivots.
 *
 * Built once per run from the orbit product nodes, the roster coordinates and a Sun
 * function (injected so that tests need no ephemeris). Satellite positions between nodes are
 * a Lagrange interpolation over a window of nodes in the Earth-fixed frame of the query,
 * the same construction the geometry stage uses, here to a metre rather than a millimetre.
 * Not thread-safe; the election runs in serial code.
 */
class NetworkPivotCoverage {
public:
    /// One orbit product node: position in the Earth-fixed frame of its own epoch.
    struct OrbitNode {
        double          t_gps_s;   ///< Continuous GPS seconds.
        Eigen::Vector3d pos;       ///< ECEF position (m).
    };
    /// The Sun's ECEF position (m) at a continuous GPS second.
    using SunFn = std::function<Eigen::Vector3d(double t_gps_s)>;

    /**
     * @param params       The settings.
     * @param stations     Roster station ECEF positions (m).
     * @param orbits       Per satellite, orbit product nodes sorted by time.
     * @param sun          The Sun function.
     */
    NetworkPivotCoverage(const PivotCoverageParams& params,
                         std::vector<Eigen::Vector3d> stations,
                         std::map<std::string, std::vector<OrbitNode>> orbits,
                         SunFn sun);

    /**
     * @brief Rank candidates at an epoch.
     *
     * The ranking of the same epoch, constellation and candidate list is computed once; the
     * preview before the rows and the solve glue share it.
     *
     * @param t_gps_s        Election epoch, continuous GPS seconds.
     * @param constellation  Constellation key ("G").
     * @param candidates     Registered satellites eligible to be the pivot, any order.
     * @return The ranking, @c sat empty when no candidate has an orbit.
     */
    PivotRanking rank(double t_gps_s, const std::string& constellation,
                      const std::vector<std::string>& candidates) const;

    /// @return The most recent ranking made for the constellation, or null when none.
    const PivotRanking* last(const std::string& constellation) const;

    /**
     * @brief Satellite position from the orbit product.
     * @param sat      Satellite id.
     * @param t_gps_s  Continuous GPS seconds.
     * @return The ECEF position (m), or nullopt when the nodes do not bracket the epoch.
     */
    std::optional<Eigen::Vector3d> positionAt(const std::string& sat, double t_gps_s) const;

    /// Record an election for the run-end table (called once per election that made a pick).
    void noteElection(const PivotRanking& ranking);
    /// @return The elections made so far, in order.
    const std::vector<PivotElectionRecord>& elections() const { return elections_; }

    /// @return The settings.
    const PivotCoverageParams& params() const { return params_; }
    /// @return The number of roster stations.
    std::size_t stationCount() const { return stations_.size(); }
    /// @return The number of satellites with orbit nodes.
    std::size_t orbitSatelliteCount() const { return orbits_.size(); }

private:
    struct Memo {
        double                   t_gps_s{0.0};
        std::vector<std::string> candidates;
        PivotRanking             ranking;
    };

    PivotCoverageParams                          params_;
    std::vector<Eigen::Vector3d>                 stations_;   ///< Roster ECEF (m).
    std::vector<Eigen::Vector3d>                 stations_up_;  ///< Unit up of each station (geocentric).
    std::map<std::string, std::vector<OrbitNode>> orbits_;
    SunFn                                        sun_;
    mutable std::map<std::string, Memo>          memo_;       ///< Last ranking per constellation.
    std::vector<PivotElectionRecord>             elections_;
};

/**
 * @brief Setup builder: the run's coverage predictor (network.pivot_election).
 *
 * Reads the orbit nodes from the product cache and the roster coordinates from the setup,
 * and fills @c run.setup.pivot_coverage. A no-op unless @c network.pivot_election is armed. A
 * product cache without orbit nodes (a broadcast-orbit run) leaves the predictor empty, so
 * every election falls back to the lexical rule, counted at the run end.
 *
 * @param run         The run (setup is written, config and knobs are read).
 * @param log_manager The log manager the Sun model logs to.
 */
void networkPivotCoverageArm(NetworkRun& run, LogManager& log_manager);

}  // namespace GnssProcessor
