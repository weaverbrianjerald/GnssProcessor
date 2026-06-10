/**
 * @file SsrCorrectionStore.cpp
 * @brief Implementation of the per-satellite SSR record cache and the
 *        RAC-to-ECEF orbit-correction transform.
 */

#include "products/SsrCorrectionStore.hpp"
#include "math/TimeUtils.hpp"
#include "common/Constants.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

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
    if (age < 0.0) return true;          // record arrived "after" query (clock drift); accept
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
        out.iode          = rec_copy.combined->iode;
        out.epoch_time    = rec_copy.combined->received_time;
        out.has_orbit     = true;
        out.has_clock     = true;
    } else {
        if (rec_copy.orbit && freshEnough(rec_copy.orbit->received_time, query_time, max_age_s)) {
            out.delta_rac     = rec_copy.orbit->delta_rac;
            out.delta_rac_dot = rec_copy.orbit->delta_rac_dot;
            out.iode          = rec_copy.orbit->iode;
            out.epoch_time    = rec_copy.orbit->received_time;
            out.has_orbit     = true;
        }
        if (rec_copy.clock && freshEnough(rec_copy.clock->received_time, query_time, max_age_s)) {
            out.c0         = rec_copy.clock->c0;
            out.c1         = rec_copy.clock->c1;
            out.c2         = rec_copy.clock->c2;
            if (!out.has_orbit) out.epoch_time = rec_copy.clock->received_time;
            out.has_clock  = true;
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
    return (sit == m.end()) ? 0.0 : sit->second;
}

std::optional<SsrPhaseBiasEntry>
SsrCorrectionStore::getPhaseBias(const std::string& sat,
                                 const std::string& rinex_obs_code) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end() || !it->second.phase_bias) return std::nullopt;
    const auto& m = it->second.phase_bias->bias_per_signal;
    const auto sit = m.find(rinex_obs_code);
    if (sit == m.end()) return std::nullopt;
    return sit->second;
}

std::optional<GnssTime> SsrCorrectionStore::getSsrEpoch(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end()) return std::nullopt;
    const GnssTime& e = it->second.header.epoch_time;
    if (e.week == 0 && e.tow == 0.0) return std::nullopt;   // header never set
    return e;
}

int SsrCorrectionStore::getSsrIod(const std::string& sat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end()) return -1;
    return it->second.header.iod_ssr;
}

bool SsrCorrectionStore::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.empty();
}

std::size_t SsrCorrectionStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.size();
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
