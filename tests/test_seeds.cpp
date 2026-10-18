/**
 * @file test_seeds.cpp
 * @brief The seeds (measurement/Seeds.hpp) against the code they replaced, bit for bit.
 *
 * The seeds used to be written out twice, in @c StateInitializer::computeRawResidual and
 * @c StateManager::computeAmbiguitySeed, and the first-epoch seeds were methods that wrote into
 * the state manager.  This file keeps the old code as a reference (namespace @c legacy, copied
 * from the sources it replaced, with the writes into the state replaced by returned values) and
 * compares the new functions to it over each posture the seeds have a branch for:
 *  - the carrier frequency of the slant ionosphere term, per constellation and band: the same
 *    number outside GLONASS FDMA bands, and channel 0 under @c FdmaChannel::Zero;
 *  - the raw residual and the ambiguity seed of every row of an epoch, bit for bit;
 *  - the order the terms are subtracted in (the inter-system bias before the gradients);
 *  - the inter-system bias, cold clock and phase-clock seeds;
 *  - the effect sets of @c observationEquation.
 *
 * The receiver code-bias seed needs the clock-reference signals of a loaded bias product (a
 * process-wide cache no unit test fills); its guards are tested here and its numbers are
 * covered by the field arms that estimate receiver biases.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/PppUtils.hpp"
#include "filter/StateKeySchema.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/ObservationEquationCheck.hpp"
#include "measurement/Seeds.hpp"
#include "measurement/StateInitializer.hpp"
#include "measurement/UserParamView.hpp"
#include "support/ModelStack.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;

namespace {

// ===========================================================================
// The code the seeds replaced
// ===========================================================================

namespace legacy {

/// StateManager.cpp's file-local ISB name table (empty for GPS, the reference).
std::string stateManagerIsbName(System system) {
    switch (system) {
    case System::GPS:     return "";
    case System::Galileo: return "Galileo";
    case System::GLONASS: return "GLONASS";
    case System::BeiDou:  return "BeiDou";
    case System::QZSS:    return "QZSS";
    case System::SBAS:    return "SBAS";
    default:              return "";
    }
}

/// StateManager.cpp's file-local carrier frequency table: the GLONASS FDMA bands at slot 0.
double bandFrequency(char band, System system) {
    switch (system) {
    case System::GPS:
        if (band == '2') return Constants::GPS_L2_FREQ;
        if (band == '5') return Constants::GPS_L5_FREQ;
        return Constants::GPS_L1_FREQ;
    case System::QZSS:
        if (band == '2') return Constants::QZSS_L2_FREQ;
        if (band == '5') return Constants::QZSS_L5_FREQ;
        if (band == '6') return Constants::QZSS_L6_FREQ;
        return Constants::QZSS_L1_FREQ;
    case System::SBAS:
        if (band == '5') return Constants::SBAS_L5_FREQ;
        return Constants::SBAS_L1_FREQ;
    case System::Galileo:
        if (band == '5') return Constants::GAL_E5A_FREQ;
        if (band == '7') return Constants::GAL_E5B_FREQ;
        if (band == '8') return Constants::GAL_E5_FREQ;
        if (band == '6') return Constants::GAL_E6_FREQ;
        return Constants::GPS_L1_FREQ;
    case System::BeiDou:
        if (band == '1') return Constants::BDS_B1C_FREQ;
        if (band == '2') return Constants::BDS_B1_FREQ;
        if (band == '5') return Constants::BDS_B2A_FREQ;
        if (band == '7') return Constants::BDS_B2_FREQ;
        if (band == '8') return Constants::BDS_B2AB_FREQ;
        if (band == '6') return Constants::BDS_B3_FREQ;
        return Constants::BDS_B1C_FREQ;
    case System::GLONASS:
        if (band == '2') return Constants::GLONASS_G2_BASE_FREQ;
        if (band == '3') return Constants::GLONASS_G3_BASE_FREQ;
        if (band == '4') return Constants::GLONASS_G1A_FREQ;
        if (band == '6') return Constants::GLONASS_G2A_FREQ;
        return Constants::GLONASS_G1_BASE_FREQ;
    default:
        return Constants::GPS_L1_FREQ;
    }
}

/// The median of a mutable vector (StateInitializer.cpp's helper).
double computeMedian(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const size_t n = values.size();
    const size_t mid = n / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    if (n % 2 == 0) {
        double upper = values[mid];
        std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
        return (values[mid - 1] + upper) / 2.0;
    }
    return values[mid];
}

/// What the old functions read: the state layout, the configuration and the initializer (whose
/// own frequency helper was the one computeRawResidual scaled the ionosphere with).
struct World {
    const StateManager&     sm;
    const GnssConfig&       config;
    const StateInitializer& init;
};

/// StateManager::computeAmbiguitySeed, as it was.
double computeAmbiguitySeed(const World& w, const PppObservation& obs, const std::string& ref_type,
                            const Eigen::VectorXd& x) {
    const StateManager& sm = w.sm;
    try {
        if (!obs.observed.count(ref_type) || !obs.modeled.count(ref_type))
            return 0.0;

        const double observed_m = obs.observed.at(ref_type);
        const double modeled_m  = obs.modeled.at(ref_type);
        const double tropo_wet  = obs.mf_w * x(sm.getTropoIndex());

        double clk_term = sm.hasClock() ? x(sm.getClockIndex()) : 0.0;
        if (sm.hasPhaseClock())
            clk_term += x(sm.getPhaseClockIndex());

        double isb = 0.0;
        {
            const std::string isb_name = stateManagerIsbName(obs.system);
            const size_t isb_idx = sm.getSystemTimeOffsetIndex(isb_name);
            if (isb_idx < static_cast<size_t>(x.size()))
                isb = x(static_cast<Eigen::Index>(isb_idx));
        }

        double tropo_grad = 0.0;
        if (sm.hasTropoGradients()) {
            const double el_rad = obs.elevation * Constants::PI / 180.0;
            const double az_rad = obs.azimuth  * Constants::PI / 180.0;
            const double mf_grad = 1.0 / (std::sin(el_rad) * std::tan(el_rad) + 0.0032);
            const double gn = x(static_cast<Eigen::Index>(sm.getTropoGradientNorthIndex()));
            const double ge = x(static_cast<Eigen::Index>(sm.getTropoGradientEastIndex()));
            tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
        }

        double iono_term = 0.0;
        if (w.config.isUncombinedEnabled() && !ref_type.empty() && ref_type[0] == 'L') {
            const int iono_idx = sm.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
                double alpha = 1.0;
                if (ref_type.size() >= 2) {
                    const double freq = bandFrequency(ref_type[1], obs.system);
                    alpha = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (freq * freq);
                }
                iono_term = -alpha * x(iono_idx);
            }
        }
        return observed_m - modeled_m - clk_term - tropo_wet - isb - tropo_grad - iono_term;
    } catch (...) {
        return 0.0;
    }
}

/// StateInitializer::computeRawResidual, as it was.
double computeRawResidual(const World& w, const PppObservation& obs, const std::string& ref_type,
                          const Eigen::VectorXd& x) {
    const StateManager& sm = w.sm;
    const PppUtils& ppp_utils = w.init.frequencyHelper();

    const double observed = obs.observed.at(ref_type);
    const double modeled  = obs.modeled.at(ref_type);
    const double rx_clk   = sm.hasClock() ? x(sm.getClockIndex()) : 0.0;
    const double zwd      = x(sm.getTropoIndex());
    const double tropo_wet = obs.mf_w * zwd;

    double isb = 0.0;
    const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
    if (!sys_name.empty()) {
        const size_t isb_idx = sm.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx < static_cast<size_t>(x.size()))
            isb = x(static_cast<Eigen::Index>(isb_idx));
    }

    double tropo_grad = 0.0;
    if (sm.hasTropoGradients()) {
        const double el_rad = obs.elevation * Constants::PI / 180.0;
        const double az_rad = obs.azimuth * Constants::PI / 180.0;
        const double sin_el = std::sin(el_rad);
        const double tan_el = std::tan(el_rad);
        const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
        const double gn = x(static_cast<Eigen::Index>(sm.getTropoGradientNorthIndex()));
        const double ge = x(static_cast<Eigen::Index>(sm.getTropoGradientEastIndex()));
        tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
    }

    double iono = 0.0;
    const ObsType ref_obs_type = obsTypeOf(ref_type);
    if (w.config.isUncombinedEnabled() && !ref_type.empty()
        && (ref_obs_type == ObsType::Code || ref_obs_type == ObsType::Phase)) {
        const int iono_idx = sm.getIonosphereIndex(obs.satellite);
        if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
            const double freq = ppp_utils.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq > 0.0) {
                const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
                const double alpha = (f_ref * f_ref) / (freq * freq);
                iono = (ref_obs_type == ObsType::Phase ? -1.0 : 1.0) * alpha * x(iono_idx);
            }
        }
    }

    double code_bias = 0.0;
    if (w.config.estimateIfb() && w.config.isUncombinedEnabled()
        && !ref_type.empty() && ref_obs_type == ObsType::Code) {
        const auto bias = w.init.resolveReceiverBias(ref_type, obs.system, obs.satellite);
        if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
            const int cb_idx = sm.getCodeBiasIndex(bias.state_key);
            if (cb_idx >= 0 && cb_idx < static_cast<int>(x.size()))
                code_bias = bias.coefficient * x(cb_idx);
        }
    }

    double sat_bias = 0.0;
    if (w.config.estimateSatBiases() && !ref_type.empty()) {
        const bool is_if = (ref_type.find('_') != std::string::npos);
        const bool applicable = w.config.isUncombinedEnabled()
            ? (!is_if && ref_obs_type == ObsType::Code)
            : (is_if && ref_obs_type == ObsType::Code);
        if (applicable) {
            const std::string sb_key = StateKeys::makeSatBiasKey(obs.satellite, ref_type);
            const int sb_idx = sm.getSatBiasIndex(sb_key);
            if (sb_idx >= 0 && sb_idx < static_cast<int>(x.size()))
                sat_bias = x(sb_idx);
        }
    }

    return observed - modeled - rx_clk - tropo_wet - isb - tropo_grad
         - iono - code_bias - sat_bias;
}

/// The rows initializeInterSystemBiases and seedColdReceiverClock took residuals of.
bool isCodeRowOfMode(const World& w, const std::string& ref_type) {
    if (obsTypeOf(ref_type) != ObsType::Code) return false;
    const bool has_underscore = (ref_type.find('_') != std::string::npos);
    if (w.config.isUncombinedEnabled()) {
        if (has_underscore) return false;
    } else {
        if (!has_underscore) return false;
    }
    return true;
}

struct IsbValue {
    std::string name;
    double      value;
    size_t      n;
};

/// StateInitializer::initializeInterSystemBiases with the writes returned.
/// @return False when the time reference had no code (the old function returned false).
bool initializeInterSystemBiases(const World& w, const std::vector<PppObservation>& observations,
                                 std::vector<IsbValue>& out, size_t& reference_n) {
    std::map<std::string, std::vector<double>> system_residuals;
    const Eigen::VectorXd x = w.sm.getStateVector();
    for (const auto& obs : observations) {
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRowOfMode(w, ref_type)) continue;
            if (!obs.modeled.count(ref_type)) continue;
            const double residual = computeRawResidual(w, obs, ref_type, x);
            const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
            const std::string key = sys_name.empty() ? systemName(timeReferenceSystem()) : sys_name;
            system_residuals[key].push_back(residual);
            break;
        }
    }
    const std::string gps_key = systemName(timeReferenceSystem());
    if (system_residuals.find(gps_key) == system_residuals.end() ||
        system_residuals[gps_key].empty())
        return false;
    const double gps_median = computeMedian(system_residuals[gps_key]);
    reference_n = system_residuals[gps_key].size();
    for (auto& [sys_name, residuals] : system_residuals) {
        if (sys_name == gps_key || residuals.empty()) continue;
        const size_t isb_idx = w.sm.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx >= static_cast<size_t>(x.size())) continue;
        out.push_back({sys_name, computeMedian(residuals) - gps_median, residuals.size()});
    }
    return true;
}

/// How seedColdReceiverClock ended, with the value it would have written.
struct ColdClock {
    bool   settled{false};
    bool   seeded{false};
    double value{0.0};
    size_t n{0};
};

/// StateInitializer::seedColdReceiverClock with the write returned.
ColdClock seedColdReceiverClock(const World& w, const std::vector<PppObservation>& observations) {
    ColdClock out;
    if (!w.sm.hasClock()) { out.settled = true; return out; }
    constexpr double kColdClockEpsilonM = 1e-6;
    const Eigen::VectorXd x = w.sm.getStateVector();
    if (std::abs(x(static_cast<Eigen::Index>(w.sm.getClockIndex()))) > kColdClockEpsilonM) {
        out.settled = true;
        return out;
    }
    std::vector<double> gps_residuals;
    for (const auto& obs : observations) {
        if (!isTimeReference(obs.system)) continue;
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRowOfMode(w, ref_type)) continue;
            if (!obs.modeled.count(ref_type)) continue;
            gps_residuals.push_back(computeRawResidual(w, obs, ref_type, x));
            break;
        }
    }
    if (gps_residuals.empty()) return out;
    out.settled = true;
    out.seeded  = true;
    out.n       = gps_residuals.size();
    out.value   = computeMedian(gps_residuals);
    return out;
}

/// StateInitializer::initializePhaseClockOffset with the write returned.
/// @return Whether any satellite had both a code and a phase residual.
bool initializePhaseClockOffset(const World& w, const std::vector<PppObservation>& observations,
                                double& value, size_t& n_pairs) {
    const Eigen::VectorXd x = w.sm.getStateVector();
    std::vector<double> code_phase_diffs;
    for (const auto& obs : observations) {
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        double code_residual = 0.0, phase_residual = 0.0;
        bool have_code = false, have_phase = false;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (ref_type.empty()) continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (w.config.isUncombinedEnabled()) {
                if (has_underscore) continue;
            } else {
                if (!has_underscore) continue;
            }
            if (!obs.modeled.count(ref_type)) continue;
            const double residual = computeRawResidual(w, obs, ref_type, x);
            const ObsType type = obsTypeOf(ref_type);
            if (type == ObsType::Code && !have_code)   { code_residual  = residual; have_code  = true; }
            if (type == ObsType::Phase && !have_phase) { phase_residual = residual; have_phase = true; }
        }
        if (have_code && have_phase)
            code_phase_diffs.push_back(code_residual - phase_residual);
    }
    if (code_phase_diffs.empty()) return false;
    n_pairs = code_phase_diffs.size();
    value   = computeMedian(code_phase_diffs);
    return true;
}

} // namespace legacy

/// Every (digit, attribute) the classifier maps to a known band for @p sys.
std::vector<SignalId> classifiedSignals(System sys) {
    std::vector<SignalId> out;
    for (char d = '0'; d <= '9'; ++d)
        for (char a = 'A'; a <= 'Z'; ++a)
            if (classifySignal(traitsOf(sys).name, std::string{'C', d, a}).band
                != FrequencyBand::Unknown)
                out.push_back(SignalId{sys, d, a});
    return out;
}

const std::vector<System> kAllSystems = {System::GPS, System::Galileo, System::GLONASS,
                                         System::BeiDou, System::QZSS, System::SBAS};

/// A lookup that holds one slant ionosphere parameter, whatever the satellite.
struct IonosphereOnly {
    ParamSlot find(const ParamRef& ref) const {
        return ref.kind == NetworkParamKind::SlantIono ? ParamSlot{7, 3.0} : ParamSlot{};
    }
};

// ===========================================================================
// The fixture
// ===========================================================================

constexpr const char* kSat = "G12";

class SeedsTest : public ::testing::Test {
protected:
    LogManager log;
    gnss_test::ModelStack stack;
    int epoch = 0;

    GnssConfig& config() { return stack.config; }
    StateManager& states() { return *stack.states; }
    MeasurementModel& model() { return *stack.model; }

    void load(const std::string& mode, const std::string& processing = "",
              const std::string& estimation = "", bool bssd = false) {
        ASSERT_TRUE(stack.load(
            log, std::string("seeds_") + ::testing::UnitTest::GetInstance()->current_test_info()->name(),
            mode, processing, estimation, bssd, /*check=*/false));
    }

    PppObservation observation(const std::string& sat, System sys,
                               const std::vector<std::string>& signals, double azimuth = 45.0,
                               double elevation = 45.0, double modeled = 2.2e7) const {
        return gnss_test::syntheticObservation(sat, sys, signals, epoch, azimuth, elevation,
                                               modeled);
    }

    EpochMeasurement update(const std::vector<PppObservation>& obs) {
        ++epoch;
        return model().computeMeasurementUpdate(obs, states().getStateVector());
    }

    legacy::World world() { return legacy::World{states(), config(), model().getStateInitializer()}; }

    /// The new functions' lookup over the current state.
    struct View {
        Eigen::VectorXd  x;
        UserParamView    view;
        View(const StateManager& sm)
            : x(sm.getStateVector()), view(sm, x, static_cast<std::size_t>(x.size())) {}
        ParamLookup lookup() const { return ParamLookup(view); }
    };

    /// Every row of @p observations: the new raw residual and ambiguity seed against the old.
    /// @return The number of rows compared.
    int expectRowsMatchLegacy(const std::vector<PppObservation>& observations) {
        const Eigen::VectorXd x = states().getStateVector();
        const View v(states());
        const ObsInput residual_in = model().residualSeedInput();
        const legacy::World w = world();
        int rows = 0;
        for (const PppObservation& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (!obs.modeled.count(ref_type)) continue;
                const bool row_of_mode =
                    config().isUncombinedEnabled() == (ref_type.find('_') == std::string::npos);
                if (!row_of_mode) continue;
                const std::optional<double> raw = rawResidual(residual_in, v.lookup(), obs, ref_type);
                ++rows;
                EXPECT_TRUE(raw.has_value()) << obs.satellite << " " << ref_type;
                if (raw)
                    EXPECT_TRUE(sameBits(*raw, legacy::computeRawResidual(w, obs, ref_type, x)))
                        << "raw residual " << obs.satellite << " " << ref_type;
                if (ref_type[0] == 'L')
                    EXPECT_TRUE(sameBits(model().ambiguitySeed(obs, ref_type, x),
                                         legacy::computeAmbiguitySeed(w, obs, ref_type, x)))
                        << "ambiguity seed " << obs.satellite << " " << ref_type;
            }
        }
        return rows;
    }

    /// Three epochs of the given satellites, then every state moved off its seed.
    void run(const std::vector<PppObservation>& sats) {
        for (int e = 0; e < 3; ++e) {
            update(sats);
            stack.perturbStates();
        }
    }
};

// ===========================================================================
// The carrier frequency
// ===========================================================================

TEST_F(SeedsTest, TheAmbiguitySeedFrequencyIsTheHelpersOutsideGlonassFdmaBands) {
    load("ppp-rtk");
    const PppUtils utils(log);
    const std::map<std::string, char> no_nlwl;
    for (const System sys : kAllSystems) {
        for (const SignalId& sig : classifiedSignals(sys)) {
            const bool fdma = sys == System::GLONASS && (sig.digit == '1' || sig.digit == '2');
            const std::string sat = SatId{sys, 5}.str();
            const std::string text = std::string{'L', sig.digit, sig.attribute};
            const PppObservation obs = observation(sat, sys, {text});
            const double freq = legacy::bandFrequency(sig.digit, sys);
            const double alpha = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (freq * freq);

            for (const FdmaChannel rule : {FdmaChannel::Satellite, FdmaChannel::Zero}) {
                const ObsInput in{RowProducts{stack.config, *stack.ppp, stack.store}, utils, nullptr,
                                  no_nlwl, rule};
                const ObservationEquation eq =
                    observationEquation(in, obs, text, ParamLookup(IonosphereOnly{}),
                                        effectsOf({ObsEffect::Modeled, ObsEffect::Ionosphere}));
                ASSERT_TRUE(eq.usable) << systemName(sys) << " " << text;
                const ObsAddend* iono = findTerm(eq, ObsEffect::Ionosphere);
                ASSERT_NE(iono, nullptr) << systemName(sys) << " " << text;
                // The old table's number: the helper's for every band but a GLONASS FDMA one,
                // channel 0 under the Zero rule.
                if (!fdma || rule == FdmaChannel::Zero)
                    EXPECT_TRUE(sameBits(iono->part[0].partial, -alpha))
                        << systemName(sys) << " " << text << " rule " << static_cast<int>(rule);
            }
        }
    }
}

TEST_F(SeedsTest, AGlonassFdmaBandOnAnotherChannelDiffersFromChannelZero) {
    load("ppp-rtk");
    PppUtils utils(log);
    utils.setGlonassSlots({{"R05", 4}});
    const std::map<std::string, char> no_nlwl;
    const PppObservation obs = observation("R05", System::GLONASS, {"L1C", "L3I"});
    const RowProducts products{stack.config, *stack.ppp, stack.store};
    const auto partial = [&](const char* signal, FdmaChannel rule) {
        const ObsInput in{products, utils, nullptr, no_nlwl, rule};
        return findTerm(observationEquation(in, obs, signal, ParamLookup(IonosphereOnly{}),
                                            effectsOf({ObsEffect::Modeled, ObsEffect::Ionosphere})),
                        ObsEffect::Ionosphere)->part[0].partial;
    };
    // G1 depends on the channel; the CDMA band G3 does not.
    EXPECT_NE(partial("L1C", FdmaChannel::Satellite), partial("L1C", FdmaChannel::Zero));
    EXPECT_TRUE(sameBits(partial("L3I", FdmaChannel::Satellite), partial("L3I", FdmaChannel::Zero)));
}

// ===========================================================================
// The residual and the ambiguity seed
// ===========================================================================

TEST_F(SeedsTest, MultiSystemUncombinedRowsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
    };
    run(sats);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 12);
}

TEST_F(SeedsTest, GradientsPhaseClockAndInterSystemBiasKeepTheOldSumOrder) {
    load("ppp-rtk", "", R"(, "estimate_tropo_gradients": true, "estimate_phase_clock": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 310.0, 21.0),
    };
    run(sats);
    ASSERT_TRUE(states().hasTropoGradients());
    ASSERT_TRUE(states().hasPhaseClock());
    EXPECT_EQ(expectRowsMatchLegacy(sats), 12);
}

TEST_F(SeedsTest, SatelliteCodeBiasStatesMatchTheOldCode) {
    load("ppp-rtk", "", R"(, "estimate_sat_biases": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
    };
    run(sats);
    ASSERT_GE(states().getSatBiasIndex(std::string(kSat) + "|C1W"), 0);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 8);
}

TEST_F(SeedsTest, IonoFreeCombinationRowsMatchTheOldCode) {
    load("ppp", R"(, "uncombined_enabled": false)", R"(, "estimate_sat_biases": true)");
    ASSERT_FALSE(config().isUncombinedEnabled());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W_C2W", "L1W_L2W"}),
        observation("G15", System::GPS, {"C1W_C2W", "L1W_L2W"}, 200.0, 60.0),
    };
    run(sats);
    ASSERT_GE(states().getSatBiasIndex(std::string(kSat) + "|C1W_C2W"), 0);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 4);
}

TEST_F(SeedsTest, ARowWithoutAnAmbiguityStillHasASeed) {
    // The seed of an ambiguity is formed before the ambiguity exists.
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});   // births the states; then ask for a signal that has no ambiguity
    PppObservation other = o;
    other.modeled["L2W"] = other.modeled.at("L1W");
    other.observed["L2W"] = other.observed.at("L1W") + 4.0;
    ASSERT_LT(states().getAmbiguityIndex(K::makeAmbiguityKey(kSat, "L2W")), 0);
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_TRUE(sameBits(model().ambiguitySeed(other, "L2W", x),
                         legacy::computeAmbiguitySeed(world(), other, "L2W", x)));
}

TEST_F(SeedsTest, ASignalWithoutObservedOrModeledValueSeedsZero) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_EQ(model().ambiguitySeed(o, "L2W", x), 0.0);
    PppObservation no_model = o;
    no_model.modeled.erase("L1W");
    EXPECT_EQ(model().ambiguitySeed(no_model, "L1W", x), 0.0);
}

TEST_F(SeedsTest, TheTermsAreSubtractedWithTheInterSystemBiasBeforeTheGradients) {
    // (1 - 0.3) - 0.1 and (1 - 0.1) - 0.3 differ in the last bit; the seeds have always
    // subtracted the inter-system bias first.
    ASSERT_FALSE(sameBits((1.0 - 0.3) - 0.1, (1.0 - 0.1) - 0.3));
    ObservationEquation eq;
    ObsAddend& modeled = eq.addend[eq.n++];
    modeled.effect = ObsEffect::Modeled;
    modeled.constant = 0.0;
    ObsAddend& gradient = eq.addend[eq.n++];   // the equation's own order puts the gradient first
    gradient.effect = ObsEffect::TropoGradient;
    gradient.part[0].partial = 1.0;
    gradient.part[0].x = 0.1;
    gradient.n_part = 1;
    ObsAddend& isb = eq.addend[eq.n++];
    isb.effect = ObsEffect::SystemTimeOffset;
    isb.part[0].partial = 1.0;
    isb.part[0].x = 0.3;
    isb.n_part = 1;
    eq.usable = true;
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.residual, 1.0), (1.0 - 0.3) - 0.1));
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.ambiguity, 1.0), (1.0 - 0.3) - 0.1));
}

TEST_F(SeedsTest, ThePhaseClockIsAddedToTheReceiverClockFirstInTheAmbiguitySeedOnly) {
    // observed - modeled - (clock + phase clock) against observed - modeled - clock.
    const double clock = 0.3, phase_clock = 0.2, observed = 1.0;
    ASSERT_FALSE(sameBits((observed - clock) - phase_clock, observed - (clock + phase_clock)));
    ObservationEquation eq;
    ObsAddend& modeled = eq.addend[eq.n++];
    modeled.effect = ObsEffect::Modeled;
    ObsAddend& rx = eq.addend[eq.n++];
    rx.effect = ObsEffect::ReceiverClock;
    rx.part[0].partial = 1.0;
    rx.part[0].x = clock;
    rx.n_part = 1;
    ObsAddend& pc = eq.addend[eq.n++];
    pc.effect = ObsEffect::PhaseClock;
    pc.part[0].partial = 1.0;
    pc.part[0].x = phase_clock;
    pc.n_part = 1;
    eq.usable = true;
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.ambiguity, observed),
                         observed - (clock + phase_clock)));
    // The residual seeds do not know the phase clock at all.
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.residual, observed), observed - clock));
}

TEST_F(SeedsTest, EverySubtractedEffectIsInTheSeedOrder) {
    for (const SeedMask& mask : {kLegacySeedMask.residual, kLegacySeedMask.ambiguity})
        for (std::size_t e = 0; e < kObsEffectCount; ++e) {
            const ObsEffect effect = static_cast<ObsEffect>(e);
            if (!mask.effects.has(effect) || effect == ObsEffect::PhaseClock) continue;
            EXPECT_NE(std::find(kSeedOrder.begin(), kSeedOrder.end(), effect), kSeedOrder.end())
                << obsEffectName(effect);
        }
}

// ===========================================================================
// The effect sets of the equation
// ===========================================================================

TEST_F(SeedsTest, ASubsetOfEffectsBuildsOnlyThoseTermsAndNeedsNoAmbiguity) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    // No epoch has run: the model has no ambiguity state.
    const ObsInput in = model().ambiguitySeedInput();
    const Eigen::VectorXd x = states().getStateVector();
    const UserParamView view(states(), x, states().getStateSize());
    EXPECT_FALSE(observationEquation(in, o, "L1W", ParamLookup(view)).usable);
    const ObservationEquation eq = observationEquation(
        in, o, "L1W", ParamLookup(view), effectsOf({ObsEffect::Modeled, ObsEffect::Troposphere}));
    ASSERT_TRUE(eq.usable);
    ASSERT_EQ(eq.n, 2);
    EXPECT_EQ(eq.addend[0].effect, ObsEffect::Modeled);
    EXPECT_EQ(eq.addend[1].effect, ObsEffect::Troposphere);
    // All effects is what a row builds: the default.
    EXPECT_EQ(kAllEffects.bits, (1u << kObsEffectCount) - 1u);
    EXPECT_TRUE(kAllEffects.has(ObsEffect::PhaseColour));
    EXPECT_FALSE(kAllEffects.without(ObsEffect::SsrBias).has(ObsEffect::SsrBias));
}

TEST_F(SeedsTest, ASatelliteTextThatIsNotCanonicalMakesNoSeedWhenATermNeedsTheName) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});
    o.satellite = "G12 ";
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_EQ(model().ambiguitySeed(o, "L1W", x), 0.0);
    const View v(states());
    EXPECT_FALSE(rawResidual(model().residualSeedInput(), v.lookup(), o, "C1W").has_value());
}

// ===========================================================================
// The inter-system bias
// ===========================================================================

TEST_F(SeedsTest, InterSystemBiasSeedsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("G20", System::GPS, {"C1W", "L1W"}, 300.0, 40.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C", "L1C"}, 310.0, 21.0),
    };
    run(sats);
    ASSERT_NE(states().getSystemTimeOffsetIndex("Galileo"), std::numeric_limits<size_t>::max());

    // Code that differs by constellation, so that the medians are not all zero.
    std::vector<PppObservation> epoch_obs = sats;
    for (PppObservation& o : epoch_obs)
        for (auto& [signal, value] : o.observed)
            if (signal[0] == 'C') value += (o.system == System::Galileo ? 31.25 : 4.5)
                                           + 0.37 * static_cast<double>(o.satellite.back());

    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    ASSERT_TRUE(legacy::initializeInterSystemBiases(world(), epoch_obs, old_values, old_reference));

    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), epoch_obs);
    ASSERT_TRUE(seeds.settled);
    EXPECT_EQ(seeds.reference_obs, old_reference);
    ASSERT_EQ(seeds.seeds.size(), old_values.size());
    ASSERT_FALSE(old_values.empty());
    for (std::size_t i = 0; i < old_values.size(); ++i) {
        EXPECT_EQ(std::string(systemName(seeds.seeds[i].sys)), old_values[i].name);
        EXPECT_TRUE(sameBits(seeds.seeds[i].value_m, old_values[i].value)) << old_values[i].name;
        EXPECT_EQ(seeds.seeds[i].n_obs, old_values[i].n);
    }
}

TEST_F(SeedsTest, WithoutTheTimeReferenceTheInterSystemBiasIsNotSettled) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0)};
    update(sats);
    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    EXPECT_FALSE(legacy::initializeInterSystemBiases(world(), sats, old_values, old_reference));
    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_FALSE(seeds.settled);
    EXPECT_TRUE(seeds.seeds.empty());
}

TEST_F(SeedsTest, ASatelliteBelowTheElevationMaskIsLeftOut) {
    load("ppp-rtk");
    const double mask = config().getElevationMaskDeg();
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W"}, 45.0, mask + 20.0),
        observation("G15", System::GPS, {"C1W", "L1W"}, 90.0, mask - 1.0),
        observation("E05", System::Galileo, {"C1C", "L1C"}, 120.0, mask + 10.0),
    };
    update(sats);
    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    ASSERT_TRUE(legacy::initializeInterSystemBiases(world(), sats, old_values, old_reference));
    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_EQ(seeds.reference_obs, old_reference);
    EXPECT_EQ(seeds.reference_obs, 1u);
}

// ===========================================================================
// The cold receiver clock
// ===========================================================================

TEST_F(SeedsTest, ColdClockSeedsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C"}, 120.0, 33.0),
    };
    run(sats);
    std::vector<PppObservation> epoch_obs = sats;
    for (PppObservation& o : epoch_obs)
        for (auto& [signal, value] : o.observed)
            if (signal[0] == 'C') value += 123456.789 + 0.5 * static_cast<double>(o.satellite.back());

    // A cold clock is seeded with the median GPS code residual.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 0.0;
        states().setStateVector(x);
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), epoch_obs);
        ASSERT_TRUE(old.seeded);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), epoch_obs);
        EXPECT_EQ(now.outcome, ColdClockOutcome::Seeded);
        EXPECT_TRUE(now.settled());
        EXPECT_TRUE(sameBits(now.value_m, old.value));
        EXPECT_EQ(now.n_obs, old.n);
    }
    // A clock that is not cold is left alone, and the seed is settled.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 5.0;
        states().setStateVector(x);
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), epoch_obs);
        EXPECT_TRUE(old.settled);
        EXPECT_FALSE(old.seeded);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), epoch_obs);
        EXPECT_EQ(now.outcome, ColdClockOutcome::AlreadySeeded);
        EXPECT_EQ(now.current_m, 5.0);
        EXPECT_TRUE(now.settled());
    }
    // Cold, but no time-reference code this epoch: retry.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 0.0;
        states().setStateVector(x);
        const std::vector<PppObservation> galileo_only = {epoch_obs.back()};
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), galileo_only);
        EXPECT_FALSE(old.settled);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), galileo_only);
        EXPECT_EQ(now.outcome, ColdClockOutcome::NoReference);
        EXPECT_FALSE(now.settled());
    }
}

TEST_F(SeedsTest, UnderBetweenSatelliteDifferencingThereIsNoClockToSeed) {
    load("ppp-rtk", "", "", /*bssd=*/true);
    const std::vector<PppObservation> sats = {observation(kSat, System::GPS, {"C1W", "L1W"})};
    update(sats);
    ASSERT_FALSE(states().hasClock());
    const View v(states());
    const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_EQ(now.outcome, ColdClockOutcome::NoClockState);
    EXPECT_TRUE(now.settled());
    EXPECT_TRUE(legacy::seedColdReceiverClock(world(), sats).settled);
}

// ===========================================================================
// The phase clock
// ===========================================================================

TEST_F(SeedsTest, PhaseClockSeedMatchesTheOldCode) {
    load("ppp-rtk", "", R"(, "estimate_phase_clock": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C"}, 310.0, 21.0),   // code only: no pair
    };
    run(sats);
    ASSERT_TRUE(states().hasPhaseClock());

    double old_value = 0.0;
    size_t old_pairs = 0;
    ASSERT_TRUE(legacy::initializePhaseClockOffset(world(), sats, old_value, old_pairs));
    const View v(states());
    const PhaseClockSeed now = seedPhaseClock(model().residualSeedInput(), v.lookup(), sats);
    ASSERT_TRUE(now.found);
    EXPECT_TRUE(sameBits(now.value_m, old_value));
    EXPECT_EQ(now.n_pairs, old_pairs);
    EXPECT_EQ(now.n_pairs, 3u);

    // No satellite with both a code and a phase observation.
    const std::vector<PppObservation> code_only = {observation(kSat, System::GPS, {"C1W"})};
    double unused = 0.0;
    size_t unused_pairs = 0;
    EXPECT_FALSE(legacy::initializePhaseClockOffset(world(), code_only, unused, unused_pairs));
    EXPECT_FALSE(seedPhaseClock(model().residualSeedInput(), v.lookup(), code_only).found);
}

// ===========================================================================
// The receiver code biases
// ===========================================================================

TEST_F(SeedsTest, WithoutReceiverBiasEstimationThereAreNoCodeBiasSeeds) {
    load("ppp-rtk");   // estimate_ifb is off
    ASSERT_FALSE(config().estimateIfb());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})};
    update(sats);
    const View v(states());
    const CodeBiasSeeds seeds =
        seedCodeBias(model().residualSeedInput(), v.lookup(), sats, {{"GPS_DCB", 1.5}});
    EXPECT_TRUE(seeds.seeds.empty());
    EXPECT_EQ(seeds.refused, 0u);
}

TEST_F(SeedsTest, WithoutAResolverThereAreNoCodeBiasSeeds) {
    load("ppp-rtk", "", R"(, "estimate_ifb": true)");
    ASSERT_TRUE(config().estimateIfb());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})};
    update(sats);
    const View v(states());
    ObsInput in = model().residualSeedInput();
    in.receiver_bias = nullptr;
    EXPECT_TRUE(seedCodeBias(in, v.lookup(), sats, {}).seeds.empty());
}

}  // namespace
