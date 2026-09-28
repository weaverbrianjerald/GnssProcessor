/**
 * @file NetworkSbasis.hpp
 * @brief Explicit minimum-constraint S-basis for the PPP-RTK network producer
 *        (W4-S2b, W5): the datum declaration, the shared-block clock datum
 *        builder, the satellite code-bias datum rows, a null-space rank gate and
 *        the per-epoch S-basis completion (SBC,
 *        @ref NetworkSbasis::completeSbasisDimension).
 *
 * @ref NetworkEstimator owns the per-epoch shared satellite-parameter column
 * layout and @ref NetworkNormalEquations is the pure Schur kernel; this class
 * holds the S-system datum: which gauges are fixed, to what, and how the choice
 * is declared so that the emitted product is self-describing and a consumer can
 * check compatibility (Teunissen–Khodabandeh covariance).
 *
 * @par Datum (V1: estimable satellite clock and phase bias, coordinates and orbits
 *      fixed, GPS+GAL, per-band uncombined)
 * The undifferenced uncombined network normal matrix is rank-deficient by:
 *  - **Clock gauge, one per constellation.**  Adding a constant to every
 *    satellite clock of a constellation and to the receivers' clock/ISB of that
 *    constellation cancels on every row.  Every constellation, GPS included, is
 *    gauged the same way, by one of three configurable realizations
 *    (@ref SbasisDeclaration::IsbDatum):
 *      * a shared hard pin on one satellite's clock column per constellation
 *        (@ref NetworkEstimator::Datum), the legacy anchor-based "pivot" datum;
 *      * a per-constellation sum-to-zero over that constellation's satellite
 *        clocks (@c network.clock_datum_sum_to_zero), folded as a tight all-ones
 *        gauge row like the code-bias sum-to-zero rows of
 *        @ref NetworkSbasis::buildCodeBiasDatumRows; anchor-free, so there is no
 *        pivot and no handover seam; or
 *      * a soft anchor of each satellite clock to its own a-priori clock
 *        (@c network.clock_datum_broadcast_anchored), one single-entry row per
 *        satellite.
 *    In the two constraint-realized modes the shared @ref Datum is empty: the
 *    folded rows pin the clock gauge before the rank gate, so the reduced shared
 *    system is full-rank, @ref Plan::declared_dimension is 0, and @ref Plan
 *    carries the constraint rows instead.
 *    No system is anchored to a receiver clock; each station's receiver
 *    clock/ISB stays a free local state that absorbs the constellation's lump.
 *  - **Phase-bias↔ambiguity gauge, one per (satellite, signal).**  Each
 *    satellite's phase bias @f$b^s_\sigma@f$ is co-linear with that satellite's
 *    ambiguity column @f$\{N^s_{r,\sigma}\}_r@f$ (null vector
 *    @f$\delta b^s_\sigma=-\delta N^s_{r,\sigma}@f$ for any per-satellite shift).
 *    In the default convention it is realized as local tight-prior pins on the
 *    pivot receiver's ambiguities, so the estimable phase bias carries the
 *    pivot-receiver ambiguity lump (Odijk Eq. 4); the driver folds these pins into
 *    the pivot station's design rows, where they enter the Schur reduction.
 *    @ref SbasisDeclaration::AmbDatum lists the other conventions.  None of them
 *    appears in the shared @ref NetworkEstimator::Datum produced here.
 *
 * In the pin datum, therefore, the shared datum this builder emits is exactly the
 * per-constellation clock pins: once the local ambiguity pins are eliminated, the
 * reduced shared normals have exactly @c #constellations residual deficiencies,
 * which the @ref assertNullSpaceRank gate verifies before the solve.  In the
 * constraint-realized datums the folded rows pin the clock gauges instead, so the
 * reduced shared normals are full-rank and the gate's declared dimension is 0.
 *
 * Not a state owner (StateManager stays the single source of truth for state
 * layout): a per-epoch builder over the live registered column set.
 */

#pragma once

#include <Eigen/Dense>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config/GnssConfigTypes.hpp"   // NetworkSatGfDatum, the GF datum mode (RF-0c)
#include "filter/NetworkEstimator.hpp"
#include "products/BroadcastEphemerisTypes.hpp"   // the group-delay latch's records (card OWD)

namespace GnssProcessor {

/**
 * @brief The wind-up / yaw-attitude model this binary implements.
 *
 * A compile-time constant rather than a config knob: it declares what the code
 * does, and a declaration that a QC or trace toggle could change would drift away
 * from the thing it declares.  It is written into every emitted product's S record
 * (field 3, legended @c windup_id) and is the value a consumer pins its own
 * expectation to, so a product built under a different wind-up convention fails
 * @c NetworkProductReader::checkContract instead of being consumed silently.
 *
 * Revisions:
 *   - @c "nominal"      yaw-steering triads, effective dipole built with a plus
 *                       sign on both quadrature terms (the inverted-sign defect);
 *                       every product emitted before 2026-08-21.
 *   - @c "nominal-wu93" the corrected Wu et al. (1993) construction (minus on
 *                       both), with all three satellite-frame branches sharing
 *                       the nadir handedness.
 *
 * @see docs/SPEC_phase_windup_sign.md
 */
inline constexpr const char* kWindUpYawModelId = "nominal-wu93";

/**
 * @brief Serializable record of the network product's S-basis convention.
 *
 * Travels with the emitted product so a consumer can confirm it adopts a
 * compatible S-basis (same pivots, sign, wind-up model) before claiming integer
 * recovery.  Holds the *convention*, not the per-epoch column indices.
 */
struct SbasisDeclaration {
    /// Pivot receiver (= NetworkConfig::clock_datum_station).  Declared on the S line
    /// and compared by the consumer's contract check.  The clock gauge pins
    /// satellite clocks, not this receiver's clock (see the file header).
    std::string reference_receiver_id;
    /// Constellation char ("G", "E", ...) -> satellite pinned for that
    /// constellation's clock gauge.  An empty entry selects automatically (first
    /// registered).
    std::map<std::string, std::string> clock_pivot_sat;
    /// Ambiguity datum convention: how the per-(satellite, signal) phase-bias↔
    /// ambiguity gauge is fixed.  Recorded so the consumer knows which receiver
    /// frame the emitted satellite phase bias references.
    ///  - @c PivotReceiverZeroAmbiguity (default): one elected receiver's ambiguity
    ///    is pinned to zero per (satellite, signal), so the estimable phase bias
    ///    carries that receiver's integer lump (legacy single pivot and prioritized
    ///    anchor).
    ///  - @c SumToZeroPerReceiver (@c network.phase_datum_ensemble, Fix A):
    ///    anchor-free; the gauge is fixed by @f$\sum_r z^s_{r,\sigma}=0@f$ over the
    ///    tracking receivers, so the phase bias references the receiver ensemble
    ///    (no pivot, no handover, and a consistent between-satellite reference).
    ///    The ambiguities are not directly integer (Odijk CC-S, needs Eq-34
    ///    recovery).
    ///  - @c SingleReceiverChain (@c network.phase_datum_eq34, card 3): the
    ///    Eq-34 / de Jonge datum.  Per-(receiver, system, signal) RPB split states
    ///    let the gauge be fixed by one datum-receiver RPB pin plus a spanning tree
    ///    of ambiguity pins over the bipartite (receiver × satellite) graph, per
    ///    (constellation, signal) connected component.  The emitted
    ///    between-satellite reference is then integer-consistent across all
    ///    satellites (each chain hop contributes only an integer, because the hop
    ///    receiver's fractional bias is a separate estimable state) while staying
    ///    full-rank at global scale.  In the full-visibility regional case it
    ///    reduces to the Odijk Eq-34 star (CC-R).  Its wire value, 2, uses the
    ///    existing v2 S line (no format bump); a consumer that expects another
    ///    convention gets "amb_datum differs" from
    ///    @c NetworkProductReader::checkContract, as intended.
    enum class AmbDatum { PivotReceiverZeroAmbiguity, SumToZeroPerReceiver,
                          SingleReceiverChain }
        amb_datum{ AmbDatum::PivotReceiverZeroAmbiguity };
    /// Inter-system / clock-gauge datum.  Every constellation, GPS included, has
    /// its clock gauge fixed on that constellation's own satellite clocks; no
    /// system is anchored to a receiver clock, and each station's receiver
    /// clock/ISB stays a free local state.  Three realizations (the enumerator
    /// names the realized basis, in the style of the sibling AmbDatum and
    /// CodeBiasDatum):
    ///  - @c PerConstellationSatClockPin (legacy default): a hard pin on one
    ///    satellite's clock per constellation (the "pivot").  Anchor-based: a pivot
    ///    that sets or is deselected forces a constellation-wide datum
    ///    re-reference (handover), which the consumer pays for with an ambiguity
    ///    reset, and an epoch missing every candidate pivot has no datum at all.
    ///  - @c SumToZeroPerConstellation (@c network.clock_datum_sum_to_zero): the
    ///    per-constellation clock common mode is fixed by a sum-to-zero over that
    ///    constellation's satellite clocks (Σ δtˢ = 0), realized like the code-bias
    ///    per-(constellation, signal) gauge: a tight all-ones constraint row folded
    ///    into the shared normals before the rank gate.  Anchor-free: there is no
    ///    pivot satellite and so no handover seam, and the datum is defined for any
    ///    epoch with at least one clock satellite per constellation.  The absolute
    ///    per-constellation level lands on the receiver clocks/ISB in either datum
    ///    (the two differ by a per-constellation constant that a consumer absorbs
    ///    into its receiver clock), so this is a pure S-transform of the pin
    ///    datum's estimable (between-satellite) solution.
    ///    Caveat: the reference is the raw satellite set present each epoch, so
    ///    when satellites rise or set the survivors shift by ∓ΔC0/N to re-zero the
    ///    sum, and the absolute clock-product frame moves with membership.  Position
    ///    is unaffected for a consumer that estimates its receiver clock; for the
    ///    absolute frame, clock holding across gaps and the disc semantics it is a
    ///    contract issue.
    ///  - @c BroadcastAnchoredPerSatellite (@c network.clock_datum_broadcast_anchored):
    ///    each satellite's clock correction Δδtˢ is anchored to 0, that is to the
    ///    satellite's own a-priori clock, by a soft pseudo-observation at
    ///    @c broadcast_anchor_sigma_m, folded into the shared normals before the
    ///    rank gate; the anchors are the gauge (declared dimension 0).  The a-priori
    ///    is the broadcast clock under @c network.apriori_source = brdc_rts, and the
    ///    precise (.CLK) clock under sp3, where the loader requires an explicit
    ///    @c network.apriori_clock_anchor_sigma_m.  No pivot and no handover seam,
    ///    and because each satellite is anchored independently there is no
    ///    redistribution when membership changes: the common mode stays bound to
    ///    the external a-priori reference (under brdc_rts, the broadcast clock the
    ///    consumer already reconstructs against).  Strictly an over-constraint
    ///    (N soft pins against 1 gauge degree of freedom); at σ = 3 m against tight
    ///    phase data the pull on the estimable between-satellite differences is
    ///    sub-cm, and the absolute common mode keeps about σ/√N of freedom, which
    ///    the consumer absorbs into its receiver clock.
    enum class IsbDatum { PerConstellationSatClockPin, SumToZeroPerConstellation,
                          BroadcastAnchoredPerSatellite }
        isb_datum{ IsbDatum::PerConstellationSatClockPin };
    /// Used by BroadcastAnchoredPerSatellite only: 1σ (m) of the per-satellite clock
    /// anchor pseudo-observation that realizes the clock gauge.  The driver
    /// resolves it from @c network.apriori_clock_anchor_sigma_m, or the brdc_rts
    /// auto default of 3.0 m.  Unused in the other datums.
    double broadcast_anchor_sigma_m{ 3.0 };
    /// SingleReceiverChain diagnostic (card 3): the component-0 datum receiver r1,
    /// whose RPB was pinned.  Serialized as the optional S-line token
    /// @c pdr=<station> when non-empty.  Diagnostic only (Odijk §6: the user never
    /// needs the network pivot) and not part of the compatibility check in
    /// @c NetworkProductReader::checkContract.  Empty means unset: nothing is
    /// written, so replay and older readers are unaffected, as with @c gid=.
    std::string eq34_datum_receiver;
    /// Satellite bias sign convention (matches kSsr*BiasSign at MeasurementModel).
    double bias_sign{ +1.0 };
    /// Declared wind-up / yaw-attitude model id; producer and consumer must agree
    /// ("nominal" = yaw steering from sun/velocity geometry, eclipse AR ungated).
    ///
    /// The suffix is a wind-up model revision, not a yaw-attitude change.
    /// "nominal-wu93" marks the corrected Wu et al. (1993) effective-dipole
    /// construction (@c GnssCorrections::computePhaseWindUp, minus on both
    /// quadrature terms).  Products that carry the bare "nominal" predate that fix
    /// and are in a different phase frame: their satellite clocks and phase biases
    /// absorbed +2*phi_wu.  The changed token is what makes a pairing of a
    /// corrected producer with an uncorrected consumer detectable; see
    /// docs/SPEC_phase_windup_sign.md section 8 item 2.
    std::string windup_yaw_model_id{ kWindUpYawModelId };
    /// True when the network emits its own satellite code bias (OSB): W6,
    /// network.estimate_sat_code_bias.  V1 emits satellite clock and phase bias only.
    bool emit_code_bias{ false };
    /// Satellite code-bias (OSB) datum convention (W6).  The emitted
    /// per-(satellite, signal) code biases are referenced so that each satellite's
    /// ionosphere-free combination is zero (the IF lump lives in the estimable
    /// clock) and they sum to zero over each (constellation, signal) channel (the
    /// absolute level that the GIM common-mode-free iono weighting leaves free).
    /// Recorded so a consumer can check compatibility.
    enum class CodeBiasDatum { SumToZeroPerConstellation }
        code_bias_datum{ CodeBiasDatum::SumToZeroPerConstellation };
};

/**
 * @brief Builds the shared-block minimum-constraint datum and validates the
 *        reduced-system rank against the declared S-basis dimension.
 */
class NetworkSbasis {
public:
    /// One shared solution epoch's datum plan.
    struct Plan {
        NetworkEstimator::Datum shared_datum;            ///< Shared hard pins: one satellite-clock pin per constellation in pin mode; empty in the sum-to-zero and broadcast-anchored modes.
        int declared_dimension{ 0 };                     ///< Expected residual shared null-space dimension: #constellations in pin mode (= shared_datum.size()); 0 in the sum-to-zero and broadcast-anchored modes (clock gauges folded before the gate).
        std::map<std::string, std::string> clock_pivot;  ///< Constellation char -> chosen pivot satellite (for the declaration and handover detection).  Empty in the sum-to-zero and broadcast-anchored modes (no pivot).
        /// Constraint-realized clock datums only: gauge rows that the driver folds
        /// onto the shared border via @ref NetworkEstimator::addSharedConstraintRows
        /// before the rank gate, like the code-bias per-(constellation, signal)
        /// sum-to-zero.  Sum-to-zero mode: one all-ones row per constellation over
        /// that constellation's registered clock columns (Σ δtˢ = 0), at
        /// @ref kClockGaugeWeight.  Broadcast-anchored mode: one single-entry row
        /// per registered clock satellite (Δδtˢ → 0, i.e. its own a-priori clock),
        /// at 1/σ² from @ref SbasisDeclaration::broadcast_anchor_sigma_m; the soft
        /// anchors are the datum.  Empty (0 rows) in pin mode, where the driver
        /// folds nothing.
        Eigen::MatrixXd clock_constraint_H;              ///< Gauge-row Jacobian (see above); empty in pin mode.
        Eigen::VectorXd clock_constraint_z;              ///< Constraint targets (zeros); empty in pin mode.
        double          clock_constraint_weight{ 0.0 };  ///< Row information weight (@ref kClockGaugeWeight for sum-to-zero; 1/σ² for broadcast-anchored); 0 in pin mode.
    };

    /// Tight gauge-row information weight (= 1/σ²) for the sum-to-zero clock
    /// constraint.  The row pins a pure gauge direction (zero data information), so
    /// any positive weight fixes it exactly without biasing the estimable
    /// (between-satellite) clock structure.  The value sits far above the rank-gate
    /// cutoff (1e-9·λmax at the default tolerance) and far below λmax, so it neither
    /// trips nor inflates the spectrum the gate inspects.  Equal to the code-bias
    /// gauge weight (@ref kCodeBiasGaugeWeight) by design.
    static constexpr double kClockGaugeWeight = 1.0e3;

    /// Tight gauge-row information weight (= 1/σ²) for the W6 satellite code-bias
    /// datum rows (IF pin and per-(constellation, signal) sum-to-zero).  The rows
    /// pin pure gauge directions (zero data information), so any positive weight
    /// fixes them exactly without biasing the estimable between-satellite DCB
    /// structure.  The value sits far above the rank-gate cutoff (1e-9·λmax at the
    /// default tolerance) and far below λmax, so it neither trips nor inflates the
    /// spectrum the gate inspects.  Equal to @ref kClockGaugeWeight by design; the
    /// two are documented separately because they gauge different parameter
    /// families.
    static constexpr double kCodeBiasGaugeWeight = 1.0e3;

    /// Tight gauge-row information weight (= 1/σ²) for the per-epoch S-basis
    /// completion rows (@ref completeSbasisDimension, SBC).  Equal by design to its
    /// siblings @ref kClockGaugeWeight and @ref kCodeBiasGaugeWeight: a completion
    /// row pins a measured (near-)null direction, so like the other two it fixes a
    /// gauge without biasing the estimable structure.  The completion emits no rows
    /// when this weight would not clear the armed rank-gate cutoff by at least two
    /// decades (@ref GaugeCompletion::weight_marginal, the K2 anti-fragility rule),
    /// so a folded row never itself sits at the cutoff it exists to lift the
    /// direction above.
    static constexpr double kSbasisCompleteWeight = 1.0e3;

    /// Attribution and identity span threshold of the S-basis completion: a column
    /// participates in a constrained direction when the magnitude of its
    /// eigenvector component is at least this.  Deliberately looser than the
    /// drop-and-heal's 0.3: the heal selects columns to prior, where a false
    /// positive costs information on a real parameter, while this only names
    /// participants for events and for the engage/release identity, where a false
    /// positive costs a log token.  On the FT63 GPS+Galileo network the μ-weighted
    /// E5-family components measure 0.33–0.36 and the E6 member about 0.30, which a
    /// 0.3 threshold would flicker on.
    static constexpr double kSbasisAttrSpan = 0.25;

    /// What the latched broadcast group delay says about one satellite's
    /// geometry-free pin pair (card GAL, R3).  The four cases are the four counters
    /// of the RF-0c run-end line; deciding them in one place keeps the pair pin and
    /// the time-linked extra-channel anchor from disagreeing about a satellite's
    /// target.
    enum class SatGfTargetKind : std::uint8_t {
        BroadcastHit,    ///< The pair is one the group delay describes and the value is non-zero.
        BroadcastZero,   ///< The pair is described and the navigation message broadcasts exactly 0.
        Missing,         ///< The pair is described but the satellite has no navigation record.
        NoGroupDelay,    ///< The pair is not one the latched group delay describes: target 0.
    };

    /// The absolute geometry-free target of one satellite's pin pair (card GAL, R3).
    struct SatGfTarget {
        double          target_m{0.0};                          ///< Metres; 0 unless @c kind is @c BroadcastHit.
        SatGfTargetKind kind{SatGfTargetKind::NoGroupDelay};    ///< Which of the four cases applied.
    };

    /// Per-constellation geometry-free datum counters (card GAL, R3 iii).  Each
    /// field mirrors the @ref CodeBiasDatumRows counter of the same meaning.
    struct SatGfConstCounts {
        int rows{0};           ///< Pair rows folded (@ref CodeBiasDatumRows::n_sat_gf_rows).
        int tgd_hits{0};       ///< Pair rows at a non-zero broadcast group delay.
        int tgd_zero{0};       ///< Pair rows whose satellite broadcasts exactly 0.
        int tgd_missing{0};    ///< Pair rows whose satellite had no navigation record.
        int pair_no_gd{0};     ///< Pair rows whose pair the latched group delay does not describe.
        int extra_rows{0};     ///< Extra-channel rows (@ref CodeBiasDatumRows::n_sat_gf_extra_rows).
        int isc_zero{0};       ///< Extra rows folded under broadcast_tgd (@ref CodeBiasDatumRows::n_sat_gf_isc_zero).
        int sameband_free{0};  ///< Same-band columns left estimable.
        int carried{0};        ///< Rows (pair and extra) carried at the previous solved value.
        int anchored{0};       ///< Rows (pair and extra) anchored at the absolute target.
    };

    /// Result of @ref buildCodeBiasDatumRows: stacked W6 gauge rows for the driver
    /// to fold via NetworkEstimator::addSharedConstraintRows at
    /// @ref kCodeBiasGaugeWeight (and mirror into the RAIM-replay cap channel).
    struct CodeBiasDatumRows {
        Eigen::MatrixXd H;      ///< k × numShared gauge Jacobian (0 rows ⇒ fold nothing).
        Eigen::VectorXd z;      ///< Length-k RHS: 0 except on time-linked rows (card 2, RF-0e e2: carried previous-epoch values) and on RF-0c geometry-free rows under `broadcast_tgd` (the latched group delay).
        int n_if_pin_sats{0};   ///< Satellites attempted for the IF pin (census order size).
        int n_s2z_channels{0};  ///< (constellation, signal) sum-to-zero channels registered, suppressed ones included.
        int n_gf_pin_sats{0};   ///< Satellites that received a geometry-free pin row (slant posture; 0 when off).
        int n_gf_only_pair{0};  ///< GF-pinned satellites with no third band, whose code bias is therefore fully datum-fixed with no estimable content left (Odijk §5.1).  Counted because "emitted nothing" and "estimated zero" look identical downstream.
        int n_family_gf_rows{0};   ///< (Satellite, partitioned secondary family) pairs that received an extra geometry-free row (FAMILY-GF).
        int n_family_gf_only_pair{0}; ///< Of those, the ones whose family has no third band, so the ISC is the family's entire estimable content (FAMILY-GF).  Scanned over the family's signal subset, not the satellite's whole signal list: a Q-pinned satellite with X channels registered would otherwise always appear to have a third band.
        int n_sat_gf_rows{0};      ///< Satellites that received the hard per-satellite geometry-free datum row (@c network.sat_gf_datum, RF-0c).  Under `w6_iono_constraint: "none"` this row is the datum for the between-satellite split, so 0 here with the knob armed means the run had no datum at all (standing invariant #4).
        int n_sat_gf_tgd_hits{0};  ///< Of those rows, the ones pinned at a non-zero `c*T_GD` (c*BGD for Galileo) from the navigation message (RF-0c, `broadcast_tgd`).
        int n_sat_gf_tgd_zero{0};  ///< Rows whose satellite has a navigation record that broadcasts `T_GD = 0` exactly: physically pinned, numerically identical to `zero` (RF-0c, `broadcast_tgd`).  Kept apart from the fallbacks so that a broadcast zero is never read as a missing value.
        int n_sat_gf_tgd_missing{0}; ///< Rows whose satellite had no navigation record and fell back to a zero target (RF-0c, `broadcast_tgd`).
        int n_sat_gf_pair_not_l1l2{0}; ///< Rows whose ionosphere-free pin pair is not one the latched broadcast group delay describes (@ref broadcastGroupDelayDescribesPair: (L1, L2) for every constellation but Galileo, (E1, E5a) for Galileo), so no broadcast value applies and the row falls back to a zero target rather than assert a delay for a pair the navigation message never referenced (RF-0c, `broadcast_tgd`).  The name predates the Galileo rule (card GAL) and is kept for the log and trace readers that key on it.
        int n_sat_gf_extra_rows{0};   ///< Extra per-signal geometry-free rows, one per registered code column outside the ionosphere-free pair whose band differs from the reference, unless RF-0f or B5D frees or holds it (RF-0d).  They make a sparse channel's level (GPS C2X) a datum rather than a quantity determined by the few stations that track it.
        int n_sat_gf_isc_zero{0};     ///< Extra rows folded under `broadcast_tgd`, carried ones included (RF-0d).  RINEX navigation carries no ISC for GPS (only T_GD), so an extra row anchored at the broadcast target asserts an inter-signal correction of zero.  Counted because that assertion is invisible on the wire: it is the part of a "broadcast" datum that the navigation message does not state.
        int n_sat_gf_sameband_free{0};///< Registered columns left unpinned on purpose (RF-0d): a channel in the reference's band has no geometry-free combination against it (the divisor is zero), and its difference is a pure ISC, observable without any ionosphere at any coverage.  Pinning it would discard estimable content.
        int n_sat_gf_extra_nofreq{0}; ///< Extra columns skipped because no carrier frequency was available from any present station's model this epoch (RF-0d).
        int n_s2z_suppressed{0};      ///< Per-(constellation, signal) sum-to-zero rows suppressed because the geometry-free datum already pins every satellite column of that channel (RF-0d).  Under `broadcast_tgd` the two are inconsistent, not merely redundant (the pinned values sum to mu_j*c*Sum_s T_GD^s, not to zero), and both fold at the same weight; see the suppression block in NetworkSbasis.cpp.
        int n_if_level_rows{0};       ///< Absolute ionosphere-free level rows folded, one per constellation with at least one satellite carrying a valid IF pin pair (RF-0e e1).  0 with the knob armed means no satellite presented a dual-frequency pair, so the set level ran unanchored (standing invariant #4).
        int n_if_level_sats{0};       ///< Satellites summed into those rows (RF-0e e1).
        int n_sat_gf_carried{0};      ///< Geometry-free rows (pair and extra) whose RHS came from the satellite's previous kept-epoch solved values, i.e. change pins (RF-0e e2; B5D extra-band carries included).
        int n_isc_observed{0};        ///< Band-2 same-band companion columns left unpinned because some present station tracks both that companion and the pin's second signal on that satellite (RF-0f): their difference is a pure inter-signal correction, which the data determine.
        int n_isc_pinned{0};          ///< Companion columns still pinned (inter-signal correction asserted zero) because no station bridged the two signals this epoch (RF-0f).  Their biases are not emitted; this count reports how much of a run's C2X content is assertion rather than estimate.
        int n_sat_gf_anchored{0};     ///< Geometry-free rows that fell back to the absolute target because the satellite had no veteran value (RF-0e e2): the first-sighting anchor, and the only place the broadcast frame is re-asserted.
        int n_extra_band_free{0};     ///< Extra-band columns (band neither of the pin pair's) left unpinned because some present station carries code on both of the pair's bands and on that signal for that satellite (B5D).  With the pinned pair fixing the slant ionosphere and the clock, the data determine d_k up to the family constant, whose datum is the channel's sum-to-zero row.
        int n_extra_band_carried{0};  ///< Extra-band columns the data did not determine this epoch but that had a previous solved value on both the reference and themselves, so the row carries their own previous geometry-free combination instead of the absolute broadcast target (B5D).
        int n_extra_band_held{0};     ///< Extra-band columns the data did not determine at a first sighting (B5D).  With nothing to carry, a single level row in the channel's own frame holds the column (never the absolute broadcast target, which would pull a sum-to-zero family into the broadcast frame) and the bias is withheld from the wire.  This count is the data that shows whether the case occurs at all (standing invariant #3).
        int n_s2z_reseed_rows{0};     ///< Per-(constellation, signal) sum-to-zero rows whose veteran link re-seeded this epoch: no veteran was present, so the channel's level re-references (B5D, invariant #7).  The affected satellites are marked in @c this_epoch_cb_reseed and disc-bumped at the emit site; this count covers the decoupling edge.
        int n_s2z_reseed_sats{0};     ///< Satellites marked by those re-seeds, the marks the consumer reacts to (B5D, invariant #7).  A satellite with no previous kept epoch is not marked unless the run resumed.
        /// The `"sat|signal"` keys of the extra-band columns held this epoch (see
        /// @ref n_extra_band_held; B5D).  Returned rather than re-derived by the caller
        /// because the hold verdict is a three-way branch over the determination roster
        /// and the carry map, and a second derivation could disagree with this one.
        /// Empty unless @c network.sat_gf_extra_band_free is armed, so the emit prep's
        /// withholding loop is not entered otherwise.
        std::set<std::string> extra_band_held;
        /// The geometry-free datum counters above, split per constellation letter
        /// ("G", "E", ...) (card GAL, R3 iii).  Filled on the same branches as the
        /// global counters, so the per-constellation sums equal them exactly.  Empty
        /// when @c network.sat_gf_datum is off.
        std::map<std::string, SatGfConstCounts> sat_gf_by_const;
    };

    /// A receiver channel family tag (FAMILY-GF, docs/SPEC_family_gf_datum.md §3.1).
    /// It is not the raw RINEX attribute character: the Galileo primary family is
    /// C1C/C5Q/C7Q, whose attributes are 'C', 'Q', 'Q', so keying on the character
    /// would split one family in two (and on GPS would split C2W from C2L on a
    /// posture that works as is).  The organizing physical fact is that an 'X'
    /// channel is the combined I+Q correlator output and carries its own hardware
    /// bias, while the discrete components share a chain.
    enum class ChannelFamily : std::uint8_t {
        Primary,    ///< Discrete-component tracking (Galileo A/B/C/I/Q/Z, GPS C/W/P/Y/S/L/I/Q/M, BDS I/Q/D/P/A/Z).
        Secondary,  ///< Combined I+Q tracking ('X').
    };

    /**
     * @brief Whether the latched broadcast group delay describes this pair (card GAL,
     *        R3).
     *
     * The navigation message carries one group delay per satellite that the setup
     * latches (@c KeplerianEphemeris::tgd0): T_GD for GPS and QZSS, which the
     * IS-GPS-200 single-frequency corrections reference to the (L1, L2) pair, and
     * BGD(E1, E5a) for Galileo (ORBIT-6 field 3 of the RINEX record, carried by the
     * I/NAV and the F/NAV message alike).  The Galileo ICD corrections
     * d_E1 = c*BGD and d_E5a = c*(f_E1/f_E5a)^2*BGD have an ionosphere-free
     * combination of exactly zero and a geometry-free combination of exactly c*BGD,
     * the same identity T_GD satisfies on (L1, L2).  The rule is therefore:
     *  - Galileo ('E'): the pair (band 1, band 5) and nothing else;
     *  - every other constellation: the pair (band 1, band 2), the RF-0c rule
     *    unchanged (for BeiDou that band-digit test does not name the pair its
     *    TGD1 describes; see card GAL's owed list).
     * A pair outside the rule gets a zero target and its own counter.
     *
     * @param sat   RINEX satellite id ("G05", "E11").
     * @param sig1  The pin pair's first (band-1) code signal.
     * @param sig2  The pin pair's second code signal.
     * @return @c true iff the latched group delay describes @p sig1 / @p sig2.
     */
    static bool broadcastGroupDelayDescribesPair(const std::string& sat,
                                                 const std::string& sig1,
                                                 const std::string& sig2);

    /**
     * @brief The absolute geometry-free target of one satellite (card GAL, R3).
     *
     * The single rule that the pair pin and the time-linked extra-channel anchor both
     * read, so that an extra channel of a satellite is never anchored at a value its
     * pair was not pinned at.  (Anchoring a Galileo C5X column at c*BGD while its
     * (C1C, C5Q) pair sits at zero would assert C5X minus C5Q equal to about
     * 0.79 c*BGD, which is metres.)  The order of the tests, and therefore of the
     * counters, is the RF-0c order: the pair rule first, then the navigation record,
     * then a broadcast value of exactly zero.
     *
     * @param sat              RINEX satellite id.
     * @param sig1             The pin pair's first code signal.
     * @param sig2             The pin pair's second code signal.
     * @param sat_gf_target_m  The run-latched c*T_GD / c*BGD per satellite, metres.
     * @return The target and which of the four cases applied.
     */
    static SatGfTarget satGfBroadcastTarget(const std::string& sat,
                                            const std::string& sig1,
                                            const std::string& sig2,
                                            const std::map<std::string, double>& sat_gf_target_m);

    /// Two group delays closer than this are the same broadcast value (card OWD, O3).
    /// 1 ps sits two orders below the smallest broadcast step (BeiDou's 0.1 ns) and
    /// eight above the last-digit differences with which a merged RINEX file prints
    /// one value.  A latched value that is the same broadcast value as the file's
    /// first record keeps that record's exact bits, so an unchanged datum is
    /// unchanged to the bit.
    static constexpr double kSameBroadcastGroupDelayS = 1e-12;

    /// Which records a satellite's broadcast group delay was latched from (card OWD, O3).
    enum class GroupDelayLatchReason {
        Window,                      ///< its healthy records whose clock epoch lies in the window
        InForceAtStart,              ///< none in the window: the healthy record in force at its start
        AllUnhealthy,                ///< no healthy record at all: the window rule over all records
        AllUnhealthyInForceAtStart   ///< no healthy record at all, and none in the window
    };

    /// One satellite's run-latched broadcast group delay (card OWD, O3).
    struct GroupDelayLatchEntry {
        double value_s{0.0};         ///< latched @c tgd0 (GPS T_GD, Galileo BGD(E1,E5a), BeiDou TGD1), s
        double first_s{0.0};         ///< the file's first record (the RF-0c latch value), s
        double spread_file_s{0.0};   ///< max |tgd0 − first| over the file, s (the spread report)
        GroupDelayLatchReason reason{GroupDelayLatchReason::Window};   ///< which records
        int n_records{0};            ///< the satellite's records in the file
        int n_used{0};               ///< the records the median was taken over
    };

    /// The latch of every satellite, with the records set aside per reason (card OWD, O3).
    struct GroupDelayLatch {
        std::map<std::string, GroupDelayLatchEntry> sats;   ///< per satellite id
        long long dropped_unhealthy{0};   ///< unhealthy records set aside (a healthy one existed)
        long long dropped_inav{0};        ///< Galileo non-F/NAV records set aside (F/NAV existed)
        long long outside_window{0};      ///< records outside the window, after those two filters
        int n_window{0};                  ///< healthy satellites on the window rule
        int n_in_force{0};                ///< healthy satellites on the record in force at the start
        int n_all_unhealthy{0};           ///< satellites with no healthy record at all
    };

    /**
     * @brief Latch each satellite's broadcast group delay for the run (card OWD, O3).
     *
     * The file's first navigation record (the RF-0c latch) holds the value in force at
     * the file start, which is out of date for a run that starts after a later
     * record.  This latch takes instead the low median (always a broadcast value) of
     * the group delays of the satellite's healthy records (health 0) whose clock
     * reference epoch lies in the half-open processing window
     * [@p t0_gps_s, @p t1_gps_s), with Galileo's F/NAV (E1, E5a) records preferred
     * when present (data-source bits 1 and 8).  With no such record it takes the
     * healthy record in force at the window start (the latest at or before it, else
     * the earliest after it).  A satellite with no healthy record at
     * all is latched by the same rule over all its records rather than dropped: a
     * dropped satellite falls back to a zero target, which moves its datum by c*T_GD
     * (5.863 m for the GPS pivot G01 on DOY 129).  The health policy itself is a
     * sequenced item of card OWD.  Zeros are real values.  A latched value that is
     * the same broadcast value as the file's first record (within
     * @ref kSameBroadcastGroupDelayS) keeps that record's exact bits.  One value per
     * run, so the datum cannot churn within a run.  GLONASS records are skipped.
     *
     * @param nav      The parsed navigation records, file order.
     * @param t0_gps_s Window start, continuous GPS seconds.
     * @param t1_gps_s Window end (exclusive), continuous GPS seconds.
     * @return Every satellite's latch and the per-reason counts.
     */
    static GroupDelayLatch latchBroadcastGroupDelays(const std::vector<BroadcastEphemeris>& nav,
                                                     double t0_gps_s, double t1_gps_s);

    /**
     * @brief Map a (constellation, code signal) to its channel family (FAMILY-GF).
     *
     * @param constellation RINEX constellation key ("E", "G", "C", …).
     * @param signal        raw 3-char RINEX code observable ("C1C", "C5X", …).
     * @return @c Secondary iff the signal is a combined-tracking ('X') channel of a
     *         constellation on the allow-list (E, G, C); @c Primary otherwise.  An
     *         unrecognised constellation always returns @c Primary, so a
     *         constellation outside the allow-list can never partition and can never
     *         start emitting rows (§7's allow-list, enforced here rather than at the
     *         call site).
     */
    static ChannelFamily channelFamilyOf(const std::string& constellation,
                                         const std::string& signal);

    /// Per-constellation verdict of the run-latched family census (FAMILY-GF, §3.2).
    struct FamilyCensus {
        /// Constellations whose secondary family is partitioned: it carries real
        /// observations and shares no station with the primary family, so the primary
        /// family's pin cannot reach it.  Only these get extra rows.
        std::set<std::string> partitioned;
        int n_stations_primary{0};    ///< Stations carrying only primary-family channels (summed over constellations).
        int n_stations_secondary{0};  ///< Stations carrying only secondary-family channels.
        int n_bridges{0};             ///< Stations carrying both: the population whose existence makes the inter-family GF difference observable and the partition false.
        /// Human-readable per-constellation census lines, for the always-on log
        /// (invariant #4: an armed-but-inert configuration must read as such).
        std::vector<std::string> lines;
        bool latched{false};          ///< Set once the census has run; the verdict is fixed for the run (§7).
    };

    /**
     * @brief Run the family census over a live per-station channel-support map
     *        (FAMILY-GF).
     *
     * @param station_signals per station, the set of "SAT|SIGNAL" code keys it holds
     *        (i.e. @c StateManager::getSatBiasStates() keys), for present stations only.
     * @return the census verdict; @c partitioned lists the constellations where a
     *         secondary family exists and @c bridges == 0 for it.
     *
     * An independent scan rather than a reuse of the CB-ADM admission census: that
     * census keeps an integer support count, not station identity, and runs only
     * when @c network.cb_admission is armed, so reusing it would mean arming a
     * different registration path, which is a posture change.
     */
    static FamilyCensus runFamilyCensus(
        const std::map<std::string, std::set<std::string>>& station_signals);

    /**
     * @brief Build the W6 satellite code-bias (OSB) datum gauge rows (moved out of
     *        Application::runNetwork at decomposition step INC-8).
     *
     * Row order is contractual because byte identity depends on it: per satellite,
     * in first-seen census order over @p registered_code_keys, the ionosphere-free
     * pin followed by that satellite's optional geometry-free rows (the
     * @p sat_gf_datum rows with their RF-0d / B5D extras, the @p gf_datum pin, the
     * FAMILY-GF rows); then the optional per-constellation ionosphere-free level rows
     * (@p sat_if_level_datum); then the sum-to-zero rows in sorted
     * "<constellation>|<signal>" channel-key order.  The two base families:
     *
     *  1. Per-satellite ionosphere-free pin (clock-common gauge): the two lowest
     *     distinct-frequency code signals (deterministic in the signal set, via
     *     @ref selectIfPinPair) weighted a1 = f1²/(f1²−f2²), a2 = −f2²/(f1²−f2²),
     *     so the pinned combination is the one the satellite clock absorbs.  A
     *     single-frequency satellite degenerates to an all-ones gauge over its
     *     registered signals.
     *  2. Per-(constellation, signal) sum-to-zero: one row per registered channel,
     *     summed over the satellites that carry it.  It pins every channel's
     *     absolute level per signal rather than per band, which is robust to
     *     same-band channels that are not co-tracked at network scale.  A channel
     *     whose every column the geometry-free datum pins has its row suppressed
     *     (@ref CodeBiasDatumRows::n_s2z_suppressed).
     *
     * Time-linking (@p cb_time_linked, card 2): both families pin the change of the
     * datum instead of its level.  The IF row's RHS becomes the satellite's previous
     * solved IF combination, but only when the pair is unchanged and both signals
     * have prior values; otherwise the row re-seeds to 0 and the satellite is
     * recorded in @p this_epoch_cb_reseed, unless this is its first appearance in a
     * run that did not resume.  The sum-to-zero row spans the channel's veterans
     * with RHS Σ b̂_prev, via @ref codeS2zTimeLink.
     *
     * Clock signal-basis hold (@p clock_signal_basis_hold): the satellite keeps its
     * first-latched reference pair while both signals stay registered, and re-latches
     * when one is lost.  A re-latch is a real datum move: one "CLK_BASIS"/"relatch"
     * event through @p emitEvent, plus a pending per-satellite disc when
     * @p emit_clock_disc is set.  With the hold off and @p emit_clock_disc set, this
     * epoch's selection is recorded in @p this_epoch_if_pin_sel for the emit-site
     * compare (emit-only discipline).  Independently of both knobs, every realized
     * dual-frequency pair whose IF row folded is recorded in
     * @p this_epoch_if_pin_real (and, under @p gf_datum, the satellite in
     * @p this_epoch_gf_pin_real; for a partitioned secondary family, its pair in
     * @p this_epoch_family_gf_pin_real), so the S contract can declare the datum it
     * realized.  The declaration channel is kept separate from the disc channel so
     * that the declaration (the @c cbpin= token) does not depend on a QC knob.
     *
     * The caller owns the fold (addSharedConstraintRows at
     * @ref kCodeBiasGaugeWeight), the RAIM-replay cap_* capture and the first-N
     * summary log.  The method builds rows from const estimator queries; its other
     * effects go only through the side channels documented below: the
     * @p held_clock_ref latch (read/write), the @p this_epoch_* per-epoch outputs,
     * inserts into @p basis_relatch_disc_pending, and the @p emitEvent callback.
     *
     * @param est  Joint estimator, read only through numShared() and codeBiasColumn().
     * @param registered_code_keys  ≥2-station (sat,signal) OSB keys, first-seen order.
     * @param freqOf  Carrier-frequency oracle (sat,signal)→Hz, passed by reference;
     *                the caller owns the station roster it closes over.
     * @param cb_time_linked            network.code_datum_time_linked.
     * @param clock_signal_basis_hold   network.clock_signal_basis_hold.
     * @param emit_clock_disc           network.emit_clock_disc.
     * @param code_bias_prev_           Previous kept-epoch solved code biases,
     *                                  "<const>|<sig>" → sat → b̂_prev (card 2 carry).
     * @param code_bias_ifpin_pair_     sat → last kept-epoch IF pair (card 2 carry).
     * @param code_bias_sat_seen_       Satellites with any prior kept epoch (card 2 carry).
     * @param code_bias_datum_resumed_  Warm-resume flag (treat re-seeds as bumps).
     * @param held_clock_ref            Basis-hold latch (read/write).
     * @param this_epoch_if_pin_sel     [out] Disc scratch for the hold-off path (cleared
     *                                  by caller).
     * @param this_epoch_if_pin_real    [out] Declaration scratch (S contract §1.5): the
     *                                  realized pair per satellite, written on both the
     *                                  hold and the hold-off paths and guarded only by a
     *                                  valid dual-frequency pin whose IF row folded,
     *                                  never by @p emit_clock_disc, so a
     *                                  product-contract declaration does not depend on
     *                                  a QC channel (cleared by caller).
     * @param this_epoch_gf_pin_real    [out] Satellites that also got a geometry-free
     *                                  row this epoch (@p gf_datum; S contract §1.5);
     *                                  empty when the GF datum is off (cleared by
     *                                  caller).
     * @param this_epoch_cb_ifpin       [out] IF pair scratch for the card 2 time link
     *                                  (cleared by caller).
     * @param gf_family_constellations  Constellations whose secondary channel family is
     *                                  partitioned (the run-latched
     *                                  @ref runFamilyCensus verdict; FAMILY-GF).  For
     *                                  these, one extra geometry-free row per
     *                                  (satellite, secondary family) is folded, and no
     *                                  extra IF row: the satellite clock, shared with
     *                                  the primary family, is already realized by the
     *                                  primary IF pin, so the family's IF combination
     *                                  is the estimable inter-family ISC.  Empty when
     *                                  the knob is off or nothing partitions, in which
     *                                  case the fold is byte-identical.  Only the
     *                                  verdict is passed, never precomputed pairs, so
     *                                  the family pair is selected by
     *                                  @ref selectIfPinPair on the family-filtered
     *                                  signal set, through the same code path and the
     *                                  same frequency source as the IF pin.
     * @param this_epoch_cb_reseed      [out] Re-seed scratch for the card 2 time link
     *                                  (cleared by caller).
     * @param sat_gf_datum              The hard per-satellite geometry-free datum mode
     *                                  (@c network.sat_gf_datum, RF-0c).
     *                                  @c NoneMode builds no row (byte-identical).
     * @param sat_gf_target_m           Per-satellite geometry-free pin target in
     *                                  metres, used only under
     *                                  @c NetworkSatGfDatum::BroadcastTgd (@c c*T_GD,
     *                                  or c*BGD for Galileo, run-latched at setup;
     *                                  RF-0c).  A satellite absent from the map is
     *                                  pinned at 0 and counted.
     * @param sat_if_level_datum        Fold one absolute per-constellation
     *                                  sum-to-zero row on the ionosphere-free
     *                                  combination (RF-0e e1).  Meant to pair with
     *                                  @p sat_gf_datum, whose suppressed sum-to-zero
     *                                  rows it replaces; the loader warns when it is
     *                                  armed alone and refuses it together with
     *                                  @p sat_gf_datum_time_linked.  This builder folds
     *                                  the rows whenever the flag is set.
     * @param sat_gf_datum_time_linked  Carry each geometry-free row's RHS from the
     *                                  previous kept epoch, using the absolute target
     *                                  only at first sighting (RF-0e e2).
     * @param sat_gf_extra_band_free    Fold no geometry-free row for an extra-band
     *                                  column (a registered signal whose band digit is
     *                                  neither of the pin pair's) that
     *                                  @p extra_band_determined reports as determined
     *                                  this epoch, so its level falls to the channel's
     *                                  sum-to-zero row and its between-satellite
     *                                  differences to the data
     *                                  (@c network.sat_gf_extra_band_free, B5D).  An
     *                                  undetermined column is carried at its own
     *                                  previous combination, or held by a single level
     *                                  row in the channel's own frame at a first
     *                                  sighting; never at the absolute broadcast
     *                                  target, which would pull a sum-to-zero family
     *                                  into the broadcast frame.  Off: RF-0d's pin,
     *                                  byte-identical.
     * @param extra_band_determined     The `"<sat>|<signal>|<b1><b2>"` keys the data
     *                                  determine this epoch (B5D); @c b1 / @c b2 are the
     *                                  pin pair's band digits, so the roster answers
     *                                  the question for the pair the builder realized
     *                                  rather than for an assumed (L1, L2).  Built by
     *                                  the driver (only it sees the stations); empty
     *                                  unless @p sat_gf_extra_band_free is armed.
     * @param basis_relatch_disc_pending  [in/out] pending per-sat disc bumps.
     * @param emitEvent                 L2 event-row callback (tier, event, sat,
     *                                  station, statistic, threshold, action, detail).
     */
    static CodeBiasDatumRows buildCodeBiasDatumRows(
        const NetworkEstimator& est,
        const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
        const std::function<double(const std::string&, const std::string&)>& freqOf,
        bool cb_time_linked, bool clock_signal_basis_hold, bool emit_clock_disc,
        bool gf_datum,
        NetworkSatGfDatum sat_gf_datum,
        const std::map<std::string, double>& sat_gf_target_m,
        bool sat_if_level_datum, bool sat_gf_datum_time_linked,
        bool sat_isc_state, const std::set<std::string>& isc_bridged,
        bool sat_gf_extra_band_free,
        const std::set<std::string>& extra_band_determined,
        const std::set<std::string>& gf_family_constellations,
        const std::map<std::string, std::map<std::string, double>>& code_bias_prev_,
        const std::map<std::string, std::pair<std::string, std::string>>& code_bias_ifpin_pair_,
        const std::set<std::string>& code_bias_sat_seen_,
        bool code_bias_datum_resumed_,
        std::map<std::string, std::pair<std::string, std::string>>& held_clock_ref,
        std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_sel,
        std::map<std::string, std::pair<std::string, std::string>>& this_epoch_if_pin_real,
        std::set<std::string>& this_epoch_gf_pin_real,
        std::map<std::string, std::pair<std::string, std::string>>& this_epoch_family_gf_pin_real,
        std::map<std::string, std::pair<std::string, std::string>>& this_epoch_cb_ifpin,
        std::set<std::string>& this_epoch_cb_reseed,
        std::set<std::string>& basis_relatch_disc_pending,
        const std::function<void(const char*, const char*, const std::string&,
                                 const std::string&, std::optional<double>,
                                 std::optional<double>, const char*,
                                 const std::string&)>& emitEvent);

    /**
     * @brief Map a RINEX satellite id to its constellation key (first char).
     * @return "G","E","C","R","J","S","I" or the first char as-is.
     */
    static std::string constellationOf(const std::string& sat) {
        return sat.empty() ? std::string{} : std::string(1, sat.front());
    }

    /**
     * @brief Deterministically select a satellite's ionosphere-free reference
     *        pair (the clock-common code-bias pin) from its registered code
     *        signals, as a pure function of the signal set and the frequency map.
     *
     * The two returned signals form the W6 satellite-clock IF pin
     * (@f$a_1+a_2=1@f$): fixing this pair's ionosphere-free combination pins the
     * constant that is common to all of a satellite's code biases and that its
     * estimable clock absorbs, which keeps the emitted OSB pure-DCB.  Because the
     * emitted @f$(\delta t^s,\,d^s)@f$ split is anchored to this pair, the choice
     * must not depend on registration or coverage order; otherwise a same-band
     * channel swap (C1C ↔ C1W) re-references the clock by the satellite's
     * inter-channel bias, a per-satellite step that no consumer is told about.
     *
     * Selection uses a strict total order over the signal set:
     *  - signals are sorted by @b (RINEX band digit, then full signal name), so
     *    same-band channels tie-break deterministically on their name rather than
     *    on the order they happen to be passed in;
     *  - @c first  = the lowest-ordered signal;
     *  - @c second = the first later signal on a distinct carrier frequency
     *    (@f$|f-f_{\text{first}}|>1@f$ Hz), or empty when the satellite has only a
     *    single distinct frequency (a degenerate single-band pin).
     *
     * @param sigs   the satellite's registered code signals, in any order (taken
     *               by value; the caller's container is not reordered).
     * @param freqOf signal → carrier frequency (Hz); a non-finite or @f$\le 0@f$
     *               result is treated as "frequency unavailable" for the
     *               distinct-frequency test on @c second, and a non-finite
     *               frequency of @c first leaves @c second empty.
     * @return @c {first, second}; @c second is empty when no distinct second
     *         frequency exists (or @p sigs is empty, in which case both are empty).
     */
    static std::pair<std::string, std::string> selectIfPinPair(
        std::vector<std::string> sigs,
        const std::function<double(const std::string&)>& freqOf);

    /// Result of @ref codeS2zTimeLink: the time-linked sum-to-zero target for one
    /// (constellation,signal) code-bias channel (card 2, @c network.code_datum_time_linked).
    struct CodeS2zTimeLink {
        std::vector<std::string> jac_sats;  ///< satellites getting @c +1 in the sum-to-zero row: the veterans when ≥1 exists, else every present satellite.
        double                   rhs{0.0};  ///< RHS @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$ over @ref jac_sats (0 when @ref reseed).
        bool                     reseed{false}; ///< true ⇒ no veteran present: the row re-seeds at @f$\sum=0@f$, a real re-reference, and the driver disc-bumps the channel's present satellites (first appearances excepted, unless the run resumed).
    };

    /**
     * @brief Compute the time-linked sum-to-zero target for one code-bias channel
     *        (card 2): pin the change of the previous epoch's veteran mean to zero
     *        rather than the current set's level.
     *
     * The legacy datum spans every present satellite with target 0, so on a rise or
     * set the target shifts by (departed bias)/@f$m'@f$, and the stiff near-zero-@f$q@f$
     * carry then relaxes the level over many epochs: a hysteretic, unsignalled
     * common-mode wander.  Time-linking instead spans the "veterans" (present
     * satellites that also have a previous-epoch solved value) with RHS
     * @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$.  Each veteran is referenced to itself,
     * so membership changes do not move the target, and new satellites take their
     * level from the network plus this row.  A channel with no veterans (cold start,
     * warm-resume miss, or every satellite fresh) falls back to the legacy
     * all-present @f$\sum=0@f$ seed and flags @ref CodeS2zTimeLink::reseed so the
     * driver can bump the disc counters; that is the one path on which the emitted
     * level re-references.
     *
     * Rank is preserved: exactly one row per channel either way, and one veteran
     * suffices to pin the level gauge (the all-ones common shift changes the veteran
     * sum by @f$m_{\text{vet}}\cdot c\neq 0@f$).  Pure: no estimator or emitter state.
     *
     * @param present_sats satellites present this epoch carrying this channel
     *                      (order preserved into @ref CodeS2zTimeLink::jac_sats on the
     *                      no-veteran fallback).
     * @param prev_vals    satellite → previous-epoch solved bias for this channel; a
     *                      present satellite is a veteran iff it is a key here.
     * @return the row's spanned satellites, RHS, and re-seed flag.
     */
    static CodeS2zTimeLink codeS2zTimeLink(
        const std::vector<std::string>& present_sats,
        const std::map<std::string, double>& prev_vals);

    /**
     * @brief Build the shared-block clock datum for the constellations present in
     *        @p registered_clock_sats, per @p decl.isb_datum.
     *
     * @par Sum-to-zero mode (@c decl.isb_datum == SumToZeroPerConstellation)
     * The per-constellation clock common mode is fixed by a sum-to-zero over that
     * constellation's registered satellite clocks.  Returns a Plan with an empty
     * @ref Plan::shared_datum and an empty @ref Plan::clock_pivot (no pivot,
     * @ref Plan::declared_dimension = 0) and one all-ones @ref Plan::clock_constraint_H
     * row per constellation, at @ref kClockGaugeWeight, for the driver to fold before
     * the rank gate.  Pivot overrides and stickiness are ignored; pivot selection
     * applies to pin mode only.
     *
     * @par Broadcast-anchored mode (@c decl.isb_datum == BroadcastAnchoredPerSatellite)
     * Each registered satellite clock is anchored to its own a-priori clock (the
     * broadcast clock under brdc_rts; Δδtˢ → 0) by one single-entry constraint row at
     * information 1/@c decl.broadcast_anchor_sigma_m².  The soft anchors realize the
     * gauge, so @ref Plan::shared_datum and @ref Plan::clock_pivot are empty and
     * @ref Plan::declared_dimension = 0.  The common-mode eigenvalue the rank gate
     * sees is 1/σ² per constellation, independent of N: the anchor block is
     * (1/σ²)·I and the gauge direction is supported on the clock columns, so
     * (N_ss+ΔN)·u = (1/σ²)·u.  That is about 0.11 at σ = 3 m, far above the relative
     * cutoff (about 1e-9·λmax); a loose σ ≳ 60 m at about 305 stations drops it
     * below the cutoff (see the binding-σ check in GnssConfigLoadNetwork.cpp).  The
     * datum does not wander when membership changes, because each anchor is
     * independent and nothing is redistributed.  Throws when the declared σ is
     * non-finite or non-positive (the datum would be undefined).
     *
     * @par Pin mode (@c decl.isb_datum == PerConstellationSatClockPin, default)
     * One satellite-clock pin per constellation.  Pivot selection per
     * constellation, in priority order:
     *  1. **Config pin**: @p decl.clock_pivot_sat, when set and present in the
     *     registered set.  Honored in both the sticky and the lexical mode: a pin
     *     declares user intent, so the pinned satellite reclaims the datum when it
     *     returns; that flip-back is deliberate and the driver signals it as a
     *     handover.
     *  2. **Sticky incumbent** (@c network.sticky_pivot, RTA-1): with
     *     @p sticky_prev_pivot non-null, the incumbent pivot recorded in that map,
     *     as long as it is still present in the registered set.  A new pivot is
     *     selected by rule 3 only when the incumbent is absent (set or dropped),
     *     and that new pivot then becomes the sticky incumbent in turn: the old
     *     one returning does not flip the datum back.
     *  3. **Lexical fallback**: the first registered satellite of that
     *     constellation (stable lexical order).  With @p sticky_prev_pivot null
     *     (the default, legacy mode) this rule is re-derived every epoch, so the
     *     automatic pivot changes whenever a lexically lower satellite rises; each
     *     change is a constellation-wide datum re-reference that the product
     *     consumer pays for with an ambiguity reset.
     *
     * The phase-bias↔ambiguity gauge is fixed by local ambiguity pins that the
     * driver folds (pivot-receiver pins in the default convention), so it is absent
     * here by design.
     *
     * @param est                   the estimator after all register* calls (layout frozen).
     * @param registered_clock_sats every satellite for which a clock column was registered.
     * @param decl                  the S-basis convention (clock datum mode, pivot
     *                              overrides, anchor σ).
     * @param sticky_prev_pivot     Constellation char → incumbent pivot satellite,
     *                              i.e. the datum of the last emitted product that
     *                              contained that constellation (the driver's
     *                              persistent emit-baseline map, advanced only at
     *                              the emit site).  Pass @c nullptr for the legacy
     *                              per-epoch lexical auto-pick (byte-identical).
     * @return the datum plan (shared pins or constraint rows, and the declared
     *         residual dimension).
     */
    Plan buildSharedDatum(const NetworkEstimator& est,
                          const std::vector<std::string>& registered_clock_sats,
                          const SbasisDeclaration& decl,
                          const std::map<std::string, std::string>* sticky_prev_pivot
                              = nullptr) const;

    /**
     * @brief The structural per-constellation clock null direction @f$v_c@f$
     *        (queue 1(b), docs/SPEC_pivot_visibility_gauge_defect.md §8.1 rule 1).
     *
     * The clock gauge of a constellation is the direction documented in this
     * file's header: *adding a constant to every satellite clock of a
     * constellation and to the receivers' clock/ISB of that constellation
     * cancels on every row*.  The receiver clock/ISB are local states eliminated
     * by the Schur reduction, so inside the shared border the direction is
     * exactly the indicator over that constellation's registered satellite-clock
     * columns, coefficient 1.0 each.  Every other shared family carries its own
     * independent gauge (the satellite code bias through the W6 per-satellite IF
     * pin and the per-(constellation, signal) sum-to-zero rows, the phase bias
     * through the local ambiguity pins at its anchor receiver), so their component
     * along @f$v_c@f$ is zero and they are not part of this direction.  A measured
     * pivot handover on the simulation bed agrees: the clock steps rigidly while the
     * code-bias and phase-bias step means stay within their scatter, so the biases
     * do not move with the clock frame.
     *
     * Returned as (satellite, coefficient) pairs in the same deterministic lexical
     * order that @ref buildSharedDatum groups and sorts by, so the direction a
     * caller transforms along is the gauge this builder fixes.  Callers take it
     * from here rather than writing it by hand at a call site or taking an
     * eigenvector: eigenvectors of equal-scale eigenvalues mix, while structural
     * membership is exact and carries per-satellite identity.
     *
     * @param registered_clock_sats This epoch's registered satellite-clock set:
     *                              the same vector passed to @ref buildSharedDatum.
     * @param constellation         Constellation key (e.g. "G"), as produced by
     *                              @ref constellationOf.
     * @return The direction's nonzero entries; empty when the constellation has
     *         no registered clock satellite (the caller must treat that as a
     *         no-op, not as a zero direction).
     */
    static std::vector<std::pair<std::string, double>> clockNullDirection(
        const std::vector<std::string>& registered_clock_sats,
        const std::string& constellation);

    /**
     * @brief Build the loose Kaula / physical-bound priors on the admitted
     *        satellite code-bias (OSB) columns (card KP).
     *
     * The conditional second lever of docs/SPEC_beidou_w6_datum.md.  Its trigger
     * is a residual below-cutoff direction that the calibrated rank gate (card DG)
     * and the phase drop-and-heal (card PA) leave behind: a `CB`+`CLK` direction
     * with zero phase-bias weight, out of reach of the drop-and-heal, which anchors
     * only PB columns, and therefore a genuine deficiency of the admitted code
     * columns.  (On the 305-station arm it was the one residual direction besides
     * the 3 declared clock gauges.)  A satellite's OSBs are physically bounded
     * (tens of ns), so a loose prior toward 0 is real prior information: it lifts
     * that direction without appreciably moving any well-observed OSB, whose data
     * information exceeds 1/σ² by orders of magnitude.
     *
     * @par Priors rather than gauge rows (a deviation from the site the card names)
     * The card names @ref buildCodeBiasDatumRows, whose output is folded at
     * @ref kCodeBiasGaugeWeight, the tight datum weight a gauge row needs.  A Kaula
     * prior is loose on purpose, so stacking it into the same rows would either
     * mis-weight it or force a second weight channel through a row order that
     * carries byte identity (@ref buildCodeBiasDatumRows documents that contract).
     * The diagonal information-prior channel (@c NetworkEstimator::addSharedPrior)
     * exists for this purpose and is what the phase drop-and-heal uses.  It also
     * makes the card's declared-dimension audit (K3) hold by construction: a prior
     * is not a datum row, so @c Plan::declared_dimension cannot move.
     *
     * @par Anti-fragility (K2)
     * A loose prior whose information sits at the rank cutoff does not lift the
     * direction; the CB-ADM card rejected that configuration.  The caller must
     * therefore check that @c info = 1/σ² lands at least two decades above the
     * armed cutoff (@c rank_gate_tol · max(λmax,1)) and log the margin.
     *
     * @param registered_code_keys The (satellite, signal) code columns registered
     *                             this epoch; under CB-ADM these are exactly the
     *                             admitted pairs with live coverage.
     * @param sigma_m              Prior standard deviation in metres (the physical
     *                             OSB bound; 1 ns ≈ 0.2998 m).  Must be > 0: a
     *                             non-positive value yields an empty list, so a
     *                             mis-set knob cannot silently fold info = ∞.
     * @return One zero-valued prior per registered code column, @c info = 1/σ².
     *         Empty ⇒ the caller folds nothing (byte-identical).
     */
    static std::vector<NetworkEstimator::SharedPrior> buildCodeBiasKaulaPriors(
        const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
        double sigma_m);

    /// Result of @ref completeSbasisDimension, the per-epoch S-basis completion
    /// (SBC, @c network.sbasis_complete): the realized near-null directions beyond
    /// the declared structural dimension, as eigenvector gauge rows for the driver
    /// to fold (at @ref kSbasisCompleteWeight) before the rank gate, plus
    /// per-direction attribution for the engage/release event contract (standing
    /// invariant #7).
    struct GaugeCompletion {
        /// One residual (weak or undeclared) direction, constrained or skipped.
        struct Dir {
            double ev{ 0.0 };         ///< its eigenvalue in the inspected reduced normals (raw value, not scaled by λmax; may be ~0)
            double target{ 0.0 };     ///< folded RHS = vᵀ·x_prev (the combination's carried level; 0 cold / on datum change)
            double top_span{ 0.0 };   ///< |component| of the heaviest participating column
            std::string top_label;    ///< that column's label (from the caller's labeler)
            std::set<std::string> sats;  ///< satellites of the columns at |component| ≥ @ref kSbasisAttrSpan (of the heaviest column when none reaches it): the engage/release identity
            std::string detail;       ///< "label:span,…" over the top six participants (trace attribution)
        };
        int observed{ 0 };        ///< near-null count measured before completion: the epoch's realized S-basis dimension
        int n_extra{ 0 };         ///< directions beyond declared_dim actually constrained (= H.rows())
        int n_skipped{ 0 };       ///< extras skipped as unseparated from the declared-gauge cluster (less than one decade above it, or at machine zero: exact orphaned gauges, card PA's class).  Skips are per direction, not epoch-fatal: the separated extras above them are still constrained, the gate then throws by exactly n_skipped, and the drop-and-heal works on what remains.  Refusing the whole epoch instead would leave the drop-and-heal alone with the trades as well (its PA5 insufficiency).
        bool capped{ false };     ///< extras exceeded max_extra: no rows emitted (caller falls through to the gate → withhold)
        bool degenerate{ false }; ///< n_skipped > 0 (informational; folding proceeds on the separated extras)
        bool weight_marginal{ false }; ///< @ref kSbasisCompleteWeight is less than two decades above the cutoff: no rows emitted (K2)
        Eigen::MatrixXd H;        ///< n_extra × n gauge rows (the extra directions' unit eigenvectors)
        Eigen::VectorXd z;        ///< length-n_extra continuity targets (see Dir::target)
        std::vector<Dir> dirs;    ///< per-row attribution, same order as @ref H
        std::vector<Dir> skipped; ///< attribution of the n_skipped unconstrained directions (target unused, 0), so that the sbc_skip event names each skipped orphan and the withhold it causes can be diagnosed from the trace.  An orphan whose phase-bias components all stay below 0.3 is also outside the drop-and-heal's selection.
    };

    /**
     * @brief Derive the epoch's realized S-basis dimension and return
     *        minimum-constraint gauge rows for every residual near-null
     *        direction beyond the declared structural dimension (SBC).
     *
     * The declared dimension counts only the structural gauges (the
     * per-constellation clock in pin mode; 0 in the constraint-realized clock
     * datums).  The realized per-epoch system can carry more: per-satellite
     * phase-bias↔slant-iono trade directions confined to one receiver channel
     * family.  The Galileo C/Q-versus-X tracking split partitions the station set,
     * so, for example, the X-family observations of one satellite form their own
     * weakly coupled component, with eigenvector components ∝ μ_σ = f₁²/f_σ² over
     * that family's phase-bias columns; once the family's code support thins, only
     * the SH-VTEC prior (λ ≈ 1e-6 ≪ cutoff) resists.  The per-(sat,signal) anchor
     * pins close the phase-bias↔ambiguity gauge but not this trade, so the declared
     * dimension under-counts.  Without the completion the gate withholds the epoch,
     * or the card PA soft anchors act on columns: a column prior also touches the
     * estimable part and misses spread directions.
     *
     * The method eigen-decomposes the reduced shared normals with the cutoff
     * arithmetic of the gate, takes the near-null directions in ascending order,
     * skips the @p declared_dim smallest (the structural gauges the declared datum
     * already pins, which sit at machine zero, decades below the weak extras), and
     * returns one gauge row per remaining direction,
     * @f$v^\top x = v^\top x_{\text{prev}}@f$: the minimum-constraint completion of
     * the S-basis.  Each row pins exactly the deficient combination (eigenvectors
     * are orthogonal to every estimable direction at the cutoff's resolution), so
     * the estimable solution is untouched (S-transform invariance) and the pinned
     * combination holds its carried level instead of stepping.  The emitted frame
     * therefore stays continuous at the engage edge, and at the release edge the
     * data re-determine the combination from the held level.
     *
     * Epoch-level refusals: the method returns the counts and the flag but no rows
     * (and no attribution for the refused extras), and the caller falls through to
     * the unchanged gate (→ withhold), because folding would be worse than
     * withholding:
     *  - @ref GaugeCompletion::capped: more than @p max_extra separated extras.  A
     *    mass deficiency is a pathological epoch (or a mis-set tolerance counting
     *    real directions as null), and pinning dozens of directions without notice
     *    would fabricate a solution.  Counted, never silent (standing invariant #3).
     *  - @ref GaugeCompletion::weight_marginal: @ref kSbasisCompleteWeight is less
     *    than two decades above the armed cutoff (K2 anti-fragility: the CB-ADM
     *    card rejected the prior-at-the-cutoff configuration).  Checked before the
     *    cap.
     *
     * Per-direction skip (@ref GaugeCompletion::n_skipped): an extra unseparated
     * from the declared-gauge cluster (less than one decade above the running
     * cluster top, or below the machine-zero floor 1e-14·max(λmax, 1)) is skipped,
     * which is not epoch-fatal.  Eigenvectors mix within a degenerate cluster, so a
     * row there could re-target a combination the declared pins own, and an extra
     * at machine zero is an orphaned exact gauge, card PA's class.  Each skipped
     * extra joins the cluster, so the separated extras above it (at least one
     * decade above its top) do not mix with it and are constrained; the gate then
     * throws by exactly n_skipped, and the drop-and-heal works on that remainder.
     * With @p declared_dim 0 there is no declared cluster and nothing is skipped.
     * Splitting the two classes lets each mechanism take its own: withhold-cluster
     * epochs can carry one orphan and one or two trades together, and an
     * all-or-nothing refusal would leave the drop-and-heal alone with both, which
     * reproduces its PA5 insufficiency.
     *
     * A pure function of its inputs (no estimator or driver state).  The driver
     * owns the fold (@c addSharedConstraintRows at @ref kSbasisCompleteWeight),
     * the RAIM-replay capture, the engage/release events and the counters.
     *
     * @param reduced_N_ss The pre-datum reduced shared normals: the same matrix
     *                     state that @ref assertNullSpaceRank inspects (all
     *                     pre-gate priors and rows already folded).
     * @param declared_dim The declared structural dimension
     *                     (@c Plan::declared_dimension).
     * @param tol          The armed relative rank-gate tolerance
     *                     (@c NetworkEstimator::rankGateTol, the one value all
     *                     gate consumers share; card DG2).
     * @param x_prev       Carried shared state mapped to this epoch's columns
     *                     (0 where no carry exists / on a datum re-reference);
     *                     an empty or wrong-length vector ⇒ all targets 0.
     * @param labelOf      Column labeler for attribution (pass
     *                     @c NetworkEstimator::sharedColumnLabel, which is always
     *                     populated, unlike the diagnostic-only col_desc).  May be
     *                     empty (labels become "col<N>").
     * @param max_extra    Cap on constrained directions per epoch
     *                     (@c network.sbasis_complete_max); ≤ 0 ⇒ uncapped.
     * @return The completion (see @ref GaugeCompletion).  @c n_extra == 0 with no
     *         refusal flag and no skip means the epoch needs no completion,
     *         including the observed < declared case, which is the gate's
     *         "over-specified" verdict to make, not this method's.
     */
    GaugeCompletion completeSbasisDimension(
        const Eigen::MatrixXd& reduced_N_ss, int declared_dim, double tol,
        const Eigen::VectorXd& x_prev,
        const std::function<std::string(Eigen::Index)>& labelOf,
        int max_extra) const;

    /**
     * @brief Null-space rank gate: eigen-decompose the pre-constraint reduced
     *        shared normals and assert that the count of (relatively) zero
     *        eigenvalues equals the declared S-basis dimension.
     *
     * Runs after the kernel's @c reduce() (local ambiguity pins already folded and
     * eliminated) and before the datum solve.  A mismatch means the datum is
     * under- or over-specified, and the producer would emit a meaningless
     * least-norm split, so the gate throws rather than let the solve proceed.
     *
     * The reduced normals are symmetric positive-semidefinite here (the gate
     * precedes any indefinite modification), so eigenvalues equal singular values
     * and a self-adjoint eigensolve gives the same count as an SVD far more
     * cheaply.
     *
     * @param reduced_N_ss The reduced shared normal matrix @f$\tilde N_{ss}@f$.
     * @param declared_dim Expected null-space dimension (Plan::declared_dimension).
     * @param tol          Relative eigenvalue threshold: the cutoff is
     *                     @f$\text{tol}\cdot\max(\lambda_{max},1)@f$.  The driver
     *                     passes @ref NetworkEstimator::rankGateTol, the one
     *                     resolved value that the dump and the drop-and-heal also
     *                     read (card DG2, @c network.rank_gate_tol).
     * @param observed_out [out, optional] The observed near-zero count, written
     *                     before the equality test, so a caller gets the number
     *                     on the throwing path too (card DG6: "over-specified"
     *                     must be countable as its own case, and the run-wide
     *                     min/max observed count is the calibration evidence).
     *                     Set to 0 on entry.  With @c nullptr (the default)
     *                     nothing is reported; the test itself is the same either
     *                     way: exact equality, a throw in both directions,
     *                     relative cutoff.
     * @param ev_max_out   [out, optional] The largest eigenvalue, written beside
     *                     @p observed_out (so also on the throwing path); -1 when
     *                     no spectrum was computed.  It is the one ingredient of
     *                     the gate's cutoff @f$\text{tol}\cdot\max(\lambda_{max},1)@f$
     *                     that a caller lacks, and the gate computes it anyway;
     *                     returning it lets the invariant-#7 datum-direction
     *                     information ratio be measured against the bar the gate
     *                     judges by, without a second eigensolve.
     * @throws std::runtime_error with observed-vs-declared counts on mismatch; also
     *         when the matrix is not square or the eigensolve does not converge.
     */
    void assertNullSpaceRank(const Eigen::MatrixXd& reduced_N_ss,
                             int declared_dim, double tol = 1e-6,
                             int* observed_out = nullptr,
                             double* ev_max_out = nullptr) const;
};

} // namespace GnssProcessor
