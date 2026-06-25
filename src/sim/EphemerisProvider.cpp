/**
 * @file EphemerisProvider.cpp
 * @brief Implementation of the precise SP3/CLK truth orbit/clock provider.
 */

#include "sim/EphemerisProvider.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "common/Constants.hpp"
// NOTE: ModelGeometry.hpp holds a Troposphere by value, so including it provides
// the complete Troposphere type.  We deliberately do NOT include
// "atmosphere/Troposphere.hpp" directly: as of this writing that header's guard
// is a bare `#pragma` (not `#pragma once`), so a second include in the same TU
// triggers a class redefinition.  Single-path inclusion via ModelGeometry avoids it.
#include "atmosphere/Ionosphere.hpp" // includes ModelGeometry.hpp (Troposphere via that path only)
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

namespace {
// The broadcast nav file carries usable GPS Klobuchar only if it yields EXACTLY
// 8 finite coefficients — one clean GPSA (alpha[4]) + one clean GPSB (beta[4]).
// Every coefficient load path (ProductParser::parseKlobuchar, loadKlobucharCoeffs)
// CLEARS the vector to size 0 on any count != 8 (e.g. duplicate GPSA/GPSB lines in
// merged BRDC products, or a malformed value), and
// Ionosphere::computeKlobucharDelay then indexes coeffs[0..7] WITHOUT a guard.
// So match the parser's success condition exactly: anything else -> fall back to
// the analytic quiet background rather than risk an out-of-bounds read.
bool navHasGpsKlobuchar(const std::filesystem::path& nav) {
    std::ifstream f(nav);
    if (!f.is_open()) return false;
    auto parse4finite = [](std::istringstream& iss) {
        for (int i = 0; i < 4; ++i) {
            double v;
            if (!(iss >> v) || !std::isfinite(v)) return false;
        }
        return true;
    };
    std::string line;
    int gpsa_ok = 0, gpsb_ok = 0;
    while (std::getline(f, line)) {
        if (line.find("END OF HEADER") != std::string::npos) break;
        if (line.find("IONOSPHERIC CORR") == std::string::npos) continue;
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if (tag == "GPSA" && parse4finite(iss)) ++gpsa_ok;
        else if (tag == "GPSB" && parse4finite(iss)) ++gpsb_ok;
    }
    return gpsa_ok == 1 && gpsb_ok == 1; // exactly 8 clean coeffs will load
}
} // namespace

namespace GnssProcessor {
namespace sim {

EphemerisProvider::EphemerisProvider(const std::filesystem::path& setup_obs,
                                     const std::vector<std::filesystem::path>& product_paths,
                                     const std::filesystem::path& log_dir,
                                     const std::filesystem::path& nav_path) {
    config_.setObservationPath(setup_obs);
    config_.setProductPaths(product_paths);
    if (!nav_path.empty()) config_.setNavigationPath(nav_path); // getProductPath("RNX") -> Klobuchar source
    log_.initialize(log_dir, LogLevel::Warn);

    // Mirror Application::loadInputData(): parse obs, load products, populate
    // the shared cache, then construct Geometry (whose ctor calls loadData()).
    product_parser_ = std::make_unique<ProductParser>(config_, log_);
    rinex_ = std::make_unique<RinexParser>(config_, log_, *product_parser_);
    if (!rinex_->parse()) return;

    product_parser_->loadProducts();
    ProductCache& cache = ProductCache::getInstance();
    cache.clearCache(); // singleton — ensure a clean load for this provider
    if (!cache.loadProducts(*product_parser_, log_)) return;
    if (cache.getOrbitCount() == 0 || cache.getClockCount() == 0) return;

    geometry_ = std::make_unique<Model::Geometry>(config_, cache, *rinex_, log_);
    // Same Troposphere model + config the processor uses (default standard
    // atmosphere + GMF), so synthesized ZHD/mapping cancel against the verifier.
    troposphere_ = std::make_unique<Troposphere>(config_, log_);

    // Optional broadcast Klobuchar field, reused from the processor's Ionosphere.
    // Only stand it up if the nav file actually carries GPS alpha+beta (the delay
    // routine indexes coeffs[0..7] unguarded).
    if (!nav_path.empty() && std::filesystem::exists(nav_path) && navHasGpsKlobuchar(nav_path)) {
        ionosphere_ = std::make_unique<Ionosphere>(config_, log_, *product_parser_, *rinex_);
        has_klobuchar_ = true;
    }
    ok_ = true;
}

EphemerisProvider::~EphemerisProvider() = default;

double EphemerisProvider::klobucharDelayMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                               const GnssTime& t, double freq_hz) const {
    if (!has_klobuchar_ || !ionosphere_) return 0.0;
    return ionosphere_->computeKlobucharDelay(t, rx, sat_pos, freq_hz);
}

double EphemerisProvider::tropoZhd(const Eigen::Vector3d& rx, const GnssTime& t) const {
    return troposphere_ ? troposphere_->computeSaastamoinenZHD(rx, t) : 0.0;
}

void EphemerisProvider::tropoMapping(double elev_deg, const Eigen::Vector3d& rx, const GnssTime& t,
                                     double& mf_d, double& mf_w) const {
    if (troposphere_) {
        troposphere_->getTroposphereMapping(elev_deg, rx, t, mf_d, mf_w);
    } else {
        mf_d = mf_w = 1.0 / std::sin(elev_deg * Constants::PI / 180.0);
    }
}

std::size_t EphemerisProvider::orbitNodeCount() const {
    return ProductCache::getInstance().getOrbitCount();
}

std::size_t EphemerisProvider::clockSampleCount() const {
    return ProductCache::getInstance().getClockCount();
}

bool EphemerisProvider::positionVelocity(const std::string& sat, const GnssTime& t,
                                         Eigen::Vector3d& pos, Eigen::Vector3d& vel) const {
    if (!geometry_) return false;
    auto pv = geometry_->getSatellitePV(sat, t);
    pos = pv.first;
    vel = pv.second;
    return pos.allFinite() && pos.norm() > 1.9e7; // reject zero/degenerate (no coverage)
}

double EphemerisProvider::clock(const std::string& sat, const GnssTime& t, bool* broadcast) const {
    if (!geometry_) {
        if (broadcast) *broadcast = true;
        return 0.0;
    }
    return geometry_->getCorrections().getSatelliteClockCorrection(sat, t, broadcast);
}

} // namespace sim
} // namespace GnssProcessor
