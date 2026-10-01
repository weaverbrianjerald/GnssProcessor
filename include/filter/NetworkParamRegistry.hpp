/**
 * @file NetworkParamRegistry.hpp
 * @brief The network producer's parameter registry: one declared property set per
 *        parameter kind (card PRM, docs/refound/PRM_parameter_registry_2026-09-29.md).
 *
 * The PPP path has a registry (@c StateManager owns the layout, @c StateKeySchema every
 * key format).  The network path had identities without properties: a border parameter
 * is a (kind, satellite, signal) tuple whose process noise, variance floor and carry
 * route were re-derived at every site that needed them, and a site that needed a
 * property it did not decide read a different answer from the one another site applied
 * (card CTU's root: the coupling rows shrank a clock term at one q while the fold
 * anchored the same clock at another).
 *
 * This header declares the kinds (@ref NetworkParamKind) and one descriptor per kind
 * (@ref ParamSpec), built once at run start from the knob snapshot
 * (@ref NetworkParamRegistry::build) and held read-only on @c NetworkRunContext.  The
 * registry is read, never written, inside the epoch loop.  Per-instance runtime
 * dispositions (anchored, loose, reinit, departed) stay on the instance, decided by the
 * mechanism that decides them (card PRM section 3, principle 5).
 *
 * Phase 1 scope (byte-identical by construction): the kinds and descriptors, the single
 * process-noise lookup for the border kinds (@ref NetworkParamRegistry::q), read by the
 * diagonal and dense folds, the CTU mirror, the anchor and the coupling builder in place
 * of their own per-kind selections, and the "model as armed" table
 * (@ref NetworkParamRegistry::modelTable) written at run start and in the run-end tail.
 * The descriptor states what the configuration arms; it does not move any decision.
 */
#pragma once

#include "filter/NetworkEstimator.hpp"   // SharedParamKind

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkKnobs;

/**
 * @brief Every parameter kind of the network estimator.
 *
 * The first four are the shared border (the satellite parameters every station sees,
 * the existing @c SharedParamKind values, which stay as the border subset); the rest
 * are station-local (a column of one station's local block).  The gross-screen holds are
 * instances of Ambiguity and SlantIono with a hold state, not kinds.
 */
enum class NetworkParamKind : int {
    SatClock = 0,     ///< Satellite clock (border; signal-independent).
    SatPhaseBias,     ///< Satellite phase bias per signal (border; carries the pivot lump).
    SatCodeBias,      ///< Satellite code bias per signal (border).
    VtecSh,           ///< Global SH-VTEC coefficient (border; network-wide).
    Ambiguity,        ///< Float ambiguity per (station, satellite, signal) (local).
    Zwd,              ///< Zenith wet delay per station (local).
    SlantIono,        ///< Slant ionosphere per (station, satellite) (local).
    RxClock,          ///< Receiver clock and ISB per station (local).
    RxPhaseBias,      ///< Receiver phase bias of the eq34 chain datum (local).
    RxCodeBias,       ///< Receiver code bias per (station, signal) (local).
    PhaseColour,      ///< Band-common phase nuisance per (station, satellite) (local; card LGM).
};

/// Number of @ref NetworkParamKind enumerators (the registry's table size).
inline constexpr std::size_t kNetworkParamKindCount = 11;

/// @brief How a kind's process noise scales across an epoch gap.
enum class ParamQRule {
    None,                ///< Not carried, or carried with no process noise stated.
    RandomWalk,          ///< var += q · gap_s (q in m²/s).
    RandomWalkPerDegree, ///< SH ladder: q / n² per degree n ≥ 1, q at n = 0 (TECU²/s).
    AnchorElapsed,       ///< Card Q3: anchored at the anchor's q over the satellite's own elapsed time, loose at the legacy q until the guard re-validates.
    RandomWalkPerEpoch,  ///< var += q · gap_s / epoch (q in m² per epoch; the in-block local carries).
    GaussMarkov,         ///< x' = phi x + w, phi = exp(-gap/tau), var(w) = q (1 - phi²); q is the stationary variance (m²) at the zenith.
};

/// @brief The route a carried instance of a kind takes into the next epoch's solve.
enum class ParamRoute {
    NotCarried,           ///< Re-estimated from the data every epoch.
    DenseBorder,          ///< One information block over the border carry (C1, shared_carry_full_cov).
    DiagonalBorder,       ///< A diagonal prior per border key (D2).
    LocalBlock,           ///< A row of the station's jointly whitened local block (F3 / the float vehicle).
    ConditionalRow,       ///< A conditional coupling row on the border, one per column (card 1).
    DiagonalStationPrior, ///< A diagonal prior on the local column (addStationPrior).
    PriorRow,             ///< A pseudo-observation row of the station's A_r (before the PD test).
};

/**
 * @brief The declared properties of one parameter kind on this run.
 *
 * Built once from the knob snapshot.  A value that a kind does not have on this run is
 * NaN (printed as "-").  @c source names the knob each number came from, so the table a
 * human reads can be checked against the configuration.
 */
struct ParamSpec {
    NetworkParamKind kind{NetworkParamKind::SatClock};
    const char*  name{""};          ///< Short table name (the kind's lower-case label).
    bool         estimated{false};  ///< The kind has columns on this run.
    bool         carried{false};    ///< Instances are carried across epochs on this run.
    double       q{0.0};            ///< The process-noise rate (units in @ref q_units).
    const char*  q_units{"m^2/s"};  ///< Units of @ref q.
    ParamQRule   q_rule{ParamQRule::None};
    /// The anchored rate under @c ParamQRule::AnchorElapsed (card Q3's
    /// network.clock_anchor_q, m²/s): the rate a held satellite clock is folded at, while
    /// @ref q is the loose rate it takes until the guard re-validates it.  NaN otherwise.
    double       q_anchored{std::numeric_limits<double>::quiet_NaN()};
    /// The time constant under @c ParamQRule::GaussMarkov (s).  NaN otherwise.
    double       tau_s{std::numeric_limits<double>::quiet_NaN()};
    double       var_floor_m2{0.0}; ///< Variance floor of the carried instance (m²; 0 = none).
    double       birth_var_m2{std::numeric_limits<double>::quiet_NaN()};  ///< Variance floor at an instance's birth (m²; 0 = none, NaN = the kind has no birth rule).
    ParamRoute   route{ParamRoute::NotCarried};
    std::string  source;            ///< The knob(s) behind q, the floor and the route.
};

/**
 * @brief The registry: one @ref ParamSpec per @ref NetworkParamKind.
 *
 * Immutable after @ref build; one instance per run, a const member of
 * @c NetworkRunContext.
 */
class NetworkParamRegistry {
public:
    /**
     * @brief Build the descriptors from the run's knob snapshot.
     *
     * Every number is read from @p k (or, for the few properties the snapshot does not
     * carry, from @p config), never recomputed, so the registry's values are the doubles
     * the folds used before it existed and the solve is unchanged.
     *
     * @param k      The run's knob snapshot (@c NetworkRunContext::knobs).
     * @param config The run's configuration (estimation switches and the SH ladder).
     * @return The registry.
     */
    static NetworkParamRegistry build(const NetworkKnobs& k, const GnssConfig& config);

    /**
     * @brief The descriptor of one kind.
     * @param kind The kind.
     * @return Its descriptor (a reference into the registry's table).
     */
    const ParamSpec& spec(NetworkParamKind kind) const {
        return specs_[static_cast<std::size_t>(kind)];
    }

    /**
     * @brief The single process-noise source for the border kinds.
     *
     * The random-walk rate a carried border instance of @p kind is inflated at (m²/s):
     * the satellite clock's legacy rate (which card Q3's anchor replaces per satellite
     * while it holds the clock), the phase bias's and the code bias's.  For @c VtecSh it
     * is the degree-0 rate of the legacy shell (TECU²/s); the per-degree ladder stays in
     * @c CouplingRowBuilder::couplingQ and the SH prior inject until an SH card re-arms
     * them (card PRM section 10).
     *
     * @param kind The border kind.
     * @return The rate.
     */
    double q(SharedParamKind kind) const;

    /**
     * @brief The "model as armed" table: a header line and one line per kind (card PRM
     *        section 6).
     *
     * Columns: kind, estimated, carried, q with its units and rule, the anchored rate,
     * the Gauss-Markov time constant, the variance floor, the birth floor, the route and
     * the knob sources.  Trace class: written at run start
     * and in the run-end tail, never read by the solve.
     *
     * @return The lines, without trailing newlines.
     */
    std::vector<std::string> modelTable() const;

    /**
     * @brief The same table as one JSON object (the run manifest's @c param_model
     *        record): @c {"record":"param_model","kinds":[{...}, ...]}.
     * @return The JSON text, one line.
     */
    std::string modelJson() const;

private:
    std::array<ParamSpec, kNetworkParamKindCount> specs_{};
};

/// @brief The registry kind of a border @c SharedParamKind.
NetworkParamKind paramKindOf(SharedParamKind kind);

/// @brief Lower-case table name of a kind (e.g. "sat_clock").
const char* paramKindName(NetworkParamKind kind);

/// @brief Table name of a q rule (e.g. "rw").
const char* paramQRuleName(ParamQRule rule);

/// @brief Table name of a route (e.g. "local_block").
const char* paramRouteName(ParamRoute route);

} // namespace GnssProcessor
