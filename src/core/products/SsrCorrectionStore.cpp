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
    // Symmetric tolerance.  A mildly-negative age (record stamped a touch after
    // the query — sub-second clock drift, frame jitter) is fine.  But a record
    // from far in the FUTURE relative to the query is NOT a correction for this
    // observation: it is the live-corrections-vs-lagging-obs desync (obs ~1.5 h
    // behind fresh SSR → age ≈ −5400 s).  Applying it extrapolates the SSR
    // clock backward over that gap and detonates (c1·dt + c2·dt² → "zero/invalid
    // clock product" on every satellite).  Reject it so the satellite falls back
    // to its (self-consistent, obs-time) broadcast clock instead.
    if (age < -max_age_s) return false;  // record far in the future vs query → REJECT
    if (age < 0.0) return true;          // mild negative (drift/jitter) → accept
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

void SsrCorrectionStore::pushCombined(const SsrCombinedCorrection& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.combined = rec; r.header = header;
}

void SsrCorrectionStore::pushCodeBias(const SsrCodeBias& rec, const SsrHeader& header) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& r = by_sat_[rec.sat]; r.code_bias = rec; r.header = header;

    // Diagnostic (trace): the code-bias signal keys the SSR stream actually
    // carries per Galileo satellite.  getCodeBias() matches the receiver's exact
    // RINEX obs code, so if a sat shows only "C1C" (stream omits E5a/E5b) or
    // "C5X"/"C5I" where the receiver tracks C5Q (sig-id mismatch), the E5a/E5b
    // lookup returns 0 and the sat-bias state must cover it.  "corrections" is
    // registered in the global spdlog registry by LogManager.
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
        // Orbit + clock share one content epoch in a combined message.  The dt
        // base is the SSR CONTENT reference epoch (not received_time): the clock
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
            out.has_orbit        = true;
            out.orbit_reference_point = rec_copy.orbit->reference_point;  // RTB-5(b)
        }
        if (rec_copy.clock && freshEnough(rec_copy.clock->received_time, query_time, max_age_s)) {
            out.c0         = rec_copy.clock->c0;
            out.c1         = rec_copy.clock->c1;
            out.c2         = rec_copy.clock->c2;
            // Separate orbit/clock subtypes (the IGS-SSR norm) can carry different
            // content epochs; the clock polynomial MUST use the clock epoch (its
            // dt is c1/c2-sensitive), independent of any orbit record's epoch.
            out.epoch_time = rec_copy.clock->epoch_time;
            out.has_clock  = true;
            // out.iode is intentionally left as the orbit branch set it (or the
            // -1 "unknown" default when this is a STANDALONE clock subtype — the
            // network / IGS-SSR clock stream carries no orbit IODE).  Leaving it
            // -1 keeps the downstream iodeMatches() gate permissive; stamping {0}
            // here would collide with a real broadcast IODE and silently drop
            // every network clock in live RT (populated live store, brdc_iode>=0;
            // post-processing has an empty store so brdc_iode=-1 masked the bug).
            // Full per-record IODE stamping is the clock-anchor contract (RTF-2).
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
    // RTA-13: exact miss — is a same-band different-attribute sibling present?
    // Count it either way (the mismatch stays visible even when the fallback
    // resolves it); return the sibling's bias only under the opt-in CODE-domain
    // fallback (the residual attribute DCB is absorbed by the receiver
    // code-bias states downstream).
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
    // RTA-13: with the CODE-domain fallback armed, a same-band sibling COVERS
    // this signal (must agree with getCodeBias so a covered signal is never
    // both estimated and SSR-corrected).  No counting here — the presence
    // check runs per state-allocation gate, not per applied row; getCodeBias
    // owns the counter.
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

// --- A5: age-gated code/phase-bias getters (reuse the file-local freshEnough) ---
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
    // RTA-13: exact miss on a FRESH record (a stale record is a staleness
    // problem, not an attribute mismatch — not counted).  Same semantics as the
    // no-time overload above.
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
    // RTA-13: PHASE never falls back (integer property) — count-only.
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
    // RTA-13: fallback-armed sibling coverage — must agree with the aged
    // getCodeBias (see the no-time overload for rationale).
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
    // RTA-13: PHASE never falls back (integer property) — count-only.
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
    // RTA-10 wire tokens (NetworkProductWriter::writeEpochStatus).  Unknown tokens
    // from a NEWER producer surface as Unknown — forward-compatible, never fatal.
    if (token == "ok")              return State::Ok;
    if (token == "partial")         return State::Partial;
    if (token == "withheld_sigma0") return State::WithheldSigma0;
    if (token == "withheld_gate")   return State::WithheldGate;
    return State::Unknown;
}

const char* NetworkProducerStatus::stateName(State s) {
    switch (s) {
        case State::Ok:             return "ok";
        case State::Partial:        return "partial";
        case State::WithheldSigma0: return "withheld_sigma0";
        case State::WithheldGate:   return "withheld_gate";
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
    return producer_status_;   // nullopt until a heartbeat is pushed (legacy content never reacts)
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
        // Orbit/clock are high-rate: gate on freshness (combined wins, as in
        // getOrbitClock).  Biases update slowly — presence is enough.
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
    generation_id_ = 0;   // RTA-3: a cleared store carries no generation marker
    producer_status_.reset();      // RTA-10: a cleared store carries no producer heartbeat
    bias_attr_mismatch_.clear();   // RTA-13: mismatch tallies are per-run diagnostics
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
