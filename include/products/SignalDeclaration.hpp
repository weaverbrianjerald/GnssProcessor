#pragma once
/**
 * @file SignalDeclaration.hpp
 * @brief The producer's declared signal roster as it travels on the network
 *        product wire, and the helpers that write and parse it (card SID
 *        phase 5, E2).
 *
 * ### Purpose
 *
 * Without a declaration, the set of corrections a consumer applies can change
 * with no event row, and the consumer cannot ask whether the network carries a
 * family for the signal it tracks at all.  The SID audit
 * (`runs/_msc/sid_audit_report_2026-09-22.md`, Part C) lists this as an
 * information gate with no identity at either edge; card MSC measured its
 * effect on consumer code-bias dropouts.
 *
 * The declaration states, in the wire's own grammar, per constellation and
 * RINEX band digit: which phase and code signals the network models, which
 * codes travel as card SCC companions, and which signal the S-basis pins.  A
 * consumer that holds it can distinguish "the network does not carry this
 * family" from "the record is stale", which the freshness flip alone cannot.
 *
 * ### Grammar
 *
 * One record per (constellation, band digit), written as a contiguous block:
 *
 * @verbatim
 *   N,<constellation>,<band digit>[,ph=<sig>[+<sig>...]][,co=<sig>[+<sig>...]]
 *                                 [,comp=<sig>[+<sig>...]][,dph=<sig>][,dco=<sig>]
 * @endverbatim
 *
 * The `key=value` tokens follow the format's self-describing convention, so a
 * reader that predates a future token ignores it.  A token whose list is empty
 * is omitted rather than written as a placeholder, so "no phase family on this
 * band" is the absence of `ph=`, which is a statement the consumer can act on.
 *
 * The signals of `ph=` and `co=` are ordered by the number of producer stations
 * that model them, descending, with a lexical tie break.  This is the network's
 * preference order, which a consumer keyed to the declaration walks.
 *
 * ### Block semantics
 *
 * A declaration replaces the previous one entirely.  The producer therefore
 * writes every row of the current roster together, and the reader treats the
 * first `N` record after any non-`N` line as the start of a new declaration.
 * The block is written only when the declaration changes (and so at the first
 * emitted epoch), following the S-record precedent.  A consumer that joins
 * mid-file has no declaration until the next change; this revision has no
 * periodic re-broadcast.
 */

#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief One (constellation, band digit) row of the declared roster.
     *
     * Every list is in the producer's own preference order (station support
     * descending, lexical tie break) and never contains duplicates.
     */
    struct SignalDeclarationBand {
        std::vector<std::string> phase;      ///< Phase signals the network models on this band.
        std::vector<std::string> code;       ///< Code signals the network models on this band.
        std::vector<std::string> companion;  ///< Companion codes emitted beside the modelled code.
        std::string              datum_phase;///< The phase the S-basis pinned, or empty.
        std::string              datum_code; ///< The code the S-basis pinned, or empty.

        bool operator==(const SignalDeclarationBand& o) const {
            return phase == o.phase && code == o.code && companion == o.companion
                   && datum_phase == o.datum_phase && datum_code == o.datum_code;
        }
        bool operator!=(const SignalDeclarationBand& o) const { return !(*this == o); }
        /// @brief True when the row names nothing at all (never written to the wire).
        bool empty() const {
            return phase.empty() && code.empty() && companion.empty()
                   && datum_phase.empty() && datum_code.empty();
        }
    };

    /**
     * @brief The producer's whole declared roster: constellation -> band digit -> row.
     *
     * @c present distinguishes "the producer declared an empty roster" from "no
     * declaration was ever seen", which are different statements to a consumer:
     * the first says the network carries nothing; the second says the wire
     * predates the declaration and nothing may be inferred from it.
     */
    struct NetworkSignalDeclaration {
        bool present{ false };
        std::map<std::string, std::map<char, SignalDeclarationBand>> bands;

        bool operator==(const NetworkSignalDeclaration& o) const {
            return present == o.present && bands == o.bands;
        }
        bool operator!=(const NetworkSignalDeclaration& o) const { return !(*this == o); }

        /// @brief The row for one (constellation, band digit), or nullptr.
        const SignalDeclarationBand* find(const std::string& cons, char band) const;

        /// @brief True when the declaration names this constellation at all.
        bool knowsConstellation(const std::string& cons) const {
            return bands.find(cons) != bands.end();
        }

        /**
         * @brief Whether the network models this phase signal on this constellation
         *        (on any band).
         *
         * A false answer on a declaration whose @c present is true is a positive
         * statement: the producer carries no phase-bias family for that signal,
         * so its absence from the wire is structural rather than a staleness.
         */
        bool declaresPhase(const std::string& cons, const std::string& signal) const;

        /// @brief The code twin of @ref declaresPhase.  A card SCC companion counts.
        bool declaresCode(const std::string& cons, const std::string& signal) const;
    };

    /**
     * @brief Serialize a declaration into its wire lines (the `N` block).
     *
     * @param decl The declaration.
     * @return One string per record, without the trailing newline, in
     *         (constellation, band digit) order.  Empty rows and rows under an
     *         empty constellation are skipped, so a declaration that names
     *         nothing produces no lines.
     */
    std::vector<std::string> signalDeclarationToWire(const NetworkSignalDeclaration& decl);

    /**
     * @brief Parse one already-split `N` record into @p out.  Pure function.
     *
     * @param f   The comma-split fields of the line; @c f[0] has to be "N" (not
     *            checked here).
     * @param out Receives the row and has @c present set; the caller owns block
     *            accumulation (a record replaces the row it names and leaves every
     *            other row alone).  Unknown key=value tokens are ignored.
     * @return false when the record is malformed (fewer than three fields, an
     *         empty constellation, or a band field that is not a single
     *         character), in which case @p out is untouched.
     */
    bool signalDeclarationParseRecord(const std::vector<std::string>& f,
                                      NetworkSignalDeclaration& out);

    /// @brief A one-line human summary of a declaration, for log text.
    std::string signalDeclarationSummary(const NetworkSignalDeclaration& decl);

    /**
     * @brief Why a consumer's correction for one signal stopped being applied
     *        (card SID phase 5, E3).
     *
     * The values are tested in declaration order, and each applies only when the
     * ones before it did not.  Only @ref Undeclared states that the network
     * carries nothing for the signal by construction, which is the class the
     * phase 5b response keys on.
     */
    enum class FlipAttribution {
        NoDeclaration,  ///< no declaration was held, so the flip cannot be attributed.
        Undeclared,     ///< the declaration names no family for this signal: structurally absent.
        OffWire,        ///< it does, but this satellite has no record: the family is absent here.
        Stale           ///< a record exists and the age gate refused it: a genuine staleness.
    };

    /// @brief Human-readable name of an attribution, for the trace and the table.
    const char* flipAttributionName(FlipAttribution a);

    /**
     * @brief Attribute one freshness flip against the declaration and the wire.
     *        Pure function.
     *
     * @param decl     The declaration the consumer holds.
     * @param cons     Constellation display name of the satellite.
     * @param signal   RINEX signal the correction is keyed on.
     * @param is_phase true for a phase bias, false for a code bias.
     * @param on_wire  Whether a store read without the age gate finds a record
     *                 for this key.
     */
    FlipAttribution signalDeclarationAttributeFlip(const NetworkSignalDeclaration& decl,
                                                   const std::string& cons,
                                                   const std::string& signal,
                                                   bool is_phase, bool on_wire);

} // namespace GnssProcessor
