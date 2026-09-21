/**
 * @file NetworkSbasis.hpp
 * @brief Explicit minimum-constraint S-basis for the PPP-RTK network producer
 *        (W4-S2b / W5): the datum *declaration* + the shared-block clock datum
 *        builder + a null-space rank gate + the SBC per-epoch S-basis
 *        completion (@ref NetworkSbasis::completeSbasisDimension).
 *
 * Where @ref NetworkEstimator owns the per-epoch shared satellite-parameter
 * column layout and @ref NetworkNormalEquations is the pure Schur kernel, this
 * class holds the S-system DATUM — which gauges are fixed, to what, and how the
 * choice is *declared* so the emitted product is self-describing and a consumer
 * can cross-check compatibility (Teunissen–Khodabandeh covariance).
 *
 * @par Datum (V1: estimable satellite clock + phase bias, coords/orbits fixed,
 *      GPS+GAL, per-band uncombined)
 * The undifferenced uncombined network normal matrix is rank-deficient by:
 *  - **Clock gauge — one per constellation.**  Adding a constant to every
 *    satellite clock of a constellation and to the receivers' clock/ISB of that
 *    constellation cancels on every row.  Every constellation — GPS included —
 *    is gauged identically, in one of two configurable ways
 *    (@ref SbasisDeclaration::IsbDatum):
 *      * a SHARED hard pin on ONE satellite's clock column per constellation
 *        (@ref NetworkEstimator::Datum) — the legacy anchor-based "pivot" datum; or
 *      * a per-constellation SUM-TO-ZERO over that constellation's satellite
 *        clocks (@c network.clock_datum_sum_to_zero), folded as a tight all-ones
 *        gauge row exactly like the code-bias gauge below — anchor-free, so no
 *        pivot and no handover seam.  In this mode the SHARED @ref Datum is EMPTY
 *        (the clock gauge is pinned by the folded constraint before the rank gate,
 *        so the reduced shared system is full-rank; @ref Plan::declared_dimension
 *        is 0) and @ref Plan carries the constraint rows instead.
 *    No system is anchored to a receiver clock; each station's receiver
 *    clock/ISB stays a free local state that absorbs the constellation's lump.
 *  - **Phase-bias↔ambiguity gauge — one per (satellite, signal).**  Each
 *    satellite's phase bias @f$b^s_\sigma@f$ is co-linear with that satellite's
 *    ambiguity column @f$\{N^s_{r,\sigma}\}_r@f$ (null vector
 *    @f$\delta b^s_\sigma=-\delta N^s_{r,\sigma}@f$ for any per-satellite shift).
 *    Realized as LOCAL tight-prior pins on the pivot receiver's ambiguities
 *    (the estimable phase bias then carries the pivot-receiver ambiguity lump,
 *    Odijk Eq. 4).  These LOCAL pins are folded into the pivot station's design
 *    rows by the driver (they enter the Schur reduction), so they do NOT appear
 *    in the shared @ref NetworkEstimator::Datum produced here.
 *
 * Hence in the legacy PIN datum the SHARED datum this builder emits is exactly
 * the per-constellation clock pins; after the local ambiguity pins are
 * eliminated, the reduced shared normals have precisely @c #constellations
 * residual deficiencies, which the @ref assertNullSpaceRank gate verifies before
 * the solve.  In the SUM-TO-ZERO datum the clock gauges are instead pinned by the
 * folded constraint rows, so the reduced shared normals are full-rank and the
 * gate's declared dimension is 0.
 *
 * Not a state owner (respects the StateManager-is-single-source-of-truth rule):
 * a pure, per-epoch builder operating on the live registered column set.
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

#include "config/GnssConfigTypes.hpp"   // RF-0c: NetworkSatGfDatum (the GF datum mode)
#include "filter/NetworkEstimator.hpp"

namespace GnssProcessor {

/**
 * @brief The wind-up / yaw-attitude model this BINARY implements.
 *
 * A compile-time constant, deliberately NOT a config knob: it declares what the
 * code does, and a declaration that a QC/trace toggle could move would drift
 * away from the thing it declares.  It is written into every emitted product's
 * S record (field 3, legended @c windup_id) and is the value a consumer pins its
 * own expectation to, so a product built under a different wind-up convention
 * trips @c NetworkProductIO::checkContract instead of being consumed silently.
 *
 * Revisions:
 *   - @c "nominal"      yaw-steering triads, effective dipole built with a PLUS
 *                       on both quadrature terms — the inverted-sign defect;
 *                       every product emitted before 2026-08-21.
 *   - @c "nominal-wu93" the corrected Wu et al. (1993) construction (MINUS on
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
    /// Pivot receiver whose clock anchors network time (= NetworkConfig::clock_datum_station).
    std::string reference_receiver_id;
    /// constellation char ("G","E",...) -> reference satellite pinned for that
    /// constellation's clock gauge.  Empty entry => auto-pick (first registered).
    std::map<std::string, std::string> clock_pivot_sat;
    /// Ambiguity datum convention (how the per-(satellite,signal) phase-bias↔
    /// ambiguity gauge is fixed).  Recorded so the consumer knows which receiver
    /// frame the emitted satellite phase bias references.
    ///  - @c PivotReceiverZeroAmbiguity (default): ONE elected receiver's ambiguity
    ///    is pinned to zero per (satellite,signal), so the estimable phase bias
    ///    carries that receiver's integer lump (legacy single-pivot + prioritized
    ///    anchor).
    ///  - @c SumToZeroPerReceiver (@c network.phase_datum_ensemble, Fix A):
    ///    anchor-free — the gauge is fixed by @f$\sum_r z^s_{r,\sigma}=0@f$ over the
    ///    tracking receivers, so the phase bias references the receiver ENSEMBLE
    ///    (no pivot, no handover, and a consistent between-satellite reference).
    ///    Ambiguities are NOT directly integer (Odijk CC-S, needs Eq-34 recovery).
    ///  - @c SingleReceiverChain (@c network.phase_datum_eq34, Card 3): the
    ///    Eq-34 / de Jonge datum — per-(receiver,system,signal) RPB split states
    ///    let the gauge be fixed by ONE datum-receiver RPB pin + a spanning-tree
    ///    of ambiguity pins over the bipartite (receiver × satellite) graph, per
    ///    (constellation,signal) connected component.  The emitted between-
    ///    satellite reference is then integer-consistent across ALL satellites
    ///    (each chain hop contributes only an integer, because the hop receiver's
    ///    fractional bias is a separate estimable state) while staying full-rank
    ///    at global scale.  Degenerates to the Odijk Eq-34 star (CC-R) in the
    ///    full-visibility regional case.  Wire value 2 rides the existing v2 S
    ///    line (no format bump); an old consumer's verifyContract flags
    ///    "amb_datum differs" as designed.
    enum class AmbDatum { PivotReceiverZeroAmbiguity, SumToZeroPerReceiver,
                          SingleReceiverChain }
        amb_datum{ AmbDatum::PivotReceiverZeroAmbiguity };
    /// Inter-system / clock-gauge datum: every constellation (GPS included) has
    /// its clock gauge fixed by pinning that constellation's OWN satellite clocks;
    /// no system is anchored to a receiver clock, and each station's receiver
    /// clock/ISB stays a free local state.  Two realizations (enumerator names the
    /// realized basis, mirroring the sibling AmbDatum / CodeBiasDatum style):
    ///  - @c PerConstellationSatClockPin (legacy default): a HARD pin on ONE
    ///    satellite's clock per constellation (the "pivot").  Anchor-based — a
    ///    pivot that sets / is deselected forces a constellation-wide datum
    ///    re-reference (handover) the consumer pays for with an ambiguity reset,
    ///    and an epoch missing every candidate pivot has no datum at all.
    ///  - @c SumToZeroPerConstellation (@c network.clock_datum_sum_to_zero): the
    ///    per-constellation clock common-mode is fixed by a SUM-TO-ZERO over that
    ///    constellation's satellite clocks (Σ δtˢ = 0), realized exactly like the
    ///    code-bias per-(constellation,signal) gauge — a tight all-ones constraint
    ///    row folded into the shared normals BEFORE the rank gate.  Fully
    ///    anchor-free: no pivot satellite, so no handover seam ever, and the datum
    ///    is defined for any epoch with ≥1 clock satellite per constellation.  The
    ///    absolute per-constellation level lands identically on the receiver
    ///    clocks/ISB either way (the two datums differ by a per-constellation
    ///    constant a consumer absorbs into its receiver clock), so this is a pure
    ///    S-transform of the pin datum's estimable (between-satellite) solution.
    ///    CAVEAT (measured, Jul 2026): the reference is the RAW satellite set
    ///    present each epoch, so as satellites rise/set the survivors
    ///    redistribute by ∓ΔC0/N to re-zero the sum — the absolute clock-product
    ///    frame WANDERS with membership (position-invariant for a
    ///    receiver-clock-estimating consumer, but a real contract issue for the
    ///    absolute frame, clock-holding across gaps, and disc semantics).
    ///  - @c BroadcastAnchoredPerSatellite (@c network.clock_datum_broadcast_anchored,
    ///    @c apriori_source=brdc_rts only): each satellite's clock correction
    ///    Δδtˢ is anchored to ITS OWN broadcast clock (the a-priori, so the
    ///    anchor target is 0) by a soft pseudo-observation at
    ///    @c broadcast_anchor_sigma_m, folded into the shared normals BEFORE the
    ///    rank gate — the anchors ARE the gauge (declared dimension 0).  No
    ///    pivot, no handover seam, and — because each satellite is anchored
    ///    independently — NO redistribution wander when membership changes: the
    ///    common-mode is bound to the stable EXTERNAL broadcast reference the
    ///    consumer already reconstructs against.  Technically an over-constraint
    ///    (N soft pins vs 1 gauge dof); at σ = 3 m against tight phase data the
    ///    pull on the estimable between-satellite differences is sub-cm, and the
    ///    absolute common-mode retains ~σ/√N of freedom the consumer absorbs
    ///    into its receiver clock.
    enum class IsbDatum { PerConstellationSatClockPin, SumToZeroPerConstellation,
                          BroadcastAnchoredPerSatellite }
        isb_datum{ IsbDatum::PerConstellationSatClockPin };
    /// BroadcastAnchoredPerSatellite only: 1σ (m) of the per-satellite
    /// broadcast-clock anchor pseudo-observation that REALIZES the clock gauge
    /// (the driver resolves it from @c network.apriori_clock_anchor_sigma_m /
    /// the brdc_rts auto default of 3.0 m).  Unused in the other datums.
    double broadcast_anchor_sigma_m{ 3.0 };
    /// Card 3 (SingleReceiverChain) diagnostic: the component-0 datum receiver r1
    /// (the receiver whose RPB was pinned).  Serialized as the optional S-line
    /// token @c pdr=<station> when non-empty — DIAGNOSTIC ONLY (Odijk §6: the user
    /// never needs the network pivot), and NOT part of @ref verifyContract's
    /// compatibility check.  Empty ⇒ unset (nothing written; replay/back-compat
    /// safe, exactly like @c gid=).
    std::string eq34_datum_receiver;
    /// Satellite bias sign convention (matches kSsr*BiasSign at MeasurementModel).
    double bias_sign{ +1.0 };
    /// Declared wind-up / yaw-attitude model id (producer and consumer must agree;
    /// "nominal" = yaw-steering from sun/velocity geometry, eclipse AR ungated).
    ///
    /// The suffix is a WIND-UP MODEL REVISION, not a yaw-attitude change.
    /// "nominal-wu93" marks the corrected Wu et al. (1993) effective-dipole
    /// construction (@c GnssCorrections::computePhaseWindUp, minus on BOTH
    /// quadrature terms).  Products emitted before that fix carry the bare
    /// "nominal" and are in a DIFFERENT phase frame — their satellite clocks and
    /// phase biases absorbed +2*phi_wu.  Bumping the token is what makes a
    /// producer-fixed / consumer-unfixed pairing visible at all; see
    /// docs/SPEC_phase_windup_sign.md section 8 item 2.
    std::string windup_yaw_model_id{ kWindUpYawModelId };
    /// V1 emits satellite clock + phase bias only; W6 (network.estimate_sat_code_bias)
    /// adds satellite code bias.  True when the network emits its own OSB.
    bool emit_code_bias{ false };
    /// Satellite code-bias (OSB) datum convention (W6).  The emitted per-(sat,
    /// signal) code biases are referenced so each satellite's ionosphere-free
    /// combination is zero (the IF lump lives in the estimable clock) and they sum
    /// to zero per constellation (the absolute level the GIM common-mode-free iono
    /// weighting leaves free).  Recorded so a consumer can cross-check compatibility.
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
        NetworkEstimator::Datum shared_datum;            ///< SHARED hard pins: one sat-clock pin per constellation in PIN mode; EMPTY in SUM-TO-ZERO mode.
        int declared_dimension{ 0 };                     ///< Expected residual shared null-space dim: #constellations in PIN mode (= shared_datum.size()); 0 in SUM-TO-ZERO mode (clock gauges folded before the gate).
        std::map<std::string, std::string> clock_pivot;  ///< constellation char -> chosen pivot sat (for the declaration + handover detection).  EMPTY in SUM-TO-ZERO mode (no pivot).
        /// Constraint-realized clock datums only: gauge rows to be folded onto
        /// the shared border via @ref NetworkEstimator::addSharedConstraintRows
        /// BEFORE the rank gate, exactly like the code-bias per-(const,signal)
        /// sum-to-zero.  SUM-TO-ZERO mode: one all-ones row per constellation
        /// over that constellation's registered clock columns (Σ δtˢ = 0), at
        /// @ref kClockGaugeWeight.  BROADCAST-ANCHORED mode: one single-entry
        /// row per registered clock satellite (Δδtˢ → 0 = its own broadcast
        /// a-priori), at 1/σ² from
        /// @ref SbasisDeclaration::broadcast_anchor_sigma_m — the soft anchors
        /// ARE the datum.  EMPTY (0 rows) in PIN mode — the driver folds
        /// nothing then.
        Eigen::MatrixXd clock_constraint_H;              ///< Gauge-row Jacobian (see above); empty in PIN mode.
        Eigen::VectorXd clock_constraint_z;              ///< Constraint targets (zeros); empty in PIN mode.
        double          clock_constraint_weight{ 0.0 };  ///< Row information weight (@ref kClockGaugeWeight sum-to-zero; 1/σ² broadcast-anchored); 0 in PIN mode.
    };

    /// Tight gauge-row information weight (= 1/σ²) for the SUM-TO-ZERO clock
    /// constraint.  A pure GAUGE direction (zero data information), so any positive
    /// weight fixes it exactly without biasing the estimable (between-satellite)
    /// clock structure; chosen » the rank-gate cutoff (1e-9·σmax) and « σmax so it
    /// neither trips nor inflates the spectrum the gate inspects.  Deliberately the
    /// SAME value the code-bias gauge uses (@ref kCodeBiasGaugeWeight).
    static constexpr double kClockGaugeWeight = 1.0e3;

    /// Tight gauge-row information weight (= 1/σ²) for the W6 satellite code-bias
    /// datum rows (IF pin + per-(constellation,signal) sum-to-zero).  A pure GAUGE
    /// direction (zero data information), so any positive weight fixes it exactly
    /// without biasing the estimable between-satellite DCB structure; chosen
    /// » the rank-gate cutoff (1e-9·σmax) and « σmax so it neither trips nor
    /// inflates the spectrum the gate inspects.  Deliberately the SAME value as
    /// @ref kClockGaugeWeight (sibling constants — equal by design, documented
    /// separately because they gauge different parameter families).
    static constexpr double kCodeBiasGaugeWeight = 1.0e3;

    /// Tight gauge-row information weight (= 1/σ²) for the SBC per-epoch S-basis
    /// completion rows (@ref completeSbasisDimension).  Third sibling of
    /// @ref kClockGaugeWeight / @ref kCodeBiasGaugeWeight — equal by design; a
    /// completion row pins a MEASURED (near-)null direction, so like the other
    /// two it fixes a gauge without biasing the estimable structure.  The
    /// completion refuses to emit rows when this weight would not clear the armed
    /// rank-gate cutoff by ≥ 2 decades (@ref GaugeCompletion::weight_marginal) —
    /// the K2 anti-fragility rule, so a folded row can never itself sit at the
    /// cutoff it exists to lift the direction above.
    static constexpr double kSbasisCompleteWeight = 1.0e3;

    /// Attribution/identity span threshold of the S-basis completion: a column
    /// participates in a constrained direction when its eigenvector component
    /// magnitude is ≥ this.  Looser than the drop-and-heal's 0.3 ON PURPOSE — the
    /// heal SELECTS columns to prior (a false positive costs information on a
    /// real parameter), while this only NAMES participants for events and the
    /// engage/release identity (a false positive costs a log token).  The
    /// measured FT63 GPS+Gal directions put μ-weighted E5-family components at
    /// 0.33–0.36 and the E6 member at ~0.30, which 0.3 would flicker on.
    static constexpr double kSbasisAttrSpan = 0.25;

    /// Result of @ref buildCodeBiasDatumRows: stacked W6 gauge rows for the driver
    /// to fold via NetworkEstimator::addSharedConstraintRows at
    /// @ref kCodeBiasGaugeWeight (and mirror into the RAIM-replay cap channel).
    struct CodeBiasDatumRows {
        Eigen::MatrixXd H;      ///< k × numShared gauge Jacobian (0 rows ⇒ fold nothing).
        Eigen::VectorXd z;      ///< Length-k RHS (all 0 legacy; prev-epoch targets under Card-2).
        int n_if_pin_sats{0};   ///< Satellites attempted for the IF pin (census order size).
        int n_s2z_channels{0};  ///< (constellation,signal) sum-to-zero channels.
        int n_gf_pin_sats{0};   ///< Satellites that received a GEOMETRY-FREE pin row (slant posture; 0 when off).
        int n_gf_only_pair{0};  ///< GF-pinned satellites carrying NO third band, i.e. whose code bias is now fully datum-fixed and has no estimable content left (Odijk §5.1) — a count, because "emitted nothing" and "estimated zero" look identical downstream.
        int n_family_gf_rows{0};   ///< FAMILY-GF: (satellite, partitioned secondary family) pairs that received an extra geometry-free row.
        int n_family_gf_only_pair{0}; ///< FAMILY-GF: of those, the ones whose family carries NO third band, so the ISC is the family's entire estimable content (the 11 XX− stations' world).  Scanned over the FAMILY's signal subset, never over the satellite's whole signal list — a Q-pinned satellite with X channels registered would otherwise always look like it had a third band.
        int n_sat_gf_rows{0};      ///< RF-0c: satellites that received the HARD per-satellite geometry-free datum row (@c network.sat_gf_datum).  Standing invariant #4 — under `w6_iono_constraint: "none"` this row IS the datum for the between-satellite split, so 0 here with the knob armed means the run had no datum at all.
        int n_sat_gf_tgd_hits{0};  ///< RF-0c / `broadcast_tgd`: of those rows, the ones pinned at a NON-ZERO `c*T_GD` from the navigation message.
        int n_sat_gf_tgd_zero{0};  ///< RF-0c / `broadcast_tgd`: rows whose satellite HAS a navigation record but broadcasts `T_GD = 0` exactly — physically pinned, numerically identical to `zero`.  Separated from the fallbacks so "the constellation says zero" never reads as "we could not find a value".
        int n_sat_gf_tgd_missing{0}; ///< RF-0c / `broadcast_tgd`: rows whose satellite had NO navigation record — fell back to a zero target.
        int n_sat_gf_pair_not_l1l2{0}; ///< RF-0c / `broadcast_tgd`: rows whose ionosphere-free pin pair is not an (L1, L2) band pair, so the broadcast T_GD does not describe it — fell back to a zero target rather than assert a delay for a pair the navigation message never referenced.
        int n_sat_gf_extra_rows{0};   ///< RF-0d: EXTRA per-signal geometry-free rows, one per registered code column beyond the ionosphere-free pair whose band differs from the reference.  These are what make a sparse channel's level (GPS C2X) a DATUM rather than a quantity the few stations tracking it happen to determine.
        int n_sat_gf_isc_zero{0};     ///< RF-0d / `broadcast_tgd`: extra rows that asserted an inter-signal correction of ZERO because RINEX navigation carries no ISC for GPS (only T_GD).  Counted because the assertion is invisible on the wire — it is the part of a "broadcast" datum that the navigation message does not actually state.
        int n_sat_gf_sameband_free{0};///< RF-0d: registered columns DELIBERATELY left unpinned — a channel in the SAME band as the reference has no geometry-free combination against it (the divisor is zero) and its difference is a pure ISC, observable without any ionosphere at any coverage.  Pinning it would discard estimable content.
        int n_sat_gf_extra_nofreq{0}; ///< RF-0d: extra columns skipped because no carrier frequency was available from any present station's model this epoch.
        int n_s2z_suppressed{0};      ///< RF-0d: per-(constellation, signal) sum-to-zero rows SUPPRESSED because the geometry-free datum already pins every satellite column of that channel.  Under `broadcast_tgd` the two are not merely redundant but INCONSISTENT (the pinned values sum to mu_j*c*Sum_s T_GD^s, not to zero), and both fold at the same weight — see the suppression block.
        int n_if_level_rows{0};       ///< RF-0e e1: ABSOLUTE ionosphere-free level rows folded — one per constellation with at least one satellite carrying a valid IF pin pair.  0 with the knob armed means no satellite presented a dual-frequency pair, i.e. the set level ran unanchored (standing invariant #4).
        int n_if_level_sats{0};       ///< RF-0e e1: satellites summed into those rows.
        int n_sat_gf_carried{0};      ///< RF-0e e2: geometry-free rows whose RHS came from the satellite's PREVIOUS kept-epoch solved value (a change pin).
        int n_isc_observed{0};        ///< RF-0f: band-2 same-band companion columns left UNPINNED because some present station tracks BOTH that companion and the pin's second signal on that satellite — their difference is a pure inter-signal correction and the data determines it.
        int n_isc_pinned{0};          ///< RF-0f: companion columns that still had to be pinned at 0 because NO station bridged the two signals this epoch.  The wire does not emit those biases; this count is how a run says how much of its C2X content is assertion rather than estimate.
        int n_sat_gf_anchored{0};     ///< RF-0e e2: geometry-free rows that fell back to the ABSOLUTE target because the satellite had no veteran value — the first-sighting anchor, and the only place the broadcast frame is re-asserted.
        int n_extra_band_free{0};     ///< B5D: EXTRA-BAND columns (band neither of the pin pair's) left UNPINNED because some present station carries code on both of the pair's bands AND on that signal for that satellite — with the pinned pair fixing the slant ionosphere and the clock, the data determine d_k up to the family constant the channel's sum-to-zero row datums.
        int n_extra_band_carried{0};  ///< B5D: extra-band columns the data did NOT determine this epoch but which had a previous solved value on both the reference and themselves, so the row carries their OWN previous geometry-free combination instead of the absolute broadcast target.
        int n_extra_band_held{0};     ///< B5D: extra-band columns the data did not determine at a FIRST SIGHTING — nothing to carry, so a single level row in the channel's own frame HOLDS the column (never the absolute broadcast target, which would drag a sum-to-zero family into the broadcast frame) and the bias is withheld from the wire.  Standing invariant #3: a deferral is triaged with data, so this count is what says whether the case occurs at all.
        int n_s2z_reseed_rows{0};     ///< B5D / invariant #7: per-(constellation, signal) sum-to-zero rows whose veteran link RE-SEEDED this epoch (no veteran present ⇒ the channel's LEVEL re-references).  The affected satellites are marked in @c this_epoch_cb_reseed and disc-bumped at the emit site; this is the count of the DECOUPLING edge.
        int n_s2z_reseed_sats{0};     ///< B5D / invariant #7: satellites marked by those re-seeds (the marks the consumer reacts to).
        /// B5D: the `"sat|signal"` keys of the extra-band columns HELD this epoch (see
        /// @ref n_extra_band_held).  Returned rather than re-derived by the caller because
        /// the hold verdict is a three-way branch over the determination roster AND the
        /// carry map, and two views of one truth drift apart.  Empty unless the knob is
        /// armed, so the emit prep's withholding loop is never entered otherwise.
        std::set<std::string> extra_band_held;
    };

    /// FAMILY-GF (docs/SPEC_family_gf_datum.md §3.1): a receiver channel FAMILY tag.
    /// NOT the raw RINEX attribute character — the Galileo primary family is
    /// C1C/C5Q/C7Q, whose attributes are 'C','Q','Q', so keying on the character
    /// splits one family in two (and on GPS would call C2W and C2L different
    /// families on a posture that works today).  The organizing physical fact is
    /// that an 'X' channel is the COMBINED I+Q correlator output and carries its
    /// own hardware bias, while the discrete components share a chain.
    enum class ChannelFamily : std::uint8_t {
        Primary,    ///< Discrete-component tracking (Galileo A/B/C/I/Q/Z, GPS C/W/P/Y/S/L/I/Q/M, BDS I/Q/D/P/A/Z).
        Secondary,  ///< Combined I+Q tracking ('X').
    };

    /**
     * @brief FAMILY-GF: map a (constellation, code signal) to its channel family.
     *
     * @param constellation RINEX constellation key ("E", "G", "C", …).
     * @param signal        raw 3-char RINEX code observable ("C1C", "C5X", …).
     * @return @c Secondary iff the signal is a combined-tracking ('X') channel of a
     *         constellation this card recognises; @c Primary otherwise.  An
     *         unrecognised constellation always returns @c Primary, so a
     *         constellation nobody has validated can never partition and can never
     *         start emitting rows (§7's allow-list, enforced here rather than at the
     *         call site).
     */
    static ChannelFamily channelFamilyOf(const std::string& constellation,
                                         const std::string& signal);

    /// FAMILY-GF: per-constellation verdict of the run-latched family census (§3.2).
    struct FamilyCensus {
        /// Constellations whose SECONDARY family is PARTITIONED — i.e. carries real
        /// observations and shares no station with the primary family, so the primary
        /// family's pin provably cannot reach it.  Only these get extra rows.
        std::set<std::string> partitioned;
        int n_stations_primary{0};    ///< Stations carrying only primary-family channels (summed over constellations).
        int n_stations_secondary{0};  ///< Stations carrying only secondary-family channels.
        int n_bridges{0};             ///< Stations carrying BOTH — the population whose existence makes the inter-family GF difference observable and the partition false.
        /// Human-readable per-constellation census lines, for the always-on log
        /// (invariant #4: an armed-but-inert configuration must read as such).
        std::vector<std::string> lines;
        bool latched{false};          ///< Set once the census has run; the verdict is fixed for the run (§7).
    };

    /**
     * @brief FAMILY-GF: run the family census over a live per-station channel-support map.
     *
     * @param station_signals per station, the set of "SAT|SIGNAL" code keys it holds
     *        (i.e. @c StateManager::getSatBiasStates() keys), for PRESENT stations only.
     * @return the census verdict; @c partitioned lists the constellations where a
     *         secondary family exists and @c bridges == 0 for it.
     *
     * Deliberately an independent scan rather than a rider on the CB-ADM admission
     * census: that census discards station identity (it keeps an int support count),
     * and it does not run at all on this lineage (@c network.cb_admission appears in
     * two configs repo-wide, neither in the pc305 chain), so piggybacking would mean
     * arming a different registration path — a posture change, not a free ride.
     */
    static FamilyCensus runFamilyCensus(
        const std::map<std::string, std::set<std::string>>& station_signals);

    /**
     * @brief Build the W6 satellite code-bias (OSB) datum gauge rows — relocated
     *        VERBATIM from Application::runNetwork at decomposition INC-8.
     *
     * Two row families, in a CONTRACTUAL order (byte-identity-bearing: IF pins in
     * first-seen census order over @p registered_code_keys, then sum-to-zero rows
     * in sorted "<constellation>|<signal>" channel-key order):
     *
     *  1. Per-satellite ionosphere-free pin (clock-common gauge): the two lowest
     *     DISTINCT-frequency code signals (deterministic in the signal SET via
     *     @ref selectIfPinPair) weighted a1 = f1²/(f1²−f2²), a2 = −f2²/(f1²−f2²)
     *     so the pinned combination is the one the satellite CLOCK absorbs.  A
     *     single-frequency satellite degenerates to an all-ones gauge over its
     *     registered signals.
     *  2. Per-(constellation,signal) sum-to-zero: one row per registered channel,
     *     summed over the satellites that carry it — pins every channel's absolute
     *     level per-SIGNAL (robust to non-co-tracked same-band channels at network
     *     scale), not per-band.
     *
     * Card-2 time-linking (@p cb_time_linked): both families pin the CHANGE of
     * the datum instead of its level — the IF row's RHS becomes the satellite's
     * previous solved IF combination (only when the pair is unchanged AND both
     * signals have prior values; otherwise re-seed to 0 and record the sat in
     * @p this_epoch_cb_reseed unless it is a genuine first appearance), and the
     * s2z row spans the channel's VETERANS with RHS Σ b̂_prev via
     * @ref codeS2zTimeLink.
     *
     * Clock signal-basis hold (@p clock_signal_basis_hold): HOLD the satellite's
     * first-latched reference pair while both signals stay registered; RE-LATCH
     * on loss (a genuine datum move — one "CLK_BASIS"/"relatch" event through
     * @p emitEvent, plus a pending per-sat disc when @p emit_clock_disc).  Hold
     * OFF + @p emit_clock_disc: record this epoch's selection in
     * @p this_epoch_if_pin_sel for the emit-site compare (emit-only discipline).
     * Independently of BOTH knobs, every realized pair is also recorded in
     * @p this_epoch_if_pin_real (and, under @p gf_datum, the satellite in
     * @p this_epoch_gf_pin_real) purely so the S contract can DECLARE the datum it
     * realized — the disc channel and the declaration channel are deliberately
     * separate, because gating a declaration on a QC knob is what made every field
     * posture-C product ship with no @c cbpin= token.
     *
     * The caller owns the FOLD (addSharedConstraintRows at
     * @ref kCodeBiasGaugeWeight), the RAIM-replay cap_* capture, and the first-N
     * summary log — this method is a pure row builder over const estimator
     * queries plus the five documented side channels (@p held_clock_ref
     * read/write latch; @p this_epoch_* per-epoch scratch outs;
     * @p basis_relatch_disc_pending inserts; @p emitEvent callback).
     *
     * @param est  Joint estimator — const: only numShared()/codeBiasColumn().
     * @param registered_code_keys  ≥2-station (sat,signal) OSB keys, first-seen order.
     * @param freqOf  Carrier-frequency oracle (sat,signal)→Hz, BY REFERENCE —
     *                the caller owns the station roster it closes over.
     * @param cb_time_linked            network.code_datum_time_linked.
     * @param clock_signal_basis_hold   network.clock_signal_basis_hold.
     * @param emit_clock_disc           network.emit_clock_disc.
     * @param code_bias_prev_           Card-2 carry: "<const>|<sig>" → sat → b̂_prev.
     * @param code_bias_ifpin_pair_     Card-2 carry: sat → last kept-epoch IF pair.
     * @param code_bias_sat_seen_       Card-2 carry: sats with any prior kept epoch.
     * @param code_bias_datum_resumed_  Warm-resume flag (treat re-seeds as bumps).
     * @param held_clock_ref            Basis-hold latch (read/write).
     * @param this_epoch_if_pin_sel     [out] hold-OFF disc scratch (cleared by caller).
     * @param this_epoch_if_pin_real    [out] S-contract §1.5 DECLARATION scratch: the
     *                                  realized pair per satellite, written on BOTH the
     *                                  hold and hold-OFF paths and guarded ONLY by a
     *                                  valid dual-frequency pin — never by
     *                                  @p emit_clock_disc, so a product-contract
     *                                  declaration never depends on a QC channel
     *                                  (cleared by caller).
     * @param this_epoch_gf_pin_real    [out] S-contract §1.5: satellites that also got a
     *                                  GEOMETRY-FREE row this epoch (@p gf_datum); empty
     *                                  when the GF datum is off (cleared by caller).
     * @param this_epoch_cb_ifpin       [out] Card-2 pair scratch (cleared by caller).
     * @param gf_family_constellations  FAMILY-GF: constellations whose SECONDARY channel
     *                                  family is PARTITIONED (the run-latched
     *                                  @ref runFamilyCensus verdict), for which one EXTRA
     *                                  geometry-free row per (satellite, secondary family)
     *                                  is folded — and deliberately NO extra IF row (the
     *                                  clock already spent that direction, so the family's
     *                                  IF combination is the ESTIMABLE inter-family ISC).
     *                                  EMPTY when the knob is off or nothing partitions, in
     *                                  which case not one byte of the fold changes.  The
     *                                  verdict alone is passed — never precomputed pairs —
     *                                  so the family pair is selected by @ref selectIfPinPair
     *                                  on the family-filtered signal set, through the SAME
     *                                  code path and the SAME frequency source as the IF pin.
     * @param this_epoch_cb_reseed      [out] Card-2 re-seed scratch (cleared by caller).
     * @param sat_gf_datum              RF-0c (@c network.sat_gf_datum): the HARD
     *                                  per-satellite geometry-free datum mode.
     *                                  @c NoneMode builds no row (byte-identical).
     * @param sat_gf_target_m           RF-0c: per-satellite geometry-free pin target in
     *                                  metres, used only under
     *                                  @c NetworkSatGfDatum::BroadcastTgd (@c c*T_GD,
     *                                  run-latched at setup).  A satellite absent from
     *                                  the map is pinned at 0 and counted.
     * @param sat_if_level_datum        RF-0e e1: fold one ABSOLUTE per-constellation
     *                                  sum-to-zero row on the ionosphere-free
     *                                  combination.  Inert without @p sat_gf_datum.
     * @param sat_gf_datum_time_linked  RF-0e e2: carry each geometry-free row's RHS
     *                                  from the previous kept epoch, using the
     *                                  absolute target only at first sighting.
     * @param sat_gf_extra_band_free    B5D (@c network.sat_gf_extra_band_free): fold NO
     *                                  geometry-free row for an EXTRA-BAND column (a
     *                                  registered signal whose band digit is neither of
     *                                  the pin pair's) that @p extra_band_determined
     *                                  reports as determined this epoch, so its level
     *                                  falls to the channel's sum-to-zero row and its
     *                                  between-satellite differences to the data.  An
     *                                  UNDETERMINED column is carried at its own previous
     *                                  combination, or held by a single level row in the
     *                                  channel's own frame at a first sighting — never at
     *                                  the absolute broadcast target, which is what would
     *                                  drag a sum-to-zero family into the broadcast frame.
     *                                  OFF ⇒ RF-0d's pin ⇒ byte-identical.
     * @param extra_band_determined     B5D: the `"<sat>|<signal>|<b1><b2>"` keys the data
     *                                  determine this epoch — @c b1 / @c b2 are the pin
     *                                  pair's band digits, so the roster answers the
     *                                  question for the pair the builder actually
     *                                  realized rather than for an assumed (L1, L2).
     *                                  Built by the driver (only it sees the stations),
     *                                  empty unless @p sat_gf_extra_band_free is armed.
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
     *        signals — a pure function of the signal SET and the frequency map.
     *
     * The two returned signals form the W6 satellite-clock IF pin
     * (@f$a_1+a_2=1@f$): the constant added to every one of a satellite's code
     * biases that its estimable clock absorbs is pinned by fixing this pair's
     * ionosphere-free combination, keeping the emitted OSB pure-DCB.  Because the
     * emitted @f$(\delta t^s,\,d^s)@f$ split is anchored to THIS pair, the choice
     * must not depend on registration/coverage order — otherwise a same-band
     * channel swap (C1C ↔ C1W) silently re-references the clock by the
     * satellite's inter-channel bias (a per-satellite step no consumer is told
     * about).
     *
     * Selection is a strict TOTAL order over the signal set:
     *  - signals are sorted by @b (RINEX band digit, then full signal name), so
     *    same-band channels tie-break deterministically on their name rather than
     *    on the order they happen to be passed in;
     *  - @c first  = the lowest-ordered signal;
     *  - @c second = the first later signal on a DISTINCT carrier frequency
     *    (@f$|f-f_{\text{first}}|>1@f$ Hz), or empty when the satellite has only a
     *    single distinct frequency (a degenerate single-band pin).
     *
     * @param sigs   the satellite's registered code signals, in any order (taken
     *               by value; the caller's container is not reordered).
     * @param freqOf signal → carrier frequency (Hz); a non-finite or @f$\le 0@f$
     *               result is treated as "frequency unavailable" for the
     *               distinct-frequency test on @c second.
     * @return @c {first, second}; @c second is empty when no distinct second
     *         frequency exists (or @p sigs is empty, in which case both are empty).
     */
    static std::pair<std::string, std::string> selectIfPinPair(
        std::vector<std::string> sigs,
        const std::function<double(const std::string&)>& freqOf);

    /// Result of @ref codeS2zTimeLink — the time-linked sum-to-zero target for one
    /// (constellation,signal) code-bias channel (Card 2, @c network.code_datum_time_linked).
    struct CodeS2zTimeLink {
        std::vector<std::string> jac_sats;  ///< satellites getting @c +1 in the sum-to-zero row: the veterans when ≥1 exists, else every present satellite.
        double                   rhs{0.0};  ///< RHS @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$ over @ref jac_sats (0 when @ref reseed).
        bool                     reseed{false}; ///< true ⇒ NO veteran present: the row re-seeds at @f$\sum=0@f$ (a real re-reference — the driver disc-bumps the channel's present satellites).
    };

    /**
     * @brief Compute the time-linked sum-to-zero target for one code-bias channel
     *        (Card 2): pin the CHANGE of the previous epoch's veteran mean to zero
     *        rather than the current-set level to zero.
     *
     * The legacy datum spans every present satellite with target 0, so on a rise/set
     * the target shifts by (departed bias)/@f$m'@f$ and the stiff near-zero-@f$q@f$
     * carry then relaxes the level over many epochs — a hysteretic, unsignalled
     * common-mode wander.  Time-linking instead spans the "veterans" (present
     * satellites that ALSO have a previous-epoch solved value) with RHS
     * @f$\sum_{\text{vet}}\hat b_{s,k-1}@f$: each veteran is referenced to itself, so
     * membership changes no longer move the target and new satellites take their level
     * from the network plus this row.  A channel with NO veterans (cold start, warm-
     * resume miss, or every satellite fresh) falls back to the legacy all-present
     * @f$\sum=0@f$ seed and flags @ref CodeS2zTimeLink::reseed so the driver can bump
     * the disc counters — the one path where the emitted level genuinely re-references.
     *
     * Rank is preserved: exactly one row per channel either way, and one veteran
     * suffices to pin the level gauge (the all-ones common shift changes the veteran
     * sum by @f$m_{\text{vet}}\cdot c\neq 0@f$).  Pure — no estimator/emitter state.
     *
     * @param present_sats satellites present this epoch carrying this channel
     *                      (order preserved into @ref CodeS2zTimeLink::jac_sats on the
     *                      no-veteran fallback).
     * @param prev_vals    satellite → previous-epoch solved bias for THIS channel; a
     *                      present satellite is a veteran iff it is a key here.
     * @return the row's spanned satellites, RHS, and re-seed flag.
     */
    static CodeS2zTimeLink codeS2zTimeLink(
        const std::vector<std::string>& present_sats,
        const std::map<std::string, double>& prev_vals);

    /**
     * @brief Build the SHARED-block clock datum for the constellations present in
     *        @p registered_clock_sats, per @p decl.isb_datum.
     *
     * @par SUM-TO-ZERO mode (@c decl.isb_datum == SumToZeroPerConstellation)
     * The per-constellation clock common-mode is fixed by a sum-to-zero over that
     * constellation's registered satellite clocks.  Returns a Plan with an EMPTY
     * @ref Plan::shared_datum and EMPTY @ref Plan::clock_pivot (no pivot,
     * @ref Plan::declared_dimension = 0) and one all-ones @ref Plan::clock_constraint_H
     * row per constellation for the driver to fold BEFORE the rank gate.  Pivot
     * overrides / stickiness are irrelevant and ignored.  Everything below (pivot
     * selection) applies to the legacy PIN mode only.
     *
     * @par BROADCAST-ANCHORED mode (@c decl.isb_datum == BroadcastAnchoredPerSatellite)
     * Each registered satellite clock is anchored to its own broadcast a-priori
     * (Δδtˢ → 0) by one single-entry constraint row at information
     * 1/@c decl.broadcast_anchor_sigma_m² — the soft anchors realize the gauge, so
     * @ref Plan::shared_datum and @ref Plan::clock_pivot are EMPTY and
     * @ref Plan::declared_dimension = 0.  The common-mode eigenvalue the rank gate
     * sees is 1/σ² per constellation — independent of N, since the anchor block is
     * (1/σ²)·I and the gauge direction lies in the clock subspace, so (N_ss+ΔN)·u =
     * (1/σ²)·u (≈ 0.11 at σ = 3 m, far above the relative cutoff ~1e-9·smax; but a
     * loose σ ≳ 60 m at ~305 stations drops it BELOW the cutoff — see the binding-σ
     * gate in GnssConfigLoad).  It does NOT wander when membership changes
     * (each anchor is independent — no redistribution).  Throws when the declared
     * σ is non-finite or non-positive (the datum would be undefined).
     *
     * @par PIN mode (@c decl.isb_datum == PerConstellationSatClockPin, default)
     * One satellite-clock pin per constellation.  Pivot selection per
     * constellation, in priority order:
     *  1. **Config pin** — @p decl.clock_pivot_sat, when set and present in the
     *     registered set.  Honored in BOTH modes (pinning declares user intent,
     *     so the pinned satellite reclaims the datum on its return; that
     *     flip-back is deliberate and the driver signals it as a handover).
     *  2. **Sticky incumbent** (@c network.sticky_pivot, RTA-1) — with
     *     @p sticky_prev_pivot non-null, the incumbent pivot recorded in that
     *     map, as long as it is still present in the registered set.  Only when
     *     the incumbent is absent (set / dropped) is a new pivot selected via
     *     rule 3, and that NEW pivot then becomes the sticky incumbent in turn:
     *     the old one returning does NOT flip the datum back.
     *  3. **Lexical fallback** — the first registered satellite of that
     *     constellation (stable lexical order).  With @p sticky_prev_pivot null
     *     (the default / legacy mode) this rule is re-derived EVERY epoch, so
     *     the auto-picked pivot flips whenever a lexically lower satellite
     *     rises — each flip a constellation-wide datum re-reference the product
     *     consumer pays for with an ambiguity reset.
     *
     * The phase-bias↔ambiguity gauge is fixed by LOCAL pivot-receiver ambiguity
     * pins folded by the driver, so it is intentionally absent here.
     *
     * @param est                   the estimator AFTER all register* calls (layout frozen).
     * @param registered_clock_sats every satellite for which a clock column was registered.
     * @param decl                  the S-basis convention (pivot overrides).
     * @param sticky_prev_pivot     Constellation char → incumbent pivot satellite,
     *                              i.e. the datum of the last EMITTED product that
     *                              contained that constellation (the driver's
     *                              persistent emit-baseline map, advanced only at
     *                              the emit site).  Pass @c nullptr for the legacy
     *                              per-epoch lexical auto-pick (byte-identical).
     * @return the datum plan (shared pins + declared residual dimension).
     */
    Plan buildSharedDatum(const NetworkEstimator& est,
                          const std::vector<std::string>& registered_clock_sats,
                          const SbasisDeclaration& decl,
                          const std::map<std::string, std::string>* sticky_prev_pivot
                              = nullptr) const;

    /**
     * @brief The STRUCTURAL per-constellation clock null direction @f$v_c@f$
     *        (queue 1(b), docs/SPEC_pivot_visibility_gauge_defect.md §8.1 rule 1).
     *
     * The clock gauge of a constellation is the direction documented in this
     * file's header: *adding a constant to every satellite clock of a
     * constellation and to the receivers' clock/ISB of that constellation
     * cancels on every row*.  The receiver clock/ISB are LOCAL states eliminated
     * by the Schur reduction, so inside the SHARED border the direction is
     * exactly the indicator over that constellation's registered satellite-clock
     * columns, coefficient 1.0 each.  Every other shared family carries its own
     * independent gauge — the satellite code bias by the W6 per-satellite IF pin
     * plus the per-(constellation,signal) sum-to-zero rows, the phase bias by the
     * LOCAL ambiguity pins at its anchor receiver — so their component along
     * @f$v_c@f$ is **zero** and they are not part of this direction.  (Measured:
     * across the 319260 handover of `runs/golden_sim_campaign_zwdblk_scoped` the
     * clock step is a rigid +1.8365 m while the code-bias step mean is 1e-6 m and
     * the phase-bias step mean is −0.026 m against 0.18 m of scatter — the biases
     * do not ride the clock frame.)
     *
     * Returned as (satellite, coefficient) pairs in the SAME deterministic
     * lexical order @ref buildSharedDatum groups and sorts by, so the direction a
     * caller transforms along is the one this builder actually pinned — never
     * hand-written at a call site, never an eigenvector (same-scale eigenvectors
     * mix; structural membership is exact and carries per-satellite identity).
     *
     * @param registered_clock_sats This epoch's registered satellite-clock set —
     *                              the same vector passed to @ref buildSharedDatum.
     * @param constellation         Constellation key (e.g. "G"), as produced by
     *                              @ref constellationOf.
     * @return The direction's nonzero entries; EMPTY when the constellation has
     *         no registered clock satellite (the caller must treat that as a
     *         no-op, not as a zero direction).
     */
    static std::vector<std::pair<std::string, double>> clockNullDirection(
        const std::vector<std::string>& registered_clock_sats,
        const std::string& constellation);

    /**
     * @brief Card KP — build the loose Kaula/physical-bound priors on the
     *        ADMITTED satellite code-bias (OSB) columns.
     *
     * The CONDITIONAL second lever of docs/SPEC_beidou_w6_datum.md, built after
     * its trigger fired with data: on the 305-station arm with the rank gate
     * calibrated (Card DG) and the phase drop-and-heal armed (Card PA), the
     * residual below-cutoff population is 3 declared clock gauges + ONE
     * `CB`+`CLK` direction carrying ZERO phase-bias weight — structurally out of
     * the heal's reach (it only anchors PB columns), and therefore a genuine
     * admitted-CODE deficiency.  A satellite's OSBs are physically BOUNDED (tens
     * of ns), so a loose prior toward 0 is real prior information, not a fudge:
     * it lifts that direction without meaningfully moving any well-observed OSB,
     * whose data information exceeds 1/σ² by orders of magnitude.
     *
     * @par Why PRIORS, not gauge rows (deviation from the card's stated site)
     * The card names @ref buildCodeBiasDatumRows.  That function's output is
     * folded at @ref kCodeBiasGaugeWeight — a TIGHT datum weight, which is the
     * entire point of a gauge row.  A Kaula prior is deliberately LOOSE, so
     * stacking it into the same rows would either mis-weight it or force a second
     * weight channel through a row order that is byte-identity-bearing
     * (@ref CodeBiasDatumRows documents that contract).  The diagonal
     * information-prior channel (@c NetworkEstimator::addSharedPrior) already
     * exists for exactly this and is what the phase drop-and-heal uses.  It also
     * makes the card's declared-dimension audit (K3) hold BY CONSTRUCTION: a
     * prior is not a datum row, so @c Plan::declared_dimension cannot move.
     *
     * @par Anti-fragility (K2)
     * The rejected loose-prior alternative of the CB-ADM card failed because its
     * information sat AT the rank cutoff.  The caller must therefore check that
     * @c info = 1/σ² lands at least two decades ABOVE the armed cutoff
     * (@c rank_gate_tol · max(λmax,1)) and log the margin; a prior near the
     * cutoff is exactly the configuration that card already rejected.
     *
     * @param registered_code_keys The (satellite, signal) code columns registered
     *                             this epoch — under CB-ADM these are precisely
     *                             the ADMITTED pairs with live coverage.
     * @param sigma_m              Prior standard deviation in metres (the physical
     *                             OSB bound; 1 ns ≈ 0.2998 m).  Must be > 0 —
     *                             a non-positive value yields an EMPTY list, so a
     *                             mis-set knob cannot silently fold info = ∞.
     * @return One zero-valued prior per registered code column, @c info = 1/σ².
     *         Empty ⇒ the caller folds nothing (byte-identical).
     */
    static std::vector<NetworkEstimator::SharedPrior> buildCodeBiasKaulaPriors(
        const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
        double sigma_m);

    /// Result of @ref completeSbasisDimension — the SBC per-epoch S-basis
    /// completion (@c network.sbasis_complete): the realized near-null
    /// directions BEYOND the declared structural dimension, as eigenvector
    /// gauge rows for the driver to fold (at @ref kSbasisCompleteWeight)
    /// BEFORE the rank gate, plus per-direction attribution for the
    /// engage/release event contract (standing invariant #7).
    struct GaugeCompletion {
        /// One residual (weak/undeclared) direction selected for constraint.
        struct Dir {
            double ev{ 0.0 };         ///< its eigenvalue in the inspected reduced normals (absolute, may be ~0)
            double target{ 0.0 };     ///< folded RHS = vᵀ·x_prev (the combination's carried level; 0 cold / on datum change)
            double top_span{ 0.0 };   ///< |component| of the heaviest participating column
            std::string top_label;    ///< that column's label (from the caller's labeler)
            std::set<std::string> sats;  ///< satellites of columns at |component| ≥ @ref kSbasisAttrSpan — the engage/release identity
            std::string detail;       ///< "label:span,…" over the top participants (trace attribution)
        };
        int observed{ 0 };        ///< near-null count measured pre-completion — the epoch's REALIZED S-basis dimension
        int n_extra{ 0 };         ///< directions beyond declared_dim actually constrained (= H.rows())
        int n_skipped{ 0 };       ///< extras SKIPPED as unseparated from the declared-gauge cluster (< 1 decade or at machine zero — exact orphaned gauges, Card-PA's class).  Skips are per-DIRECTION, not epoch-fatal: the separated extras above them are still constrained, the gate then throws by exactly n_skipped, and the drop-and-heal composes on what remains (measured: the FT63 gg withhold-cluster epochs carry 1 orphan + 1-2 trades — refusing the whole epoch left PA alone with both, which is precisely its PA5 insufficiency).
        bool capped{ false };     ///< extras exceeded max_extra: NOTHING emitted (caller falls through to the gate → withhold)
        bool degenerate{ false }; ///< n_skipped > 0 (informational — folding proceeds on the separated extras)
        bool weight_marginal{ false }; ///< @ref kSbasisCompleteWeight < 2 decades above the cutoff: NOTHING emitted (K2)
        Eigen::MatrixXd H;        ///< n_extra × n gauge rows (the extra directions' unit eigenvectors)
        Eigen::VectorXd z;        ///< length-n_extra continuity targets (see Dir::target)
        std::vector<Dir> dirs;    ///< per-row attribution, same order as @ref H
        std::vector<Dir> skipped; ///< attribution of the n_skipped unconstrained directions (target unused, 0) — a skipped orphan must NAME itself in the sbc_skip event, or the withhold it causes is undiagnosable from the trace (the FT63 cluster orphans carried no PB span ≥ 0.3, i.e. exactly the class the heal cannot see either)
    };

    /**
     * @brief SBC — derive the epoch's REALIZED S-basis dimension and return
     *        minimum-constraint gauge rows for every residual near-null
     *        direction beyond the declared structural dimension.
     *
     * The declared dimension counts only the STRUCTURAL gauges (per-
     * constellation clock in PIN mode; 0 in the constraint-realized clock
     * datums).  The realized per-epoch system can carry MORE: measured on
     * FT63 GPS+Galileo (63 sta, DOY-129, 2026-08), 41% of epochs held 1–3
     * additional near-null directions — per-satellite phase-bias↔slant-iono
     * trade directions confined to ONE receiver channel family (the Galileo
     * C/Q-vs-X tracking split partitions the station set, so e.g. the X-family
     * observations of one satellite form their own weakly-coupled component),
     * with eigenvector components ∝ μ_σ = f₁²/f_σ² over that family's
     * phase-bias columns and only the SH-VTEC prior (λ ≈ 1e-6 ≪ cutoff)
     * resisting once the family's code support thins.  The per-(sat,signal)
     * anchor pins close the phase-bias↔ambiguity gauge but NOT this trade, and
     * the declared dimension therefore under-counts — the gate withheld the
     * epoch or Card PA soft-anchored columns (a COLUMN prior that also touches
     * the estimable part, and misses spread directions).
     *
     * This method is the datum-derivation fix: eigen-decompose the reduced
     * shared normals with the SAME cutoff arithmetic as the gate, take the
     * near-null directions in ascending order, skip the @p declared_dim
     * smallest (the structural gauges the declared datum already pins — they
     * sit at machine-zero, decades below the weak extras), and return one
     * gauge row per REMAINING direction: @f$v^\top x = v^\top x_{\text{prev}}@f$
     * — the minimum-constraint completion of the S-basis.  Each row pins
     * exactly the deficient combination (eigenvectors are orthogonal to every
     * estimable direction at the cutoff's resolution), so the estimable
     * solution is untouched (S-transform invariance) and the pinned
     * combination HOLDS its carried level instead of stepping — the emitted
     * frame stays continuous at the engage edge, and at the release edge the
     * data re-determines the combination from the held level.
     *
     * Epoch-level refusals — the method returns attribution but NO rows, and
     * the caller falls through to the unchanged gate (→ withhold), because
     * folding would be worse than withholding:
     *  - @ref GaugeCompletion::capped — more than @p max_extra separated
     *    extras.  A mass deficiency is a pathological epoch (or a mis-set
     *    tolerance counting real directions as null); silently pinning dozens
     *    of directions would fabricate a solution.  Counted, never silent
     *    (standing invariant #3).
     *  - @ref GaugeCompletion::weight_marginal — @ref kSbasisCompleteWeight
     *    < 2 decades above the armed cutoff (K2 anti-fragility: the CB-ADM
     *    card rejected exactly the prior-at-the-cutoff configuration).
     *
     * Per-DIRECTION skip (@ref GaugeCompletion::n_skipped) — an extra
     * UNSEPARATED from the declared-gauge cluster (not ≥ 1 decade above the
     * cluster top, or below the 1e-14·λmax machine-zero floor) is skipped,
     * NOT epoch-fatal: eigenvectors mix within a degenerate cluster, so a row
     * there could re-target a combination the declared pins own — and an
     * extra AT machine zero is an orphaned EXACT gauge, which is Card PA's
     * class.  The separated extras above the skipped cluster (≥ 1 decade
     * above its top) do not mix with it and ARE constrained; the gate then
     * throws by exactly n_skipped and the drop-and-heal composes on that
     * remainder.  Measured (FT63 gg leg, Aug-2): the withhold-cluster epochs
     * carry 1 orphan + 1–2 trades — the all-or-nothing refusal left PA alone
     * with both, reproducing its PA5 insufficiency; the split guard is what
     * lets the two mechanisms each take their own class.
     *
     * Pure function of its inputs (no estimator/driver state) — the driver
     * owns the fold (@c addSharedConstraintRows at @ref kSbasisCompleteWeight),
     * the RAIM-replay capture, the engage/release events and the counters.
     *
     * @param reduced_N_ss The PRE-datum reduced shared normals — the SAME
     *                     matrix state @ref assertNullSpaceRank inspects (all
     *                     pre-gate priors/rows already folded).
     * @param declared_dim The declared STRUCTURAL dimension
     *                     (@c Plan::declared_dimension).
     * @param tol          The armed relative rank-gate tolerance
     *                     (@c NetworkEstimator::rankGateTol — the ONE value all
     *                     gate consumers share, Card DG2).
     * @param x_prev       Carried shared state mapped to this epoch's columns
     *                     (0 where no carry exists / on a datum re-reference);
     *                     an empty or wrong-length vector ⇒ all targets 0.
     * @param labelOf      Column labeler for attribution (pass
     *                     @c NetworkEstimator::sharedColumnLabel — ALWAYS
     *                     populated, unlike the diag-only col_desc).  May be
     *                     empty (labels become "col<N>").
     * @param max_extra    Cap on constrained directions per epoch
     *                     (@c network.sbasis_complete_max); ≤ 0 ⇒ uncapped.
     * @return The completion (see @ref GaugeCompletion).  @c n_extra == 0 with
     *         no refusal flag set means the epoch needs no completion —
     *         including the observed < declared case, which is the GATE's
     *         "over-specified" verdict to make, not this method's.
     */
    GaugeCompletion completeSbasisDimension(
        const Eigen::MatrixXd& reduced_N_ss, int declared_dim, double tol,
        const Eigen::VectorXd& x_prev,
        const std::function<std::string(Eigen::Index)>& labelOf,
        int max_extra) const;

    /**
     * @brief Null-space rank gate: eigen-decompose the PRE-constraint reduced
     *        shared normals and assert the count of (relatively) zero eigenvalues
     *        equals the declared S-basis dimension.
     *
     * Run after the kernel's @c reduce() (local ambiguity pins already folded and
     * eliminated) and before the datum solve.  A mismatch means the datum is
     * under- or over-specified — the producer would emit a meaningless least-norm
     * split — so this throws loudly rather than letting the solve proceed.
     *
     * The reduced normals are symmetric positive-SEMIdefinite here (the gate
     * precedes any indefinite modification), so eigenvalues equal singular values
     * and a self-adjoint eigensolve gives the same count as an SVD far more
     * cheaply.
     *
     * @param reduced_N_ss The reduced shared normal matrix @f$\tilde N_{ss}@f$.
     * @param declared_dim Expected null-space dimension (Plan::declared_dimension).
     * @param tol          Relative eigenvalue threshold (vs the largest λ).  The
     *                     driver passes @ref NetworkEstimator::rankGateTol — the
     *                     ONE resolved value the dump and the drop-and-heal also
     *                     read (Card DG2, @c network.rank_gate_tol).
     * @param observed_out [out, optional] The OBSERVED near-zero count, written
     *                     before the equality test — so a caller gets the number
     *                     on the THROWING path too (Card DG6: "over-specified"
     *                     must be countable as its own case, and the run-wide
     *                     min/max observed count is the calibration evidence).
     *                     @c nullptr (the default) is the pre-DG behaviour; the
     *                     gate's semantics are otherwise untouched — exact
     *                     equality, both-direction throw, relative cutoff.
     * @param ev_max_out   [out, optional] The largest eigenvalue, written beside
     *                     @p observed_out (so also on the THROWING path).  It is
     *                     the ONLY missing ingredient of the gate's own cutoff
     *                     @f$\text{tol}\cdot\max(\lambda_{max},1)@f$, and the gate
     *                     already computes and discards it — handing it back lets
     *                     the invariant-#7 datum-direction information ratio be
     *                     measured against the SAME bar the gate judges by,
     *                     without a second eigensolve.
     * @throws std::runtime_error with observed-vs-declared counts on mismatch.
     */
    void assertNullSpaceRank(const Eigen::MatrixXd& reduced_N_ss,
                             int declared_dim, double tol = 1e-6,
                             int* observed_out = nullptr,
                             double* ev_max_out = nullptr) const;
};

} // namespace GnssProcessor
