/**
 * @file NetworkSbasis.hpp
 * @brief Explicit minimum-constraint S-basis for the PPP-RTK network producer
 *        (W4-S2b / W5): the datum *declaration* + the shared-block clock datum
 *        builder + a null-space rank gate.
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
 *    is gauged identically: the datum is realized as a SHARED hard pin on one
 *    satellite's clock column per constellation (@ref NetworkEstimator::Datum).
 *    No system is anchored to a receiver clock; each station's receiver
 *    clock/ISB stays a free local state that absorbs the pinned satellite's lump.
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
 * Hence the SHARED datum this builder emits is exactly the per-constellation
 * clock pins; after the local ambiguity pins are eliminated, the reduced shared
 * normals have precisely @c #constellations residual deficiencies, which the
 * @ref assertNullSpaceRank gate verifies before the solve.
 *
 * Not a state owner (respects the StateManager-is-single-source-of-truth rule):
 * a pure, per-epoch builder operating on the live registered column set.
 */

#pragma once

#include <Eigen/Dense>

#include <map>
#include <string>
#include <vector>

#include "filter/NetworkEstimator.hpp"

namespace GnssProcessor {

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
    /// Ambiguity datum convention: the pivot receiver's ambiguities are pinned
    /// (the estimable phase bias carries that lump).  Recorded for the consumer.
    enum class AmbDatum { PivotReceiverZeroAmbiguity } amb_datum{ AmbDatum::PivotReceiverZeroAmbiguity };
    /// Inter-system / clock-gauge datum: every constellation (GPS included) has
    /// its clock gauge fixed by a hard pin on one of its OWN satellite clocks; no
    /// system is anchored to a receiver clock, and each station's receiver
    /// clock/ISB stays a free local state.  (Enumerator names the realized basis,
    /// mirroring the sibling AmbDatum / CodeBiasDatum style.)
    enum class IsbDatum { PerConstellationSatClockPin }
        isb_datum{ IsbDatum::PerConstellationSatClockPin };
    /// Satellite bias sign convention (matches kSsr*BiasSign at MeasurementModel).
    double bias_sign{ +1.0 };
    /// Declared wind-up / yaw-attitude model id (producer and consumer must agree;
    /// "nominal" = yaw-steering from sun/velocity geometry, eclipse AR ungated).
    std::string windup_yaw_model_id{ "nominal" };
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
        NetworkEstimator::Datum shared_datum;            ///< SHARED pins: one sat-clock pin per constellation.
        int declared_dimension{ 0 };                     ///< Expected residual shared null-space dim (= shared_datum.size()).
        std::map<std::string, std::string> clock_pivot;  ///< constellation char -> chosen pivot sat (for the declaration + handover detection).
    };

    /**
     * @brief Map a RINEX satellite id to its constellation key (first char).
     * @return "G","E","C","R","J","S","I" or the first char as-is.
     */
    static std::string constellationOf(const std::string& sat) {
        return sat.empty() ? std::string{} : std::string(1, sat.front());
    }

    /**
     * @brief Build the SHARED-block datum: one satellite-clock pin per
     *        constellation present in @p registered_clock_sats.
     *
     * Pivot selection per constellation, in priority order:
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
     * @param tol          Relative eigenvalue threshold (vs the largest λ).
     * @throws std::runtime_error with observed-vs-declared counts on mismatch.
     */
    void assertNullSpaceRank(const Eigen::MatrixXd& reduced_N_ss,
                             int declared_dim, double tol = 1e-6) const;
};

} // namespace GnssProcessor
