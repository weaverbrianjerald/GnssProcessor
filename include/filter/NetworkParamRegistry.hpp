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
#include <cstdint>
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
 * @brief The events a carried instance responds to (card PRM section 4.2, the descriptor's
 *        @c on_slip, @c on_gap, @c on_handover, @c on_decouple and @c on_readmit).
 */
enum class ParamEvent : int {
    Slip = 0,   ///< A discontinuity detected in the carried quantity: a cycle slip, a RAIM attribution, a receiver jump, the clock monitor's flag.
    Gap,        ///< Absence: the column leaves the layout or goes unseen, and the horizon it is carried across.
    Handover,   ///< A datum move: an anchor or pivot re-election, a frame re-reference, a declared re-base.
    Decouple,   ///< A guard separates the instance from its data or its anchor: the gross line-of-sight screen, a withheld epoch, the clock anchor's guard.
    Readmit,    ///< A departed border column returns, or a decoupled instance is re-validated.
};

/// Number of @ref ParamEvent values (array sizing).
inline constexpr std::size_t kParamEvents = 5;

/**
 * @brief The named responses (card PRM section 4.5), one value per thing a response does
 *        to a carried instance.
 *
 * Phase 3a declares which of these each kind uses on the run and counts every response at
 * its site (@c filter/NetworkResponses.hpp); phase 3b gives each one implementation.
 */
enum class ResponsePolicy : int {
    Drop = 0,          ///< Variance back to birth strength, border regression and block row cleared, value kept (the float vehicle).
    EraseAndRebirth,   ///< Entry erased; the next harvest re-creates it with fresh birth stamps.
    FreeEpoch,         ///< Not folded for one epoch (or one replay); the entry is kept and re-stashed from that epoch's data.
    GapSkip,           ///< Not folded while the gap since it was last seen exceeds the horizon; re-stashed from that epoch.
    GapPrune,          ///< Erased once unseen past the horizon.
    Marginalize,       ///< A border column left the layout: its prior is folded and the estimator marginalizes it.
    Rereference,       ///< A border entry whose frame was re-referenced: not folded for one epoch, re-stashed in the new frame.
    Retire,            ///< A border entry of a changed clock datum that was not re-expressed: not folded.
    Stransform,        ///< Re-expressed by a declared datum move (the value moved; the variance where the transform states it).
    Park,              ///< Kept past the column's departure, not folded, until it returns or the horizon expires.
    SeedAtVarPlusQGap, ///< A parked entry seeds its returned column at var + q times the gap.
    Discard,           ///< A parked or held entry dropped without a fold (a datum change, a return it was not consumed by).
    Loosen,            ///< Folded at the loose rate until a guard re-validates it (card Q3's anchor).
    Recouple,          ///< The guard re-validated a loosened instance: it folds at its anchored rate again.
    Reinit,            ///< Folded at the re-initialization variance (the clock monitor's flag).
    Retap,             ///< Value kept, variance inflated.
    Hold,              ///< Frozen and re-applied while a guard holds it.
    ReanchorAtTarget,  ///< A carried time link re-anchored at its absolute target.
};

/// Number of @ref ResponsePolicy values (array sizing).
inline constexpr std::size_t kResponsePolicies = 18;

/// @brief Table name of an event (e.g. "slip").
const char* paramEventName(ParamEvent e);

/// @brief Table name of a response policy (e.g. "erase_and_rebirth").
const char* responsePolicyName(ResponsePolicy p);

/// @brief Bit of @p p in a policy mask (@ref ParamSpec::on).
inline constexpr std::uint32_t responsePolicyBit(ResponsePolicy p) {
    return std::uint32_t{1} << static_cast<int>(p);
}

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
    /// The gap horizon of a carried instance of the kind (s): the time unseen past which
    /// it is no longer folded and is pruned (card PRM phase 3b; the test is
    /// @c carryPastHorizon, the prune @c carryGapPrune, both in
    /// @c filter/NetworkResponses.hpp).  The double the kind's gap sites read, set from
    /// the same knob whether or not the kind is carried on this run (as @ref q is), so a
    /// site reads it in place of the knob: network.phase_leveling_max_gap_s for the
    /// ambiguity, the wet delay and the receiver phase bias;
    /// network.phase_colour_max_gap_s for the phase colour; for the slant ionosphere the
    /// horizon of the carry in force (network.carry_iono_in_block_max_gap_s under the
    /// block carry, network.iono_carry_max_gap_s under the diagonal carry; the loader
    /// refuses both at once); network.carry_rx_clock_max_gap_s for the receiver clock;
    /// the constant @c kRxCbMaxGapS for the receiver code bias.  NaN for a kind with no
    /// gap horizon declared here: the slant ionosphere with neither carry armed, and the
    /// border kinds, whose gap response is READMIT's park (its expiry, a GapPrune of the
    /// parked entry, runs on network.readmit_seed_horizon_s against the park time).
    double       gap_horizon_s{std::numeric_limits<double>::quiet_NaN()};
    ParamRoute   route{ParamRoute::NotCarried};
    std::string  source;            ///< The knob(s) behind q, the floor and the route.
    /// The response policies armed for this kind on this run, per @ref ParamEvent (a mask of
    /// @ref responsePolicyBit): the union of the policies of this kind's response sites whose
    /// arming condition holds on the knob snapshot, and Stransform where an armed datum
    /// mechanism moves a carried value of the kind (card PRM phase 3a,
    /// @c filter/NetworkResponses.hpp; with @c network.carry_rebase_complete its owed value
    /// sites count too, card PRM phase 3b).  Zero for an event the kind does not respond to.
    std::array<std::uint32_t, kParamEvents> on{};
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
     * the Gauss-Markov time constant, the variance floor, the birth floor, the gap
     * horizon (@ref ParamSpec::gap_horizon_s), the route and the knob sources.  Trace
     * class: written at run start
     * and in the run-end tail, never read by the solve.
     *
     * @return The lines, without trailing newlines.
     */
    std::vector<std::string> modelTable() const;

    /**
     * @brief The response policies armed per kind, one line per kind: the kind, then each
     *        event it responds to with its policies (card PRM phase 3a, @ref ParamSpec::on).
     *
     * Trace class: written at run start beside @ref modelTable.
     *
     * @return The lines, without trailing newlines.
     */
    std::vector<std::string> responseTable() const;

    /**
     * @brief The same table as one JSON object (the run manifest's @c param_model
     *        record): @c {"record":"param_model","kinds":[{...}, ...]}, each kind with its
     *        armed response policies per event under @c "on".
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
