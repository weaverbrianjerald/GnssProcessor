#pragma once

/**
 * @file StateFamily.hpp
 * @brief The dynamic state families of the user-side filter, declared once.
 *
 * `StateManager` keeps the dynamic part of the state vector in one key->index
 * map per family (ambiguities, slant ionosphere, receiver code biases, ...).
 * Every place that visits "all families" (the state size, the index shift after
 * a removal, the state labels, the RT snapshot's restore kinds, its writer and
 * its reader) reads this header, so the lists cannot drift apart.  It is the
 * one list: a reader finds every
 * family in @ref kStateFamilies with its label prefix and snapshot tag, its
 * re-inflation counter, its gap response, the rule that names the satellite of
 * a key and its process-noise rule, the configuration that arms its
 * birth, the source of its initial variance and the events wired to it (printed at run start by `StateManager::logFamiliesAsArmed`), and a new
 * family is added by one row here plus its map in `StateManager`.
 *
 * Plain value types and a constant table, with two small pure lookups; no
 * behaviour lives here.  The fixed core block (position, clock, ZWD, velocity,
 * gradients, IMU, the inter-system offsets) is index-stable by construction and
 * is not a dynamic family.
 */

#include "config/GnssConfigTypes.hpp"
#include "filter/StateKeySchema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace GnssProcessor {

    /**
     * @brief One dynamic state family, i.e. one key->index map of `StateManager`.
     *
     * The enumerator order is the table order of @ref kStateFamilies and the
     * order in which the RT snapshot writer harvests the maps.
     */
    enum class StateFamily : std::uint8_t {
        Ionosphere,         ///< Per-satellite slant ionosphere ("G01").
        IonosphereRate,     ///< Per-satellite ionosphere rate ("G01"); only with `enableIonoRate`.
        Ambiguity,          ///< Carrier ambiguity ("G01_L1C", "G01_L1C_L2W", NL/WL, DD keys).
        ReceiverCodeBias,   ///< Receiver code bias: IFB, DCB, ISC, GLONASS per-slot IFB.
        SatBias,            ///< Satellite code bias ("G01|C1W").
        SatPhaseBias,       ///< Satellite phase bias ("G01|L1C|PB"); network stations only.
        ReceiverPhaseBias,  ///< Receiver phase bias of the eq34 split ("GPS_RPB_L1C"); network only.
        PhaseColour,        ///< Per-satellite phase colour state; network only.
        SatClock,           ///< Satellite clock ("G01|SC"); network stations only.
    };

    /// Number of dynamic state families.
    inline constexpr std::size_t kStateFamilyCount = 9;

    /**
     * @brief The variance re-inflations, in the order of the run-end line.
     *
     * Six belong to a dynamic family (that family's @ref StateFamilySpec::reinflation);
     * the inter-system time offset is a core state.  `StateManager::Reinflation`
     * names this type.
     */
    enum class Reinflation : int {
        CodeBias = 0,       ///< StateManager::reinflateCodeBiasVariance
        SystemTimeOffset,   ///< StateManager::reinflateSystemTimeOffsetVariance (the ISB, a core state)
        Ionosphere,         ///< StateManager::reinflateIonosphereVariance
        IonosphereRate,     ///< StateManager::reinflateIonosphereRateVariance
        SatBias,            ///< StateManager::reinflateSatBiasVariancesForSat (per state)
        SatPhaseBias,       ///< StateManager::reinflateSatPhaseBiasVariancesForSat (per state)
        SatClock,           ///< StateManager::reinflateSatClockVarianceForSat
        Count
    };

    /// The re-inflation slot of a family that has no re-inflation.
    inline constexpr Reinflation kNoReinflation = Reinflation::Count;

    /// The stable short names of the re-inflations in the run-end lines, indexed by
    /// @ref Reinflation; the grading scripts parse them.
    inline constexpr std::array<const char*, static_cast<std::size_t>(Reinflation::Count)>
        kReinflationNames{{ "code_bias", "isb", "iono", "iono_rate", "sat_bias",
                            "sat_phase_bias", "sat_clock" }};

    /**
     * @brief How the satellite of a family's key is read.
     *
     * The process-noise builder gives no Q to a state whose satellite is a frozen
     * BSSD pivot (`StateManager::setFrozenPivotSatellites`); this rule names that
     * satellite.  A family whose key names no satellite has @c None.
     */
    enum class SatOfKey : std::uint8_t {
        None,              ///< The key names no satellite (the receiver-side families).
        Bare,              ///< The key is the satellite ("G01").
        BeforeUnderscore,  ///< Text before the first '_' ("G01_L1C"); a key without one (DD) names none.
        BeforePipe,        ///< Text before the first '|' ("G01|C1W", "G01|L1C|PB"); a key without one is taken whole.
        SatClockSuffix,    ///< "G01|SC" without its suffix; a key without the suffix is taken whole.
    };

    /**
     * @brief The process-noise rule of a family (`StateManager::getProcessNoiseMatrix`).
     */
    enum class QRule : std::uint8_t {
        None,                 ///< No process noise (the network owns the family's prior).
        RandomWalk,           ///< q * dt on the diagonal.
        ObliquityRandomWalk,  ///< q / max(sin^2 el, 0.01) * dt, or the CWNA pair with the rate
                              ///< state; applied on every run that carries slant-ionosphere
                              ///< states (GnssConfig::carriesSlantIonosphere).
        CwnaWithLevel,        ///< The rate half of the ionosphere CWNA pair (same condition).
    };

    /**
     * @brief The configuration that arms a family's birth site.
     *
     * The family's states are created by the measurement model or the network
     * producer; this names the configuration they test, for the run-start table
     * (`StateManager::familiesAsArmed`).  The birth sites decide; the table reports.
     */
    enum class BirthGate : std::uint8_t {
        PhaseRows,                 ///< Every processed phase row (none in a baseline code-only run).
        Uncombined,                ///< processing.uncombined_enabled.
        UncombinedAndIonoRate,     ///< uncombined_enabled and estimation.enable_iono_rate.
        UncombinedAndEstimateIfb,  ///< uncombined_enabled and estimation.estimate_ifb.
        EstimateSatBiases,         ///< estimation.estimate_sat_biases (user rows; on the network, the OSB).
        NetworkSatPhaseBiases,     ///< The network producer with estimation.estimate_sat_phase_biases.
        NetworkPhaseDatumEq34,     ///< The network producer with network.phase_datum_eq34.
        NetworkPhaseColour,        ///< network.phase_colour_state.
        NetworkSatClocks,          ///< The network producer with estimation.estimate_sat_clocks.
    };

    /**
     * @brief Where a family's initial variance comes from.
     */
    enum class InitialStdRule : std::uint8_t {
        ConfigField,     ///< filter.initial_std.<field> (@ref StateFamilySpec::initial_std).
        AmbiguityStd,    ///< GnssConfig::getAmbiguityStd() (1000 m, no JSON key) unless the caller
                         ///< passes an override.
        CallerSupplied,  ///< The creating code passes the variance (network.phase_colour_sigma_zen_m).
    };

    /**
     * @brief The declared properties of one dynamic state family.
     */
    struct StateFamilySpec {
        StateFamily family;        ///< The family this row describes (equals its table position).
        const char* name;          ///< Human-readable name, for tables and log lines.
        const char* label_prefix;  ///< Prefix of `StateManager::getStateLabels` ("AMB:"), a trace contract.
        /// Tag of the RT filter-state snapshot ("AMB"), or nullptr when the family
        /// is not serialized.  A layout holding an unserialized family does not
        /// tile the dynamic block, so the snapshot writer refuses it (fail-safe).
        const char* snapshot_tag;
        /// The run-end counter of the family's variance re-inflation, or
        /// @ref kNoReinflation when the family has none.
        Reinflation reinflation;
        /// True when a data gap zeroes the cross-covariances between this family's
        /// states and the position and clock
        /// (`StateManager::zeroPositionCrossCovariancesToSatStates`): the
        /// per-satellite states, whose correlations describe the pre-gap geometry.
        /// The receiver code bias keeps them (receiver hardware, nearly constant).
        bool gap_decouple;
        /// How the satellite of a key is read (@ref satelliteOfKey); a state whose
        /// satellite is a frozen pivot gets no process noise.
        SatOfKey sat_of_key;
        QRule q_rule;                   ///< The family's process-noise rule.
        /// The @c filter.process_noise field the rule reads (a spectral density), or
        /// nullptr for @c QRule::None.
        double ProcessNoise::* q_psd;
        /// The JSON name of that field under @c filter.process_noise, or nullptr.
        const char* q_key;
        BirthGate birth;                   ///< The configuration that arms the birth site.
        InitialStdRule initial_std_rule;   ///< Where the initial variance comes from.
        /// The @c filter.initial_std field (squared at birth) for
        /// @c InitialStdRule::ConfigField; nullptr otherwise.
        double InitialStd::* initial_std;
        /// The JSON name of that field under @c filter.initial_std, or nullptr.
        const char* initial_std_key;
        bool has_reset;     ///< A single state's value and variance can be reset in place.
        bool prune_cache;   ///< A pruned state is kept in a warm-restart cache.
    };

    /**
     * @brief Every dynamic state family, in @ref StateFamily order.
     *
     * Label prefixes and snapshot tags are serialized contracts: the labels key
     * the normal-equation accumulators and the RHS attribution
     * (`RhsAttrib.hpp`), the tags are written to RT snapshot files.
     * Neither may change.
     *
     * The phase-colour family has no snapshot tag: it exists only on network
     * stations, which never write an RT snapshot.
     */
    inline constexpr std::array<StateFamilySpec, kStateFamilyCount> kStateFamilies{{
        //  family / name / label prefix / snapshot tag
        //      re-inflation, gap decouple, satellite of key
        //      Q rule, Q field, Q key
        //      birth gate, initial-std rule, field, key; reset in place, prune to a cache
        { StateFamily::Ionosphere, "slant ionosphere", "IONO:", "ION",
              Reinflation::Ionosphere, true, SatOfKey::Bare,
              QRule::ObliquityRandomWalk, &ProcessNoise::ionosphere, "ionosphere_q",
              BirthGate::Uncombined, InitialStdRule::ConfigField, &InitialStd::ionosphere, "ionosphere",
              false, false },
        { StateFamily::IonosphereRate, "ionosphere rate", "IONORATE:", "IONR",
              Reinflation::IonosphereRate, true, SatOfKey::Bare,
              QRule::CwnaWithLevel, &ProcessNoise::iono_rate_psd, "iono_rate_psd",
              BirthGate::UncombinedAndIonoRate, InitialStdRule::ConfigField, &InitialStd::ionosphere_rate, "ionosphere_rate",
              false, false },
        { StateFamily::Ambiguity, "ambiguity", "AMB:", "AMB",
              kNoReinflation, true, SatOfKey::BeforeUnderscore,
              QRule::RandomWalk, &ProcessNoise::ambiguity, "ambiguity_q",
              BirthGate::PhaseRows, InitialStdRule::AmbiguityStd, nullptr, nullptr,
              true, false },
        { StateFamily::ReceiverCodeBias, "receiver code bias", "CBIAS:", "RXCB",
              Reinflation::CodeBias, false, SatOfKey::None,
              QRule::RandomWalk, &ProcessNoise::code_bias, "code_bias_q",
              BirthGate::UncombinedAndEstimateIfb, InitialStdRule::ConfigField, &InitialStd::code_bias, "code_bias",
              false, false },
        { StateFamily::SatBias, "satellite code bias", "SATB:", "SATB",
              Reinflation::SatBias, true, SatOfKey::BeforePipe,
              QRule::RandomWalk, &ProcessNoise::sat_bias, "sat_bias_q",
              BirthGate::EstimateSatBiases, InitialStdRule::ConfigField, &InitialStd::sat_bias, "sat_bias",
              true, true },
        { StateFamily::SatPhaseBias, "satellite phase bias", "SATPB:", "SATPB",
              Reinflation::SatPhaseBias, false, SatOfKey::BeforePipe,
              QRule::RandomWalk, &ProcessNoise::sat_phase_bias, "sat_phase_bias_q",
              BirthGate::NetworkSatPhaseBiases, InitialStdRule::ConfigField, &InitialStd::sat_phase_bias, "sat_phase_bias",
              false, false },
        { StateFamily::ReceiverPhaseBias, "receiver phase bias", "RPB:", "RXPB",
              kNoReinflation, false, SatOfKey::None,
              QRule::None, nullptr, nullptr,
              BirthGate::NetworkPhaseDatumEq34, InitialStdRule::ConfigField, &InitialStd::sat_phase_bias, "sat_phase_bias",
              false, false },
        { StateFamily::PhaseColour, "phase colour", "PCOL:", nullptr,
              kNoReinflation, false, SatOfKey::Bare,
              QRule::None, nullptr, nullptr,
              BirthGate::NetworkPhaseColour, InitialStdRule::CallerSupplied, nullptr, nullptr,
              false, false },
        { StateFamily::SatClock, "satellite clock", "SATCLK:", "SATCLK",
              Reinflation::SatClock, false, SatOfKey::SatClockSuffix,
              QRule::RandomWalk, &ProcessNoise::sat_clock, "sat_clock_q",
              BirthGate::NetworkSatClocks, InitialStdRule::ConfigField, &InitialStd::sat_clock, "sat_clock",
              false, false },
    }};

    /**
     * @brief The table row of one family.
     * @param f The family.
     * @return Its row of @ref kStateFamilies.
     */
    inline constexpr const StateFamilySpec& specOf(StateFamily f) {
        return kStateFamilies[static_cast<std::size_t>(f)];
    }

    /**
     * @brief The family whose RT snapshot tag is @p tag.
     * @param tag A snapshot tag as written by the RT snapshot writer ("AMB").
     * @return The family, or nullopt for an unknown tag (a newer writer, or a
     *         corrupt file); families without a tag never match.
     */
    inline std::optional<StateFamily> stateFamilyFromSnapshotTag(std::string_view tag) {
        for (const auto& s : kStateFamilies)
            if (s.snapshot_tag && tag == s.snapshot_tag) return s.family;
        return std::nullopt;
    }

    /**
     * @brief The satellite a key names under @p rule.
     *
     * @c BeforePipe reads the satellite with @c StateKeys::satelliteFromSatBiasKey;
     * @c StateKeys::satelliteFromSatPhaseBiasKey is the same first-pipe split.
     *
     * @param rule The family's rule (@ref StateFamilySpec::sat_of_key).
     * @param key  A key of that family.
     * @return The satellite ID, or nullopt when the key names none.
     */
    inline std::optional<std::string> satelliteOfKey(SatOfKey rule, std::string_view key) {
        switch (rule) {
            case SatOfKey::None:
                return std::nullopt;
            case SatOfKey::Bare:
                return std::string(key);
            case SatOfKey::BeforeUnderscore: {
                const auto us = key.find('_');
                if (us == std::string_view::npos) return std::nullopt;
                return std::string(key.substr(0, us));
            }
            case SatOfKey::BeforePipe:
                return StateKeys::satelliteFromSatBiasKey(key);
            case SatOfKey::SatClockSuffix:
                return StateKeys::satelliteFromSatClockKey(key);
        }
        return std::nullopt;
    }

    static_assert([] {
        for (std::size_t i = 0; i < kStateFamilies.size(); ++i) {
            if (static_cast<std::size_t>(kStateFamilies[i].family) != i) return false;
            if ((kStateFamilies[i].q_rule == QRule::None) != (kStateFamilies[i].q_psd == nullptr))
                return false;
            if ((kStateFamilies[i].initial_std_rule == InitialStdRule::ConfigField)
                != (kStateFamilies[i].initial_std != nullptr))
                return false;
        }
        return true;
    }(), "kStateFamilies must list every StateFamily once, in enumerator order, and give a "
         "process-noise field exactly to the families with a Q rule, and an initial-std "
         "field exactly to the families whose initial variance is a configured field");

} // namespace GnssProcessor
