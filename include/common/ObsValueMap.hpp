#pragma once

/**
 * @file ObsValueMap.hpp
 * @brief Flat, layout-shared replacement for the per-observation
 *        @c std::map channels carried by @c RinexObservation.
 *
 * A RINEX observation file declares, in its header, the ordered set of
 * observation codes recorded for each GNSS system (the @c "SYS / # / OBS TYPES"
 * records).  Every epoch/satellite record then carries a value for some subset
 * of those declared codes.  Storing each record's values in three
 * @c std::map<std::string,…> instances (measurements, variances, LLI) costs,
 * per observation, three red-black trees, a heap node per code (about 36 node
 * allocations) and a copy of the obs-code string in every node, about 2.7 KB
 * per observation.  At network scale (hundreds of stations over a full day)
 * that overhead dominates the working set.
 *
 * @c ObsValueMap reduces that to:
 *   - one @b shared @c ObsCodeIndex per (file, system): the obs-code strings and
 *     their code→index mapping are stored once and referenced (@c shared_ptr)
 *     by every observation of that system, including the copies that
 *     @c MultiRinexSync makes when grouping and synchronising epochs and the
 *     long-lived @c synced_ store held by the network solve; and
 *   - one flat @c std::vector<T> per channel, indexed by the shared layout, with
 *     a sentinel marking absent codes: a single heap allocation instead of a
 *     tree of nodes.
 *
 * The container mirrors the subset of the @c std::map API used on these
 * channels (@c operator[], @c at, @c count, @c find, range-for, @c size,
 * @c empty, @c clear, assignment), so the parser and the downstream consumers
 * compile against it without call-site changes.  Iteration is in sorted code
 * order, the same as @c std::map, so consumer logic that depends on key order
 * behaves as before and results stay bit-identical.
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Shared obs-code ↔ flat-index table for one (file, system).
     *
     * Built once from the RINEX header's declared obs-type order (RINEX 3 codes
     * verbatim; RINEX 2 codes translated to their v3 form) and shared by every
     * @c ObsValueMap channel of every observation of that system.  Streaming
     * producers (SBF / RTCM / Meas3) that discover codes incrementally grow a
     * private layout via @c getOrAdd.
     *
     * @c sorted holds the indices ordered by code string so that @c ObsValueMap
     * iteration reproduces @c std::map key ordering.
     */
    struct ObsCodeIndex {
        std::vector<std::string> codes;                      ///< index → obs code (declared/insertion order)
        std::unordered_map<std::string, int> code_to_index;  ///< obs code → index
        std::vector<int> sorted;                             ///< indices ordered by code (std::map iteration order)

        /** @brief Number of distinct codes in the layout. */
        std::size_t size() const { return codes.size(); }

        /**
         * @brief Index of @p code, or -1 if not in the layout.
         */
        int find(const std::string& code) const {
            const auto it = code_to_index.find(code);
            return it == code_to_index.end() ? -1 : it->second;
        }

        /**
         * @brief Return the index of @p code, appending it (and rebuilding the
         *        sorted view) if not already present.
         *
         * Only the rare ad-hoc/streaming producer path grows a layout here; the
         * RINEX bulk path pre-builds the layout from the header so every write
         * hits an existing index and never mutates the shared table.
         */
        int getOrAdd(const std::string& code) {
            const auto it = code_to_index.find(code);
            if (it != code_to_index.end()) return it->second;
            const int idx = static_cast<int>(codes.size());
            codes.push_back(code);
            code_to_index.emplace(code, idx);
            rebuildSorted();
            return idx;
        }

        /** @brief Recompute @c sorted from the current @c codes. */
        void rebuildSorted() {
            sorted.resize(codes.size());
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) sorted[i] = i;
            std::sort(sorted.begin(), sorted.end(),
                      [this](int a, int b) { return codes[a] < codes[b]; });
        }

        /**
         * @brief Build a shared layout from an ordered list of obs codes.
         * @param cs Declared (already v3-translated) obs codes, in header order.
         *           Duplicates are dropped, keeping the first occurrence.
         */
        static std::shared_ptr<ObsCodeIndex> fromCodes(const std::vector<std::string>& cs) {
            auto p = std::make_shared<ObsCodeIndex>();
            p->codes.reserve(cs.size());
            p->code_to_index.reserve(cs.size());
            for (const auto& c : cs) {
                if (p->code_to_index.find(c) != p->code_to_index.end()) continue;
                const int idx = static_cast<int>(p->codes.size());
                p->codes.push_back(c);
                p->code_to_index.emplace(c, idx);
            }
            p->rebuildSorted();
            return p;
        }
    };

    /**
     * @brief Per-value-type "absent" sentinel and presence test.
     *
     * Measurement and variance values are never NaN and LLI flags are 0..9, so
     * these sentinels never collide with real data.  @c operator[] inserts a
     * value-initialised (0) entry, as @c std::map does; 0 is distinct from the
     * absent sentinel, so a code touched via @c operator[] reads back as
     * present even when its value is 0.
     */
    template <typename T> struct ObsAbsent;
    template <> struct ObsAbsent<double> {
        static double value() { return std::numeric_limits<double>::quiet_NaN(); }
        static bool present(double v) { return !std::isnan(v); }
    };
    template <> struct ObsAbsent<int> {
        static int value() { return INT_MIN; }
        static bool present(int v) { return v != INT_MIN; }
    };

    /**
     * @brief Flat, layout-shared map from obs code to a value of type @p T.
     *
     * Drop-in for the subset of @c std::map<std::string,T> the observation
     * channels use.  Backed by a shared @c ObsCodeIndex (the obs-code strings)
     * and a flat @c std::vector<T> (the values, sentinel for holes).  Copies are
     * cheap: the value vector is cloned (one allocation) and the layout pointer's
     * reference count is incremented; the obs-code strings are not copied.
     *
     * @tparam T @c double for measurements/variances, @c int for LLI flags.
     */
    template <typename T>
    class ObsValueMap {
    public:
        using mapped_type = T;
        using Absent = ObsAbsent<T>;

        std::shared_ptr<ObsCodeIndex> layout;  ///< Shared code↔index table (null until first use).
        std::vector<T> vals;                   ///< Values indexed by layout index; @c Absent for holes.

        ObsValueMap() = default;

        /**
         * @brief Attach a prebuilt shared layout and size the value store to it,
         *        all-absent (the RINEX bulk path).
         *
         * After this, @c operator[] for any declared code hits an existing index
         * with no layout growth and no per-observation string allocation.
         */
        void attachLayout(std::shared_ptr<ObsCodeIndex> shared) {
            layout = std::move(shared);
            vals.assign(layout ? layout->size() : 0u, Absent::value());
        }

        /** @brief True when no code is present. */
        bool empty() const {
            for (const T& v : vals) if (Absent::present(v)) return false;
            return true;
        }

        /** @brief Number of present codes (matches @c std::map::size). */
        std::size_t size() const {
            std::size_t n = 0;
            for (const T& v : vals) if (Absent::present(v)) ++n;
            return n;
        }

        /** @brief Mark every slot absent (matches @c std::map::clear semantics for callers that only re-test presence). */
        void clear() { std::fill(vals.begin(), vals.end(), Absent::value()); }

        /** @brief 1 if @p code is present, else 0. */
        std::size_t count(const std::string& code) const {
            const int i = idxOf(code);
            return (i >= 0 && Absent::present(vals[static_cast<std::size_t>(i)])) ? 1u : 0u;
        }

        /**
         * @brief Reference to the value for @p code.
         * @throws std::out_of_range if @p code is not present (matches @c std::map::at).
         */
        const T& at(const std::string& code) const {
            const int i = idxOf(code);
            if (i >= 0 && Absent::present(vals[static_cast<std::size_t>(i)]))
                return vals[static_cast<std::size_t>(i)];
            throw std::out_of_range("ObsValueMap::at: code '" + code + "' not present");
        }
        /** @overload */
        T& at(const std::string& code) {
            const int i = idxOf(code);
            if (i >= 0 && Absent::present(vals[static_cast<std::size_t>(i)]))
                return vals[static_cast<std::size_t>(i)];
            throw std::out_of_range("ObsValueMap::at: code '" + code + "' not present");
        }

        /**
         * @brief Insert-or-access (matches @c std::map::operator[]).
         *
         * If @p code is absent it is inserted with a value-initialised (0) mapped
         * value (creating a private layout when none is attached), and a
         * reference to that slot is returned.
         */
        T& operator[](const std::string& code) {
            if (!layout) layout = std::make_shared<ObsCodeIndex>();
            int i = layout->find(code);
            if (i < 0) {
                // The code is not in the layout yet.  If the layout is shared (the
                // RINEX bulk path shares one table across every observation of a
                // system), copy it first so that growing it here cannot change the
                // table other observations use.  The RINEX path writes only
                // declared codes and never reaches this branch; the copy keeps
                // ad-hoc and streaming writes safe without relying on that.
                if (layout.use_count() > 1)
                    layout = std::make_shared<ObsCodeIndex>(*layout);
                i = layout->getOrAdd(code);
            }
            if (static_cast<int>(vals.size()) <= i)
                vals.resize(layout->size(), Absent::value());
            T& slot = vals[static_cast<std::size_t>(i)];
            if (!Absent::present(slot)) slot = T(0);
            return slot;
        }

        /**
         * @brief Materialise a real @c std::map, for the sinks that still hold
         *        @c std::map channels (for example @c PppObservation).
         */
        std::map<std::string, T> toStdMap() const {
            std::map<std::string, T> out;
            if (!layout) return out;
            const std::size_t n = std::min(vals.size(), layout->codes.size());
            for (std::size_t i = 0; i < n; ++i)
                if (Absent::present(vals[i])) out.emplace(layout->codes[i], vals[i]);
            return out;
        }

        // -------------------------------------------------------------------
        // Forward iteration in sorted code order, skipping absent slots.
        // -------------------------------------------------------------------
        template <bool Const>
        class Iter {
        public:
            using container = typename std::conditional<Const, const ObsValueMap, ObsValueMap>::type;
            using value_ref = typename std::conditional<Const, const T&, T&>::type;
            using pair_type = std::pair<const std::string&, value_ref>;

            Iter(container* m, std::size_t pos) : m_(m), pos_(pos) { skipForward(); }

            pair_type operator*() const {
                const int idx = m_->layout->sorted[pos_];
                return pair_type(m_->layout->codes[static_cast<std::size_t>(idx)],
                                 m_->vals[static_cast<std::size_t>(idx)]);
            }

            /** Arrow proxy so @c it->first / @c it->second resolve like a map iterator. */
            struct ArrowProxy {
                pair_type p;
                const pair_type* operator->() const { return &p; }
            };
            ArrowProxy operator->() const { return ArrowProxy{ **this }; }

            Iter& operator++() { ++pos_; skipForward(); return *this; }
            bool operator==(const Iter& o) const { return pos_ == o.pos_; }
            bool operator!=(const Iter& o) const { return pos_ != o.pos_; }

        private:
            void skipForward() {
                if (!m_ || !m_->layout) { pos_ = 0; return; }
                const auto& s = m_->layout->sorted;
                while (pos_ < s.size()) {
                    const int idx = s[pos_];
                    if (idx < static_cast<int>(m_->vals.size()) &&
                        Absent::present(m_->vals[static_cast<std::size_t>(idx)]))
                        break;
                    ++pos_;
                }
            }
            container* m_;
            std::size_t pos_;
        };

        using const_iterator = Iter<true>;
        using iterator = Iter<false>;

        const_iterator begin() const { return const_iterator(this, 0); }
        const_iterator end() const { return const_iterator(this, endPos()); }
        iterator begin() { return iterator(this, 0); }
        iterator end() { return iterator(this, endPos()); }
        const_iterator cbegin() const { return begin(); }
        const_iterator cend() const { return end(); }

        /** @brief Iterator at @p code, or @c end() if absent (matches @c std::map::find). */
        const_iterator find(const std::string& code) const { return makeFind<const_iterator>(this, code); }
        /** @overload */
        iterator find(const std::string& code) { return makeFind<iterator>(this, code); }

    private:
        int idxOf(const std::string& code) const {
            if (!layout) return -1;
            const int i = layout->find(code);
            return (i >= 0 && i < static_cast<int>(vals.size())) ? i : -1;
        }

        std::size_t endPos() const { return layout ? layout->sorted.size() : 0u; }

        template <typename It, typename Self>
        static It makeFind(Self self, const std::string& code) {
            const int idx = self->idxOf(code);
            if (idx < 0 || !Absent::present(self->vals[static_cast<std::size_t>(idx)]))
                return It(self, self->endPos());
            const auto& s = self->layout->sorted;
            for (std::size_t p = 0; p < s.size(); ++p)
                if (s[p] == idx) return It(self, p);
            return It(self, self->endPos());
        }
    };

} // namespace GnssProcessor
