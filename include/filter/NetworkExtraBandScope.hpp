/**
 * @file NetworkExtraBandScope.hpp
 * @brief The EXTRA-BAND primitive shared by card L5P's two knobs
 *        (docs/refound/L5P_l5_phase_posture_2026-09-21.md).
 *
 * A three-band lineage carries one signal per band per station (the canonical
 * latch), so "the station's primary pair" and "the pair whose two signals are
 * both on the constellation's ionosphere-free pin bands" name the same two
 * signals.  Card B5D already works in that vocabulary: an EXTRA-BAND signal is
 * one whose band is neither band of the pin pair, and on the RF-0hv GPS posture
 * that is exactly band 5 against the pinned L1/L2.
 *
 * The pin pair itself is the BIA product's satellite clock-reference pair, which
 * is the same source @c StateInitializer::resolveReceiverBias uses to decide
 * whether a receiver code bias is a reference-band DCB or an extra-band IFB, and
 * the same one @c PppModel reads for the clock-ref band lock.  Reading it here
 * keeps one definition of "the pin pair's bands" across the producer.
 *
 * Two knobs consume it, each in its own seam:
 *
 *  - `network.cycle_slip.mw_detect_pairs` (L5P knob A) scopes which Melbourne-
 *    Wubbena pairs the network forms, in the PASS-A slip scan of
 *    `app/ApplicationNetworkAssembly.cpp`;
 *  - `network.extra_band_code_sigma_scale` (L5P knob B) scales the code sigma of
 *    an extra-band signal, where the row weights are formed in
 *    @c Application::networkSplitStation_ of the same file.
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
 * Band DIGITS, not band names: '1' and '2' for GPS on the RF-0hv posture, '1'
 * and '5' for Galileo.  A default-constructed value is INVALID and means "this
 * constellation's pin pair could not be resolved", which every consumer must
 * treat as "change nothing" rather than as "every band is extra".
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
 * .BIA file, e.g. `G C1W C2W`) and returns their two band digits.  A const read
 * of a map that is filled once at product load and never mutated during the
 * epoch loop, so it is safe from the per-station parallel produce — the same
 * contract @c StateInitializer::resolveReceiverBias already relies on.
 *
 * @param system_char RINEX system character ('G', 'E', 'R', 'C', 'J', 'S').
 * @return The pin bands, or an INVALID value when the constellation has no
 *         clock-reference entry or its signals are malformed.
 */
NetworkPinBands networkPinBandsFor(char system_char);

/**
 * @brief Is @p signal on a band that is neither band of the pin pair?
 *
 * Pure.  The B5D definition of an extra-band signal, applied to one RINEX
 * observation code.  An INVALID @p pin answers false for every signal: nothing
 * is extra-band when the pin pair is unknown, so a consumer keeps its
 * pre-knob behaviour rather than acting on a guess.
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
 * Pure.  True only when BOTH signals sit on a pin band, which on a lineage that
 * latches one signal per band is the station's primary pair and nothing else.
 * An INVALID @p pin answers true for every pair: the scope cannot be applied
 * without knowing the pin bands, and dropping every pair would silence the
 * detector rather than narrow it.  The caller counts that case.
 *
 * @param sig1 First phase signal of the pair, e.g. "L1C".
 * @param sig2 Second phase signal of the pair, e.g. "L5Q".
 * @param pin  The constellation's pin bands (@ref networkPinBandsFor).
 * @return True when the pair is the primary pair (or the pin is unknown).
 */
bool networkMwPairInPrimaryScope(const std::string& sig1, const std::string& sig2,
                                 const NetworkPinBands& pin);

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_EXTRA_BAND_SCOPE_HPP
