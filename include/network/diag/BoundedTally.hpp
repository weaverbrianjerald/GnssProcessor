#pragma once
/**
 * @file BoundedTally.hpp
 * @brief A count that names what it counts, within a fixed bound: the first few distinct keys with their
 *        counts, and one remainder for every later occurrence of a key not kept.
 *
 * A run-end line that reports "N skipped" without naming what was skipped hides a wrong setup or a silent
 * loss behind an anonymous number (card CNN).  A @ref BoundedTally keeps the first @ref kBoundedTallyCap
 * distinct keys in the order they were first seen, each with its own count, and adds every occurrence of a
 * key past the bound to @ref BoundedTally::more.  The memory is bounded whatever the run's length, the
 * total is exact (@ref BoundedTally::total), and the kept keys are the earliest ones, which are the ones a
 * reader follows to the first event.
 *
 * Fill a tally serially only (from a serial seam, or from a parallel produce's per-station results merged in
 * station order), so that the kept keys and the log line are the same on every run.  Nothing in the solve
 * reads a tally: it exists for the run-end report.
 */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// The number of distinct keys a @ref BoundedTally keeps by name (card CNN, decision 3: ten, then "and N more").
inline constexpr std::size_t kBoundedTallyCap = 10;

/**
 * @brief The first @ref kBoundedTallyCap distinct keys with their counts, and the occurrences of every later key.
 * @tparam Key An equality-comparable key (a name, or an identity a report renders at the run end).
 */
template <class Key>
struct BoundedTally {
    std::vector<std::pair<Key, long long>> kept;   ///< The first distinct keys, in the order first seen, with their counts.
    long long more{0};                             ///< Occurrences of keys past the bound (not named).

    /**
     * @brief Counts @p n occurrences of @p key: on its kept entry, as a new entry while the bound allows, else in
     *        @ref more.
     * @param key The key.
     * @param n The occurrences to add (1 by default).
     */
    void add(const Key& key, long long n = 1) {
        for (auto& [k, c] : kept)
            if (k == key) {
                c += n;
                return;
            }
        if (kept.size() < kBoundedTallyCap) kept.emplace_back(key, n);
        else more += n;
    }

    /**
     * @brief Every occurrence counted: the kept keys' counts and @ref more.
     * @return The total.
     */
    long long total() const {
        long long t = more;
        for (const auto& [k, c] : kept) t += c;
        return t;
    }

    /** @brief Whether nothing was counted.  @return True when the tally is empty. */
    bool empty() const { return kept.empty() && more == 0; }

    /**
     * @brief The tally as one clause, "A x3, B x1 and 5 more", each key rendered by @p name.
     * @tparam NameOf A callable taking a @c const @c Key& and returning a @c std::string.
     * @param name Renders a key.
     * @param none The text of an empty tally.
     * @return The clause.
     */
    template <class NameOf>
    std::string text(NameOf name, const std::string& none = "none") const {
        if (empty()) return none;
        std::string out;
        for (const auto& [k, c] : kept) {
            if (!out.empty()) out += ", ";
            out += name(k) + " x" + std::to_string(c);
        }
        if (more > 0) out += (out.empty() ? "" : " ") + std::string("and ") + std::to_string(more) + " more";
        return out;
    }
};

}  // namespace GnssProcessor
