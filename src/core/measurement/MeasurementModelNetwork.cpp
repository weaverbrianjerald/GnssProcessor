/**
 * @file MeasurementModelNetwork.cpp
 * @brief Network-only surface, owned with the network producer: the parts of MeasurementModel
 *        only the PPP-RTK network producer reaches.
 *
 * Holds the shared satellite-state pre-allocation ensureNetworkSharedStates
 * (the satellite clock, phase-bias and code-bias columns of each station, and
 * the record of the shared states a gate withholds from the station), the
 * absorbed-record append noteAbsorbedSharedState, the phase-bias coverage
 * allowlist test, and carrierFrequency (one network caller).  Every function
 * here is inert outside ProcessingMode::PPP_RTK_Network.  The network sections
 * that remain inside the user pre-pass (phase colour, the LV-1 iono seed frame,
 * the cold clock pre-seed, the GLONASS per-slot receiver-bias filter, the
 * network-owned satellite code bias) are listed in the pre-pass table in
 * MeasurementModel.hpp.  The parameter inventory names
 * MeasurementModel::ensureNetworkSharedStates as a code site
 * (network/assembly/ParamInventory.cpp); the name is load-bearing.
 */

#include "measurement/MeasurementModel.hpp"
#include "common/PppTypes.hpp"
#include "common/SignalBand.hpp"
#include "filter/StateKeySchema.hpp"
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Network producer: carrier frequency for the IF datum pin
    // ===================================================================

    double MeasurementModel::carrierFrequency(const std::string& obs_code,
                                              const std::string& satellite) const {
        System system = System::Unknown;
        if (!satellite.empty()) {
            switch (satellite.front()) {
                case 'G': system = System::GPS;     break;
                case 'E': system = System::Galileo; break;
                case 'R': system = System::GLONASS; break;
                case 'C': system = System::BeiDou;  break;
                case 'J': system = System::QZSS;    break;
                case 'S': system = System::SBAS;    break;
                default:  system = System::Unknown; break;
            }
        }
        return ppp_utils_.getFrequency(obs_code, system, satellite);
    }

    // ===================================================================
    // Network producer: shared satellite-state pre-allocation
    // ===================================================================

    void MeasurementModel::ensureNetworkSharedStates(
        const std::vector<PppObservation>& observations,
        const std::set<std::string>& allowed_phase_keys,
        const std::set<std::string>* cbadm_admitted_code_keys) {
        if (config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network) return;
        if (!config_.isUncombinedEnabled()) return;  // network V1 is uncombined-only

        // The parameter inventory's absorbed instances: every shared
        // state a gate below withholds from this station is noted with the gate's
        // reason, and recordStationParams counts it after the split as Absorbed.  The
        // list is the station's for this epoch, so it starts empty here (the later
        // serial gates append through noteAbsorbedSharedState).  Network mode only:
        // the two returns above leave every other mode, and the user-side filter never
        // calls this function, so a PPP run neither writes nor holds a record.
        absorbed_shared_ = StationAbsorbed{};
        const auto withhold = [this](StateContainer container, const std::string& key,
                                     AbsorbedReason reason) {
            noteAbsorbed(absorbed_shared_, container, key, reason);
        };

        // Empty exclusion set: the network driver runs no KF rebuild loop, so no
        // row is carried-excluded.  Matches the excluded set computeMeasurementUpdate
        // is called with below, keeping this admission gate identical to the build.
        const std::set<std::string> no_excl;

        std::set<std::string> required_clock_keys;
        std::set<std::string> required_phase_keys;
        std::set<std::string> required_code_keys;

        for (const auto& obs : observations) {
            // GLONASS in the network product (code-only scope, enableGlonassNetwork):
            // it gets a shared satellite clock column (allocated below), but a
            // shared per-(sat,signal) phase bias is unphysical under FDMA (the
            // receiver group-delay-vs-frequency response is per receiver and
            // antenna), so the phase-bias branch skips GLONASS.  The receiver group
            // delay is carried by the per-FCN-slot receiver IFB as a local
            // nuisance.  With the flag off (the default), GLONASS is excluded.
            if (obs.system == System::GLONASS && !config_.enableGlonassNetwork())
                continue;

            // Mirror the obs-level admission gates in computeMeasurementUpdate's
            // row-assembly loop, so every allocated shared column is written by at
            // least one row this epoch (no phantom all-zero columns → no spurious
            // null-space dimension for the network rank gate to trip on).
            if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) continue;
            if (obs.range < 1.5e7 || !std::isfinite(obs.range))         continue;
            if (obs.elevation < config_.getElevationMaskDeg())          continue;
            if (obs.los.norm() < 0.5 || !obs.los.allFinite())           continue;
            if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w))            continue;

            bool sat_usable = false;
            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeasurement(config_, log_manager_, bssd_pivot_signals_, obs, ref_type,
                                         no_excl, "")) continue;
                if (ref_type.find('_') != std::string::npos) continue;  // raw signals only
                sat_usable = true;

                // Per-(sat,signal) phase bias on raw phase rows.  Every gate below
                // withholds the shared column only: the phase row is still built
                // (buildJacobianRow writes the phase-bias column only when the state
                // exists), so the station's float ambiguity absorbs the bias, and each
                // withheld state is noted for the parameter inventory with its gate.
                if (config_.estimateSatPhaseBiases()
                    && classifyMeasurement(ref_type) == MeasurementType::Phase) {
                    const std::string pk =
                        StateKeys::makeSatPhaseBiasKey(obs.satellite, ref_type);
                    // GLONASS (code-only network scope): a shared per-(sat,signal)
                    // satellite phase bias is unphysical for FDMA, so GLONASS phase
                    // observations contribute to the satellite clock only (the float
                    // ambiguity absorbs the per-receiver phase nuisance).
                    if (obs.system == System::GLONASS) {
                        withhold(StateContainer::SatPhaseBiasStates, pk,
                                 AbsorbedReason::GlonassCodeOnlyScope);
                        continue;
                    }
                    // Phase-bias coverage gate (network.phase_bias_min_station_fraction):
                    // a signal tracked too sparsely across the network gets no phase-bias
                    // state; the float ambiguity absorbs the bias and the signal is not
                    // AR-eligible.  The observation is still used (the phase row is built);
                    // only the shared bias column is withheld.  Empty allowlist = admit all.
                    if (!phaseBiasAdmitted(PppUtils::getSystemString(obs.satellite), ref_type)) {
                        withhold(StateContainer::SatPhaseBiasStates, pk,
                                 AbsorbedReason::CoverageGate);
                        continue;
                    }
                    // Legacy single-pivot datum: only the pivot station's keys are shared.
                    if (!allowed_phase_keys.empty() && !allowed_phase_keys.count(pk)) {
                        withhold(StateContainer::SatPhaseBiasStates, pk,
                                 AbsorbedReason::Allowlist);
                        continue;
                    }
                    // Phase co-gate, symmetric with the code sponge (CB-ADM): when
                    // the run-latched census has quarantined this signal's code pair,
                    // its shared phase bias is withheld as well.  The phase then folds
                    // into each station's float ambiguity as the code folds into the
                    // local sponge, so a 2-station signal leaves the shared system on
                    // both families and cannot leave an orphaned pure-PB near-null
                    // gauge (which a code-only quarantine produces).  The gate sits at
                    // state allocation so getSatPhaseBiasStates()/shared_phase_keys stay
                    // consistent everywhere (a gate at the registration site breaks
                    // the invariant that every shared_phase_key is a column).
                    // Companion code key = same sat + L<band><attr> -> C<band><attr>.
                    // A null pointer (flag off or census inactive) is byte-identical.
                    if (cbadm_admitted_code_keys) {
                        std::string csig = ref_type;
                        if (!csig.empty()) csig[0] = 'C';
                        if (!cbadm_admitted_code_keys->count(obs.satellite + "|" + csig)) {
                            withhold(StateContainer::SatPhaseBiasStates, pk,
                                     AbsorbedReason::CodeCensusCogate);
                            continue;
                        }
                    }
                    required_phase_keys.insert(pk);
                    if (state_manager_.getSatPhaseBiasIndex(pk) < 0)
                        state_manager_.addSatPhaseBiasState(pk, 0.0);
                }

                // Per-(sat,signal) code bias (OSB) on raw code rows, a shared border
                // column for every constellation except GLONASS (GPS and Galileo in
                // the network scope): FDMA makes a single shared per-(sat,signal) OSB
                // unphysical for GLONASS, whose receiver group delay stays in the
                // per-FCN-slot rx IFB and whose code feeds the satellite clock alone.
                // (Checked: the per-slot IFB exists only on the FDMA bands
                // at a known slot k != 0, and the GLONASS branch below is not reached
                // in a loaded run, because the loader refuses
                // network.estimate_sat_code_bias together with network.estimate_glonass.)
                // Allocated at 0 because, like the clock and phase bias, this is a
                // shared state that the network solve treats as absolute
                // (linearization point 0, never written back), so the innovation
                // carries the full bias and the joint solve recovers it.
                // buildJacobianRow's sat_bias_term writes the +1 Jacobian.
                if (config_.estimateNetworkSatCodeBias()
                    && classifyMeasurement(ref_type) == MeasurementType::Code) {
                    const std::string cbk = obs.satellite + "|" + ref_type;  // matches sat_bias_term key
                    if (obs.system == System::GLONASS) {
                        withhold(StateContainer::SatBiasStates, cbk,
                                 AbsorbedReason::GlonassCodeOnlyScope);
                    } else {
                        required_code_keys.insert(cbk);
                        if (state_manager_.getSatBiasIndex(cbk) < 0)
                            state_manager_.addSatBiasState(cbk, 0.0);
                    }
                }
            }

            // One clock per satellite with any usable raw measurement.
            if (config_.estimateSatClocks() && sat_usable) {
                const std::string ck = StateKeys::makeSatClockKey(obs.satellite);
                required_clock_keys.insert(ck);
                if (state_manager_.getSatClockIndex(ck) < 0)
                    state_manager_.addSatClockState(ck, 0.0);
            }
        }

        // Drop shared states whose satellite/signal is not usable this epoch, so
        // the per-station column set equals this epoch's required set.
        if (config_.estimateSatClocks())
            state_manager_.pruneSatClockStatesExcept(required_clock_keys);
        if (config_.estimateSatPhaseBiases())
            state_manager_.pruneSatPhaseBiasStatesExcept(required_phase_keys);
        if (config_.estimateNetworkSatCodeBias())
            state_manager_.pruneSatBiasStatesExcept(required_code_keys);
    }

    // ===================================================================
    // Network producer: the absorbed record and the coverage allowlist
    // ===================================================================

    void MeasurementModel::noteAbsorbedSharedState(StateContainer container, std::string_view key,
                                                   AbsorbedReason reason) {
        noteAbsorbed(absorbed_shared_, container, key, reason);
    }

    bool MeasurementModel::phaseBiasAdmitted(const std::string& system,
                                             const std::string& signal) const {
        if (phase_bias_signal_allowlist_.empty()) return true;
        auto it = phase_bias_signal_allowlist_.find(system);
        if (it == phase_bias_signal_allowlist_.end()) return true;
        return it->second.count(signal) > 0;
    }

} // namespace GnssProcessor
