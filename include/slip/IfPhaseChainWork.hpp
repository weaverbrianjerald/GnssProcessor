#pragma once

/**
 * @file IfPhaseChainWork.hpp
 * @brief The values of one gradient chain's double-difference step
 *        (@c IfPhaseSlipDetector::processDoubleDifferences): what it reads,
 *        which reference arm it runs, what it returns, and the working value
 *        its steps share.
 *
 * processDoubleDifferences took twelve parameters, three of them outputs.  They
 * are now three values: @c ChainEpoch (the chain's epoch, built by @c detect),
 * @c RefArmPlan (which reference arm runs, §14.10.4 and §22.6) and
 * @c ChainResult (the double differences, the reset keys, the reference-arm
 * outcome).  The function itself is a short driver over the steps D1 to D7 (the
 * list in IfPhaseSlipDetector.cpp), which share one @c ChainWork; each step
 * opens with an alias preamble (for example
 * @c const auto& sys_str = w.chain.sys_str;) naming exactly the members it
 * reads (@c const) and writes, so its body reads the names it always had.
 *
 * Internal to the detector: included by IfPhaseSlipDetector.cpp and
 * IfPhaseStagedUserPath.cpp only.
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief One gradient chain's epoch: the input of the double-difference step.
 *
 * Built by @c detect after the reference selection and the between-satellite
 * single differencing; every member refers to (or copies) a value of that
 * call.
 */
struct IfPhaseSlipDetector::ChainEpoch {
    /// Chain key: "GPS" for a satellite's first band pair, "GPS|L1C_L5Q" for an
    /// additional chain (its '|' marks it).  Names the chain in log lines and
    /// per-chain telemetry.
    const std::string& sys_str;
    const std::vector<SatData>& sat_vec;              ///< The chain's satellites this epoch.
    const std::map<std::string, double>& grad_phi_if; ///< Current-epoch between-satellite gradients (m).
    double lambda_if;                                 ///< Median IF wavelength of the chain (m).
    bool ref_changed;                                 ///< The reference satellite changed this epoch.
    /// Epoch-to-epoch receiver position change (m); 0 for a pinned station.
    /// Read by the staged geometry correction only.
    const Eigen::Vector3d& delta_pos;
    double position_sigma_3d;                         ///< The caller's 3D position sigma (m), for the slip log line.
};

/**
 * @brief Which reference arm a chain runs this epoch (§14.10.4, §22.6).
 *
 * The first chain of a system runs its own arm.  An additional chain runs its
 * own arm only with @c setRefArmAllChains and the hardened gate on an
 * own-elected reference, or inherits the first chain's verdict on an adopted
 * one.  processDoubleDifferences refuses both on an additional chain while the
 * hardened gate is off (the second lock), on its own copy of the plan.
 */
struct IfPhaseSlipDetector::RefArmPlan {
    bool run_ref_arm{ true };                  ///< Run the chain's own reference-satellite slip test (tier T1r).
    /// The first chain's verdict to inherit on an adopted reference; nullptr
    /// for none.  Applied only when it fired and names this chain's reference.
    const RefArmOutcome* inherit{ nullptr };
};

/**
 * @brief What the double-difference step returns for one chain.
 */
struct IfPhaseSlipDetector::ChainResult {
    /// Geometry- and reference-slip-corrected between-epoch double
    /// differences (m) by satellite; empty when the chain's reference is
    /// absent this epoch.
    std::map<std::string, double> delta_grad;
    /// Ambiguity keys this chain flags ("G01_L1C_L2W"), in flag order; detect
    /// appends them to the epoch's reset keys.
    std::vector<std::string> reset_keys;
    /// This chain's reference-arm verdict ("" satellite: the arm did not run or
    /// had no usable family).  Recorded on every chain; detect publishes the
    /// first chain's to the additional chains that adopted its reference.
    RefArmOutcome outcome;
};

/**
 * @brief The working value of one processDoubleDifferences call, shared by its
 *        steps D1 to D7; discarded when the call returns.
 */
struct IfPhaseSlipDetector::ChainWork {
    /**
     * @brief Bind the chain's epoch and state.
     * @param chain_in  The chain's epoch.
     * @param cs_in     The chain's persistent state (written by D2, D5, D6).
     * @param logger_in The "if_slip_detector" module logger.
     */
    ChainWork(const ChainEpoch& chain_in, ConstellationState& cs_in,
              std::shared_ptr<spdlog::logger> logger_in)
        : chain(chain_in), cs(cs_in), logger(std::move(logger_in)) {}

    const ChainEpoch& chain;                 ///< The chain's epoch.
    ConstellationState& cs;                  ///< The chain's persistent state.
    std::shared_ptr<spdlog::logger> logger;  ///< "if_slip_detector" logger.

    // --- D1: the satellite lookup and the reference row -------------------------
    /// The chain's satellite rows by id (this epoch).
    std::map<std::string, const SatData*> sat_map;
    /// The chain's reference satellite's row; nullptr when it is absent (the
    /// driver then returns an empty result).
    const SatData* ref_data = nullptr;

    // --- D3 onward ---------------------------------------------------------------
    ChainResult result;                      ///< The step's result, built in place.
    /// Every satellite has a finite double difference (selects the staged
    /// per-station outlier test or its fixed-threshold fallback).
    bool all_finite = true;
};

} // namespace GnssProcessor
