#include "sim/OtlField.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace GnssProcessor {
namespace sim {

namespace {

/// Monotone time key; plain tow would run backwards at a week rollover.
inline double gpsSeconds(const GnssTime& t) {
    return static_cast<double>(t.week) * 604800.0 + t.tow;
}

constexpr double kPi  = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;
constexpr double kR2D = 180.0 / kPi;

/**
 * @brief (amplitude, Greenwich phase lag [deg]) -> complex z = A*exp(-i*phi).
 *
 * Exact inverse of the conversion @c OceanTideModel::parseEot20Load applies to
 * the EOT20 grid (@c A = sqrt(re^2+im^2), @c phi = atan2(-im, re)), so a
 * round trip through this pair is the identity to within rounding.
 */
inline void toComplex(double amp, double phase_deg, double& re, double& im) {
    const double p = phase_deg * kD2R;
    re =  amp * std::cos(p);
    im = -amp * std::sin(p);
}

inline void fromComplex(double re, double im, double& amp, double& phase_deg) {
    amp       = std::sqrt(re * re + im * im);
    phase_deg = std::atan2(-im, re) * kR2D;
}

/// Blend one (amplitude, phase) component pair through the complex plane.
inline void lerpComponent(double amp_a, double phase_a,
                          double amp_b, double phase_b, double f,
                          double& amp_out, double& phase_out) {
    double re_a, im_a, re_b, im_b;
    toComplex(amp_a, phase_a, re_a, im_a);
    toComplex(amp_b, phase_b, re_b, im_b);
    fromComplex(re_a + (re_b - re_a) * f,
                im_a + (im_b - im_a) * f,
                amp_out, phase_out);
}

} // namespace

OtlStationCoefficients lerpOtlCoefficients(const OtlStationCoefficients& a,
                                           const OtlStationCoefficients& b,
                                           double f) {
    // Endpoints (and NaN) return the anchor itself rather than a round trip
    // through the complex plane, so anchor epochs are bit-exact.
    if (!(f > 0.0)) return a;
    if (f >= 1.0)   return b;

    OtlStationCoefficients out;
    out.station_name = "EOT20-alongtrack";
    out.valid        = a.valid && b.valid;

    for (std::size_t j = 0; j < out.constituents.size(); ++j) {
        const OtlConstituentCoeffs& ca = a.constituents[j];
        const OtlConstituentCoeffs& cb = b.constituents[j];
        OtlConstituentCoeffs& co = out.constituents[j];

        lerpComponent(ca.amp_up,    ca.phase_up,    cb.amp_up,    cb.phase_up,    f,
                      co.amp_up,    co.phase_up);
        lerpComponent(ca.amp_west,  ca.phase_west,  cb.amp_west,  cb.phase_west,  f,
                      co.amp_west,  co.phase_west);
        lerpComponent(ca.amp_south, ca.phase_south, cb.amp_south, cb.phase_south, f,
                      co.amp_south, co.phase_south);
    }
    return out;
}

OtlField::OtlField(LogManager& log)
    : log_(log), model_(log) {}

bool OtlField::build(const std::filesystem::path& eot20_nc,
                     const std::vector<TrackSample>& track,
                     double anchor_spacing_km,
                     const Eigen::Vector3d& frozen_site) {
    auto log = log_.getLogger("ocean_tide");

    anchors_.clear();
    memo_valid_ = false;
    frozen_     = OtlStationCoefficients{};

    if (track.empty()) {
        log->error("OTL field: cannot build from an empty truth track");
        return false;
    }
    if (!(anchor_spacing_km > 0.0)) {
        log->error("OTL field: anchor spacing must be > 0 km (got {})", anchor_spacing_km);
        return false;
    }

    // The frozen set is sampled at frozen_site, independently of the track, so the
    // reported "what the processor models" term does not assume that the RINEX
    // APPROX POSITION coincides with the first truth epoch.
    if (!model_.parseEot20Load(eot20_nc.string(), frozen_site, frozen_)) {
        log->error("OTL field: EOT20 synthesis failed at the frozen site "
                   "({:.3f}, {:.3f}, {:.3f})",
                   frozen_site.x(), frozen_site.y(), frozen_site.z());
        return false;
    }

    // Place anchors by travelled distance: first sample, every spacing_km of
    // chord length thereafter, and always the last sample.  A static track
    // collapses to a single anchor, which reproduces the frozen behaviour.
    const double spacing_m = anchor_spacing_km * 1000.0;
    std::vector<std::size_t> idx;
    idx.push_back(0);
    double since_anchor_m = 0.0;
    double track_len_m    = 0.0;
    for (std::size_t i = 1; i < track.size(); ++i) {
        const double step = (track[i].ecef - track[i - 1].ecef).norm();
        track_len_m    += step;
        since_anchor_m += step;
        if (since_anchor_m >= spacing_m) {
            idx.push_back(i);
            since_anchor_m = 0.0;
        }
    }
    if (idx.back() != track.size() - 1) idx.push_back(track.size() - 1);

    anchors_.reserve(idx.size());
    for (std::size_t k : idx) {
        Anchor a;
        a.time  = track[k].time;
        a.gps_s = gpsSeconds(track[k].time);
        a.ecef  = track[k].ecef;
        if (!model_.parseEot20Load(eot20_nc.string(), a.ecef, a.coeffs)) {
            log->error("OTL field: EOT20 synthesis failed at track sample {} (tow {:.1f})",
                       k, a.time.tow);
            anchors_.clear();
            return false;
        }
        anchors_.push_back(std::move(a));
    }

    log->info("OTL field: {} anchors over {:.1f} km of track ({:.1f} km spacing); "
              "M2 up {:.4f} m -> {:.4f} m, phase {:+.1f} deg -> {:+.1f} deg",
              anchors_.size(), track_len_m / 1000.0, anchor_spacing_km,
              anchors_.front().coeffs.constituents[0].amp_up,
              anchors_.back().coeffs.constituents[0].amp_up,
              anchors_.front().coeffs.constituents[0].phase_up,
              anchors_.back().coeffs.constituents[0].phase_up);

    return true;
}

const OtlStationCoefficients& OtlField::coefficientsAt(const GnssTime& t,
                                                       const Eigen::Vector3d& rx) {
    if (anchors_.empty()) return frozen_;
    const double key = gpsSeconds(t);
    // The blend depends on both the epoch and the position, so the memo keys on both.
    // Every call within an epoch currently passes the same rx (one truth position per
    // epoch, reused across satellites), but a time-only key would return a stale blend
    // if a caller re-queried an epoch at a different position.
    if (memo_valid_ && memo_gps_s_ == key && memo_rx_ == rx) return memo_coeffs_;

    if (anchors_.size() == 1) {
        memo_coeffs_ = anchors_.front().coeffs;
    } else {
        // Bracket in time (monotone, so the pair is always the temporally adjacent one
        // even where the meander doubles back on itself)...
        auto it = std::upper_bound(anchors_.begin(), anchors_.end(), key,
                                   [](double v, const Anchor& an) { return v < an.gps_s; });
        if (it == anchors_.begin()) {
            memo_coeffs_ = anchors_.front().coeffs;   // before the arc: clamp
        } else if (it == anchors_.end()) {
            memo_coeffs_ = anchors_.back().coeffs;    // at/after the last anchor: clamp
        } else {
            const Anchor& lo = *(it - 1);
            const Anchor& hi = *it;
            // ...but blend by where the vessel is within that segment, not by when.
            // Anchors are placed by distance, so a time fraction equals a distance
            // fraction only at constant speed, and a landfall trajectory can decelerate
            // to a stop, where the last segment spans minutes but little distance.
            // Projecting rx onto the anchor chord is exact for a straight segment,
            // approximate on a curved one, and holds the coefficients fixed while the
            // vessel is stationary.
            const Eigen::Vector3d seg = hi.ecef - lo.ecef;
            const double seg_len2 = seg.squaredNorm();
            double f = 0.0;
            if (seg_len2 > 1.0) {                      // > 1 m^2; below that the anchors
                f = (rx - lo.ecef).dot(seg) / seg_len2; // coincide and the blend is moot
                f = std::min(1.0, std::max(0.0, f));
            }
            memo_coeffs_ = lerpOtlCoefficients(lo.coeffs, hi.coeffs, f);
        }
    }

    memo_gps_s_ = key;
    memo_rx_    = rx;
    memo_valid_ = true;
    return memo_coeffs_;
}

Eigen::Vector3d OtlField::evaluate(const OtlStationCoefficients& coeffs,
                                   const Eigen::Vector3d& rx, const GnssTime& t) {
    // Same evaluator the processor runs: identical Doodson arguments, nodal
    // corrections and (Up, West, South)->ECEF rotation.  Only the coefficients move.
    model_.setStationCoefficients(coeffs);
    return model_.computeDisplacement(rx, t);
}

Eigen::Vector3d OtlField::displacementEcef(const Eigen::Vector3d& rx, const GnssTime& t) {
    return evaluate(coefficientsAt(t, rx), rx, t);
}

Eigen::Vector3d OtlField::frozenDisplacementEcef(const Eigen::Vector3d& rx, const GnssTime& t) {
    return evaluate(frozen_, rx, t);
}

} // namespace sim
} // namespace GnssProcessor
