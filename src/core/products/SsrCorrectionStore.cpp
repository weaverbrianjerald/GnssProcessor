/**
 * @file SsrCorrectionStore.cpp
 * @brief Implementation of the per-satellite SSR record cache and the
 *        RAC-to-ECEF orbit-correction transform.
 */

#include "products/SsrCorrectionStore.hpp"
#include "math/TimeUtils.hpp"
#include "common/Constants.hpp"
#include <spdlog/spdlog.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace GnssProcessor {

namespace {

// Compute (query_time - record_time) in seconds, week-rollover aware.
double secondsBetween(const GnssTime& q, const GnssTime& r) {
    return TimeUtils::continuousGpsSeconds(q) - TimeUtils::continuousGpsSeconds(r);
}

bool freshEnough(const GnssTime& record, const GnssTime& query, double max_age_s) {
    if (record.week == 0 && record.tow == 0.0) return false; // never set
    if (query.week == 0 && query.tow == 0.0)   return true;  // caller doesn't know time; trust the record
    const double age = secondsBetween(query, record);
    // Symmetric tolerance.  A slightly negative age (record stamped just after
    // the query: sub-second clock drift, frame jitter) is accepted.  A record more
    // than max_age_s in the future of the query is not a correction for this
    // observation: it is the desync between live corrections and lagging
    // observations (observations 1.5 h behind fresh SSR give age ≈ −5400 s).
    // Applying it would extrapolate the SSR clock polynomial (c1·dt + c2·dt²)
    // backward over that gap and yield an invalid clock on every satellite.  It
    // is rejected, so the satellite falls back to its broadcast clock, which is
    // consistent with the observation time.
    if (age < -max_age_s) return false;  // record far in the future of the query: reject
    if (age < 0.0) return true;          // slightly negative (drift, jitter): accept
    return age <= max_age_s;
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton accessor
// ---------------------------------------------------------------------------

SsrCorrectionStore& SsrCorrectionStore::getInstance() {
    static SsrCorrectionStore instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Push API
// ---------------------------------------------------------------------------

void SsrCorrectionStore::pushOrbit(const SsrOrbitCorrection& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.orbit = rec; r.header = header;
}

void SsrCorrectionStore::pushClock(const SsrClockCorrection& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.clock = rec; r.header = header;
}

bool SsrCorrectionStore::pushFastClock(const SsrClockCorrection& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(rec.sat);
    if (it == by_sat_.end() || !it->second.clock) return false;   // refresh only, never establish
    SsrClockCorrection& c = *it->second.clock;
    // Three fields change: the value, and the two times that define its age and its
    // extrapolation base.  All other fields (c1, c2, iod_ssr, iode, orbit linkage,
    // combined_code_sigma_m, code_phase_corr, disc_counter) and the shared SatRecord
    // header belong to the slow-cadence record and are left untouched by contract.
    c.c0            = rec.c0;
    c.received_time = rec.received_time;
    c.epoch_time    = rec.epoch_time;
    return true;
}

void SsrCorrectionStore::pushCombined(const SsrCombinedCorrection& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.combined = rec; r.header = header;
}

void SsrCorrectionStore::pushCodeBias(const SsrCodeBias& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.code_bias = rec; r.header = header;

    // Trace diagnostic: the code-bias signal keys the SSR stream carries for each
    // Galileo satellite.  getCodeBias() matches the receiver's exact RINEX obs
    // code, so when a satellite shows only "C1C" (the stream omits E5a/E5b) or
    // "C5X"/"C5I" where the receiver tracks C5Q (signal-ID mismatch), the E5a/E5b
    // lookup returns 0 and the satellite-bias state has to absorb that bias.
    // LogManager registers "corrections" in the global spdlog registry.
    if (!rec.sat.empty() && rec.sat[0] == 'E') {
        if (auto lg = spdlog::get("corrections");
            lg && lg->should_log(spdlog::level::trace)) {
            std::string keys;
            for (const auto& kv : rec.bias_per_signal)
                keys += kv.first + "=" + std::to_string(kv.second) + " ";
            lg->trace("SSR-CBIAS-KEYS {}: {}", rec.sat, keys);
        }
    }
}

void SsrCorrectionStore::pushPhaseBias(const SsrPhaseBias& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.phase_bias = rec; r.header = header;
}

void SsrCorrectionStore::pushUra(const SsrUra& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.ura = rec; r.header = header;
}

void SsrCorrectionStore::pushSlantIono(const SsrSlantIono& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Leaves r.header unchanged on purpose: the slant-iono channel uses its own
    // received_time for freshness, and overwriting the header would discard the
    // IOD-SSR and provider that emitToStore stamped for this satellite in the
    // same epoch.
    by_sat_[rec.sat].slant_iono = rec;
}

void SsrCorrectionStore::pushHighRateClock(const SsrHighRateClock& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.hrclock = rec; r.header = header;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

std::optional<SsrCorrectionStore::OrbitClockCorrection>
SsrCorrectionStore::getOrbitClock(const std::string& sat,
                                  const GnssTime&    query_time,
                                  double             max_age_s) const {
    SatRecord rec_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = by_sat_.find(sat);
        if (it == by_sat_.end()) return std::nullopt;
        rec_copy = it->second;
    }

    OrbitClockCorrection out;

    // Combined record (RTCM 1060/1066/...) wins over separate orbit + clock
    // because it guarantees IOD coherence within one message.
    if (rec_copy.combined && freshEnough(rec_copy.combined->received_time, query_time, max_age_s)) {
        out.delta_rac     = rec_copy.combined->delta_rac;
        out.delta_rac_dot = rec_copy.combined->delta_rac_dot;
        out.c0            = rec_copy.combined->c0;
        out.c1            = rec_copy.combined->c1;
        out.c2            = rec_copy.combined->c2;
        out.iode             = rec_copy.combined->iode;
        // A combined message guarantees IOD coherence, so the clock shares the
        // orbit IOD, and the orbit echo is the record's own IOD-SSR and content
        // epoch (the consumer's echo check then agrees by construction).
        // (RTF-2, Card 3)
        out.clock_iode       = rec_copy.combined->iode;
        out.orbit_iod_ssr    = rec_copy.combined->iod_ssr;
        out.orbit_epoch      = rec_copy.combined->epoch_time;
        // Identity of the applied orbit.  For a combined record it equals the echo
        // above by construction (in-message coherence), so the consumer echo check
        // returns Match without a second lookup.  (Card 7)
        out.applied_orbit_iod_ssr = rec_copy.combined->iod_ssr;
        // Orbit and clock share one content epoch in a combined message.  The dt
        // base is the SSR content reference epoch, not received_time: the clock
        // polynomial and RAC rates are defined relative to the provider's epoch,
        // so applyClockToBias / applyOrbitToPosition propagate over the true age.
        out.epoch_time       = rec_copy.combined->epoch_time;
        out.orbit_epoch_time = rec_copy.combined->epoch_time;
        out.has_orbit        = true;
        out.has_clock        = true;
        out.orbit_reference_point = rec_copy.combined->reference_point;  // RTB-5(b)
    } else {
        if (rec_copy.orbit && freshEnough(rec_copy.orbit->received_time, query_time, max_age_s)) {
            out.delta_rac        = rec_copy.orbit->delta_rac;
            out.delta_rac_dot    = rec_copy.orbit->delta_rac_dot;
            out.iode             = rec_copy.orbit->iode;
            out.orbit_epoch_time = rec_copy.orbit->epoch_time;   // orbit dt base (content epoch)
            out.applied_orbit_iod_ssr = rec_copy.orbit->iod_ssr; // applied-orbit identity for the echo check (Card 7)
            out.has_orbit        = true;
            out.orbit_reference_point = rec_copy.orbit->reference_point;  // RTB-5(b)
        }
        if (rec_copy.clock && freshEnough(rec_copy.clock->received_time, query_time, max_age_s)) {
            out.c0         = rec_copy.clock->c0;
            out.c1         = rec_copy.clock->c1;
            out.c2         = rec_copy.clock->c2;
            // Separate orbit and clock subtypes (the IGS-SSR norm) can carry
            // different content epochs.  The clock polynomial uses the clock epoch
            // (its dt enters through c1 and c2), independent of any orbit record's
            // epoch.
            out.epoch_time = rec_copy.clock->epoch_time;
            out.has_clock  = true;
            // out.iode (the orbit anchor) stays as the orbit branch set it, or at
            // the -1 "unknown" default for a standalone clock: network and IGS-SSR
            // clock streams carry no orbit IODE (RT0-1).
            //
            // The clock gate keys on a separate clock_iode (RTF-2, Card 3).  A
            // standard SSR clock (iode == -1 on the wire) is referenced to the same
            // broadcast baseline as its paired orbit, so it inherits out.iode.  This
            // keeps the behaviour of the earlier single gate on out.iode: a standard
            // orbit+clock pair stays gated on the orbit IODE, and a standalone
            // network clock stays permissive (-1).  The brdc_rts producer stamps
            // clock->iode >= 0 (its own C0 baseline), which takes precedence, so the
            // consumer gates that clock on the producer's baseline rather than the
            // orbit's.  The orbit coherence echo travels on the clock record (Card 7).
            out.clock_iode    = (rec_copy.clock->iode >= 0) ? rec_copy.clock->iode : out.iode;
            out.orbit_iod_ssr = rec_copy.clock->orbit_iod_ssr;
            out.orbit_epoch   = rec_copy.clock->orbit_epoch;
            // Per-satellite combined code-correction σ and code/phase correlation
            // from the network-produced clock record (M4, contract v2).  Both are 0
            // for decoded clocks and when emit_combined_sigma is off.  A satellite on
            // a combined message never carries them, because the network producer
            // pushes separate clock records.
            out.combined_code_sigma_m = rec_copy.clock->combined_code_sigma_m;
            out.code_phase_corr       = rec_copy.clock->code_phase_corr;
        }
    }

    // High-rate clock is additive on top of the polynomial.
    if (rec_copy.hrclock && freshEnough(rec_copy.hrclock->received_time, query_time, max_age_s)) {
        out.hr_clock = rec_copy.hrclock->dc;
    }

    if (!out.has_orbit && !out.has_clock) return std::nullopt;
    return out;
}

double SsrCorrectionStore::getCodeBias(const std::string& sat,
                                       const std::string& rinex_obs_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return 0.0;
    const auto& m = it->second.code_bias->bias_per_signal;
    const auto sit = m.find(rinex_obs_code);
    if (sit != m.end()) return sit->second;
    // Exact miss: look for a same-band sibling with a different tracking
    // attribute (RTA-13).  A sibling is counted either way, so the mismatch stays
    // visible even when the fallback resolves it.  Its bias is returned only under
    // the opt-in code-domain fallback; the residual attribute DCB is then absorbed
    // by the receiver code-bias states downstream.
    if (const auto sib = findBandSibling(m, rinex_obs_code); sib != m.end()) {
        countBiasAttrMismatch(sat, rinex_obs_code, 'C');
        if (code_bias_band_fallback_) return sib->second;
    }
    return 0.0;
}

bool SsrCorrectionStore::hasCodeBias(const std::string& sat,
                                     const std::string& rinex_obs_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return false;
    const auto& m = it->second.code_bias->bias_per_signal;
    if (m.find(rinex_obs_code) != m.end()) return true;
    // With the code-domain fallback armed, a same-band sibling covers this
    // signal (RTA-13).  This has to agree with getCodeBias, so that a covered
    // signal is never both estimated and SSR-corrected.  Nothing is counted here:
    // the presence check runs per state-allocation gate, not per applied row, and
    // getCodeBias owns the counter.
    return code_bias_band_fallback_ && findBandSibling(m, rinex_obs_code) != m.end();
}

std::optional<int>
SsrCorrectionStore::getCodeBiasDiscontinuity(const std::string& sat,
                                             const std::string& rinex_obs_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return std::nullopt;
    const auto& m = it->second.code_bias->disc_per_signal;   // empty for RTCM-decoded biases
    const auto sit = m.find(rinex_obs_code);
    return (sit == m.end()) ? std::nullopt : std::optional<int>{ sit->second };
}

std::optional<int>
SsrCorrectionStore::getClockDiscontinuity(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.clock) return std::nullopt;
    return it->second.clock->disc_counter;   // 0 for RTCM-decoded / toggle-off clocks
}

std::optional<double> SsrCorrectionStore::getUra(const std::string& sat,
                                                 const GnssTime& query_time,
                                                 double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.ura) return std::nullopt;
    if (!freshEnough(it->second.ura->received_time, query_time, max_age_s)) return std::nullopt;
    return it->second.ura->ura_m;
}

std::optional<SsrSlantIono> SsrCorrectionStore::getSlantIono(const std::string& sat,
                                                             const GnssTime& query_time,
                                                             double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.slant_iono) return std::nullopt;
    if (!freshEnough(it->second.slant_iono->received_time, query_time, max_age_s))
        return std::nullopt;
    return *it->second.slant_iono;
}

// --- Age-gated code/phase-bias getters, using the file-local freshEnough (A5) ---
double SsrCorrectionStore::getCodeBias(const std::string& sat,
                                       const std::string& rinex_obs_code,
                                       const GnssTime& query_time,
                                       double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return 0.0;
    if (!freshEnough(it->second.code_bias->received_time, query_time, max_age_s)) return 0.0;
    const auto& m = it->second.code_bias->bias_per_signal;
    const auto sit = m.find(rinex_obs_code);
    if (sit != m.end()) return sit->second;
    // Exact miss on a fresh record (RTA-13).  A stale record returned 0 above and
    // is not counted, because staleness is not an attribute mismatch.  Otherwise
    // the semantics match the overload without a time argument above.
    if (const auto sib = findBandSibling(m, rinex_obs_code); sib != m.end()) {
        countBiasAttrMismatch(sat, rinex_obs_code, 'C');
        if (code_bias_band_fallback_) return sib->second;
    }
    return 0.0;
}

std::optional<SsrPhaseBiasEntry>
SsrCorrectionStore::getPhaseBias(const std::string& sat,
                                 const std::string& rinex_obs_code,
                                 const GnssTime& query_time,
                                 double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.phase_bias) return std::nullopt;
    if (!freshEnough(it->second.phase_bias->received_time, query_time, max_age_s)) return std::nullopt;
    const auto& m = it->second.phase_bias->bias_per_signal;
    const auto sit = m.find(rinex_obs_code);
    if (sit != m.end()) return sit->second;
    // Phase never falls back to a sibling, to preserve the integer property;
    // the mismatch is only counted (RTA-13).
    if (findBandSibling(m, rinex_obs_code) != m.end())
        countBiasAttrMismatch(sat, rinex_obs_code, 'L');
    return std::nullopt;
}

std::optional<int>
SsrCorrectionStore::getCodeBiasDiscontinuity(const std::string& sat,
                                             const std::string& rinex_obs_code,
                                             const GnssTime& query_time,
                                             double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return std::nullopt;
    if (!freshEnough(it->second.code_bias->received_time, query_time, max_age_s)) return std::nullopt;
    const auto& m = it->second.code_bias->disc_per_signal;
    const auto sit = m.find(rinex_obs_code);
    return (sit == m.end()) ? std::nullopt : std::optional<int>{ sit->second };
}

bool SsrCorrectionStore::hasCodeBias(const std::string& sat,
                                     const std::string& rinex_obs_code,
                                     const GnssTime& query_time,
                                     double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.code_bias) return false;
    if (!freshEnough(it->second.code_bias->received_time, query_time, max_age_s)) return false;
    const auto& m = it->second.code_bias->bias_per_signal;
    if (m.find(rinex_obs_code) != m.end()) return true;
    // Sibling coverage with the fallback armed; has to agree with the age-gated
    // getCodeBias (RTA-13; rationale at the overload without a time argument).
    return code_bias_band_fallback_ && findBandSibling(m, rinex_obs_code) != m.end();
}

std::optional<SsrPhaseBiasEntry>
SsrCorrectionStore::getPhaseBias(const std::string& sat,
                                 const std::string& rinex_obs_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.phase_bias) return std::nullopt;
    const auto& m = it->second.phase_bias->bias_per_signal;
    const auto sit = m.find(rinex_obs_code);
    if (sit != m.end()) return sit->second;
    // Phase never falls back to a sibling, to preserve the integer property;
    // the mismatch is only counted (RTA-13).
    if (findBandSibling(m, rinex_obs_code) != m.end())
        countBiasAttrMismatch(sat, rinex_obs_code, 'L');
    return std::nullopt;
}

void SsrCorrectionStore::setCodeBiasBandFallback(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    code_bias_band_fallback_ = enable;
}

bool SsrCorrectionStore::codeBiasBandFallback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_bias_band_fallback_;
}

std::map<std::string, std::uint64_t>
SsrCorrectionStore::biasAttributeMismatchCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bias_attr_mismatch_;
}

// ---------------------------------------------------------------------------
// Orbit-coherence echo verification (Card 7, RTF-2)
// ---------------------------------------------------------------------------

SsrCorrectionStore::OrbitEchoVerdict SsrCorrectionStore::classifyOrbitEcho(
    const OrbitClockCorrection&                rec,
    const std::optional<OrbitClockCorrection>& applied,
    double                                     max_skew_s) {
    // No echo on the clock record: nothing to verify.  Every standard SSR stream
    // and every stream without the Card 7 echo returns here (orbit_iod_ssr
    // defaults to -1), so the check costs nothing on those paths.
    if (!rec.has_clock || rec.orbit_iod_ssr < 0) return OrbitEchoVerdict::NotStamped;
    // The producer estimated its clock against an RTS orbit, but the consumer
    // applies no SSR orbit for this satellite, so the orbit delta that the clock
    // partly absorbed is missing.
    if (!applied || !applied->has_orbit) return OrbitEchoVerdict::NoAppliedOrbit;
    // Different orbit update identity.  IOD-SSR is 4 bits and equality is all the
    // wire supports; a wrap collision is caught by the epoch-skew clause.
    if (applied->applied_orbit_iod_ssr != rec.orbit_iod_ssr)
        return OrbitEchoVerdict::IodSsrMismatch;
    // Same IOD-SSR: compare orbit content epochs, only when the budget is armed.
    // With max_skew_s == 0 the epoch clause is off (the "default 0 = check off"
    // contract); the IOD identity check above still applies.
    if (max_skew_s > 0.0
        && std::abs(secondsBetween(applied->orbit_epoch_time, rec.orbit_epoch))
               > max_skew_s)
        return OrbitEchoVerdict::EpochSkew;
    return OrbitEchoVerdict::Match;
}

SsrCorrectionStore::OrbitEchoVerdict SsrCorrectionStore::checkOrbitEcho(
    const std::string&                         sat,
    const OrbitClockCorrection&                rec,
    const std::optional<OrbitClockCorrection>& applied) const {
    double budget = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        budget = orbit_echo_max_skew_s_;
    }
    const OrbitEchoVerdict v = classifyOrbitEcho(rec, applied, budget);
    const char* reason = nullptr;
    switch (v) {
        case OrbitEchoVerdict::NoAppliedOrbit: reason = "noorbit"; break;
        case OrbitEchoVerdict::IodSsrMismatch: reason = "iod";     break;
        case OrbitEchoVerdict::EpochSkew:      reason = "skew";    break;
        case OrbitEchoVerdict::NotStamped:
        case OrbitEchoVerdict::Match:          break;
    }
    if (reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        orbit_echo_mismatch_[sat + "|" + reason] += 1;
    }
    return v;
}

void SsrCorrectionStore::setOrbitEchoMaxSkew(double max_skew_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    orbit_echo_max_skew_s_ = max_skew_s;
}

double SsrCorrectionStore::orbitEchoMaxSkew() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return orbit_echo_max_skew_s_;
}

void SsrCorrectionStore::setCombinedSigmaAgeCoeff(double agek) {
    std::lock_guard<std::mutex> lock(mutex_);
    combined_sigma_age_coeff_ = agek;
}

double SsrCorrectionStore::combinedSigmaAgeCoeff() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return combined_sigma_age_coeff_;
}

std::map<std::string, std::uint64_t>
SsrCorrectionStore::orbitEchoMismatchCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return orbit_echo_mismatch_;
}

std::optional<GnssTime> SsrCorrectionStore::getSsrEpoch(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end()) return std::nullopt;
    const GnssTime& e = it->second.header.epoch_time;
    if (e.week == 0 && e.tow == 0.0) return std::nullopt;   // header never set
    return e;
}

std::optional<GnssTime> SsrCorrectionStore::getPhaseBiasTime(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.phase_bias) return std::nullopt;
    return it->second.phase_bias->received_time;
}

std::optional<GnssTime> SsrCorrectionStore::newestSsrEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<GnssTime> newest;
    double newest_s = -1.0;
    for (const auto& [sat, rec] : by_sat_) {
        const GnssTime& e = rec.header.epoch_time;
        if (e.week == 0 && e.tow == 0.0) continue;          // header never set
        const double s = TimeUtils::continuousGpsSeconds(e);
        if (s > newest_s) { newest_s = s; newest = e; }
    }
    return newest;
}

int SsrCorrectionStore::getSsrIod(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end()) return -1;
    return it->second.header.iod_ssr;
}

void SsrCorrectionStore::setGenerationId(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_id_ = id;
}

std::optional<uint32_t> SsrCorrectionStore::getGenerationId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_id_ == 0) return std::nullopt;   // never stamped (legacy / RTCM content)
    return generation_id_;
}

NetworkProducerStatus::State NetworkProducerStatus::parseState(const std::string& token) {
    // Wire tokens written by NetworkProductWriter::writeEpochStatus (RTA-10).  A
    // token from a newer producer that is not recognised maps to Unknown, which is
    // forward-compatible and never fatal.
    if (token == "ok")              return State::Ok;
    if (token == "partial")         return State::Partial;
    if (token == "withheld_sigma0") return State::WithheldSigma0;
    if (token == "withheld_gate")   return State::WithheldGate;
    if (token == "withheld_sh")     return State::WithheldShGate;   // Phase 2.2 SH health gate
    return State::Unknown;
}

const char* NetworkProducerStatus::stateName(State s) {
    switch (s) {
        case State::Ok:             return "ok";
        case State::Partial:        return "partial";
        case State::WithheldSigma0: return "withheld_sigma0";
        case State::WithheldGate:   return "withheld_gate";
        case State::WithheldShGate: return "withheld_sh";
        case State::Unknown:        break;
    }
    return "unknown";
}

void SsrCorrectionStore::setProducerStatus(const NetworkProducerStatus& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    producer_status_ = status;
}

std::optional<NetworkProducerStatus> SsrCorrectionStore::getProducerStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_status_;   // nullopt until a heartbeat is pushed (content without heartbeats never triggers a reaction)
}

void SsrCorrectionStore::setDeclaredRoster(const NetworkSignalDeclaration& decl) {
    std::lock_guard<std::mutex> lock(mutex_);
    declared_roster_ = decl;   // a declaration replaces the previous one entirely
}

NetworkSignalDeclaration SsrCorrectionStore::declaredRoster() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return declared_roster_;   // present == false until a producer declares one
}

bool SsrCorrectionStore::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.empty();
}

std::size_t SsrCorrectionStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.size();
}

SsrCorrectionStore::Coverage
SsrCorrectionStore::coverageFor(const std::vector<std::string>& sats,
                                const GnssTime& time, double max_age_s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Coverage cov;
    for (const auto& sat : sats) {
        ++cov.total;
        const auto it = by_sat_.find(sat);
        if (it == by_sat_.end()) continue;
        const SatRecord& rec = it->second;
        // Orbit and clock are high-rate, so they are gated on freshness (a combined
        // record takes precedence, as in getOrbitClock).  Biases update slowly, so
        // presence is enough.
        const bool comb = rec.combined && freshEnough(rec.combined->received_time, time, max_age_s);
        if (comb || (rec.orbit && freshEnough(rec.orbit->received_time, time, max_age_s)))
            ++cov.orbit;
        if (comb || (rec.clock && freshEnough(rec.clock->received_time, time, max_age_s))
                 || (rec.hrclock && freshEnough(rec.hrclock->received_time, time, max_age_s)))
            ++cov.clock;
        if (rec.code_bias)  ++cov.code_bias;
        if (rec.phase_bias) ++cov.phase_bias;
    }
    return cov;
}

std::string SsrCorrectionStore::censusSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Per system-letter counts: [orbit|combined, clock|combined, code-bias, phase-bias].
    std::map<char, std::array<int, 4>> by_sys;
    for (const auto& [sat, rec] : by_sat_) {
        if (sat.empty()) continue;
        auto& c = by_sys[sat.front()];
        if (rec.orbit || rec.combined) ++c[0];
        if (rec.clock || rec.combined) ++c[1];
        if (rec.code_bias)             ++c[2];
        if (rec.phase_bias)            ++c[3];
    }
    std::string out;
    for (const auto& [sys, c] : by_sys) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%c[o%d c%d cb%d pb%d] ",
                      sys, c[0], c[1], c[2], c[3]);
        out += buf;
    }
    if (!out.empty()) out.pop_back();   // trim trailing space
    return out.empty() ? "(empty)" : out;
}

void SsrCorrectionStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    by_sat_.clear();
    generation_id_ = 0;   // a cleared store carries no generation marker (RTA-3)
    producer_status_.reset();      // a cleared store carries no producer heartbeat (RTA-10)
    bias_attr_mismatch_.clear();   // mismatch tallies are per-run diagnostics (RTA-13)
    orbit_echo_mismatch_.clear();  // per-run diagnostics too; the armed skew budget is configuration and is kept (Card 7)
}

// ---------------------------------------------------------------------------
// Static apply helpers
// ---------------------------------------------------------------------------

Eigen::Vector3d SsrCorrectionStore::applyOrbitToPosition(
    const Eigen::Vector3d& brdc_pos,
    const Eigen::Vector3d& brdc_vel,
    const Eigen::Vector3d& delta_rac,
    const Eigen::Vector3d& delta_rac_dot,
    double                 dt) {
    // Build the RTCM RAC frame from broadcast position + velocity.
    const double v_norm = brdc_vel.norm();
    if (v_norm < 1e-3) return brdc_pos; // degenerate; fall back to broadcast

    const Eigen::Vector3d e_along  = brdc_vel / v_norm;
    Eigen::Vector3d cross_v        = brdc_pos.cross(brdc_vel);
    const double cross_norm        = cross_v.norm();
    if (cross_norm < 1e-3) return brdc_pos;
    const Eigen::Vector3d e_cross  = cross_v / cross_norm;
    const Eigen::Vector3d e_radial = e_along.cross(e_cross);

    // Propagate the correction to the query epoch.
    const Eigen::Vector3d delta_t = delta_rac + delta_rac_dot * dt;

    // delta_t is in (radial, along, cross) order; transform to ECEF.
    Eigen::Vector3d delta_xyz =
        e_radial * delta_t.x() + e_along * delta_t.y() + e_cross * delta_t.z();

    // RTCM SSR sign convention: precise = broadcast − delta.
    return brdc_pos - delta_xyz;
}

double SsrCorrectionStore::applyClockToBias(double c0, double c1, double c2,
                                            double hr_clock, double dt) {
    const double dC_m = c0 + c1 * dt + c2 * dt * dt + hr_clock; // metres
    return dC_m / Constants::SPEED_OF_LIGHT;                     // -> seconds
}

} // namespace GnssProcessor
