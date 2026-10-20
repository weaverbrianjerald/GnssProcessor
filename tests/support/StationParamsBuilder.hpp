#pragma once
/**
 * @file StationParamsBuilder.hpp
 * @brief Add parameters to a test station's set by the texts a test knows them by.
 *
 * A test that needs a station with columns calls these on the station's @c params, as the
 * station's births do in a run: the parameter is appended (so the call order is the column
 * order) and registered in its typed map.  Satellites and signals are given as RINEX texts
 * ("G05", "L1C"); a text that does not name an identity fails the test.
 */
#include "network/state/StationParams.hpp"

#include <gtest/gtest.h>

#include <string>

namespace gnss_test {

/// The satellite of a text; a failed expectation, and the default satellite, when it is not one.
inline GnssProcessor::SatId satOf(const std::string& text) {
    const auto sat = GnssProcessor::SatId::parse(text);
    EXPECT_TRUE(sat.has_value()) << text;
    return sat.value_or(GnssProcessor::SatId{});
}

/// The signal of a text on a constellation.
inline GnssProcessor::SignalId signalOf(GnssProcessor::System sys, const std::string& code) {
    const auto sig = GnssProcessor::SignalId::fromObsCode(sys, code);
    EXPECT_TRUE(sig.has_value()) << code;
    return sig.value_or(GnssProcessor::SignalId{});
}

/// The receiver code-bias keys a test names.
inline GnssProcessor::RxCbKey dcbKey(GnssProcessor::System sys) {
    GnssProcessor::RxCbKey k;
    k.form = GnssProcessor::ParamForm::Dcb;
    k.sys  = sys;
    return k;
}
/// The inter-frequency bias of the band with RINEX digit @p digit.
inline GnssProcessor::RxCbKey ifbKey(GnssProcessor::System sys, char digit) {
    GnssProcessor::RxCbKey k;
    k.form = GnssProcessor::ParamForm::Ifb;
    k.sys  = sys;
    k.band = GnssProcessor::bandOfDigit(sys, digit);
    return k;
}
/// The GLONASS inter-frequency bias of one band and frequency channel.
inline GnssProcessor::RxCbKey slotIfbKey(GnssProcessor::System sys, char digit, int slot) {
    GnssProcessor::RxCbKey k;
    k.form     = GnssProcessor::ParamForm::GloSlotIfb;
    k.sys      = sys;
    k.band     = GnssProcessor::bandOfDigit(sys, digit);
    k.glo_slot = slot;
    return k;
}
/// The inter-signal code bias of one code ("C1W").
inline GnssProcessor::RxCbKey iscKey(GnssProcessor::System sys, const char* code) {
    GnssProcessor::RxCbKey k;
    k.form   = GnssProcessor::ParamForm::Isc;
    k.sys    = sys;
    k.signal = signalOf(sys, code);
    return k;
}

/// Appends a parameter at value @p x.
/// @return Its slot index, or -1 when the set did not take it.
inline int bornParam(GnssProcessor::StationParams& set, const GnssProcessor::ParamRef& ref,
                     double x = 0.0) {
    GnssProcessor::LocalSlot slot;
    slot.ref   = ref;
    slot.x_lin = x;
    GnssProcessor::addParam(set, slot);
    return GnssProcessor::findParam(set, ref);
}

/// Appends the float ambiguity of a satellite signal ("G05", "L1C").
inline int bornAmbiguity(GnssProcessor::StationParams& set, const std::string& sat,
                         const std::string& phase, double x = 0.0) {
    const GnssProcessor::SatId s = satOf(sat);
    return bornParam(set, GnssProcessor::ambiguityRef(s, signalOf(s.sys, phase)), x);
}

/// Appends the slant ionosphere of a satellite.
inline int bornSlantIono(GnssProcessor::StationParams& set, const std::string& sat, double x = 0.0) {
    return bornParam(set, GnssProcessor::slantIonoRef(satOf(sat)), x);
}

/// Appends the phase colour of a satellite with its elevation (degrees).
inline int bornPhaseColour(GnssProcessor::StationParams& set, const std::string& sat,
                           double elevation_deg, double x = 0.0) {
    const int k = bornParam(set, GnssProcessor::phaseColourRef(satOf(sat)), x);
    if (k >= 0) set.slots[static_cast<std::size_t>(k)].elevation_deg = elevation_deg;
    return k;
}

/// Appends a receiver code bias.
inline int bornRxCodeBias(GnssProcessor::StationParams& set, const GnssProcessor::RxCbKey& key,
                          double x = 0.0) {
    return bornParam(set, GnssProcessor::rxCodeBiasRef(key), x);
}

/// Appends the receiver phase bias of a constellation signal ("GPS" system, "L1C").
inline int bornRxPhaseBias(GnssProcessor::StationParams& set, GnssProcessor::System sys,
                           const std::string& phase, double x = 0.0) {
    return bornParam(set, GnssProcessor::rxPhaseBiasRef(sys, signalOf(sys, phase)), x);
}

/// Appends the clock of a satellite.
inline int bornSatClock(GnssProcessor::StationParams& set, const std::string& sat, double x = 0.0) {
    return bornParam(set, GnssProcessor::satClockRef(satOf(sat)), x);
}

/// Appends the phase bias of a satellite signal.
inline int bornSatPhaseBias(GnssProcessor::StationParams& set, const std::string& sat,
                            const std::string& phase, double x = 0.0) {
    const GnssProcessor::SatId s = satOf(sat);
    return bornParam(set, GnssProcessor::satPhaseBiasRef(s, signalOf(s.sys, phase)), x);
}

/// Appends the code bias of a satellite signal ("G05", "C1C").
inline int bornSatCodeBias(GnssProcessor::StationParams& set, const std::string& sat,
                           const std::string& code, double x = 0.0) {
    const GnssProcessor::SatId s = satOf(sat);
    return bornParam(set, GnssProcessor::satCodeBiasRef(s, signalOf(s.sys, code)), x);
}

}  // namespace gnss_test
