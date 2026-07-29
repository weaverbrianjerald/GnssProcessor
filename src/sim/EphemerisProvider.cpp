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
#include "atmosphere/GlobalIonosphereMap.hpp" // IONEX quiet background (opt-in)
#include "atmosphere/Ionosphere.hpp" // includes ModelGeometry.hpp (Troposphere via that path only)
#include "model/Antenna.hpp"         // satellite antenna PCO/PCV (reuse; brings System enum)
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
        // Fortran 'D' exponents -> 'E', matching what the real loaders now do
        // (Ionosphere.cpp / ProductParser.cpp).  This predicate MUST agree with them:
        // if it says yes and they say no, computeKlobucharDelay indexes an empty
        // vector; if it says no and they say yes, the background silently degrades.
        std::string norm = line;
        for (char& c : norm)
            if (c == 'D' || c == 'd') c = 'E';
        std::istringstream iss(norm);
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
                                     const std::filesystem::path& nav_path,
                                     const std::string& rx_antenna_type)
    : rx_antenna_type_(rx_antenna_type) {
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

    // Antenna PCO/PCV via reuse (M7 antenna).  Only stood up when an ATX was loaded
    // into the shared cache (the caller adds igs20.atx to product_paths when either
    // antenna term is requested).  The SATELLITE half is always active.  The RECEIVER
    // half is gated on a non-empty rx_antenna_type: with none, force receiver PCO/PCV
    // off (they default true) so getAntennaCorrection returns exactly
    // (-sat_pco + sat_pcv); with one, enable them so the receiver half is injected too
    // and the run config's enable_receiver_pco/pcv cancels it.  Phase wind-up stays OFF
    // either way — it is ill-defined on a rotating hull, so both sides omit it.
    if (!ProductCache::getInstance().getAntennas().empty()) {
        const bool rx_on = !rx_antenna_type_.empty();
        auto& mc = config_.getModelCorrections(); // non-const overload
        mc.enable_satellite_pco = true;
        mc.enable_satellite_pcv = true;
        mc.enable_receiver_pco = rx_on;
        mc.enable_receiver_pcv = rx_on;
        mc.enable_phase_windup = false;
        // Pin the current antenna vector so Antenna's raw reference into the singleton
        // cache can't dangle if the cache is later cleared/reloaded during our lifetime.
        antennas_hold_ = ProductCache::getInstance().getAntennasShared();
        antenna_ = std::make_unique<Antenna>(config_, *product_parser_, log_, *rinex_);
        has_antenna_ = true;
    }

    // Observable-specific signal biases (OSB): stood up whenever a .BIA parsed into the
    // shared cache (the caller adds the COD MGX .BIA to product_paths only when
    // corrections.signal_bias is requested).  No separate model object is needed — the
    // biases ride in geometry_'s own GnssCorrections, built from cache.getBiasesShared()
    // (ModelGeometry.cpp), so signalBiasMetres just calls its getBiasCorrection, exactly
    // the lookup the processor performs in computeBaseModeledValue.
    has_signal_bias_ = ProductCache::getInstance().getBiasCount() > 0;

    ok_ = true;
}

EphemerisProvider::~EphemerisProvider() = default;

double EphemerisProvider::klobucharDelayMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                               const GnssTime& t, double freq_hz) const {
    if (!has_klobuchar_ || !ionosphere_) return 0.0;
    return ionosphere_->computeKlobucharDelay(t, rx, sat_pos, freq_hz);
}

bool EphemerisProvider::loadGim(const std::filesystem::path& inx_path) {
    gim_.reset();
    if (inx_path.empty() || !std::filesystem::exists(inx_path)) return false;
    auto gim = std::make_unique<GlobalIonosphereMap>(inx_path.string(), log_);
    if (!gim->isLoaded()) return false;
    gim_ = std::move(gim);
    return true;
}

bool EphemerisProvider::gimSlantStec(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                     const GnssTime& t,
                                     double& stec_tecu, double& std_tecu,
                                     double& ipp_lat_deg, double& ipp_lon_deg) const {
    stec_tecu = std_tecu = ipp_lat_deg = ipp_lon_deg = 0.0;
    if (!gim_) return false;

    // The frequency only scales delay_m, which we discard; STEC is frequency-free.
    // Querying ONCE here (never per band) is what keeps the injected first-order term a
    // single scalar, and therefore what keeps the iono-free combination cancelling exactly.
    const GimQuery q = gim_->slantIonoDelay(t, rx, sat_pos, Constants::GPS_L1_FREQ);
    if (!q.valid || !std::isfinite(q.stec_tecu)) return false;

    // Non-positive STEC is map CONTENT, not a failed query: storm-time GIMs legitimately
    // carry near-zero/negative cells in depleted sectors (post-storm night side, plus the
    // spherical-harmonic fit overshooting toward zero — DOY 132's pre-dawn minimum at the
    // vessel is 10 TECU with negative cells nearby).  Clamp to 0 (a negative first-order
    // group delay would be unphysical) instead of misclassifying the ray as invalid,
    // which silently mixed Klobuchar into an otherwise-GIM background and tripped the
    // uniform-background gate.
    stec_tecu = std::max(0.0, q.stec_tecu);   // ALREADY SLANT — see the header warning; do not map.
    const double deg = 180.0 / Constants::PI;
    // std_m at L1 back to TECU: I = 40.3e16 * STEC / f^2.
    std_tecu = std::isfinite(q.std_m)
                   ? q.std_m * Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ / (40.3 * 1e16)
                   : 0.0;
    ipp_lat_deg = q.ipp_lat_rad * deg;
    // The map returns longitude UNWRAPPED; this vessel sits at lon ~-179, so wrap it.
    double lon = std::fmod(q.ipp_lon_rad * deg + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    ipp_lon_deg = lon - 180.0;
    return true;
}

bool EphemerisProvider::gimEpochSpan(GnssTime& first, GnssTime& last) const {
    if (!gim_ || gim_->data().maps.empty()) return false;
    first = gim_->data().maps.front().epoch;
    last  = gim_->data().maps.back().epoch;
    return true;
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

std::size_t EphemerisProvider::erpNodeCount() const {
    return ProductCache::getInstance().getErpCount();
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

// --- M7 full-correction stack ------------------------------------------------
// Signs verified against PppModel::computeBaseModeledValue: the modeled value is
//   range - c*sat_clock - c*relativistic_corr + ... + shapiro + solid_tide + pole
// and observed = modeled(truth) + noise, so the synth ADDS each term below.

double EphemerisProvider::relativityMetres(const std::string& sat, const Eigen::Vector3d& sat_pos_em,
                                           const Eigen::Vector3d& sat_vel_em) const {
    if (!geometry_) return 0.0;
    // modeled subtracts c*dtr, so the observation carries -c*dtr.
    return -Constants::SPEED_OF_LIGHT *
           geometry_->getCorrections().computeRelativisticCorrection(sat, sat_pos_em, sat_vel_em);
}

double EphemerisProvider::shapiroMetres(const std::string& sat, const Eigen::Vector3d& rx,
                                        const Eigen::Vector3d& sat_pos) const {
    if (!geometry_) return 0.0;
    return geometry_->getCorrections().computeShapiroDelay(sat, rx, sat_pos);
}

double EphemerisProvider::solidEarthTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                               const GnssTime& t) const {
    if (!geometry_) return 0.0;
    // Reuse the geometry pipeline's OWN Sun/Moon (same Planetary + ERP) so the
    // displacement is bit-for-bit what the processor computes -> cancels exactly.
    Eigen::Vector3d sun, moon;
    geometry_->sunMoonPositions(t, sun, moon);
    return geometry_->getCorrections().computeSolidEarthTide(rx, sat_pos, sun, moon, t);
}

double EphemerisProvider::poleTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                         const GnssTime& t) const {
    if (!geometry_) return 0.0;
    // Project the ECEF site displacement onto the sat->rx direction, exactly as
    // PppModel::computePoleTideProjection does (pole_ecef.dot(-los), los=rx->sat).
    const Eigen::Vector3d pole_ecef = geometry_->getCorrections().computePoleTideCorrection(rx, t);
    const Eigen::Vector3d los = (sat_pos - rx).normalized();
    return pole_ecef.dot(-los);
}

bool EphemerisProvider::loadOceanTide(const std::filesystem::path& eot20_nc,
                                      const Eigen::Vector3d& site_ecef) {
    if (!geometry_) return false;
    // Same entry point Model::Geometry::loadData uses, at the same fixed site, so the
    // 11-constituent coefficient set is bit-for-bit the one the processor will evaluate.
    has_ocean_tide_ =
        geometry_->getCorrections().loadOceanTideFromEot20(eot20_nc.string(), site_ecef);
    return has_ocean_tide_;
}

bool EphemerisProvider::loadOceanTideAlongTrack(const std::filesystem::path& eot20_nc,
                                                const std::vector<OtlField::TrackSample>& track,
                                                double anchor_spacing_km,
                                                const Eigen::Vector3d& frozen_site) {
    // Deliberately does NOT touch geometry_'s corrections: the field owns both the
    // moving coefficients and the frozen reference set, so there is exactly one
    // source for each and they cannot drift apart.
    auto field = std::make_unique<OtlField>(log_);
    if (!field->build(eot20_nc, track, anchor_spacing_km, frozen_site)) return false;
    otl_field_      = std::move(field);
    has_ocean_tide_ = true;
    return true;
}

std::size_t EphemerisProvider::oceanTideAnchorCount() const {
    return otl_field_ ? otl_field_->anchors().size() : 0u;
}

Eigen::Vector3d EphemerisProvider::oceanTideDisplacementEcef(const Eigen::Vector3d& rx,
                                                             const GnssTime& t) const {
    if (!has_ocean_tide_) return Eigen::Vector3d::Zero();
    if (otl_field_) return otl_field_->displacementEcef(rx, t);
    if (!geometry_) return Eigen::Vector3d::Zero();
    return geometry_->getCorrections().computeOceanTideCorrection(rx, t);
}

Eigen::Vector3d EphemerisProvider::oceanTideModeledEcef(const Eigen::Vector3d& rx,
                                                        const GnssTime& t) const {
    if (!has_ocean_tide_) return Eigen::Vector3d::Zero();
    if (otl_field_) return otl_field_->frozenDisplacementEcef(rx, t);
    // Frozen mode: what the sim injects IS what the processor models.
    if (!geometry_) return Eigen::Vector3d::Zero();
    return geometry_->getCorrections().computeOceanTideCorrection(rx, t);
}

double EphemerisProvider::oceanTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                          const GnssTime& t) const {
    if (!has_ocean_tide_) return 0.0;
    if (!otl_field_ && !geometry_) return 0.0;
    // Project the ECEF site displacement onto the sat->rx direction, exactly as
    // ModelGeometry does (otl_ecef.dot(-los), los = rx->sat) — same form as pole tide.
    // With an along-track field installed the displacement is the one at the vessel's
    // actual position, so this is the ONE M7 term that does not cancel by construction.
    const Eigen::Vector3d otl_ecef =
        otl_field_ ? otl_field_->displacementEcef(rx, t)
                   : geometry_->getCorrections().computeOceanTideCorrection(rx, t);
    const Eigen::Vector3d los = (sat_pos - rx).normalized();
    return otl_ecef.dot(-los);
}

double EphemerisProvider::antennaMetres(const std::string& sat, const std::string& code,
                                        const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                        const Eigen::Vector3d& sat_vel,
                                        double elev_deg, double az_deg, const GnssTime& t) const {
    if (!has_antenna_ || !antenna_ || !geometry_) return 0.0;
    // Map the constellation from the sat id exactly as RinexParser::systemFromSatPrefix
    // does, so this stays correct if the signal plan is ever extended past GPS+Galileo
    // (a first-char heuristic that defaulted unknown systems to GPS would inject a
    // GPS-keyed term on e.g. a BeiDou row and silently break its cancellation).  An
    // unhandled prefix returns 0 (no injection) rather than a misclassified term.
    System system;
    switch (sat.empty() ? '?' : sat[0]) {
        case 'G': system = System::GPS;     break;
        case 'E': system = System::Galileo; break;
        case 'R': system = System::GLONASS; break;
        case 'C': system = System::BeiDou;  break;
        case 'J': system = System::QZSS;    break;
        case 'S': system = System::SBAS;    break;
        default:  return 0.0;
    }
    // Reuse the geometry pipeline's cached Sun (the SAME source the processor passes
    // as getCachedSunPosition()) so the satellite body-frame yaw-steering attitude —
    // hence the PCO rotation into ECEF — matches the processor and cancels.
    Eigen::Vector3d sun, moon;
    geometry_->sunMoonPositions(t, sun, moon);
    // rx_antenna_type_ empty + receiver terms disabled => exactly (-sat_pco + sat_pcv);
    // non-empty + receiver terms enabled => that PLUS (+rx_pco + rx_pcv).  The SAME
    // string is written to the RINEX "ANT # / TYPE" header, so the antenna the file
    // declares is by construction the antenna whose pattern was injected.
    return antenna_->getAntennaCorrection(sat, code, system, rx, sat_pos, sat_vel,
                                          elev_deg, az_deg, rx_antenna_type_, t,
                                          sun, /*skip_satellite_pco=*/false);
}

double EphemerisProvider::signalBiasMetres(const std::string& sat, const std::string& code,
                                           const GnssTime& t) const {
    if (!has_signal_bias_ || !geometry_) return 0.0;
    // The processor ADDS this same value to its modeled value (computeBaseModeledValue
    // "+ bias_corr") when enable_code_phase_bias is on, so ADDING it to the synthesized
    // observable cancels it back to truth.  getBiasCorrection returns 0 for any
    // (satellite, observable) the product does not cover, which cancels trivially.
    return geometry_->getCorrections().getBiasCorrection(sat, code, t);
}

} // namespace sim
} // namespace GnssProcessor
