/**
 * @file test_eq34_consumer_integer.cpp
 * @brief Card 3 / Eq-34 acceptance harness: a synthetic KNOWN-INTEGER product
 *        driven through the REAL consumer-side chain proves end-to-end that an
 *        integer-consistent satellite phase-bias frame lets a held-out PPP-RTK
 *        user recover integer double-difference ambiguities and that MLAMBDA
 *        actually executes (docs/HANDOFF_eq34_datum_card3.md, "Falsifier" #1 —
 *        that doc was pruned 2026-07-28 in commit a8f044f and is recoverable at
 *        39b263a; it was NOT attic'd, so the path does not resolve in-tree).
 *
 * THE CLAIM UNDER TEST (S-system theory reference §6 / Odijk Eq. 34): a user
 * receiving emitted satellite phase biases  δ̃_s = b_s + ref_s  can fix integer
 * DDs IFF the datum references have integer between-satellite differences,
 * ref_s − ref_p ∈ λℤ, for every pair.  A COMMON fractional offset δ_σ riding
 * every bias of one signal is legal — it cancels in the user's between-
 * satellite difference exactly like the user's own receiver bias δ_{u,σ}.
 *
 * CHAIN EXERCISED (every stage is production code, not a reimplementation):
 *   1. @c NetworkProductWriter — v2 product file with the S-basis contract
 *      declaring @c AmbDatum::SingleReceiverChain (Card 3, D5) + E/C/B/P
 *      records carrying the synthetic integer frame.
 *   2. @c NetworkProductReader::read — the real parse, contract included.
 *   3. @c NetworkProductReader::makeReplayEmitter + @c replay — the real
 *      consumer store-routing path (discontinuity counters included), into an
 *      OWNED @c SsrCorrectionStore (never the singleton; see
 *      tests/test_ssr_store_injection.cpp for the DI-seam contract).
 *   4. @c SsrCorrectionStore::getPhaseBias — the EXACT lookup
 *      @c MeasurementModel uses for the SSR phase-bias term, applied with the
 *      SAME sign convention (kSsrPhaseBiasSign = +1: the bias is ADDED to the
 *      predicted observable, so the float ambiguity absorbs
 *      observed − predicted; src/core/measurement/MeasurementModel.cpp,
 *      "SSR satellite biases (PPP-RTK)" block).
 *   5. @c StateManager + @c AmbiguityResolver::resolve — the real DD
 *      formation (per-(constellation,signal) highest-elevation pivot,
 *      @c buildDdPairs) and the real MLAMBDA search + ratio / bootstrap /
 *      per-pair-fraction acceptance battery.
 *
 * NOT exercised: @c MeasurementModel's full row assembly (it needs the
 * ProductCache / RinexParser construction chain — the same "too heavy for the
 * unit tier" boundary documented in tests/test_filter_core.cpp).  The float
 * ambiguities are formed here from the store lookup + the documented sign
 * convention instead; the sign convention itself is pinned by the comment
 * cited above and by the closed-loop field harness.
 *
 * ACCEPTANCE SEMANTICS (the C7-misread guard): "MLAMBDA executed" means
 * @c gate_reason == 0 AND @c ratio > 0.  A ratio of exactly 0.00 is the
 * early-return sentinel of the pre-MLAMBDA gates (ar_mode / min_epochs /
 * position-σ / no-candidates — @c AmbiguityResolution::gate_reason) and MUST
 * NEVER be read as "the search ran and rejected".  A dedicated test pins that
 * sentinel co-occurrence so the harness (and any grader built on it) cannot
 * regress into the misread.
 *
 * FRAME REQUIREMENTS COVERED (harness spec):
 *   - two constellations (GPS ×5 sats, Galileo ×4 sats), two signals each
 *     (GPS L1C/L2W, Galileo L1C/L5Q) → four independent DD groups, 14 DDs;
 *   - a common fractional offset δ_σ on ALL biases of each signal (the L1C
 *     offset is shared across BOTH constellations) — the per-satellite float
 *     ambiguities are provably NON-integer while their DDs are integer, so
 *     the test demonstrates the cancellation is real work, not a triviality;
 *   - one deliberately NON-integer bias pair (G05 L1C, +0.37 cycles) as the
 *     negative control: its DD fraction lands far from integer and the
 *     resolver's acceptance battery refuses the fix.
 *
 * This harness doubles as the acceptance gate for the later producer self-AR
 * build (memory project_network_ar_producer_scope): integers to re-pin only
 * exist in an integer-consistent frame, which is exactly what it certifies.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

const GnssTime kT0{2313, 259200.0};

/// PPP core size used by Application::run (position ×3, clock, tropo) — the
/// same constant tests/test_filter_core.cpp pins.
constexpr size_t kPppCoreSize = 5;

/// Deterministic consumer float covariance: position variance per axis
/// (σ_3D = √(3·4e-4) ≈ 0.035 m, safely under the 0.05 m AR σ-gate) and
/// per-ambiguity variance ((4 mm)² ≈ (0.02 cyc)² on L1 — a converged float).
constexpr double kPosVar = 4.0e-4;
constexpr double kAmbVar = 1.6e-5;

double fracDist(double v) { return std::abs(v - std::round(v)); }

std::string tempFile(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// ===========================================================================
// The synthetic known-integer frame (all values deterministic — no RNG).
//
// Emitted satellite phase bias (metres), per (sat s, signal σ):
//     δ̃_{s,σ} = b_{s,σ} + λ_σ·k_{s,σ} + δ_σ  (+ poison on the control pair)
// where b is an arbitrary physical bias, k ∈ ℤ the frame integers and δ_σ the
// COMMON fractional datum offset of signal σ.  So ref_s − ref_p =
// λ(k_s − k_p) ∈ λℤ for every same-signal pair — the Eq-34 property.
//
// Synthetic user observation (geometry/atmosphere removed, metres):
//     L_{u,s,σ} = b_{s,σ} + δ_{u,σ} + λ_σ·(z_{u,s,σ} + ε_{s,σ})
// with z ∈ ℤ the user integers, δ_u the user receiver phase bias and ε a small
// deterministic float-noise term (|ε| ≤ 0.034 cyc).  After the consumer
// applies the emitted bias (MeasurementModel convention: predicted += δ̃), the
// float ambiguity absorbs
//     a_{s,σ} = L − δ̃ = (δ_{u,σ} − δ_σ) + λ_σ·(z − k + ε)
// — per-satellite NON-integer (δ_u − δ_σ is engineered ≥ 0.27 cycles on every
// signal), while the within-group DD is λ·[(z_i−k_i) − (z_ref−k_ref)] + λ·Δε:
// integer to within the noise.  The KNOWN DD integer is therefore
//     N = (z_i − k_i) − (z_ref − k_ref).
// ===========================================================================

/// One (constellation, signal) slice of the synthetic frame.  Vectors are
/// indexed like the owning constellation's satellite list.
struct SignalFrame {
    std::string signal;           ///< RINEX phase code ("L1C") — store/product key.
    std::string code_signal;      ///< Companion code observable for the B record.
    double delta_common_m;        ///< Common fractional datum offset δ_σ (m), on EVERY bias of this signal.
    double delta_user_m;          ///< User receiver phase bias δ_{u,σ} (m).
    std::vector<double> b_phys_m; ///< Physical satellite phase bias b_{s,σ} (m).
    std::vector<int>    k_frame;  ///< Emitted frame integers k_{s,σ} (cycles).
    std::vector<int>    z_user;   ///< User integer ambiguities z_{u,s,σ} (cycles).
    std::vector<double> eps_cyc;  ///< Deterministic float noise ε (cycles), |ε| ≤ 0.034.
};

struct ConstellationFrame {
    System                   system;
    std::vector<std::string> sats;
    std::vector<double>      elev_deg;  ///< Distinct values ⇒ deterministic DD pivot (max).
    std::vector<SignalFrame> signals;
};

struct SynthFrame {
    std::vector<ConstellationFrame> cons;
    // Negative control: this (sat, signal) bias gains an extra +poison_cycles·λ
    // in the POISONED product — a non-integer between-satellite reference pair.
    std::string poison_sat{"G05"};
    std::string poison_signal{"L1C"};
    double      poison_cycles{0.37};
};

SynthFrame makeFrame() {
    SynthFrame f;

    ConstellationFrame gps;
    gps.system   = System::GPS;
    gps.sats     = {"G01", "G02", "G03", "G04", "G05"};
    gps.elev_deg = {80.0, 70.0, 60.0, 50.0, 35.0};
    // The L1C common fractional δ_σ (+0.0832 m ≈ +0.44 cyc) is deliberately the
    // SAME value in the Galileo block below — "one signal" per the harness spec.
    gps.signals.push_back(SignalFrame{
        "L1C", "C1C", /*δ_σ=*/+0.0832, /*δ_u=*/-0.0311,
        {0.113, -0.071, 0.049, -0.152, 0.088},
        {3, -2, 5, 1, -4},
        {7, -5, 2, 9, -1},
        {+0.021, -0.034, +0.012, -0.027, +0.018}});
    gps.signals.push_back(SignalFrame{
        "L2W", "C2W", /*δ_σ=*/-0.0417, /*δ_u=*/+0.0486,
        {-0.037, 0.094, -0.126, 0.061, -0.018},
        {-1, 4, 2, -3, 0},
        {-4, 6, 1, 3, -8},
        {-0.015, +0.029, -0.008, +0.024, -0.031}});
    f.cons.push_back(std::move(gps));

    ConstellationFrame gal;
    gal.system   = System::Galileo;
    gal.sats     = {"E01", "E02", "E03", "E04"};
    gal.elev_deg = {75.0, 62.0, 48.0, 33.0};
    gal.signals.push_back(SignalFrame{
        "L1C", "C1C", /*δ_σ=*/+0.0832, /*δ_u=*/-0.0311,
        {0.067, -0.104, 0.032, -0.059},
        {2, -6, 0, 4},
        {-7, 3, 8, -2},
        {+0.026, -0.019, +0.033, -0.011}});
    gal.signals.push_back(SignalFrame{
        "L5Q", "C5Q", /*δ_σ=*/+0.0890, /*δ_u=*/+0.0119,
        {-0.081, 0.116, -0.044, 0.023},
        {-3, 1, 5, -2},
        {5, -9, 4, 6},
        {-0.023, +0.017, -0.029, +0.014}});
    f.cons.push_back(std::move(gal));

    return f;
}

/// The emitted (product-file) bias for one (constellation, signal, sat index),
/// WITHOUT the negative-control poison.  Shared by the writer and the store
/// spot-checks so byte-exact comparisons compare the identical expression.
double emittedBiasM(const PppUtils& utils, const ConstellationFrame& con,
                    const SignalFrame& sf, std::size_t i) {
    const double lam = utils.getWavelength(con.system, sf.signal, con.sats[i]);
    return sf.b_phys_m[i] + lam * static_cast<double>(sf.k_frame[i]) + sf.delta_common_m;
}

/// The KNOWN user DD integer for satellite index @p i against pivot index
/// @p ref within one (constellation, signal) group: N = (z_i−k_i) − (z_ref−k_ref).
int knownDdInteger(const SignalFrame& sf, std::size_t i, std::size_t ref) {
    return (sf.z_user[i] - sf.k_frame[i]) - (sf.z_user[ref] - sf.k_frame[ref]);
}

/// The Card 3 S-basis contract a chain-datum producer stamps (D5): amb_datum =
/// SingleReceiverChain riding the existing int field (no format bump) plus the
/// diagnostic pdr= datum-receiver token.
SbasisContract makeEq34Contract() {
    SbasisContract c;
    c.present                       = true;
    c.decl.reference_receiver_id    = "R1DATUM";
    c.decl.bias_sign                = 1.0;
    c.decl.windup_yaw_model_id      = "igs2017";
    c.decl.amb_datum                = SbasisDeclaration::AmbDatum::SingleReceiverChain;
    c.decl.emit_code_bias           = true;
    c.decl.eq34_datum_receiver      = "R1DATUM";
    c.apriori_product_id            = "SYNTH_EQ34_KNOWN_INTEGER";
    return c;
}

/// Write the synthetic product: v2 header (Card 3 contract) + two identical
/// 30 s epochs (constant biases ⇒ zero discontinuity bumps on replay; two
/// epochs so the store's overwrite path is exercised, not just first-sight).
/// @p poisoned adds +poison_cycles·λ to the control pair's bias — the ONLY
/// difference between the positive and negative-control products.
void writeSynthProduct(const std::string& path, const SynthFrame& frame,
                       const PppUtils& utils, bool poisoned) {
    NetworkProductWriter w(path, makeEq34Contract());
    ASSERT_TRUE(w.good()) << "cannot open " << path;

    GnssTime t = kT0;
    for (int ep = 0; ep < 2; ++ep) {
        NetworkSolutionEpoch sol;
        sol.epoch_time  = t;
        sol.iod_ssr     = 5;
        sol.provider_id = 42;
        sol.solution_id = 7;
        for (const auto& con : frame.cons) {
            for (std::size_t i = 0; i < con.sats.size(); ++i) {
                NetworkSatSolution s;
                s.sat        = con.sats[i];
                s.has_clock  = true;
                s.clock_c0_m = 0.75 * static_cast<double>(i + 1) - 2.0;  // arbitrary, epoch-constant
                for (const auto& sf : con.signals) {
                    // Consistent (arbitrary) satellite code bias per band; the
                    // 0.21 base keeps every value away from exactly 0.0 (the
                    // store's getCodeBias cannot distinguish genuine-zero from
                    // absent — see SsrCorrectionStore::hasCodeBias).
                    s.code_bias_m[sf.code_signal] =
                        0.21 + 0.05 * static_cast<double>(i) - 0.30 * (sf.code_signal[1] - '1');
                    double bias = emittedBiasM(utils, con, sf, i);
                    if (poisoned && s.sat == frame.poison_sat && sf.signal == frame.poison_signal)
                        bias += frame.poison_cycles
                              * utils.getWavelength(con.system, sf.signal, s.sat);
                    s.phase_bias[sf.signal] =
                        NetworkPhaseBias{bias, /*signal_integer=*/true, /*wl=*/0};
                }
                sol.sats.push_back(std::move(s));
            }
        }
        w.write(sol);
        t.tow += 30.0;
    }
}

/// Read the product back and replay every epoch into @p store through the SAME
/// replay path the batch/tail consumers use, with the contract-configured
/// emitter (@c makeReplayEmitter) — stages 2+3 of the chain.
void replayIntoStore(const std::string& path, SsrCorrectionStore& store,
                     SbasisContract& contract_out) {
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &contract_out, &ok);
    ASSERT_TRUE(ok) << "product parse flagged a malformed line";
    ASSERT_EQ(recs.size(), 2u);
    NetworkProductEmitter emitter = NetworkProductReader::makeReplayEmitter(contract_out);
    for (const auto& r : recs) NetworkProductReader::replay(r, store, emitter);
}

/// The consumer model: form the float ambiguity of every (sat, signal) from
/// the synthetic observation minus the STORE-APPLIED bias (stage 4 — the
/// MeasurementModel kSsrPhaseBiasSign=+1 convention), seed them into
/// @p sm as "SAT_SIG" ambiguity states with a deterministic covariance, and
/// build the per-satellite observation list (elevation → DD pivot election).
void buildConsumerState(const SynthFrame& frame, const SsrCorrectionStore& store,
                        const PppUtils& utils, StateManager& sm,
                        std::vector<PppObservation>& obs) {
    for (const auto& con : frame.cons) {
        for (std::size_t i = 0; i < con.sats.size(); ++i) {
            PppObservation o{};
            o.satellite = con.sats[i];
            o.system    = con.system;
            o.time      = kT0;
            o.elevation = con.elev_deg[i];
            o.azimuth   = 0.0;
            obs.push_back(o);
            for (const auto& sf : con.signals) {
                const double lam = utils.getWavelength(con.system, sf.signal, con.sats[i]);
                ASSERT_GT(lam, 0.0) << con.sats[i] << " " << sf.signal;
                const auto pb = store.getPhaseBias(con.sats[i], sf.signal);
                ASSERT_TRUE(pb.has_value())
                    << "store has no phase bias for " << con.sats[i] << " " << sf.signal;
                // Synthetic user phase observable (geometry/atmosphere removed).
                const double observed = sf.b_phys_m[i] + sf.delta_user_m
                                      + lam * (static_cast<double>(sf.z_user[i]) + sf.eps_cyc[i]);
                // MeasurementModel applies the SSR phase bias with sign +1 on the
                // PREDICTED observable, so the float ambiguity absorbs the rest.
                const double float_amb = observed - pb->bias_m;
                ASSERT_NE(float_amb, 0.0);  // buildDdPairs skips exact-zero (uninit) floats
                sm.addAmbiguity(con.sats[i] + "_" + sf.signal, float_amb);
            }
        }
    }
    // Deterministic covariance: converged position (σ-gate passes), unit noise
    // on the remaining core/time-offset states, tight independent ambiguities.
    const std::size_t n = sm.getStateSize();
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(n),
                                                  static_cast<Eigen::Index>(n));
    for (int d = 0; d < 3; ++d) P(d, d) = kPosVar;
    for (const auto& kv : sm.getAmbiguities())
        P(static_cast<Eigen::Index>(kv.second), static_cast<Eigen::Index>(kv.second)) = kAmbVar;
    sm.setCovariance(P);
}

/// One consumer DD against the group pivot, with everything the assertions need.
struct DdCheck {
    std::string sat_i, sat_ref, signal;
    int         idx_i{-1}, idx_ref{-1};  ///< State indices of the two SD ambiguities.
    double      lambda_m{0.0};
    double      dd_cycles{0.0};          ///< Float DD in cycles.
    int         known_n{0};              ///< The KNOWN integer N = (z_i−k_i) − (z_ref−k_ref).
};

/// Form every within-(constellation,signal) DD against the HIGHEST-ELEVATION
/// pivot — the same election rule as AmbiguityResolver::buildDdPairs — from
/// the consumer float state.
void collectConsumerDds(const SynthFrame& frame, const StateManager& sm,
                        const PppUtils& utils, std::vector<DdCheck>& out) {
    const Eigen::VectorXd x = sm.getStateVector();
    for (const auto& con : frame.cons) {
        std::size_t ref = 0;
        for (std::size_t i = 1; i < con.sats.size(); ++i)
            if (con.elev_deg[i] > con.elev_deg[ref]) ref = i;
        for (const auto& sf : con.signals) {
            const int idx_ref = sm.getAmbiguityIndex(con.sats[ref] + "_" + sf.signal);
            ASSERT_GE(idx_ref, 0);
            for (std::size_t i = 0; i < con.sats.size(); ++i) {
                if (i == ref) continue;
                DdCheck d;
                d.sat_i    = con.sats[i];
                d.sat_ref  = con.sats[ref];
                d.signal   = sf.signal;
                d.idx_i    = sm.getAmbiguityIndex(d.sat_i + "_" + sf.signal);
                d.idx_ref  = idx_ref;
                ASSERT_GE(d.idx_i, 0);
                d.lambda_m = utils.getWavelength(con.system, sf.signal, d.sat_i);
                d.dd_cycles = (x(d.idx_i) - x(d.idx_ref)) / d.lambda_m;
                d.known_n   = knownDdInteger(sf, i, ref);
                out.push_back(std::move(d));
            }
        }
    }
}

/// AR configuration for the harness.  Every acceptance gate keeps its
/// production default (ratio 3.0, upper bound 1e4, bootstrap 0.999, per-pair
/// frac cap 0.15) — the fix must clear the REAL battery.  Only the two
/// convergence guards are seeded for a single-shot call (min_epochs=0; the
/// σ-gate is satisfied by the deterministic covariance, not widened), and the
/// hold variance is tightened so the post-fix state pulls hard onto the
/// integers for the fixed-state assertion.
AmbiguityResolverConfig makeArConfig() {
    AmbiguityResolverConfig cfg;
    cfg.ar_mode              = 1;      // continuous
    cfg.min_epochs           = 0;      // single-shot harness call
    cfg.elevation_mask_ar    = 10.0;   // every synthetic sat participates
    cfg.max_position_sigma_m = 0.05;   // production default — kPosVar passes it honestly
    cfg.hold_variance        = 1e-10;  // pull the fixed state onto the integers
    return cfg;
}

// ===========================================================================
// 1. Product round-trip: the Card 3 contract and the integer frame survive the
//    writer → reader → replay → store pipeline byte-exactly.
// ===========================================================================
TEST(Eq34ConsumerIntegerTest, SynthProductRoundTripsContractAndFrame) {
    LogManager log;
    PppUtils   utils(log);
    const SynthFrame frame = makeFrame();
    const std::string path = tempFile("gnss_eq34_synth_roundtrip.txt");
    std::filesystem::remove(path);

    ASSERT_NO_FATAL_FAILURE(writeSynthProduct(path, frame, utils, /*poisoned=*/false));

    SsrCorrectionStore store;
    SbasisContract     contract;
    ASSERT_NO_FATAL_FAILURE(replayIntoStore(path, store, contract));

    // Card 3 contract round-trip: the SingleReceiverChain enumerator rides the
    // existing S-line int field (no format bump) and the pdr= diagnostic token
    // carries the datum receiver.
    ASSERT_TRUE(contract.present);
    EXPECT_EQ(contract.decl.amb_datum, SbasisDeclaration::AmbDatum::SingleReceiverChain);
    EXPECT_EQ(contract.decl.eq34_datum_receiver, "R1DATUM");
    EXPECT_EQ(contract.apriori_product_id, "SYNTH_EQ34_KNOWN_INTEGER");

    // Every emitted phase bias round-trips byte-exactly (shortest-round-trip
    // serialization), is flagged signal_integer, and — the biases being
    // epoch-constant — carries a ZERO discontinuity counter after replay.
    for (const auto& con : frame.cons) {
        for (std::size_t i = 0; i < con.sats.size(); ++i) {
            for (const auto& sf : con.signals) {
                const auto pb = store.getPhaseBias(con.sats[i], sf.signal);
                ASSERT_TRUE(pb.has_value()) << con.sats[i] << " " << sf.signal;
                EXPECT_DOUBLE_EQ(pb->bias_m, emittedBiasM(utils, con, sf, i))
                    << con.sats[i] << " " << sf.signal;
                EXPECT_TRUE(pb->signal_integer) << con.sats[i] << " " << sf.signal;
                EXPECT_EQ(pb->discontinuity_counter, 0) << con.sats[i] << " " << sf.signal;
            }
            // Code bias + clock also flow (consistency records; the phase-only
            // consumer below never reads them — mirrors consume_network_clock=false,
            // the "clean integer-recovery test" of the closed-loop harness).
            EXPECT_NE(store.getCodeBias(con.sats[i], con.signals[0].code_signal), 0.0);
        }
    }

    // The emitted frame satisfies the Eq-34 property BY CONSTRUCTION — verify
    // it anyway so a broken table edit cannot silently invalidate the harness:
    // within a signal, (δ̃_s − b_s) − (δ̃_p − b_p) ∈ λℤ.
    for (const auto& con : frame.cons) {
        for (const auto& sf : con.signals) {
            const double lam = utils.getWavelength(con.system, sf.signal, con.sats[0]);
            for (std::size_t s = 0; s < con.sats.size(); ++s) {
                for (std::size_t p = 0; p < con.sats.size(); ++p) {
                    const double ref_s = emittedBiasM(utils, con, sf, s) - sf.b_phys_m[s];
                    const double ref_p = emittedBiasM(utils, con, sf, p) - sf.b_phys_m[p];
                    EXPECT_LT(fracDist((ref_s - ref_p) / lam), 1e-9)
                        << con.sats[s] << "-" << con.sats[p] << " " << sf.signal;
                }
            }
        }
    }

    std::filesystem::remove(path);
}

// ===========================================================================
// 2. THE HEADLINE: the consumer's float DD ambiguities converge to the KNOWN
//    integers (the common per-signal fractional cancels), MLAMBDA actually
//    executes (gate_reason==0, ratio>0), the fix clears the production
//    acceptance battery, and the fixed state lands on the known integers.
// ===========================================================================
TEST(Eq34ConsumerIntegerTest, FloatDdConvergesToKnownIntegersAndMlambdaFixes) {
    LogManager log;
    PppUtils   utils(log);
    const SynthFrame frame = makeFrame();
    const std::string path = tempFile("gnss_eq34_synth_positive.txt");
    std::filesystem::remove(path);

    ASSERT_NO_FATAL_FAILURE(writeSynthProduct(path, frame, utils, /*poisoned=*/false));
    SsrCorrectionStore store;
    SbasisContract     contract;
    ASSERT_NO_FATAL_FAILURE(replayIntoStore(path, store, contract));

    GnssConfig   config;
    StateManager sm(kPppCoreSize, log, config);
    std::vector<PppObservation> obs;
    ASSERT_NO_FATAL_FAILURE(buildConsumerState(frame, store, utils, sm, obs));

    // (a) The PER-SATELLITE floats are far from integer: each carries the
    // engineered δ_u − δ_σ (≥ 0.27 cycles on every signal).  This is what makes
    // the DD-integer assertion below non-trivial — the common fractional is
    // demonstrably present and only the DIFFERENCE cancels it.
    {
        const Eigen::VectorXd x = sm.getStateVector();
        for (const auto& con : frame.cons) {
            for (std::size_t i = 0; i < con.sats.size(); ++i) {
                for (const auto& sf : con.signals) {
                    const int idx = sm.getAmbiguityIndex(con.sats[i] + "_" + sf.signal);
                    ASSERT_GE(idx, 0);
                    const double lam = utils.getWavelength(con.system, sf.signal, con.sats[i]);
                    EXPECT_GT(fracDist(x(idx) / lam), 0.20)
                        << "per-sat float should NOT be integer: "
                        << con.sats[i] << " " << sf.signal;
                }
            }
        }
    }

    // (b) Every within-group float DD is integer to within the injected noise
    // (|Δε| ≤ 0.055 cyc) AND rounds to the KNOWN integer N.
    std::vector<DdCheck> dds;
    ASSERT_NO_FATAL_FAILURE(collectConsumerDds(frame, sm, utils, dds));
    ASSERT_EQ(dds.size(), 14u);   // GPS 2×4 + Galileo 2×3
    for (const auto& d : dds) {
        EXPECT_LT(fracDist(d.dd_cycles), 0.08)
            << d.sat_i << "-" << d.sat_ref << " " << d.signal;
        EXPECT_EQ(static_cast<int>(std::round(d.dd_cycles)), d.known_n)
            << d.sat_i << "-" << d.sat_ref << " " << d.signal;
    }

    // (c) MLAMBDA executes and fixes through the REAL resolver (real
    // buildDdPairs pivot election, real search, production acceptance gates).
    AmbiguityResolver resolver(makeArConfig(), log);
    const AmbiguityResolution res = resolver.resolve(sm, obs);

    EXPECT_EQ(res.gate_reason, 0) << "an early gate suppressed AR — MLAMBDA never ran";
    EXPECT_GT(res.ratio, 0.0)     << "ratio 0.00 is the never-ran sentinel";
    EXPECT_TRUE(res.success);
    EXPECT_EQ(res.num_candidates, 14);
    EXPECT_EQ(res.num_fixed, 14);
    EXPECT_LT(res.ratio, makeArConfig().ratio_upper_bound);

    // (d) The fixed state is pulled onto the KNOWN integers (hold_variance is
    // tight, so the post-fix DD equals λ·N to well under a millicycle).
    ASSERT_EQ(res.fixed_state.size(), sm.getStateVector().size());
    for (const auto& d : dds) {
        const double dd_fixed_m = res.fixed_state(d.idx_i) - res.fixed_state(d.idx_ref);
        EXPECT_NEAR(dd_fixed_m, d.lambda_m * static_cast<double>(d.known_n), 1e-3)
            << d.sat_i << "-" << d.sat_ref << " " << d.signal;
    }

    std::filesystem::remove(path);
}

// ===========================================================================
// 3. NEGATIVE CONTROL: one deliberately non-integer bias pair (G05 L1C,
//    +0.37 cyc).  The consumer's DD fraction for that pair lands far from
//    integer; every other pair stays clean; MLAMBDA RUNS but the production
//    acceptance battery refuses the fix (ratio and/or per-pair frac cap).
// ===========================================================================
TEST(Eq34ConsumerIntegerTest, NonIntegerBiasPairFailsAsNegativeControl) {
    LogManager log;
    PppUtils   utils(log);
    const SynthFrame frame = makeFrame();
    const std::string path = tempFile("gnss_eq34_synth_poisoned.txt");
    std::filesystem::remove(path);

    ASSERT_NO_FATAL_FAILURE(writeSynthProduct(path, frame, utils, /*poisoned=*/true));
    SsrCorrectionStore store;
    SbasisContract     contract;
    ASSERT_NO_FATAL_FAILURE(replayIntoStore(path, store, contract));

    GnssConfig   config;
    StateManager sm(kPppCoreSize, log, config);
    std::vector<PppObservation> obs;
    ASSERT_NO_FATAL_FAILURE(buildConsumerState(frame, store, utils, sm, obs));

    std::vector<DdCheck> dds;
    ASSERT_NO_FATAL_FAILURE(collectConsumerDds(frame, sm, utils, dds));
    ASSERT_EQ(dds.size(), 14u);

    std::size_t n_poisoned = 0;
    for (const auto& d : dds) {
        const bool poisoned_pair =
            (d.sat_i == frame.poison_sat && d.signal == frame.poison_signal);
        if (poisoned_pair) {
            ++n_poisoned;
            // Expected fraction ≈ poison (0.37) ± |Δε| ≤ 0.055.
            EXPECT_GT(fracDist(d.dd_cycles), 0.25)
                << "the poisoned pair must be far from integer";
        } else {
            EXPECT_LT(fracDist(d.dd_cycles), 0.08)
                << "the poison must be surgical: " << d.sat_i << "-" << d.sat_ref
                << " " << d.signal;
            EXPECT_EQ(static_cast<int>(std::round(d.dd_cycles)), d.known_n)
                << d.sat_i << "-" << d.sat_ref << " " << d.signal;
        }
    }
    EXPECT_EQ(n_poisoned, 1u);   // G05 is never the pivot (lowest elevation)

    // MLAMBDA reaches the search (this is NOT a gate suppression) but the
    // acceptance battery — ratio test and/or the 0.15-cyc per-pair fraction
    // cap — refuses to fix a frame containing a 0.37-cycle non-integer pair.
    AmbiguityResolver resolver(makeArConfig(), log);
    const AmbiguityResolution res = resolver.resolve(sm, obs);
    EXPECT_EQ(res.gate_reason, 0) << "the search must RUN for the control to mean anything";
    EXPECT_GT(res.ratio, 0.0);
    EXPECT_FALSE(res.success)
        << "a non-integer-consistent product must not certify integer recovery";

    std::filesystem::remove(path);
}

// ===========================================================================
// 4. THE σ-GATE SENTINEL GUARD (the C7 misread): ratio == 0.00 with
//    gate_reason != 0 means the MLAMBDA search NEVER RAN — it must never be
//    read as "AR executed and produced a zero-confidence result".  Pins the
//    sentinel for both convergence guards so any grader built on this harness
//    keys acceptance on (gate_reason == 0 && ratio > 0), never on ratio alone.
// ===========================================================================
TEST(Eq34ConsumerIntegerTest, RatioZeroSentinelMeansMlambdaNeverRan) {
    LogManager log;
    PppUtils   utils(log);
    const SynthFrame frame = makeFrame();
    const std::string path = tempFile("gnss_eq34_synth_sentinel.txt");
    std::filesystem::remove(path);

    ASSERT_NO_FATAL_FAILURE(writeSynthProduct(path, frame, utils, /*poisoned=*/false));
    SsrCorrectionStore store;
    SbasisContract     contract;
    ASSERT_NO_FATAL_FAILURE(replayIntoStore(path, store, contract));

    GnssConfig   config;
    StateManager sm(kPppCoreSize, log, config);
    std::vector<PppObservation> obs;
    ASSERT_NO_FATAL_FAILURE(buildConsumerState(frame, store, utils, sm, obs));

    // (a) Position-σ gate: same healthy candidate set as the headline test,
    // but an unconverged position covariance.  gate_reason=3 with the σ
    // recorded; ratio stays the 0.00 sentinel; num_candidates shows the DD
    // set existed (the search was suppressed, not empty).
    {
        Eigen::MatrixXd P = sm.getCovariance();
        for (int d = 0; d < 3; ++d) P(d, d) = 1.0;   // σ_3D ≈ 1.73 m ≫ 0.05 m
        sm.setCovariance(P);

        AmbiguityResolver resolver(makeArConfig(), log);
        const AmbiguityResolution res = resolver.resolve(sm, obs);
        EXPECT_EQ(res.gate_reason, 3);
        EXPECT_DOUBLE_EQ(res.ratio, 0.0);
        EXPECT_FALSE(res.success);
        EXPECT_GT(res.gate_sigma_3d, 0.05);
        EXPECT_EQ(res.num_candidates, 14);

        // Restore the converged covariance for part (b).
        for (int d = 0; d < 3; ++d) P(d, d) = kPosVar;
        sm.setCovariance(P);
    }

    // (b) min_epochs gate: a fresh resolver with a warm-up requirement returns
    // the sentinel on the first invocation — before DD pairs are even built.
    {
        AmbiguityResolverConfig cfg = makeArConfig();
        cfg.min_epochs = 3;
        AmbiguityResolver resolver(cfg, log);
        const AmbiguityResolution res = resolver.resolve(sm, obs);
        EXPECT_EQ(res.gate_reason, 2);
        EXPECT_DOUBLE_EQ(res.ratio, 0.0);
        EXPECT_FALSE(res.success);
    }

    std::filesystem::remove(path);
}

// ===========================================================================
// 5. CONTRACT CROSS-CHECK (D5's designed back-compat): a consumer expecting
//    the legacy pivot-receiver datum flags "amb_datum differs" against an
//    eq34 product — and an eq34-aware expectation verifies clean.
// ===========================================================================
TEST(Eq34ConsumerIntegerTest, ContractFlagsAmbDatumForLegacyConsumer) {
    LogManager log;
    PppUtils   utils(log);
    const SynthFrame frame = makeFrame();
    const std::string path = tempFile("gnss_eq34_synth_contract.txt");
    std::filesystem::remove(path);

    ASSERT_NO_FATAL_FAILURE(writeSynthProduct(path, frame, utils, /*poisoned=*/false));

    bool ok = false;
    SbasisContract file_contract;
    (void)NetworkProductReader::read(path, &file_contract, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(file_contract.present);

    // eq34-aware consumer: compatible.
    {
        std::vector<std::string> mismatches;
        EXPECT_TRUE(NetworkProductReader::checkContract(file_contract, makeEq34Contract(),
                                                        &mismatches))
            << (mismatches.empty() ? "" : mismatches.front());
    }

    // Legacy consumer (PivotReceiverZeroAmbiguity expectation): incompatible,
    // with the amb_datum mismatch named — exactly the designed refusal path
    // for a pre-Card-3 consumer meeting a chain-datum product.
    {
        SbasisContract expected = makeEq34Contract();
        expected.decl.amb_datum = SbasisDeclaration::AmbDatum::PivotReceiverZeroAmbiguity;
        std::vector<std::string> mismatches;
        EXPECT_FALSE(NetworkProductReader::checkContract(file_contract, expected, &mismatches));
        bool named = false;
        for (const auto& m : mismatches)
            if (m.find("amb_datum") != std::string::npos) named = true;
        EXPECT_TRUE(named) << "mismatch list must name amb_datum";
    }

    std::filesystem::remove(path);
}

} // namespace
} // namespace GnssProcessor
