/**
 * @file ExtraBandScope.hpp
 * @brief The extra-band primitive shared by the two knobs of the L5 phase posture
 *        (docs/refound/L5P_l5_phase_posture_2026-09-21.md).
 *
 * A three-band lineage carries one signal per band per station (the canonical
 * latch), so "the station's primary pair" and "the pair whose two signals are
 * both on the constellation's ionosphere-free pin bands" name the same two
 * signals.  Card B5D uses the same vocabulary: an extra-band signal is one whose
 * band is neither band of the pin pair; on the RF-0hv GPS posture that is band 5
 * against the pinned L1/L2.
 *
 * The pin pair is the BIA product's satellite clock-reference pair, the same source
 * @c StateInitializer::resolveReceiverBias uses to decide whether a receiver code
 * bias is a reference-band DCB or an extra-band IFB, and the one @c PppModel reads
 * for the clock-ref band lock.  Reading it here keeps one definition of "the pin
 * pair's bands" across the producer.
 *
 * Two knobs consume it, both in `network/driver/ApplicationNetworkAssembly.cpp`:
 *
 *  - `network.cycle_slip.mw_detect_pairs` (L5P knob A) scopes which Melbourne-
 *    Wubbena pairs the network forms, in the PASS-A slip scan;
 *  - `network.extra_band_code_sigma_scale` (L5P knob B) scales the code sigma of
 *    an extra-band signal, where the row weights are formed in
 *    @c Application::networkSplitStation_.
 *
 * @ref networkPinBandsFor is also read by network/solve/ClockLevel.cpp.
 *
 * The predicates are pure and are unit-tested directly
 * (tests/test_network_extra_band_scope.cpp); only @ref networkPinBandsFor
 * touches global state, and it only reads the product cache.
 */
#ifndef GNSS_PROCESSOR_NETWORK_EXTRA_BAND_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_EXTRA_BAND_SCOPE_HPP

#include <string>

namespace GnssProcessor {

/**
 * @struct NetworkPinBands
 * @brief The two RINEX band digits of one constellation's ionosphere-free pin pair.
 *
 * Band digits, not band names: '1' and '2' for GPS on the RF-0hv posture, '1'
 * and '5' for Galileo.  A default-constructed value is invalid and means "this
 * constellation's pin pair could not be resolved"; every consumer treats it as
 * "change nothing", not as "every band is extra".
 */
struct NetworkPinBands {
    char band1{ '\0' };   ///< First pin band digit, e.g. '1'.
    char band2{ '\0' };   ///< Second pin band digit, e.g. '2'.

    /// True when both digits were resolved and are distinct.
    bool valid() const {
        return band1 != '\0' && band2 != '\0' && band1 != band2;
    }
};

/**
 * @brief Resolve a constellation's ionosphere-free pin bands from the BIA product.
 *
 * Reads the satellite clock-reference observables of the process-wide
 * @c ProductCache (the `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` header of the
 * .BIA file, e.g. `G C1W C2W`) and returns their two band digits.  This is a const
 * read of a map that is filled once at product load and not mutated during the
 * epoch loop, so it is safe from the per-station parallel produce (the same
 * contract @c StateInitializer::resolveReceiverBias relies on).
 *
 * @param system_char RINEX system character ('G', 'E', 'R', 'C', 'J', 'S').
 * @return The pin bands, or an invalid value when the constellation has no
 *         clock-reference entry or its signals are malformed.
 */
NetworkPinBands networkPinBandsFor(char system_char);

/**
 * @brief Is @p signal on a band that is neither band of the pin pair?
 *
 * Pure.  The B5D definition of an extra-band signal, applied to one RINEX
 * observation code.  An invalid @p pin answers false for every signal: nothing
 * is extra-band when the pin pair is unknown, so a consumer keeps its
 * pre-knob behaviour instead of acting on a guess.
 *
 * @param signal RINEX observation code, e.g. "C5Q" or "L1W".
 * @param pin    The constellation's pin bands (@ref networkPinBandsFor).
 * @return True when the signal's band digit is neither pin band.
 */
bool networkSignalIsExtraBand(const std::string& signal, const NetworkPinBands& pin);

/**
 * @brief May the network form the Melbourne-Wubbena pair (@p sig1, @p sig2)
 *        under `network.cycle_slip.mw_detect_pairs: primary`?
 *
 * Pure.  True when both signals sit on a pin band; on a lineage that latches one
 * signal per band, that is the station's primary pair and no other.  An invalid
 * @p pin answers true for every pair: the scope cannot be applied without the pin
 * bands, and dropping every pair would silence the detector instead of narrowing
 * it.  The slip-scan caller does not count this case separately.
 *
 * @param sig1 First phase signal of the pair, e.g. "L1C".
 * @param sig2 Second phase signal of the pair, e.g. "L5Q".
 * @param pin  The constellation's pin bands (@ref networkPinBandsFor).
 * @return True when the pair is the primary pair (or the pin is unknown).
 */
bool networkMwPairInPrimaryScope(const std::string& sig1, const std::string& sig2,
                                 const NetworkPinBands& pin);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the extra-band code deweight.
 *
 * The extra-band code deweight: code rows folded at the scaled sigma and rows
 * left unscaled, summed from the stations into the report's own two counts (they were
 * summed into the run context).
 *
 * Gate: returns 0 at once unless `config_.networkExtraBandCodeSigmaScale() > 1.0`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkExtraBandScopeRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_EXTRA_BAND_SCOPE_HPP
