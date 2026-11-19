#pragma once
/**
 * @file SolutionEpochBuilder.hpp
 * @brief Builds one small network solution epoch, the input of the correction-stream writer,
 *        for round-trip and response tests.
 *
 * The writer (@c NetworkProductWriter::write) consumes a plain @c NetworkSolutionEpoch plus
 * three pieces of stream state that the epoch does not carry: the datum-change flag with its
 * constellation set, the retro-marked satellites, and an emitter queried for the post-emit
 * discontinuity counters.  None of it needs a network run, so the builder fills the struct
 * directly and keeps the stream state beside it.
 *
 * The default epoch has three satellites of two constellations, each with a clock, two code
 * biases and two phase biases:
 * @code
 *   G05  clock 12.5       C1C 0.3125   C2W -1.0625   L1C 0.0421 (si 1, wl 1)  L2W -0.137 (si 1, wl 1)
 *   G12  clock -8.75      C1C 0.27     C2W 0.5       L1C 0.11   (si 1, wl 1)  L2W 0.093  (si 0, wl 0)
 *   E11  clock 3.0001     C1C -0.44    C5Q 0.78      L1C -0.052 (si 1, wl 2)  L5Q 0.064  (si 0, wl 0)
 * @endcode
 * at week 2313, time of week 259230.5, IOD-SSR 7, provider 42, solution 3.  Every optional
 * field of the format (generation id and time, level steps, hold generations, drift, URA) is
 * off, so the default epoch writes the base format.  Each setter changes one field and
 * returns the builder; a setter naming a satellite or signal that is not there adds it.
 *
 * Discontinuity counters are written only by an emitter.  @ref counterSource returns a fresh
 * emitter into which the builder's counters are adopted through the public
 * @c adoptDiscontinuityCounters, so the writer serializes exactly the values the test set;
 * a key the test did not set reads 0, as the emitter answers for an untracked key.
 */
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnss_test {

/**
 * @brief One network solution epoch for the correction-stream writer, with the stream state
 *        the writer takes beside it.
 */
class SolutionEpochBuilder {
public:
    /** @brief The default three-satellite epoch described in the file banner. */
    SolutionEpochBuilder() {
        sol_.epoch_time  = GnssProcessor::GnssTime{2313, 259230.5};
        sol_.iod_ssr     = 7;
        sol_.provider_id = 42;
        sol_.solution_id = 3;
        addDefaultSat_("G05", 12.5, "C1C", 0.3125, "C2W", -1.0625,
                       "L1C", 0.0421, true, 1, "L2W", -0.137, true, 1);
        addDefaultSat_("G12", -8.75, "C1C", 0.27, "C2W", 0.5,
                       "L1C", 0.11, true, 1, "L2W", 0.093, false, 0);
        addDefaultSat_("E11", 3.0001, "C1C", -0.44, "C5Q", 0.78,
                       "L1C", -0.052, true, 2, "L5Q", 0.064, false, 0);
    }

    // --- Epoch header -------------------------------------------------------------------

    /** @brief Sets the content epoch. @param week GPS week. @param tow Time of week (s). @return This builder. */
    SolutionEpochBuilder& time(int week, double tow) {
        sol_.epoch_time = GnssProcessor::GnssTime{week, tow};
        return *this;
    }
    /** @brief Moves the content epoch by @p dt_s within its week. @param dt_s Seconds. @return This builder. */
    SolutionEpochBuilder& advance(double dt_s) {
        sol_.epoch_time.tow += dt_s;
        return *this;
    }
    /** @brief Sets the IOD-SSR. @param v Value. @return This builder. */
    SolutionEpochBuilder& iodSsr(int v) { sol_.iod_ssr = v; return *this; }
    /** @brief Sets the provider id. @param v Value. @return This builder. */
    SolutionEpochBuilder& providerId(int v) { sol_.provider_id = v; return *this; }
    /** @brief Sets the solution id. @param v Value. @return This builder. */
    SolutionEpochBuilder& solutionId(int v) { sol_.solution_id = v; return *this; }
    /** @brief Sets the solution-generation id (0 writes no I record). @param id Value. @return This builder. */
    SolutionEpochBuilder& generationId(std::uint32_t id) { sol_.generation_id = id; return *this; }
    /**
     * @brief Sets the generation time (week 0 writes no G record).
     * @param week GPS week. @param tow Time of week (s).
     * @return This builder.
     */
    SolutionEpochBuilder& generationTime(int week, double tow) {
        sol_.generation_time = GnssProcessor::GnssTime{week, tow};
        return *this;
    }

    // --- Satellites ---------------------------------------------------------------------

    /** @brief Removes a satellite and every counter of it. @param sat RINEX id. @return This builder. */
    SolutionEpochBuilder& removeSatellite(const std::string& sat) {
        for (auto it = sol_.sats.begin(); it != sol_.sats.end(); ++it)
            if (it->sat == sat) { sol_.sats.erase(it); break; }
        clock_disc_.erase(sat);
        eraseKeysOf_(code_disc_, sat);
        eraseKeysOf_(phase_disc_, sat);
        return *this;
    }
    /** @brief Sets a satellite's clock and marks it present. @param sat RINEX id. @param c0_m Clock (m). @return This builder. */
    SolutionEpochBuilder& clock(const std::string& sat, double c0_m) {
        auto& s = sat_(sat);
        s.has_clock  = true;
        s.clock_c0_m = c0_m;
        return *this;
    }
    /** @brief Removes a satellite's clock (no C record). @param sat RINEX id. @return This builder. */
    SolutionEpochBuilder& noClock(const std::string& sat) {
        sat_(sat).has_clock = false;
        return *this;
    }
    /** @brief Sets a satellite's clock drift. @param sat RINEX id. @param c1_m_s Drift (m/s). @return This builder. */
    SolutionEpochBuilder& clockDrift(const std::string& sat, double c1_m_s) {
        sat_(sat).clock_c1_m_s = c1_m_s;
        return *this;
    }
    /** @brief Sets a satellite's URA (writes a U record). @param sat RINEX id. @param ura_m URA (m). @return This builder. */
    SolutionEpochBuilder& ura(const std::string& sat, double ura_m) {
        auto& s = sat_(sat);
        s.has_ura     = true;
        s.clock_ura_m = ura_m;
        return *this;
    }
    /**
     * @brief Sets a satellite's declared level step (the C record's lvl= token, written only
     *        with a clock and only when nonzero).
     * @param sat RINEX id. @param step_m Step (m).
     * @return This builder.
     */
    SolutionEpochBuilder& levelStep(const std::string& sat, double step_m) {
        sat_(sat).level_step_m = step_m;
        return *this;
    }

    // --- Code biases --------------------------------------------------------------------

    /** @brief Sets a code bias. @param sat RINEX id. @param sig Code signal ("C1C"). @param m Bias (m). @return This builder. */
    SolutionEpochBuilder& codeBias(const std::string& sat, const std::string& sig, double m) {
        sat_(sat).code_bias_m[sig] = m;
        return *this;
    }
    /** @brief Removes a code bias and its counter. @param sat RINEX id. @param sig Code signal. @return This builder. */
    SolutionEpochBuilder& removeCodeBias(const std::string& sat, const std::string& sig) {
        auto& s = sat_(sat);
        s.code_bias_m.erase(sig);
        s.cb_level_step_m.erase(sig);
        code_disc_.erase(sat + "|" + sig);
        return *this;
    }
    /**
     * @brief Sets a code bias's declared follow step (the B record's lvl= token, written only
     *        when nonzero).
     * @param sat RINEX id. @param sig Code signal. @param step_m Step (m).
     * @return This builder.
     */
    SolutionEpochBuilder& codeLevelStep(const std::string& sat, const std::string& sig, double step_m) {
        sat_(sat).cb_level_step_m[sig] = step_m;
        return *this;
    }

    // --- Phase biases -------------------------------------------------------------------

    /** @brief Sets a phase bias value. @param sat RINEX id. @param sig Phase signal ("L1C"). @param m Bias (m). @return This builder. */
    SolutionEpochBuilder& phaseBias(const std::string& sat, const std::string& sig, double m) {
        sat_(sat).phase_bias[sig].bias_m = m;
        return *this;
    }
    /** @brief Removes a phase bias and its counter. @param sat RINEX id. @param sig Phase signal. @return This builder. */
    SolutionEpochBuilder& removePhaseBias(const std::string& sat, const std::string& sig) {
        sat_(sat).phase_bias.erase(sig);
        phase_disc_.erase(sat + "|" + sig);
        return *this;
    }
    /** @brief Sets the signal integer flag. @param sat RINEX id. @param sig Phase signal. @param on Flag. @return This builder. */
    SolutionEpochBuilder& signalInteger(const std::string& sat, const std::string& sig, bool on) {
        sat_(sat).phase_bias[sig].signal_integer = on;
        return *this;
    }
    /** @brief Sets the wide-lane integer indicator. @param sat RINEX id. @param sig Phase signal. @param v Indicator. @return This builder. */
    SolutionEpochBuilder& wideLaneInteger(const std::string& sat, const std::string& sig, int v) {
        sat_(sat).phase_bias[sig].wide_lane_integer = v;
        return *this;
    }
    /**
     * @brief Sets the hold generation (the P record's gen= token, written only when nonzero).
     * @param sat RINEX id. @param sig Phase signal. @param gen Generation.
     * @return This builder.
     */
    SolutionEpochBuilder& holdGeneration(const std::string& sat, const std::string& sig, std::uint32_t gen) {
        sat_(sat).phase_bias[sig].hold_generation = gen;
        return *this;
    }

    // --- Discontinuity counters (written only through counterSource) ---------------------

    /** @brief Sets a clock counter. @param sat RINEX id. @param n Counter. @return This builder. */
    SolutionEpochBuilder& clockDisc(const std::string& sat, int n) { clock_disc_[sat] = n; return *this; }
    /** @brief Sets a code-bias counter. @param sat RINEX id. @param sig Code signal. @param n Counter. @return This builder. */
    SolutionEpochBuilder& codeDisc(const std::string& sat, const std::string& sig, int n) {
        code_disc_[sat + "|" + sig] = n;
        return *this;
    }
    /** @brief Sets a phase-bias counter. @param sat RINEX id. @param sig Phase signal. @param n Counter. @return This builder. */
    SolutionEpochBuilder& phaseDisc(const std::string& sat, const std::string& sig, int n) {
        phase_disc_[sat + "|" + sig] = n;
        return *this;
    }

    // --- Stream state beside the epoch ----------------------------------------------------

    /**
     * @brief Sets the datum-change flag and its constellations (E-line fields 7 and 8).
     * @param on Flag. @param consts Constellation letters; empty means every constellation.
     * @return This builder.
     */
    SolutionEpochBuilder& datumChanged(bool on, std::set<char> consts = {}) {
        datum_changed_ = on;
        datum_consts_  = std::move(consts);
        return *this;
    }
    /** @brief Sets the retro-marked satellites (the R record). @param sats RINEX ids. @return This builder. */
    SolutionEpochBuilder& retroSats(std::set<std::string> sats) { retro_sats_ = std::move(sats); return *this; }

    // --- Results --------------------------------------------------------------------------

    /** @brief The epoch as built. @return A reference valid while the builder lives. */
    const GnssProcessor::NetworkSolutionEpoch& epoch() const { return sol_; }
    /** @brief The datum-change flag. @return The flag. */
    bool datumChangedFlag() const { return datum_changed_; }
    /** @brief The datum-change constellations. @return The set. */
    const std::set<char>& datumConsts() const { return datum_consts_; }
    /** @brief The retro-marked satellites. @return The set. */
    const std::set<std::string>& retroSatSet() const { return retro_sats_; }

    /** @brief The clock counter the writer will serialize. @param sat RINEX id. @return The counter, 0 when unset. */
    int clockDiscOf(const std::string& sat) const { return valueOr0_(clock_disc_, sat); }
    /** @brief The code counter the writer will serialize. @param sat RINEX id. @param sig Code signal. @return The counter, 0 when unset. */
    int codeDiscOf(const std::string& sat, const std::string& sig) const {
        return valueOr0_(code_disc_, sat + "|" + sig);
    }
    /** @brief The phase counter the writer will serialize. @param sat RINEX id. @param sig Phase signal. @return The counter, 0 when unset. */
    int phaseDiscOf(const std::string& sat, const std::string& sig) const {
        return valueOr0_(phase_disc_, sat + "|" + sig);
    }

    /**
     * @brief A fresh emitter holding the builder's counters and nothing else, for the writer's
     *        counter source.
     * @return An emitter with the clock channel on, so its construction matches a producer
     *         that serializes clock counters; its only state is the adopted counters.
     */
    GnssProcessor::NetworkProductEmitter counterSource() const {
        GnssProcessor::NetworkProductEmitter e(GnssProcessor::NetworkProductEmitter::kDefaultDiscThresholdM,
                                               /*emit_clock_disc=*/true);
        e.adoptDiscontinuityCounters(phase_disc_, code_disc_, clock_disc_);
        return e;
    }

    /**
     * @brief Writes the epoch with its stream state through the real writer.
     * @param w The writer.
     * @param with_counters True to serialize the builder's counters (the counter-source path).
     */
    void writeTo(GnssProcessor::NetworkProductWriter& w, bool with_counters) const {
        if (with_counters) {
            const GnssProcessor::NetworkProductEmitter src = counterSource();
            w.write(sol_, datum_changed_, datum_consts_, retro_sats_, &src);
        } else {
            w.write(sol_, datum_changed_, datum_consts_, retro_sats_, nullptr);
        }
    }

    /**
     * @brief Expects a parsed record to carry this builder's epoch, stream state and, when
     *        @p with_counters, its counters on every C, B and P record.
     * @param rec The parsed record.
     * @param with_counters Whether the record was written with the counter source.
     */
    void expectArrivedIn(const GnssProcessor::NetworkProductRecord& rec, bool with_counters) const;

private:
    void addDefaultSat_(const std::string& sat, double c0,
                        const std::string& cb1, double b1, const std::string& cb2, double b2,
                        const std::string& pb1, double p1, bool si1, int wl1,
                        const std::string& pb2, double p2, bool si2, int wl2) {
        GnssProcessor::NetworkSatSolution s;
        s.sat         = sat;
        s.has_clock   = true;
        s.clock_c0_m  = c0;
        s.code_bias_m[cb1] = b1;
        s.code_bias_m[cb2] = b2;
        GnssProcessor::NetworkPhaseBias a;
        a.bias_m = p1; a.signal_integer = si1; a.wide_lane_integer = wl1;
        GnssProcessor::NetworkPhaseBias b;
        b.bias_m = p2; b.signal_integer = si2; b.wide_lane_integer = wl2;
        s.phase_bias[pb1] = a;
        s.phase_bias[pb2] = b;
        sol_.sats.push_back(s);
    }
    GnssProcessor::NetworkSatSolution& sat_(const std::string& sat) {
        for (auto& s : sol_.sats)
            if (s.sat == sat) return s;
        GnssProcessor::NetworkSatSolution s;
        s.sat = sat;
        sol_.sats.push_back(s);
        return sol_.sats.back();
    }
    static int valueOr0_(const std::map<std::string, int>& m, const std::string& key) {
        const auto it = m.find(key);
        return it == m.end() ? 0 : it->second;
    }
    static void eraseKeysOf_(std::map<std::string, int>& m, const std::string& sat) {
        const std::string prefix = sat + "|";
        for (auto it = m.begin(); it != m.end();)
            it = (it->first.rfind(prefix, 0) == 0) ? m.erase(it) : std::next(it);
    }

    GnssProcessor::NetworkSolutionEpoch sol_;
    std::map<std::string, int>          clock_disc_;   ///< "sat" -> clock counter.
    std::map<std::string, int>          code_disc_;    ///< "sat|signal" -> code counter.
    std::map<std::string, int>          phase_disc_;   ///< "sat|signal" -> phase counter.
    bool                                datum_changed_{false};
    std::set<char>                      datum_consts_;
    std::set<std::string>               retro_sats_;
};

/**
 * @brief Expects two solution epochs to agree in every field the writer serializes.
 *
 * Doubles are compared exactly: the writer formats each with the shortest decimal that
 * parses back to the same value.  Three facts of the format are applied rather than
 * flagged: a satellite with no clock, no code bias and no phase bias writes no record and is
 * absent from @p got; the level step travels on the C record, so a satellite without a clock
 * reads back with none; the chain component is read but never written, so it is not compared.
 *
 * @param want The epoch handed to the writer.
 * @param got  The epoch the reader returned.
 */
inline void expectSameSolutionEpoch(const GnssProcessor::NetworkSolutionEpoch& want,
                                    const GnssProcessor::NetworkSolutionEpoch& got) {
    EXPECT_EQ(got.epoch_time.week, want.epoch_time.week);
    EXPECT_EQ(got.epoch_time.tow, want.epoch_time.tow);
    EXPECT_EQ(got.iod_ssr, want.iod_ssr);
    EXPECT_EQ(got.provider_id, want.provider_id);
    EXPECT_EQ(got.solution_id, want.solution_id);
    EXPECT_EQ(got.generation_id, want.generation_id);
    if (want.generation_time.week != 0) {
        EXPECT_EQ(got.generation_time.week, want.generation_time.week);
        EXPECT_EQ(got.generation_time.tow, want.generation_time.tow);
    } else {
        EXPECT_EQ(got.generation_time.week, 0);
    }

    std::vector<const GnssProcessor::NetworkSatSolution*> written;
    for (const auto& s : want.sats)
        if (s.has_clock || !s.code_bias_m.empty() || !s.phase_bias.empty()) written.push_back(&s);
    ASSERT_EQ(got.sats.size(), written.size());

    for (std::size_t i = 0; i < written.size(); ++i) {
        const auto& w = *written[i];
        const auto& g = got.sats[i];
        SCOPED_TRACE("satellite " + w.sat);
        EXPECT_EQ(g.sat, w.sat);
        EXPECT_EQ(g.has_clock, w.has_clock);
        if (w.has_clock) {
            EXPECT_EQ(g.clock_c0_m, w.clock_c0_m);
            EXPECT_EQ(g.clock_c1_m_s, w.clock_c1_m_s);
            EXPECT_EQ(g.level_step_m, w.level_step_m);
            EXPECT_EQ(g.clock_iode, w.clock_iode);
        } else {
            EXPECT_EQ(g.level_step_m, 0.0);
        }
        EXPECT_EQ(g.has_ura, w.has_ura);
        if (w.has_ura) EXPECT_EQ(g.clock_ura_m, w.clock_ura_m);
        EXPECT_EQ(g.code_bias_m, w.code_bias_m);
        EXPECT_EQ(g.isc_observed, w.isc_observed);
        std::map<std::string, double> want_cb_lvl;
        for (const auto& [sig, v] : w.cb_level_step_m)
            if (v != 0.0 && w.code_bias_m.count(sig)) want_cb_lvl[sig] = v;
        EXPECT_EQ(g.cb_level_step_m, want_cb_lvl);
        ASSERT_EQ(g.phase_bias.size(), w.phase_bias.size());
        for (const auto& [sig, wp] : w.phase_bias) {
            SCOPED_TRACE("phase signal " + sig);
            const auto it = g.phase_bias.find(sig);
            ASSERT_NE(it, g.phase_bias.end());
            const auto& gp = it->second;
            EXPECT_EQ(gp.bias_m, wp.bias_m);
            EXPECT_EQ(gp.signal_integer, wp.signal_integer);
            EXPECT_EQ(gp.wide_lane_integer, wp.wide_lane_integer);
            EXPECT_EQ(gp.hold_generation, wp.hold_generation);
            EXPECT_EQ(gp.sigma_m, wp.sigma_m);
            EXPECT_EQ(gp.sigc_m, wp.sigc_m);
        }
    }
}

inline void SolutionEpochBuilder::expectArrivedIn(const GnssProcessor::NetworkProductRecord& rec,
                                                  bool with_counters) const {
    expectSameSolutionEpoch(sol_, rec.epoch);
    EXPECT_EQ(rec.datum_changed, datum_changed_);
    EXPECT_EQ(rec.datum_consts, datum_consts_);
    EXPECT_EQ(rec.retro_sats, retro_sats_);

    const auto& sd = rec.serialized_disc;
    if (!with_counters) {
        EXPECT_FALSE(sd.any());
        return;
    }
    // Every record written with the counter source carries one counter: the set's own
    // value, or 0 for a key the test did not set.
    std::size_t n_clock = 0, n_code = 0, n_phase = 0;
    for (const auto& s : sol_.sats) {
        if (s.has_clock) {
            ++n_clock;
            const auto it = sd.clock.find(s.sat);
            ASSERT_NE(it, sd.clock.end()) << s.sat;
            EXPECT_EQ(it->second, clockDiscOf(s.sat)) << s.sat;
        }
        for (const auto& [sig, v] : s.code_bias_m) {
            (void)v;
            ++n_code;
            const auto it = sd.code.find(s.sat + "|" + sig);
            ASSERT_NE(it, sd.code.end()) << s.sat << " " << sig;
            EXPECT_EQ(it->second, codeDiscOf(s.sat, sig)) << s.sat << " " << sig;
        }
        for (const auto& [sig, v] : s.phase_bias) {
            (void)v;
            ++n_phase;
            const auto it = sd.phase.find(s.sat + "|" + sig);
            ASSERT_NE(it, sd.phase.end()) << s.sat << " " << sig;
            EXPECT_EQ(it->second, phaseDiscOf(s.sat, sig)) << s.sat << " " << sig;
        }
    }
    EXPECT_EQ(sd.clock.size(), n_clock);
    EXPECT_EQ(sd.code.size(), n_code);
    EXPECT_EQ(sd.phase.size(), n_phase);
}

}  // namespace gnss_test
