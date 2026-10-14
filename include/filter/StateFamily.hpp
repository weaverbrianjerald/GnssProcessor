#pragma once

/**
 * @file StateFamily.hpp
 * @brief The dynamic state families of the user-side filter, declared once.
 *
 * `StateManager` keeps the dynamic part of the state vector in one key->index
 * map per family (ambiguities, slant ionosphere, receiver code biases, ...).
 * Before card UOM step U1a every place that had to visit "all families" listed
 * the maps by hand: the state size, the index shift after a removal, the state
 * labels, the RT snapshot's restore kinds, its writer and its reader.  Those
 * lists had drifted apart (the phase-colour family was in three of them and
 * missing from four).  This header is now the one list: a reader finds every
 * family, its label prefix and its snapshot tag in @ref kStateFamilies, and a
 * new family is added by one row here plus its map in `StateManager`.
 *
 * Plain value types and a constant table; no behaviour lives here.  The fixed
 * core block (position, clock, ZWD, velocity, gradients, IMU, the inter-system
 * offsets) is index-stable by construction and is not a dynamic family.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
        PhaseColour,        ///< Per-satellite phase colour state (card LGM); network only.
        SatClock,           ///< Satellite clock ("G01|SC"); network stations only.
    };

    /// Number of dynamic state families.
    inline constexpr std::size_t kStateFamilyCount = 9;

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
    };

    /**
     * @brief Every dynamic state family, in @ref StateFamily order.
     *
     * Label prefixes and snapshot tags are serialized contracts: the labels key
     * the normal-equation accumulators and the RHS attribution
     * (`NetworkRhsAttrib.hpp`), the tags are written to RT snapshot files.
     * Neither may change.
     *
     * The phase-colour family has no snapshot tag: it exists only on network
     * stations, which never write an RT snapshot.
     */
    inline constexpr std::array<StateFamilySpec, kStateFamilyCount> kStateFamilies{{
        { StateFamily::Ionosphere,        "slant ionosphere",    "IONO:",     "ION"    },
        { StateFamily::IonosphereRate,    "ionosphere rate",     "IONORATE:", "IONR"   },
        { StateFamily::Ambiguity,         "ambiguity",           "AMB:",      "AMB"    },
        { StateFamily::ReceiverCodeBias,  "receiver code bias",  "CBIAS:",    "RXCB"   },
        { StateFamily::SatBias,           "satellite code bias", "SATB:",     "SATB"   },
        { StateFamily::SatPhaseBias,      "satellite phase bias","SATPB:",    "SATPB"  },
        { StateFamily::ReceiverPhaseBias, "receiver phase bias", "RPB:",      "RXPB"   },
        { StateFamily::PhaseColour,       "phase colour",        "PCOL:",     nullptr  },
        { StateFamily::SatClock,          "satellite clock",     "SATCLK:",   "SATCLK" },
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

    static_assert([] {
        for (std::size_t i = 0; i < kStateFamilies.size(); ++i)
            if (static_cast<std::size_t>(kStateFamilies[i].family) != i) return false;
        return true;
    }(), "kStateFamilies must list every StateFamily once, in enumerator order");

} // namespace GnssProcessor
