/**
 * @file SlipBridgeKernel.hpp
 * @brief Non-causal cycle-slip integer recovery from two-sided GF + MW windows.
 *
 * Given a flagged reset boundary, this kernel re-derives geometry-free (GF) and
 * Melbourne-Wübbena (MW) series on both sides of the boundary from the resident
 * raw RINEX record, fits each with the @ref JumpGapFitter, and attributes the
 * carrier-phase step to a per-frequency integer slip @f$(dN_1, dN_2)@f$.  This
 * is the offline (non-causal) analogue of the causal MW/TurboEdit detectors: it
 * uses epochs after the boundary, which a streaming detector cannot, and is
 * intended for the smoother cycle-slip bridge.  No production code calls it yet
 * (the knob processing.enable_noncausal_slip_bridge is parsed but not read); its
 * caller is tests/test_slip_bridge_kernel.cpp.
 *
 * Attribution algebra (sig1 = higher frequency by construction):
 *   - GF step:  @f$ \lambda_1 dN_1 - \lambda_2 dN_2 = \Delta\text{GF} @f$
 *   - MW step:  @f$ (dN_1 - dN_2)\,\lambda_{WL} = \Delta\text{MW} @f$
 *   ⇒ @f$ dN_{WL} = \text{round}(\Delta\text{MW}/\lambda_{WL}) @f$,
 *     @f$ dN_2 = \text{round}\big((\Delta\text{GF} - \lambda_1 dN_{WL})/(\lambda_1-\lambda_2)\big) @f$,
 *     @f$ dN_1 = dN_2 + dN_{WL} @f$.
 *
 * The GF/MW per-epoch formulas mirror the causal kernels
 * (@c TurboEditResolver::computeGfMeters, @c MwSlipDetector::computeMwMeters).
 * The @f$\lambda_1 dN_{WL}@f$ term in the @f$dN_2@f$ numerator is the step most
 * easily written wrong; integer-injection unit tests cover it.
 */
#pragma once

#include "common/GnssTypes.hpp"   // System
#include "rinex/RinexParser.hpp"  // RinexObservation
#include "slip/JumpGapFitter.hpp"

#include <string>
#include <vector>

namespace GnssProcessor {

class PppUtils;  // borrowed; carries the GLONASS FDMA slot map, so no FCN is recomputed here

/**
 * @brief Recovers a per-frequency integer cycle-slip from two-sided windows.
 *
 * Holds a const reference to a @ref PppUtils for frequency/wavelength lookups
 * (so GLONASS FDMA is resolved from the receiver's slot map).  Stateless apart
 * from that reference; reusable and thread-safe for reads.
 */
class SlipBridgeKernel {
public:
    /// Recovered integer slip and the supporting fit diagnostics.
    struct RecoveredSlip {
        bool ok{false};          ///< True iff a slip was confidently recovered.
        int dN1{0};              ///< Integer slip on the higher-frequency signal (@c sig1).
        int dN2{0};              ///< Integer slip on the lower-frequency signal (@c sig2).
        int dN_wl{0};            ///< Wide-lane integer @f$ dN_1 - dN_2 @f$.
        double frac_resid_cyc{0};///< Max rounding residual (cycles) across GF / WL.
        double gf_step_m{0};     ///< Fitted GF step (metres).
        double mw_step_m{0};     ///< Fitted MW step (metres).
        double lambda_wl_m{0};   ///< Wide-lane wavelength (metres).
        std::string sig1;        ///< Higher-frequency phase code actually used.
        std::string sig2;        ///< Lower-frequency phase code actually used.
    };

    /// Two-sided GF / MW windows bracketing a boundary, in metres vs seconds.
    struct BridgeWindows {
        JumpGapFitter::Window gf_pre;   ///< GF samples before the boundary.
        JumpGapFitter::Window gf_post;  ///< GF samples at/after the boundary.
        JumpGapFitter::Window mw_pre;   ///< MW samples before the boundary.
        JumpGapFitter::Window mw_post;  ///< MW samples at/after the boundary.
        double t_boundary_s{0};         ///< Boundary epoch (continuous GPS seconds).
        bool ok{false};                 ///< True iff all four windows are non-empty.
    };

    /// @param ppp_utils Borrowed frequency/wavelength provider (FDMA slots set).
    explicit SlipBridgeKernel(const PppUtils& ppp_utils) : ppp_utils_(ppp_utils) {}

    /**
     * @brief Build two-sided GF/MW windows for one satellite about a boundary.
     *
     * Filters @p all_obs to @p sat, computes GF and MW per epoch from the raw
     * RINEX @c measurements, and partitions the nearest @p half_window_epochs
     * samples on each side of @p t_boundary_s (pre: t < boundary, post:
     * t ≥ boundary).  Signal/code pairs are reordered internally to
     * higher-frequency-first.
     *
     * @param all_obs           Full resident observation record.
     * @param sat               Satellite ID (e.g. "G01").
     * @param system            GNSS system enum for @p sat.
     * @param sigA,sigB         Phase obs codes of the pair (e.g. "L1C","L2W"); order-free.
     * @param codeA,codeB       Code obs codes paired with @p sigA,sigB (e.g. "C1C","C2W").
     * @param t_boundary_s      Boundary epoch (continuous GPS seconds).
     * @param half_window_epochs Max samples per side.
     * @return Populated windows; @c ok is false if frequencies are degenerate
     *         or any side ends up empty.
     */
    BridgeWindows buildWindows(const std::vector<RinexObservation>& all_obs,
                               const std::string& sat, System system,
                               const std::string& sigA, const std::string& sigB,
                               const std::string& codeA, const std::string& codeB,
                               double t_boundary_s, int half_window_epochs) const;

    /**
     * @brief Recover the integer slip from prepared windows.
     *
     * @param w            Windows from buildWindows() (or hand-built, hi-freq-first).
     * @param sat          Satellite ID (for FDMA wavelength lookup).
     * @param system       GNSS system enum.
     * @param sigA,sigB    Phase obs codes (order-free; reordered hi-first internally).
     * @param fitter       Two-sided fitter (its @c min_window_points gates each side).
     * @param frac_accept_cyc Max per-integer rounding residual to accept (default 0.30).
     * @param max_abs_cycles  Sanity cap on |dN| (default 1000).
     * @return Recovered slip; @c ok is false if either fit fails, frequencies are
     *         degenerate, the rounding residual exceeds @p frac_accept_cyc, the
     *         integers exceed the cap, or both integers are zero.
     */
    RecoveredSlip recover(const BridgeWindows& w, const std::string& sat,
                          System system, const std::string& sigA,
                          const std::string& sigB, const JumpGapFitter& fitter,
                          double frac_accept_cyc = 0.30,
                          int max_abs_cycles = 1000) const;

private:
    /// Resolved higher/lower-frequency ordering for a signal pair.
    struct Ordered {
        bool ok{false};
        std::string sig_hi, sig_lo, code_hi, code_lo;
        double f_hi{0}, f_lo{0}, lam_hi{0}, lam_lo{0};
    };
    /// Order a (sig,code) pair higher-frequency-first; validates distinct bands.
    Ordered resolveOrder(System system, const std::string& sat,
                         const std::string& sigA, const std::string& codeA,
                         const std::string& sigB, const std::string& codeB) const;

    const PppUtils& ppp_utils_;
};

}  // namespace GnssProcessor
