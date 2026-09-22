#pragma once
/**
 * @file SignalDeclaration.hpp
 * @brief Card SID phase 5 (E2): the producer's DECLARED SIGNAL ROSTER as it
 *        travels on the network product wire, and the helpers that write and
 *        parse it.
 *
 * ### The gap this closes
 *
 * The audit of 2026-09-22 (`runs/_msc/sid_audit_report_2026-09-22.md`, Part C)
 * found two information gates with identity at NEITHER edge, and one of them is
 * the consumer's: which corrections a consumer applies changes with no event
 * row, and the consumer has no way to ask whether the network carries a family
 * for the signal it tracks at all.  Card MSC measured the consequence: 50
 * code-bias dropouts across seven sites, and a consumer that leaves by up to
 * 70 m between two of them.
 *
 * The declaration is the producer's answer, stated on the wire in the wire's own
 * grammar: per constellation and RINEX band digit, which PHASE and CODE signals
 * the network models, which codes ride as card SCC companions, and which signal
 * the S-basis pins.  A consumer that holds it can distinguish "the network does
 * not carry this family" from "the record is stale", which is the split the
 * freshness flip cannot make today.
 *
 * ### Grammar
 *
 * One record per (constellation, band digit), written as a CONTIGUOUS BLOCK:
 *
 * @verbatim
 *   N,<constellation>,<band digit>[,ph=<sig>[+<sig>...]][,co=<sig>[+<sig>...]]
 *                                 [,comp=<sig>[+<sig>...]][,dph=<sig>][,dco=<sig>]
 * @endverbatim
 *
 * The `key=value` tokens follow the format's own self-describing convention, so
 * a reader that predates a future token ignores it.  A token whose list is empty
 * is omitted rather than written as a placeholder, so "no phase family on this
 * band" is the ABSENCE of `ph=`, which is a statement the consumer can act on.
 *
 * The signals of `ph=` and `co=` are ordered by the number of producer stations
 * that model them, descending, with a lexical tie break: the network's own
 * preference order, which is what a consumer keyed to the declaration walks.
 *
 * ### Block semantics
 *
 * A declaration REPLACES the previous one wholesale.  The producer therefore
 * writes every row of the current roster together, and the reader treats the
 * first `N` record after any non-`N` line as the start of a fresh declaration.
 * The block is written only when the declaration CHANGES (and at the first
 * emitted epoch), on the S-record precedent; a consumer that joins mid-file
 * therefore has none until the next change, which is a stated limitation of
 * this revision rather than an oversight.
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
        std::vector<std::string> phase;      ///< PHASE signals the network models on this band.
        std::vector<std::string> code;       ///< CODE signals the network models on this band.
        std::vector<std::string> companion;  ///< Card SCC companion codes riding beside the modelled code.
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
     * @c present distinguishes "the producer declared an EMPTY roster" from "no
     * declaration was ever seen", which are different statements to a consumer:
     * the first says the network carries nothing, the second says the wire is
     * older than this card and nothing may be inferred from it.
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
         * @brief Does the network model this PHASE signal on this constellation?
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
     * @return One string per record, WITHOUT the trailing newline, in
     *         (constellation, band digit) order.  Empty rows are skipped, so a
     *         declaration with nothing to say produces no lines at all.
     */
    std::vector<std::string> signalDeclarationToWire(const NetworkSignalDeclaration& decl);

    /**
     * @brief Parse one already-split `N` record into @p out.  PURE.
     *
     * @param f   The comma-split fields of the line; @c f[0] must be "N".
     * @param out Receives the row; the caller owns block accumulation (a record
     *            REPLACES the row it names and leaves every other row alone).
     * @return false when the record is malformed (fewer than three fields, an
     *         empty constellation, or a band field that is not a single
     *         character), in which case @p out is untouched.
     */
    bool signalDeclarationParseRecord(const std::vector<std::string>& f,
                                      NetworkSignalDeclaration& out);

    /// @brief A one-line human summary of a declaration, for log text.
    std::string signalDeclarationSummary(const NetworkSignalDeclaration& decl);

    /**
     * @brief Card SID phase 5 (E3): why a consumer's correction for one signal
     *        stopped being applied.
     *
     * The three answers are ordered: each is only meaningful when the one before
     * it did not apply, and only @ref Undeclared is a statement that the network
     * STRUCTURALLY carries nothing, which is the class phase 5b's response keys
     * on.
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
     * @brief Attribute one freshness flip against the declaration and the wire.  PURE.
     *
     * @param decl     The declaration the consumer holds.
     * @param cons     Constellation display name of the satellite.
     * @param signal   RINEX signal the correction is keyed on.
     * @param is_phase true for a phase bias, false for a code bias.
     * @param on_wire  Whether an UNGATED store read finds a record for this key.
     */
    FlipAttribution signalDeclarationAttributeFlip(const NetworkSignalDeclaration& decl,
                                                   const std::string& cons,
                                                   const std::string& signal,
                                                   bool is_phase, bool on_wire);

} // namespace GnssProcessor
