/**
 * @file PivotElection.cpp
 * @brief The clock pivot's election by predicted station coverage: the ranking, the coverage
 *        predictor and the setup builder (declarations in network/solve/PivotElection.hpp).
 */

#include "network/solve/PivotElection.hpp"

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Interpolate.hpp"
#include "math/TimeUtils.hpp"
#include "model/Planetary.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/state/RunContext.hpp"
#include "products/ProductCache.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace GnssProcessor {

namespace {

/// Orbit nodes interpolated per position. Ten nodes of a 5 or 15 minute product give a
/// position good to far better than the kilometre the coverage count needs.
constexpr std::size_t kWindowNodes = 10;

/// A window whose nodes span more than this (s) straddles a hole in the product and is refused.
constexpr double kMaxWindowSpanS = 21600.0;

/// Norm bounds (m) of a believable satellite position, as the geometry stage applies.
constexpr double kMinPosNorm = 1.0e6;
constexpr double kMaxPosNorm = 1.0e8;

/// Upper bound on the samples of one horizon; keeps one election cheap and a typo harmless.
constexpr std::size_t kMaxSamples = 2000;

GnssTime gnssTimeOf(double t_gps_s) {
    GnssTime t;
    t.week = static_cast<int>(std::floor(t_gps_s / 604800.0));
    t.tow  = t_gps_s - static_cast<double>(t.week) * 604800.0;
    return t;
}

}  // namespace

std::size_t PivotCoverageParams::sampleCount() const {
    if (!(step_s > 0.0) || !(horizon_s >= 0.0)) return 1;
    const double n = std::floor(horizon_s / step_s + 1e-9) + 1.0;
    return static_cast<std::size_t>(std::min(n, static_cast<double>(kMaxSamples)));
}

double PivotRanking::meanStations(const PivotCandidateScore& s) const {
    return samples > 0 ? static_cast<double>(s.station_samples) / static_cast<double>(samples) : 0.0;
}

const PivotCandidateScore* PivotRanking::winner() const {
    if (sat.empty()) return nullptr;
    for (const PivotCandidateScore& s : table)
        if (s.sat == sat) return &s;
    return nullptr;
}

std::string PivotRanking::describe() const {
    const PivotCandidateScore* w = winner();
    std::size_t n_umbra = 0, n_no_orbit = 0;
    for (const PivotCandidateScore& s : table) {
        if (!s.has_orbit) ++n_no_orbit;
        else if (s.entersUmbra()) ++n_umbra;
    }
    if (!w)
        return fmt::format("no candidate elected: {} candidate(s), {} without an orbit",
                           table.size(), n_no_orbit);
    // The runner-up is the winner of the same ranking without the winner.
    PivotRanking rest = *this;
    rest.table.erase(std::remove_if(rest.table.begin(), rest.table.end(),
                                    [&](const PivotCandidateScore& s) { return s.sat == w->sat; }),
                     rest.table.end());
    rankPivotCandidates(rest);
    std::string runner = "none";
    if (const PivotCandidateScore* r = rest.winner())
        runner = fmt::format("{} ({:.1f} stations)", r->sat, meanStations(*r));
    return fmt::format("{} elected at {:.1f} of {} stations on average over {} sample(s); "
                       "runner-up {}; {} candidate(s), {} entering the umbra, {} without an orbit{}",
                       w->sat, meanStations(*w), stations, samples, runner, table.size(), n_umbra,
                       n_no_orbit,
                       all_shadowed ? "; every candidate enters the umbra" : "");
}

void rankPivotCandidates(PivotRanking& ranking) {
    ranking.sat.clear();
    ranking.all_shadowed = false;
    bool any_orbit = false;
    bool any_clear = false;
    for (const PivotCandidateScore& s : ranking.table) {
        if (!s.has_orbit) continue;
        any_orbit = true;
        if (!s.entersUmbra()) any_clear = true;
    }
    if (!any_orbit) return;
    ranking.all_shadowed = !any_clear;

    // a is preferred to b: out of the umbra for the whole horizon first (when any is), else
    // the later umbra entry; then the larger station count; then the lexically first id.
    const auto preferred = [&](const PivotCandidateScore& a, const PivotCandidateScore& b) {
        if (!any_clear && a.umbra_in_s != b.umbra_in_s) return a.umbra_in_s > b.umbra_in_s;
        if (a.station_samples != b.station_samples) return a.station_samples > b.station_samples;
        return a.sat < b.sat;
    };
    const PivotCandidateScore* best = nullptr;
    for (const PivotCandidateScore& s : ranking.table) {
        if (!s.has_orbit) continue;
        if (any_clear && s.entersUmbra()) continue;
        if (!best || preferred(s, *best)) best = &s;
    }
    if (best) ranking.sat = best->sat;
}

NetworkPivotCoverage::NetworkPivotCoverage(const PivotCoverageParams& params,
                                           std::vector<Eigen::Vector3d> stations,
                                           std::map<std::string, std::vector<OrbitNode>> orbits,
                                           SunFn sun)
    : params_(params),
      stations_(std::move(stations)),
      orbits_(std::move(orbits)),
      sun_(std::move(sun)) {
    stations_up_.reserve(stations_.size());
    for (const Eigen::Vector3d& r : stations_) {
        const double n = r.norm();
        stations_up_.push_back(n > 0.0 ? Eigen::Vector3d(r / n) : Eigen::Vector3d::UnitZ());
    }
}

std::optional<Eigen::Vector3d> NetworkPivotCoverage::positionAt(const std::string& sat,
                                                                double t_gps_s) const {
    const auto it = orbits_.find(sat);
    if (it == orbits_.end()) return std::nullopt;
    const std::vector<OrbitNode>& nodes = it->second;
    if (nodes.size() < 4) return std::nullopt;
    if (t_gps_s < nodes.front().t_gps_s || t_gps_s > nodes.back().t_gps_s) return std::nullopt;

    // A window of consecutive nodes anchored on the bracket of the query, clipped at the ends.
    const std::size_t n = std::min(kWindowNodes, nodes.size());
    const auto upper = std::upper_bound(
        nodes.begin(), nodes.end(), t_gps_s,
        [](double t, const OrbitNode& node) { return t < node.t_gps_s; });
    const std::size_t after = static_cast<std::size_t>(upper - nodes.begin());
    std::size_t lo = after >= n / 2 ? after - n / 2 : 0;
    if (lo + n > nodes.size()) lo = nodes.size() - n;
    if (nodes[lo + n - 1].t_gps_s - nodes[lo].t_gps_s > kMaxWindowSpanS) return std::nullopt;

    // Each node is turned into the Earth-fixed frame of the query before the interpolation,
    // so the polynomial fits a near-inertial track instead of one carrying the Earth's rotation.
    std::vector<double> x(n), yx(n), yy(n), yz(n);
    for (std::size_t i = 0; i < n; ++i) {
        const OrbitNode& node = nodes[lo + i];
        const double dt    = node.t_gps_s - t_gps_s;
        const double alpha = Constants::EARTH_ANGULAR_VELOCITY * dt;
        const double cs = std::cos(alpha);
        const double sn = std::sin(alpha);
        x[i]  = dt;
        yx[i] = cs * node.pos.x() - sn * node.pos.y();
        yy[i] = sn * node.pos.x() + cs * node.pos.y();
        yz[i] = node.pos.z();
    }
    const std::vector<double> w = Interpolate::barycentricWeights(x);
    const Eigen::Vector3d pos(Interpolate::barycentricEval(x, w, yx, 0.0),
                              Interpolate::barycentricEval(x, w, yy, 0.0),
                              Interpolate::barycentricEval(x, w, yz, 0.0));
    if (!pos.allFinite()) return std::nullopt;
    const double norm = pos.norm();
    if (norm <= kMinPosNorm || norm >= kMaxPosNorm) return std::nullopt;
    return pos;
}

PivotRanking NetworkPivotCoverage::rank(double t_gps_s, const std::string& constellation,
                                        const std::vector<std::string>& candidates) const {
    const auto memo = memo_.find(constellation);
    if (memo != memo_.end() && memo->second.t_gps_s == t_gps_s &&
        memo->second.candidates == candidates)
        return memo->second.ranking;

    PivotRanking out;
    out.constellation = constellation;
    out.t_gps_s       = t_gps_s;
    out.samples       = params_.sampleCount();
    out.stations      = stations_.size();

    // The Sun at each sample, shared by every candidate.
    std::vector<Eigen::Vector3d> sun(out.samples);
    for (std::size_t k = 0; k < out.samples; ++k)
        sun[k] = sun_ ? sun_(t_gps_s + static_cast<double>(k) * params_.step_s)
                      : Eigen::Vector3d::Zero();

    const double sin_mask = std::sin(params_.elevation_mask_deg * Constants::PI / 180.0);
    out.table.reserve(candidates.size());
    for (const std::string& sat : candidates) {
        PivotCandidateScore s;
        s.sat       = sat;
        s.has_orbit = positionAt(sat, t_gps_s).has_value();
        if (s.has_orbit) {
            for (std::size_t k = 0; k < out.samples; ++k) {
                const double dt = static_cast<double>(k) * params_.step_s;
                const std::optional<Eigen::Vector3d> p = positionAt(sat, t_gps_s + dt);
                if (!p) continue;   // beyond the product: the sample counts for nothing
                if (!s.entersUmbra() &&
                    Planetary::isSatelliteEclipsing(*p, sun[k], params_.shadow_scale))
                    s.umbra_in_s = dt;
                for (std::size_t j = 0; j < stations_.size(); ++j) {
                    const Eigen::Vector3d d = *p - stations_[j];
                    if (d.dot(stations_up_[j]) >= sin_mask * d.norm()) ++s.station_samples;
                }
            }
        }
        out.table.push_back(std::move(s));
    }
    rankPivotCandidates(out);

    Memo& m      = memo_[constellation];
    m.t_gps_s    = t_gps_s;
    m.candidates = candidates;
    m.ranking    = out;
    return out;
}

const PivotRanking* NetworkPivotCoverage::last(const std::string& constellation) const {
    const auto it = memo_.find(constellation);
    return it == memo_.end() ? nullptr : &it->second.ranking;
}

void NetworkPivotCoverage::noteElection(const PivotRanking& ranking) {
    elections_.push_back(PivotElectionRecord{ranking});
}

void networkPivotCoverageArm(NetworkRun& run, LogManager& log_manager) {
    // ---- From the run ----
    const NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkSetupResult& setup = run.setup;
    // ---- End of the bindings ----
    if (ctx.knobs.pivot_election == 0) return;

    PivotCoverageParams params;
    params.horizon_s          = ctx.knobs.pivot_election_horizon_s;
    params.step_s             = ctx.knobs.pivot_election_step_s;
    params.elevation_mask_deg = config_.getElevationMaskDeg();
    params.shadow_scale       = config_.getEclipseShadowScale();

    std::vector<Eigen::Vector3d> stations;
    stations.reserve(setup.stations.size());
    for (const NetStation& st : setup.stations) stations.push_back(st.ecef);

    // Orbit nodes of the product cache, grouped by satellite and sorted by time. A run whose
    // orbits are broadcast records has none here.
    const ProductCache& cache = ProductCache::getInstance();
    std::map<std::string, std::vector<NetworkPivotCoverage::OrbitNode>> orbits;
    for (const OrbitData& o : cache.getOrbits()) {
        if (o.satellite.empty() || !o.position_valid || !o.position.allFinite()) continue;
        const double norm = o.position.norm();
        if (norm <= kMinPosNorm || norm >= kMaxPosNorm) continue;
        orbits[o.satellite].push_back({TimeUtils::continuousGpsSeconds(o.time), o.position});
    }
    for (auto& [sat, nodes] : orbits)
        std::sort(nodes.begin(), nodes.end(),
                  [](const NetworkPivotCoverage::OrbitNode& a, const NetworkPivotCoverage::OrbitNode& b) {
                      return a.t_gps_s < b.t_gps_s;
                  });

    // The Sun from the geometry stage's own model, so the umbra test sees the Sun the rest of
    // the run sees. The model and the ERP table are held by the closure.
    auto planetary = std::make_shared<Planetary>(log_manager, config_.enableCioFrame(),
                                                 config_.enableSunFrameCorrection());
    const std::shared_ptr<const std::vector<ErpData>> erps = cache.getErpsShared();
    NetworkPivotCoverage::SunFn sun = [planetary, erps](double t_gps_s) {
        Eigen::Vector3d sun_ecef, moon_ecef;
        planetary->computeSunMoonPosition(gnssTimeOf(t_gps_s), *erps, sun_ecef, moon_ecef);
        return sun_ecef;
    };

    const std::size_t n_orbit_sats = orbits.size();
    setup.pivot_coverage = std::make_unique<NetworkPivotCoverage>(
        params, std::move(stations), std::move(orbits), std::move(sun));
    log->info("Network: clock pivot election by coverage: {} roster station(s), orbit nodes of "
              "{} satellite(s), horizon {:.0f} s in steps of {:.0f} s ({} sample(s)), elevation "
              "mask {:.1f} deg, umbra scale {:.2f}.",
              setup.pivot_coverage->stationCount(), n_orbit_sats, params.horizon_s, params.step_s,
              params.sampleCount(), params.elevation_mask_deg, params.shadow_scale);
    if (n_orbit_sats == 0)
        log->warn("Network: network.pivot_election has no orbit nodes in the product cache "
                  "(broadcast orbits?), so no candidate can be ranked and every election falls "
                  "back to the lexical first satellite.");
}

}  // namespace GnssProcessor
