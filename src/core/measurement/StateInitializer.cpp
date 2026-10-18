/**
 * @file StateInitializer.cpp
 * @brief The receiver code-bias resolver and the system helpers of the first-epoch seeding
 *        (see measurement/StateInitializer.hpp; the seeds themselves are in measurement/Seeds.cpp).
 */

#include "measurement/StateInitializer.hpp"
#include "model/PppModel.hpp"
#include "products/ProductCache.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "measurement/UserParamView.hpp"   // userCodeBiasKey
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

StateInitializer::StateInitializer(const GnssConfig& config,
                                   const PppModel& ppp_model,
                                   LogManager& log_manager)
    : config_(config),
      ppp_model_(ppp_model),
      ppp_utils_(log_manager) {
}

// ===========================================================================
// System enum helpers
// ===========================================================================

std::string StateInitializer::systemToIsbName(System system) {
    // The time reference has no ISB state; an unknown system has none either.
    if (isTimeReference(system) || system == System::Unknown) return "";
    return systemName(system);
}

char StateInitializer::systemToClockRefChar(System system) {
    // The BIA keys its clock-reference pairs by RINEX letter.  An unknown system gives
    // '\0' (not the '?' of rinexLetter), which the callers test for.
    if (system == System::Unknown) return '\0';
    return rinexLetter(system);
}

// ===========================================================================
// Receiver bias resolution
// ===========================================================================

namespace {

/// Names the state of an entry: the typed key, and the text key it is stored under.  The text
/// is empty (and the entry has no key) for a key the identity tables cannot render.
void setKey(StateInitializer::ReceiverBiasEntry& entry, const RxCbKey& key) {
    entry.state_key = userCodeBiasKey(key);
    entry.key       = key;
    entry.has_key   = !entry.state_key.empty();
}

} // anonymous namespace

StateInitializer::ReceiverBiasEntry
StateInitializer::resolveReceiverBias(const std::string& obs_signal,
                                       System system,
                                       const std::string& satellite) const {
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    ReceiverBiasEntry result{ {}, NaN };

    const char sys_char = systemToClockRefChar(system);
    if (sys_char == '\0') return result;

    // -----------------------------------------------------------------------
    // Receiver inter-signal code bias of a second band-1 code (
    // docs/refound/SCC_second_code_channel_2026-09-21.md).
    //
    // With `network.second_code_channel` a station contributes every allow-listed
    // band-1 code it tracks, not only the one its canonical latch selected.  The
    // per-band keying below cannot represent that: C1C and C1W share a band digit,
    // so both resolve to "<sys>_DCB" with the same coefficient, and the second code
    // would assert a zero receiver inter-signal bias and push the whole difference
    // into the satellite column.
    //
    // A band-1 code that is not this station's latched canonical code therefore
    // gets its own per-(receiver, system, signal) state with unit coefficient: an
    // absolute receiver delay in the frame that the clock and ionosphere datum
    // leave free, not the ionosphere-referenced difference the DCB state carries.
    // The null direction this opens (every receiver inter-signal bias against
    // every satellite one) is closed by the satellite family's own time-linked
    // sum-to-zero row, so this mechanism changes no datum row (R3).
    //
    // Rows covered: both the R1 companions and a fallback code that arrives
    // without its canonical code beside it.  The test compares the signal with the
    // latch, not the way the row entered, so the two cases cannot disagree about
    // which state a row belongs to.  An undecided latch (empty: before the
    // constellation's first observed epoch, or for a band this station never
    // latched) keeps the per-band keying; a code is "second" only relative to a
    // known first.
    //
    // GLONASS is excluded: its FDMA per-slot group delay is a different state
    // family with its own datum, handled in the branch below, and "C1C" is a legal
    // GLONASS code that would otherwise match an allow-list written for GPS.  This
    // block sits before that branch and before the clock-reference lookup because
    // the inter-signal correction is a pure per-signal receiver delay and needs no
    // pin pair.
    //
    // With the knob off the whole block is skipped, so the output is byte-identical.
    //
    // The predicate generalises for the all-channel mode.  With
    // `processing.channel_mode: all` on the network path a station contributes
    // every admitted code attribute of every selected band, not an allow-listed
    // band-1 pair, and the per-band keying below would assert a zero receiver
    // inter-signal bias on each of them.  On band 2 and above the RF-0d
    // geometry-free pin also asserts the satellite side to zero, which repeats
    // the same defect on both sides.  Under the widening the test therefore
    // drops the allow-list and the band-1 restriction and keeps only what makes a
    // row a second one: an admitted code that is not that band's latched canonical
    // code, on any band digit.  The rest (unit coefficient, absolute frame,
    // GLONASS exclusion, undecided-latch fall-through) is unchanged.
    const bool chan_all = config_.networkChannelModeAll();
    const auto code = ObservableId::parse(system, obs_signal);   // e.g. C1W: code, band digit '1'
    if ((config_.networkSecondCodeChannel() || chan_all) && system != System::GLONASS
        && code && code->type == ObsType::Code
        && (chan_all ? (code->signal.digit >= '1' && code->signal.digit <= '9')
                     : code->signal.digit == '1')) {
        const auto& allow = config_.networkSecondCodeSignals();
        const bool listed =
            chan_all || std::find(allow.begin(), allow.end(), obs_signal) != allow.end();
        if (listed) {
            const std::string sys = systemName(system);
            const std::string canonical =
                ppp_model_.latchedCanonicalCodeOnBandDigit(sys, code->signal.digit);
            if (!canonical.empty() && canonical != obs_signal) {
                RxCbKey key;
                key.sys    = system;
                key.form   = ParamForm::Isc;
                key.signal = code->signal;
                setKey(result, key);
                result.coefficient = 1.0;
                return result;
            }
        }
    }

    // -----------------------------------------------------------------------
    // GLONASS per-FCN-slot receiver inter-frequency code bias.
    //
    // FDMA: each GLONASS slot transmits on a distinct carrier frequency
    // (f = f_base + k*df), so the receiver group delay differs per slot.  A
    // single per-band IFB/DCB state cannot represent that, which is why the
    // per-band path below excludes GLONASS.  With enable_glonass_ifb set, this
    // branch instead emits one bias state per (FDMA band, slot k) with unit
    // coefficient.
    //
    // Datum: the central channel k=0 is the held reference (no state).  In
    // undifferenced PPP its constant is absorbed by the GLONASS ISB (a hard
    // anchor when a k=0 satellite is up, otherwise a soft ISB/mean-IFB gauge).
    // Under BSSD these columns survive the receiver-block zeroing (two satellites
    // on different slots do not cancel), but only the inter-slot differences are
    // determined by the measurements; the common offset of the {IFB(k)} set is an
    // unobservable gauge held softly by the per-slot prior.  That gauge lies in
    // the SD null space, so its drift does not affect position.  Only the FDMA
    // bands (G1='1', G2='2') are slot-dependent; the CDMA bands (G3/G4/G6) have a
    // fixed frequency and keep the per-band treatment.
    //
    // This runs before the clock-reference lookup below because GLONASS usually
    // has no SATELLITE_CLOCK_REFERENCE_OBSERVABLES entry in the .BIA product
    // (empty reference pair), which would return early and never reach any IFB
    // logic.
    if (system == System::GLONASS && config_.enableGlonassIfb()) {
        const auto sig = SignalId::fromObsCode(system, obs_signal);
        const FrequencyBand fb = sig ? sig->band() : FrequencyBand::Unknown;
        if (fb == FrequencyBand::GloG1 || fb == FrequencyBand::GloG2) {   // FDMA bands only
            // Resolve k from the model's seeded PppUtils (RINEX header / live
            // broadcast), not from this class's own unseeded instance, whose
            // getGlonassChannel would fall back to the stale default table.
            const int k = ppp_model_.getGlonassChannel(satellite);
            if (k < -7 || k > 6) return result;      // unknown slot -> no IFB column
            if (k == 0) return result;               // central channel = held datum
            RxCbKey key;
            key.sys      = system;
            key.form     = ParamForm::GloSlotIfb;
            key.band     = fb;
            key.glo_slot = k;
            setKey(result, key);
            result.coefficient = 1.0;
        }
        return result;  // GLONASS fully handled here (per-slot, or none for CDMA/k=0)
    }

    const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
    auto it = clock_refs.find(sys_char);
    if (it == clock_refs.end()) return result;

    const auto& ref = it->second;

    // The band digits of this signal and of the clock-reference pair (ref.sig1, ref.sig2).
    const auto sig_j = SignalId::fromObsCode(system, obs_signal);
    const auto sig_1 = SignalId::fromObsCode(system, ref.sig1);
    const auto sig_2 = SignalId::fromObsCode(system, ref.sig2);
    if (!sig_j || !sig_1 || !sig_2) return result;
    const char band_j = sig_j->digit;
    const char band_1 = sig_1->digit;
    const char band_2 = sig_2->digit;

    // Non-reference band: independent IFB state, key "<system>_IFB<band>".
    // GLONASS is excluded here: its FDMA per-channel receiver delay cannot be
    // represented by one per-band IFB state shared across slots, which would bias
    // every channel.  The per-(band, slot) treatment is the enable_glonass_ifb
    // branch above; with that flag off, a non-reference GLONASS band carries no
    // IFB state and its delay is absorbed by the per-(sat, signal) bias path and
    // residual de-weighting.
    if (band_j != band_1 && band_j != band_2) {
        if (system == System::GLONASS) return result;   // no FDMA per-band IFB
        RxCbKey key;
        key.sys  = system;
        key.form = ParamForm::Ifb;
        key.band = bandOfDigit(system, band_j);
        setKey(result, key);
        if (!result.has_key) return result;   // a band digit the constellation does not use
        result.coefficient = 1.0;
        return result;
    }

    // Reference band (either band of the clock-reference pair): GF-IF coefficient
    // against the system DCB state, key "<system>_DCB".
    const double f1 = ppp_utils_.getFrequency(ref.sig1, system, satellite);
    const double f2 = ppp_utils_.getFrequency(ref.sig2, system, satellite);
    if (!(std::isfinite(f1) && f1 > 0.0 && std::isfinite(f2) && f2 > 0.0))
        return result;

    const double mu1 = 1.0;
    const double mu2 = (f1 * f1) / (f2 * f2);
    const double dmu = mu1 - mu2;
    if (std::abs(dmu) < 1e-12) return result;

    RxCbKey key;
    key.sys  = system;
    key.form = ParamForm::Dcb;
    setKey(result, key);
    result.coefficient = (band_j == band_1) ? (mu1 / dmu) : (mu2 / dmu);
    return result;
}

} // namespace GnssProcessor
