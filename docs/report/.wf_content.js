export const meta = {
  name: 'gnss-report-content',
  description: 'Refresh + author the GnssProcessor math-reference component docs (subfiles), then adversarially verify the new math against source',
  phases: [
    { title: 'Author', detail: 'one agent per component doc: refresh existing / author new' },
    { title: 'Verify', detail: 'adversarial correctness check vs C++ source' },
    { title: 'Fix', detail: 'apply verified critical fixes' },
  ],
}

// ----------------------------------------------------------------------
// Shared rules every agent must follow.
// ----------------------------------------------------------------------
const REPO = 'C:/GnssProcessor'
const DOCDIR = 'C:/GnssProcessor/docs/report'

const RULES = `
You are working on the GnssProcessor LaTeX *mathematical reference*, which has
just been restructured into the \`subfiles\` package layout under ${DOCDIR}/.
Each component file is a self-contained subfile that compiles BOTH standalone
(its own PDF) AND as part of the combined volume main.tex.

HARD CONSTRAINTS (violating these breaks every other document):
- Edit ONLY the file(s) assigned to you. Do NOT touch main.tex, gnssreport.sty,
  latexmkrc, build.ps1, build.sh, references.bib, the generated/ directory, or
  any other component's .tex file.
- Do NOT modify anything OUTSIDE the document body. Leave intact: the leading
  comment banner, \\documentclass[main.tex]{subfiles}, \\begin{document}, the
  \\ifSubfilesClassLoaded{...}{} title line, the top \\section{...}\\label{...},
  and the trailing \\ifSubfilesClassLoaded{\\printbibliography}{} \\end{document}.
  Your work goes in the BODY between the \\section line and the trailing guard.
- Notation macros live in gnssreport.sty (the single source of truth): \\stateV,
  \\covM, \\jacH, \\Fmat, \\Qmat, \\Rmat, \\Kgain, \\innov, \\bsd, \\brd, \\dd,
  \\piv, \\code, \\phaseo, \\geomrange, \\mwc, \\gfc, \\Nwl, \\lamwl, \\lamnl,
  \\Zmat, \\Lmat, \\Dmat, \\ambfloat, \\ambfix, \\Cbe, etc. Read gnssreport.sty
  to see them all and REUSE them. If you genuinely need a NEW symbol, define it
  with \\newcommand at the very top of your body (just after \\section) and list
  it in newMacros so it can be promoted to the .sty later. Never redefine an
  existing macro.

CROSS-DOCUMENT REFERENCES:
- For a reference to a label defined in a DIFFERENT component file, use
  \\dcref{label}{human-readable name} (renders a number in the combined volume,
  a readable name in the standalone PDF). For references WITHIN your own file,
  use plain \\Cref.
- Label map across the documents:
  Foundations: sec:notation, sec:state, sec:obs, sec:diff
  Estimation engine: sec:kf  |  Cycle-slip: sec:slip  |  Ambiguity: sec:ar
  Inertial aiding: sec:imu  |  SSR corrections: sec:ssr
  Real-time engine: sec:rt  |  Network mode: sec:net
  Convert any EXISTING raw \\Cref that points OUT of your file to \\dcref. (Grep
  your own file for \\label to learn which labels are local.)

SCOPE & STYLE (match the existing documents exactly):
- This reference documents settled estimation MATHEMATICS and DESIGN RATIONALE
  -- material that ages slowly. Cite the C++ source path inline for anything
  implementation-specific, e.g. \\texttt{src/core/filter/KalmanFilter.cpp}.
- Do NOT reproduce empirical thresholds / tuning constants (MAD, Welford, EWMA
  gates, etc.) -- state that they are calibrated and point to the code.
- Match the existing thorough, equation-dense style. Use \\begin{empheq}[box=\\fbox]{align}
  for the few key boxed results, align/equation for the rest, \\Cref for refs,
  itemize/enumerate with [leftmargin=*]. Wrap source lines around 70-78 cols.
- Read an existing well-written file first for tone, e.g. ${DOCDIR}/kalman_smoother.tex
  and ${DOCDIR}/slip_detection.tex.

CORRECTNESS (this is reviewed by an expert -- wrong math is worse than missing):
- Verify EVERY claim against the actual source in ${REPO}. The tree contains
  uncommitted work; read the real files, grep for symbols. Do NOT invent
  algorithms, equations, or features that are not in the code. If a feature is
  config-gated or default-off, say so. If you are unsure something exists, grep
  before writing it; if still unsure, omit it and note it in openQuestions.

BUILD CHECK (do this before returning):
- Compile ONLY your own file (never bare \`latexmk\`, which would build every
  target and race other agents):
    cd ${DOCDIR} && latexmk -pdf -interaction=nonstopmode <yourfile>.tex
  Iterate until it exits 0. Undefined-citation warnings for a genuinely-new
  reference you listed in newBibNeeded are acceptable (note them in buildNotes);
  LaTeX errors and undefined \\ref/\\Cref are NOT -- fix them.
- Do NOT add \\cite keys that are absent from references.bib (read it first).
  If you need a new reference, put the full BibTeX entry in newBibNeeded and use
  its key; the citation will resolve once the maintainer merges it.
`

const AUTHOR_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['file', 'summary', 'staleFixed', 'newBibNeeded', 'newMacros', 'openQuestions', 'sourcesRead', 'builtClean', 'buildNotes'],
  properties: {
    file: { type: 'string' },
    summary: { type: 'string', description: 'what you wrote/changed, 3-6 sentences' },
    staleFixed: { type: 'array', items: { type: 'string' }, description: 'stale/incorrect claims you corrected' },
    newBibNeeded: { type: 'array', items: { type: 'string' }, description: 'full BibTeX entries to add to references.bib (empty if none)' },
    newMacros: { type: 'array', items: { type: 'string' }, description: 'new \\newcommand definitions added to your body, for promotion to the .sty' },
    openQuestions: { type: 'array', items: { type: 'string' }, description: 'things you could not confirm in source' },
    sourcesRead: { type: 'array', items: { type: 'string' }, description: 'source files you actually read' },
    builtClean: { type: 'boolean', description: 'did latexmk on your file exit 0' },
    buildNotes: { type: 'string', description: 'remaining warnings (e.g. expected undefined citations)' },
  },
}

const VERIFY_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['file', 'verdict', 'issues'],
  properties: {
    file: { type: 'string' },
    verdict: { type: 'string', enum: ['pass', 'issues'] },
    issues: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['severity', 'location', 'problem', 'fix'],
        properties: {
          severity: { type: 'string', enum: ['critical', 'major', 'minor'] },
          location: { type: 'string', description: 'equation/paragraph/line anchor' },
          problem: { type: 'string', description: 'math error, invented claim, wrong citation, stale claim, or LaTeX defect' },
          fix: { type: 'string', description: 'concrete correction' },
        },
      },
    },
  },
}

const FIX_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['file', 'applied', 'skipped'],
  properties: {
    file: { type: 'string' },
    applied: { type: 'array', items: { type: 'string' } },
    skipped: { type: 'array', items: { type: 'string' }, description: 'issues left unfixed and why' },
  },
}

// ----------------------------------------------------------------------
// The component documents.
// ----------------------------------------------------------------------
const DOCS = [
  {
    file: 'foundations.tex', label: 'foundations', mode: 'refresh',
    // NOTE: foundations.tex is a wrapper that \input's four content fragments.
    edits: 'notation.tex, state_vector.tex, observation_equations.tex, bssd_dd.tex',
    task: `Refresh the FOUR content fragments that foundations.tex \\input's:
  ${DOCDIR}/notation.tex, state_vector.tex, observation_equations.tex, bssd_dd.tex.
  Do NOT edit foundations.tex itself (it only supplies the \\section headings);
  compile foundations.tex to verify your fragment edits.
  - notation.tex: symbol table + sign conventions. Ensure it matches the macros
    actually used across the suite (read gnssreport.sty) and covers any new
    symbols (satellite phase bias for network mode, GLONASS IFB).
  - state_vector.tex: the variable-dimension state vector. Confirm against
    include/filter/StateManager.hpp and include/filter/StateKeySchema.hpp the
    full set of dynamic states and their key formats: position(/velocity),
    receiver clock(s) + inter-system time offsets, ZWD, per-satellite slant
    iono, receiver code biases / IFB (incl. the per-slot GLONASS IFB scheme,
    which is config-gated -- verify enable_glonass_ifb and note default-off),
    satellite biases, and ambiguities. State when each is enabled.
  - observation_equations.tex: the UNCOMBINED pseudorange and carrier-phase
    models (no IF combination on the measurement-update path). Confirm against
    src/core/measurement/MeasurementModel.cpp and model/PppModel.cpp.
  - bssd_dd.tex: between-satellite single-difference (PPP-RTK) and
    double-difference (baseline) transformations + the S-transform structure
    mapping undifferenced parameters to differenced ones. Confirm against
    src/core/measurement/BssdTransform.cpp and BrsdTransform.cpp.
  This is the shared substrate every other doc cites -- keep it precise and
  internally consistent; mostly settled, so refresh rather than rewrite.`,
    sources: 'include/filter/StateManager.hpp, include/filter/StateKeySchema.hpp, src/core/filter/StateManager.cpp, src/core/measurement/MeasurementModel.cpp, src/core/measurement/BssdTransform.cpp, src/core/measurement/BrsdTransform.cpp, src/core/model/PppModel.cpp',
  },
  {
    file: 'kalman_smoother.tex', label: 'estimation', mode: 'refresh',
    edits: 'kalman_smoother.tex',
    task: `Refresh the estimation-engine document. Confirm and document, against
  source: the variable-dimension EKF predict/update; the Joseph-form covariance
  update; the prior-anchored iterated EKF (IEKF) relinearisation second pass
  (the anchor is the prior update() estimate stashed and dimension-matched, NOT
  a top-of-loop capture; it is config-gated by a geometry-relinearise threshold
  and rarely fires at the 1 m default; the tightly-coupled IMU path keeps the
  naive single-pass update); robust post-fit standardised-residual gating; and
  the backward RTS smoother (segment boundaries at ambiguity resets; smoothed
  static-PPP position is degenerate/constant by construction; recent frame/slip
  fixes). Mention non-causal smoother jump re-estimation ONLY if it is actually
  implemented -- grep Smoother first; if only scaffolded, omit or note as future.`,
    sources: 'include/filter/KalmanFilter.hpp, src/core/filter/KalmanFilter.cpp, include/filter/Smoother.hpp, src/core/filter/Smoother.cpp, src/core/filter/StateManager.cpp',
  },
  {
    file: 'slip_detection.tex', label: 'slip', mode: 'refresh',
    edits: 'slip_detection.tex',
    task: `Refresh the cycle-slip document to match the current detectors and
  repair logic. Document the linear-combination test statistics (Melbourne-
  Wubbena, geometry-free, GRAPHIC, extra-wide-lane) as algebra; the ISlipDetector
  registry (config estimation.cycle_slip); MW detection modes (hatch_mean |
  local_median | time_difference with mw_td_order, TD1 being quadratic-immune);
  cross-sat MAD now EWMA-smoothed and warmup-only (mature arcs use per-arc MAD);
  and the TurboEdit shared resolver: the 2x2 per-signal split (catching the MW
  dN1=dN2 blind spot), the MW-only + GF-stable "code_event" classification that
  SKIPS the phase reset, the gap-spanning GF bridge (turbo_edit_max_gap_s), and
  the rate-adaptive evidence gates that replaced the old hard <=1 s rate gates.
  Keep thresholds as pointers to code, not values. Verify everything in source.`,
    sources: 'src/core/slip/ (all), include/slip/ (all), include/filter/TurboEditResolver.hpp + src, src/core/filter/CycleSlipManager.cpp (if present), include/filter/CycleSlipManager.hpp',
  },
  {
    file: 'ambiguity_resolution.tex', label: 'ambiguity', mode: 'refresh',
    edits: 'ambiguity_resolution.tex',
    task: `Refresh the integer-ambiguity-resolution document. Document the
  MLAMBDA integer least-squares search; the LDL^T decorrelation (note the
  convention Q = L^T D L) and Z-transform; wide-lane / narrow-lane decomposition
  (incl. GLONASS FDMA handling where applicable); validation (ratio test); and
  the constrained feedback implemented as Kalman pseudo-observations (the mode-3
  destabilisation was fixed via that refactor; continuous mode-1 is the usable
  path; shipped configs are float-only mode=0). Mention partial AR and any
  standard-PPP (IF WL/NL) AR ONLY if present in source -- grep AmbiguityResolver
  first. Verify against source.`,
    sources: 'include/filter/AmbiguityResolver.hpp, src/core/filter/AmbiguityResolver.cpp, include/math/ (LAMBDA/LDL helpers), src/core/filter/StateManager.cpp',
  },
  {
    file: 'imu_aiding.tex', label: 'imu', mode: 'refresh',
    edits: 'imu_aiding.tex',
    task: `Light refresh of the inertial-aiding document. Confirm the strapdown
  mechanisation, the 15-state INS error model (position, velocity, attitude,
  accel bias, gyro bias) and its continuous F/G + discrete Phi/Q, and the
  tightly-coupled integration. Update any forward-looking references to the
  AidingSourceManager / IAidingSource abstraction to reflect current status
  (Step 0 is inert/uncommitted -- describe as the intended seam, not as done).
  Mostly correct already; fix only what drifted.`,
    sources: 'include/filter/ImuIntegrator.hpp, src/core/filter/ImuIntegrator.cpp, src/core/aiding/ImuAidingSource.cpp (if present), include/aiding/ (if present)',
  },
  {
    file: 'ssr_corrections.tex', label: 'ssr', mode: 'refresh',
    edits: 'ssr_corrections.tex',
    task: `Refresh the real-time SSR-corrections document. IMPORTANT: it
  currently contains a STALE claim that the network side is "not yet
  implemented" -- the network estimator now exists (see sec:net). Fix that and
  any other stale text. Document how SSR orbit (radial/along/cross), clock, and
  code/phase bias corrections refine the UNDIFFERENCED observation model; the
  ssr_bias_term application path and how the satellite-bias STATE gate interacts
  with SSR code biases; and GLONASS handling. Keep the streaming/transport
  mechanics OUT of this doc -- those belong to the Real-time engine document;
  cross-reference it with \\dcref{sec:rt}{the Real-time engine document} and
  \\dcref{sec:net}{the Network mode document}. Verify against source.`,
    sources: 'src/core/products/SsrCorrectionStore.cpp, include/products/SsrCorrectionStore.hpp, src/core/products/ProductParser.cpp, src/core/corrections/GnssCorrections.cpp, src/core/model/PppModel.cpp, src/core/measurement/MeasurementModel.cpp',
  },
  {
    file: 'realtime_engine.tex', label: 'realtime', mode: 'author',
    edits: 'realtime_engine.tex',
    task: `AUTHOR this NEW document (currently a stub). It describes the
  STREAMING ARCHITECTURE that distinguishes the real-time path
  (Application::runRealTime -> RealTimeEngine) from post-processing. Cover, with
  math where it matters and architecture prose elsewhere:
   - NTRIP transport: NtripClient over TcpClient, plain-HTTP only (no TLS),
     mountpoint/auth, reconnection; RtcmStreamReader framing.
   - RTCM3 decoding: frame/CRC (RtcmParser); MSM observation decode
     (RtcmMsmDecoder, MSM4/5/7) -- including the DF419 extended-satellite-info
     gating that must be honoured per the message variant (the GPS L1/L2 GF
     sign bug when DF419 is mis-read); broadcast nav (RtcmNavDecoder); SSR
     (RtcmSsrDecoder); station coordinates from message 1005/1006.
   - Time systems: GLONASS TOD -> GPS time (TimeUtils::glonassTodToGpsTime) for
     MSM / 1020 toe / SSR; DF419 FCN -> wavelength; per-record content epoch.
   - Observation/SSR synchronisation: the enable_obs_ssr_sync hold buffer and
     newestSsrEpoch logic that stops observations running ahead of lagging SSR.
   - Ephemeris management: freshest-toe selection and toe-aware pivoting.
   - A-priori seeding from RTCM 1005/1006 ARP (parity with SBF/PVT), making
     receiver.reference_position optional for stations streaming 1005/1006.
   - The per-epoch real-time predict/update path; note the RTS smoother and
     other post-processing-only stages are skipped in real time.
  Cross-ref the SSR correction MATH to \\dcref{sec:ssr}{...} (don't duplicate
  it) and the observation model to \\dcref{sec:obs}{...}. Read the source
  thoroughly; this is new and will be read critically by an expert.`,
    sources: 'include/realtime/RealTimeEngine.hpp, src/core/realtime/RealTimeEngine.cpp, include/realtime/NtripClient.hpp, src/core/realtime/NtripClient.cpp, include/realtime/RtcmStreamReader.hpp, src/core/realtime/RtcmStreamReader.cpp, include/realtime/TcpClient.hpp, src/core/rtcm/RtcmParser.cpp, src/core/rtcm/RtcmMsmDecoder.cpp, src/core/rtcm/RtcmSsrDecoder.cpp, src/core/rtcm/RtcmNavDecoder.cpp, src/core/rtcm/RtcmSignalMap.cpp, include/rtcm/ (all), src/core/app/ApplicationRealtime.cpp, src/core/math/TimeUtils.cpp',
  },
  {
    file: 'network.tex', label: 'network', mode: 'author',
    edits: 'network.tex',
    task: `AUTHOR this NEW document (currently a stub). It describes the
  multi-station PPP-RTK NETWORK estimator
  (ProcessingMode::PPP_RTK_Network) that GENERATES the SSR-style corrections the
  single-receiver user modes consume. The math must be rigorous. Cover:
   - The joint multi-station undifferenced observation model and which
     parameters are shared (satellite clocks/orbits, satellite phase/code
     biases, per-satellite ionosphere) vs per-station (receiver clock, ZWD,
     ambiguities, receiver biases).
   - The EXPLICIT satellite phase-bias state (contrast with the user side, which
     handles biases implicitly via differencing -- cross-ref \\dcref{sec:diff}).
   - The rank deficiency of the undifferenced network system and the
     minimum-constraint S-basis / datum definition that resolves it: the
     exact-gauge formulation with SOFT priors only (no hard elimination),
     the chosen datum (which clocks/biases are constrained), and the
     S-transform connecting estimable combinations to the chosen basis.
   - The per-station Schur reduction / normal-equation structure
     (NetworkNormalEquations) that keeps the joint system tractable.
   - Constellation scope: GPS + Galileo (+ BDS), GLONASS excluded, and why.
   - The closed loop: corrections feed the user consumer on a held-out station.
  Read include/filter/NetworkEstimator.hpp, NetworkNormalEquations.hpp,
  src/core/filter/NetworkEstimator.cpp, and StateKeySchema.hpp carefully. This
  is active, uncommitted work -- document what the code ACTUALLY does; if a
  planned piece is not yet wired (e.g. runNetwork dispatch), say so. Put open
  uncertainties in openQuestions rather than guessing.`,
    sources: 'include/filter/NetworkEstimator.hpp, include/filter/NetworkNormalEquations.hpp, src/core/filter/NetworkEstimator.cpp, include/filter/StateKeySchema.hpp, src/core/filter/StateManager.cpp, src/core/measurement/MeasurementBuilder.cpp, src/core/measurement/MeasurementModel.cpp, include/config/GnssConfigTypes.hpp, src/core/app/Application.cpp',
  },
  {
    file: 'pipeline_diagrams.tex', label: 'diagrams', mode: 'refresh',
    edits: 'pipeline_diagrams.tex',
    task: `Refresh the curated TikZ diagrams. Keep the existing epoch-loop and
  differencing-strategy diagrams. ADD two new diagrams: (1) the real-time
  streaming pipeline (NTRIP -> RTCM/MSM decode -> obs/SSR sync buffer -> per-
  epoch update), citing RealTimeEngine functions in the boxes; and (2) the
  network estimator data flow (multi-station obs -> joint normal equations ->
  Schur reduction -> S-basis datum -> SSR-style correction products), citing
  NetworkEstimator. Use ONLY the TikZ styles already defined in gnssreport.sty
  (proc/io/store/note/flow/feedback, and \\fitwidth). Each box should cite the
  function it maps to, matching the existing diagrams' convention. Cross-ref
  \\dcref{sec:rt}{...} and \\dcref{sec:net}{...}. This is lower priority than the
  prose docs -- if a diagram would be speculative, keep it minimal and accurate.`,
    sources: 'include/realtime/RealTimeEngine.hpp, src/core/realtime/RealTimeEngine.cpp, include/filter/NetworkEstimator.hpp, src/core/filter/NetworkEstimator.cpp',
  },
]

// ----------------------------------------------------------------------
// Prompts.
// ----------------------------------------------------------------------
function authorPrompt(doc) {
  return `${RULES}

=================  YOUR ASSIGNMENT  =================
Document: ${doc.file}   (mode: ${doc.mode})
File(s) you may edit: ${doc.edits}
Primary source files to read in ${REPO}:
  ${doc.sources}

TASK:
${doc.task}

Work in ${DOCDIR}. When done, your file(s) must compile (latexmk on your own
.tex, exit 0). Return the structured AUTHOR result describing what you did.`
}

function verifyPrompt(doc, authorResult) {
  return `${RULES}

=================  ADVERSARIAL VERIFICATION  =================
You are a skeptical GNSS estimation expert reviewing the document ${doc.file}
(in ${DOCDIR}) for TECHNICAL CORRECTNESS against the actual C++ source in
${REPO}. The author reported:
  summary: ${authorResult ? authorResult.summary : '(author failed to return)'}
  openQuestions: ${authorResult ? JSON.stringify(authorResult.openQuestions) : '[]'}

Read ${doc.file} AND the cited source (${doc.sources}). Hunt for:
  1. MATH ERRORS -- wrong signs, wrong indices, dimensionally inconsistent
     equations, incorrect linear-combination coefficients, wrong covariance/
     gain forms.
  2. INVENTED CLAIMS -- algorithms, states, equations, or config flags asserted
     in the text that do NOT exist in the source. Grep to confirm existence.
  3. WRONG CITATIONS -- \\texttt{src/...} paths that don't contain what the text
     says they do.
  4. STALE CLAIMS -- text contradicted by the current code (e.g. "not
     implemented" for things that are).
  5. LATEX DEFECTS -- anything that breaks the build or renders wrong; also
     misuse of \\Cref where \\dcref is required for a cross-document label.
Default to flagging when uncertain. For each issue give a concrete fix. If the
document is sound, return verdict "pass" with an empty issues list. Do NOT edit
the file in this step.`
}

function fixPrompt(doc, verifyResult) {
  return `${RULES}

=================  APPLY VERIFIED FIXES  =================
Document: ${doc.file} (in ${DOCDIR}). A reviewer found these issues:

${JSON.stringify(verifyResult.issues, null, 2)}

Apply the critical and major fixes (and any minor ones that are clearly
correct). For each, verify the corrected statement against source before
writing. Re-confirm the assigned file(s) still compile (latexmk on your own
.tex, exit 0). If you disagree with an issue after checking source, skip it and
explain why. Return the structured FIX result.`
}

// ----------------------------------------------------------------------
// Run: author -> verify -> conditional fix, independently per document.
// ----------------------------------------------------------------------
const results = await pipeline(
  DOCS,
  (doc) => agent(authorPrompt(doc), { label: `author:${doc.label}`, phase: 'Author', schema: AUTHOR_SCHEMA, effort: 'high' }),
  (authorResult, doc) => agent(verifyPrompt(doc, authorResult), { label: `verify:${doc.label}`, phase: 'Verify', schema: VERIFY_SCHEMA, effort: 'high' })
      .then(v => ({ author: authorResult, verify: v })),
  (vr, doc) => {
    const verify = vr && vr.verify
    const author = vr && vr.author
    const needsFix = verify && verify.verdict === 'issues' &&
      verify.issues.some(i => i.severity === 'critical' || i.severity === 'major')
    if (!needsFix) return { doc: doc.label, file: doc.file, author, verify, fix: null }
    return agent(fixPrompt(doc, verify), { label: `fix:${doc.label}`, phase: 'Fix', schema: FIX_SCHEMA, effort: 'high' })
      .then(fix => ({ doc: doc.label, file: doc.file, author, verify, fix }))
  }
)

// Aggregate for the maintainer step that follows the workflow.
const bib = []
const macros = []
const open = []
for (const r of results.filter(Boolean)) {
  if (r.author) {
    for (const b of (r.author.newBibNeeded || [])) bib.push({ doc: r.doc, entry: b })
    for (const m of (r.author.newMacros || [])) macros.push({ doc: r.doc, macro: m })
    for (const q of (r.author.openQuestions || [])) open.push({ doc: r.doc, q })
  }
}
log(`authored ${results.filter(Boolean).length}/${DOCS.length} docs; ` +
    `${bib.length} bib entries proposed, ${macros.length} new macros, ${open.length} open questions`)

return {
  perDoc: results.filter(Boolean).map(r => ({
    doc: r.doc, file: r.file,
    builtClean: r.author && r.author.builtClean,
    staleFixed: r.author ? r.author.staleFixed : [],
    verdict: r.verify ? r.verify.verdict : 'no-verify',
    unresolvedIssues: r.verify && r.verify.verdict === 'issues'
      ? r.verify.issues.filter(i => !r.fix || (r.fix.skipped || []).some(s => s.includes(i.location)))
      : [],
    fixApplied: r.fix ? r.fix.applied : [],
  })),
  newBibNeeded: bib,
  newMacros: macros,
  openQuestions: open,
}
