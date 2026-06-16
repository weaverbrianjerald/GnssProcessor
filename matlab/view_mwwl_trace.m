%% ============================================================
%  Script: view_mwwl_trace.m
%  Purpose: Plot Melbourne-Wubbena wide-lane (MW WL) traces for cycle
%           slip diagnostics.  Mirrors the look of
%           measurement_residuals.m: per-constellation columns, signal
%           pairs stacked within each column, one colored line per
%           satellite, mu/sigma annotation in the top-right corner.
%  ============================================================

clear all; %clc; %close all;

%% --- Plot style switch ---
% 'lines'   : one line per (satellite, signal-pair) with NaN breaks at
%             gaps in time.  Recommended for slip-shape inspection.
% 'scatter' : markers only, one entry per satellite per signal pair.
plotStyle = 'lines';
% A gap is declared whenever dt > gapFactor x median(dt) for that series;
% NaN is inserted there so plot() draws a break instead of crossing it.
gapFactor = 3;

%% --- Content switches ---
% Set to false to suppress the secondary figures (saves figure windows
% when you only care about the primary dev-from-mean view).
showRawMwPlot          = false;  % raw mw_cycles vs time
showZScorePlot         = false;   % dev_from_mean / mw_sigma -- approximate
                                 % "sigmas off arc mean" gauge
showThresholdRatioPlot = false;   % dev_from_mean / threshold_cycles --
                                 % EXACT detector gate: |y|>1 iff is_slip=1
showSlipExampleFigs    = true;   % zoomed two-panel example of one detected
                                 % and one repair-eligible slip event (built
                                 % from the pre-hideSlipEpochs row set)
showGapBridgeFigs      = true;   % zoomed three-panel exhibits of TurboEdit
                                 % gap-bridge adjudications (across-gap GF
                                 % jump vs the late MW flag); needs
                                 % turbo_edit_trace.csv next to the MW trace
maxGapBridgeFigs       = 2;      % cap on gap-bridge exhibit figures
showLegend             = false;  % per-tile satellite legend on top tile of each column

% Drop rows where the C++ MwSlipDetector flagged is_slip=1.  These are
% the events the slip detector ALREADY handled (post-reset the arc
% restarts cleanly), and they can be hundreds of cycles in magnitude --
% one rogue slip epoch will stretch the y-axis so far that the normal
% MW noise (typically << 1 cycle) collapses to a flat line at the
% origin.  Hiding them lets the dev-from-mean plot show the actual arc
% behaviour at full resolution.  Set false to keep the slip spikes
% visible (e.g., when inspecting slip shape itself).
hideSlipEpochs         = true;

%% --- Load CSV file ---
% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
% (The gap-bridge section reads turbo_edit_trace.csv from the same folder.)
fname = gnssTracePath('mwwl_trace.csv');
if ~isfile(fname)
    error('mwwl CSV not found: %s', fname);
end
T = readtable(fname, 'TextType', 'string');

% Required columns -- abort early with a useful message if the CSV
% schema drifts from what the C++ logger writes.
need = ["week","tow_s","satellite","system","sig1","sig2", ...
        "mw_cycles","dev_from_mean_cycles","mw_sigma_cycles", ...
        "threshold_cycles"];
miss = need(~ismember(need, string(T.Properties.VariableNames)));
if ~isempty(miss)
    error('mwwl_trace.csv missing columns: %s', strjoin(miss, ', '));
end

% Coerce numerics in case readtable picked the wrong type.
T.week                 = double(T.week);
T.tow_s                = double(T.tow_s);
T.mw_cycles            = double(T.mw_cycles);
T.dev_from_mean_cycles = double(T.dev_from_mean_cycles);
T.mw_sigma_cycles      = double(T.mw_sigma_cycles);
T.threshold_cycles     = double(T.threshold_cycles);
% Optional columns consumed by the slip-example figures; coerce when
% present (guarded for back-compat with older traces).
for optCol = ["is_slip","is_lli","arc_count","wl_locked","rounded_wl","mw_mean_cycles"]
    if ismember(optCol, string(T.Properties.VariableNames))
        T.(optCol) = double(T.(optCol));
    end
end
if ismember('is_slip', string(T.Properties.VariableNames))
    T.is_slip(isnan(T.is_slip)) = 0;
end

% NOTE: the hideSlipEpochs filter has moved below the derived-column
% sections so that Tall (the pre-filter snapshot feeding the slip-example
% figures) carries every derived column on the FULL row set.

%% --- Signal-pair composition ---
% The MW WL "signal" axis is the (sig1, sig2) pair -- e.g. L1C/L2W,
% L1C/L5Q -- since a single MW value lives on one specific pair.  Same
% sat can contribute multiple pairs simultaneously, so we treat the
% pair string as the per-tile partition key just like the single signal
% was in measurement_residuals.m.
T.signal = T.sig1 + "/" + T.sig2;

%% --- GPS time axis (trace files carry pure GPS time) ---
gpsEpoch  = datetime(1980, 1, 6, 0, 0, 0);
T.utc     = gpsEpoch + seconds(T.week * 7 * 86400 + T.tow_s);

%% --- Categoricals for filtering ---
T.system    = categorical(string(T.system));
T.satellite = categorical(string(T.satellite));

%% --- Z-score column (synthesized) ---
% dev / sigma in units of sigma -- exceeds ~5-7 at a real slip, stays
% in [-3, 3] for typical MW noise.  Guard against divide-by-zero arcs
% (first-epoch sigma can be 0 before any deviation accumulates).
% NOTE: this is an APPROXIMATION of the detector's gate -- it omits the
% kMadSigmaMultiplier constant, the kMinSigmaMeters floor on sigma, and
% diverges from the detector during the pre-MAD warmup where the
% threshold is a fixed-cycles value rather than a sigma multiple.
% Useful as a noise gauge; for the exact detector gate use the
% threshold-ratio plot below.
T.mw_zscore = T.dev_from_mean_cycles ./ T.mw_sigma_cycles;
T.mw_zscore(~isfinite(T.mw_zscore)) = NaN;

%% --- Detection-reference columns (June 2026 schema) ---
% local_ref_cycles is the median of the per-arc causal raw-MW window; it
% is emitted in EVERY detection mode (in legacy hatch_mean runs it shows
% what the local reference WOULD have been -- a free offline A/B).
% detect_mode tags the statistic that governed each row's C++ slip
% decision: 0 = Hatch-mean level dev, 1 = local-median level dev (also
% the synthesized backstop rows), 2 = first time-difference, 3 = second
% time-difference.
%
% dev_active_cycles is the deviation the detector actually gated.  The
% newest schema writes it directly (active_dev_cycles column -- the rows
% are self-describing, |active_dev|/threshold > 1 iff is_slip in every
% mode, init/LLI rows carry NaN and fall back to dev_from_mean).  For
% intermediate traces that have detect_mode but not active_dev it is
% reconstructed: local (mw - local_ref) on detect_mode = 1 rows except
% the level-disagreement backstop rows, whose SYNTHESIZED dev_from_mean
% (the hatch-vs-local level gap, vs the 0.4-cycle drift bound) must be
% kept; TD rows (detect_mode 2/3) cannot be reconstructed row-wise and
% only appear in schemas that already carry active_dev.  All guarded for
% back-compat with traces that predate these columns.
hasLocalRef = all(ismember(["local_ref_cycles","detect_mode"], ...
                           string(T.Properties.VariableNames)));
hasActiveDev = ismember("active_dev_cycles", string(T.Properties.VariableNames));
if hasLocalRef
    T.local_ref_cycles = double(T.local_ref_cycles);
    T.detect_mode      = double(T.detect_mode);
    T.detect_mode(isnan(T.detect_mode)) = 0;
    if ismember('local_sigma_cycles', T.Properties.VariableNames)
        T.local_sigma_cycles = double(T.local_sigma_cycles);
    end
    T.dev_local_cycles = T.mw_cycles - T.local_ref_cycles;
end
if hasActiveDev
    T.active_dev_cycles = double(T.active_dev_cycles);
    T.dev_active_cycles = T.active_dev_cycles;
    nanRows = ~isfinite(T.dev_active_cycles);
    T.dev_active_cycles(nanRows) = T.dev_from_mean_cycles(nanRows);
elseif hasLocalRef
    T.dev_active_cycles = T.dev_from_mean_cycles;
    locRows = T.detect_mode == 1 & isfinite(T.dev_local_cycles);
    if ismember('is_slip', string(T.Properties.VariableNames))
        backstopRows = logical(T.is_slip) & T.detect_mode == 1 ...
                       & abs(T.threshold_cycles - 0.4) < 1e-6;
        locRows = locRows & ~backstopRows;
    end
    T.dev_active_cycles(locRows) = T.dev_local_cycles(locRows);
else
    T.dev_active_cycles = T.dev_from_mean_cycles;
end
if hasLocalRef
    modeNames = ["hatch","local","td1","td2"];
    parts = strings(0);
    for m = 0:3
        n = nnz(T.detect_mode == m);
        if n > 0
            parts(end+1) = sprintf('%s=%d', modeNames(m+1), n); %#ok<SAGROW>
        end
    end
    fprintf('Detection-mode rows: %s (active_dev column: %d).\n', ...
            strjoin(parts, ', '), hasActiveDev);
end

%% --- Full-row snapshot, then optionally drop detector-flagged slip rows ---
% Tall keeps EVERY row (slips included) with all derived columns -- the
% slip-example figures below are built from it.  The grid figures and
% summary statistics then optionally hide rows the C++ MwSlipDetector
% already flagged is_slip=1: those events are already handled (post-reset
% the arc restarts cleanly) and can be hundreds of cycles in magnitude --
% one rogue slip epoch stretches the y-axis so far that the normal MW
% noise (typically << 1 cycle) collapses to a flat line at the origin.
% Set hideSlipEpochs=false to keep the slip spikes in the grid figures.
Tall = T;
if hideSlipEpochs && ismember('is_slip', string(T.Properties.VariableNames))
    slipMask  = logical(T.is_slip);
    nDropped  = nnz(slipMask);
    if nDropped > 0
        T = T(~slipMask, :);
        fprintf(['Dropped %d slip-flagged row(s) from mwwl_trace ' ...
                 '(toggle hideSlipEpochs=false to keep them).\n'], nDropped);
    end
end

%% --- Threshold-ratio column (synthesized, faithful to detector) ---
% dev / thresh, where thresh = threshold_cycles is the per-row threshold
% the C++ MwSlipDetector actually compared |dev| against
% (computeAdaptiveThreshold -- post-warmup it's kMadSigmaMultiplier x
% max(sigma_mad, kMinSigmaMeters); pre-warmup it's initial_threshold_cycles
% or 3 x sigma_pred under stochastic-aware mode).  By construction
% |y| > 1 corresponds exactly to the rows where is_slip = 1 was written.
% Uses the ACTIVE deviation so the plot stays detector-faithful in
% local_median mode (dividing the Hatch dev by a local-MAD threshold
% would mix the two statistics).
T.mw_threshold_ratio = T.dev_active_cycles ./ T.threshold_cycles;
T.mw_threshold_ratio(~isfinite(T.mw_threshold_ratio)) = NaN;

%% --- Colors ---
try
    addpath(genpath('C:\temp\gnss-qc\matlab\visualization'));
    colors = distinguishable_colors(1e3);
catch
    colors = lines(1e3);
end
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

%% --- Active constellations ---
systemList = categories(T.system);
systemsPresent = {};
for i = 1:numel(systemList)
    if any(T.system == systemList{i})
        systemsPresent{end+1} = systemList{i}; %#ok<SAGROW>
    end
end
if isempty(systemsPresent)
    error('No usable constellations in mwwl_trace.csv');
end

%% --- Figures ---
% Primary: deviation from arc mean (cycles).  Most useful single
% diagnostic -- slip episodes show as sharp steps, the noise floor as
% small zero-mean wiggles, and bias drift as a smooth ramp.
plotMwwlGrid(T, 'dev_from_mean_cycles', 'Res. (cyc.)', ...
    'MW WL Deviation by Signal Pair and Constellation', ...
    systemsPresent, colors, markerList, plotStyle, gapFactor, ...
    showLegend, 0);

if showZScorePlot
    plotMwwlGrid(T, 'mw_zscore', 'dev / \sigma_{arc}', ...
        'MW WL z-score by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, 0);
end

if showThresholdRatioPlot
    % Detector-faithful gate: |y| > 1 means is_slip = 1 in the CSV.
    % Red dashed reference lines mark the slip boundary at +/- 1.
    plotMwwlGrid(T, 'mw_threshold_ratio', 'dev / thresh_{C++}', ...
        'MW WL Detector Gate (dev / threshold) by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, [-1, 1]);
end

if showRawMwPlot
    plotMwwlGrid(T, 'mw_cycles', 'MW WL (cycles)', ...
        'MW WL Raw by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, []);
end

% Local-median deviation (cycles): the trend-immune statistic.  In a
% hatch_mean run this is the A/B counterfactual -- compare against the
% dev_from_mean figure to see how much of the legacy deviation was Hatch
% lag (smooth ramps vanish here; real steps survive in both).
if hasLocalRef
    plotMwwlGrid(T, 'dev_local_cycles', 'Res._{local} (cyc.)', ...
        'MW WL Local-Median Deviation by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, 0);
end

%% --- Example slip figures (one detected, one repaired) ---
% Built from Tall (the pre-hideSlipEpochs snapshot) so the slip rows are
% available even when the grid figures hide them.  Each figure zooms on a
% single event's (satellite, signal-pair) arc: raw MW with its reference
% levels on top, the active deviation against the +/- detector-threshold
% envelope below.
%
% "Detected" example: any is_slip=1 row qualifies.  Preference order for
% the clearest exhibit: in-arc detections (arc_count > 1) with |dev| <=
% 10 cycles (keeps the panel scale readable), then any in-arc detection,
% then any flagged row (init-epoch / backstop rows carry synthesized dev
% quantities).  Within the preferred pool the largest |dev|/threshold
% exceedance wins.
%
% "Repaired" example: the CSV does not record the downstream repair
% decision, so repair ELIGIBILITY is reconstructed from the detector's
% own unlocked-arc jump-estimate gate (MwSlipDetector::processMwSample):
%     wl_locked = 0  AND  arc_count > 1
%     AND |dev| >= 0.5 cyc  AND  |dev - round(dev)| < 0.25 cyc
% (dev = the ACTIVE deviation) -- the condition under which the
% detector publishes a known WL integer dN = round(dev) for downstream
% repair (CycleSlipManager::repairAmbiguityBssdAware / TurboEdit; note
% RT applies TurboEdit repairs only and baseline DD is reset-only, so
% eligibility here means "the detector offered an integer", not "the
% consumer applied it"; in the TimeDifference modes a further sigma gate
% 3*sigma_active <= 0.25*lambda applies in the C++, so this
% reconstruction can overcount there).  The wl_locked = 0 requirement is
% correct BY DESIGN for every publish path: locked-arc TD confirmations
% always emit unknown-integer slips (reset, never repair -- a deferred
% result is one epoch late and must not race the actions already taken
% at the flag epoch).  Backstop rows are excluded: they signal
% unknown-integer by construction and are identifiable as detect_mode=1
% rows whose threshold is exactly the 0.4-cycle drift bound.  Among the
% candidates the smallest fractional residual (most confident integer)
% is shown.
%
% Locked-arc TD confirmations appear as TWO flagged rows on the same arc
% (the flag epoch, then the action row one epoch later carrying the SAME
% firing gate quantities); an outlier-resolved flag leaves a single
% flagged row.  The inventory below collapses such adjacent pairs so a
% confirmed event is counted once.
if showSlipExampleFigs && ismember('is_slip', string(Tall.Properties.VariableNames))
    evt = Tall(logical(Tall.is_slip), :);
    if isempty(evt)
        fprintf('\nNo detected slips in trace -- skipping example slip figures.\n');
    else
        vn = string(evt.Properties.VariableNames);
        devEvt = evt.dev_active_cycles;
        ratio  = abs(devEvt) ./ evt.threshold_cycles;

        if ismember("arc_count", vn)
            inArc = evt.arc_count > 1;
        else
            inArc = true(height(evt), 1);
        end
        wlock = zeros(height(evt), 1);
        if ismember("wl_locked", vn)
            wlock = evt.wl_locked;
            wlock(isnan(wlock)) = 0;
        end
        isBackstop = false(height(evt), 1);
        if ismember("detect_mode", vn)
            isBackstop = evt.detect_mode == 1 ...
                         & abs(evt.threshold_cycles - 0.4) < 1e-6;
        end
        frac = abs(devEvt - round(devEvt));
        repairable = ~logical(wlock) & inArc & ~isBackstop ...
                     & abs(devEvt) >= 0.5 & frac < 0.25 & isfinite(devEvt);

        % Locked-arc TD confirmation pairs: the ACTION row is a flagged
        % locked TD-mode row (wl_locked=1, detect_mode 2/3) whose same
        % (satellite, signal) arc had another flagged locked TD-mode row
        % immediately before it (<= 2.5 s covers adjacent epochs at the
        % 1-10 Hz rates TD mode targets).  Collapse it into the flag row
        % for counting; a singleton locked TD flagged row is either an
        % outlier-resolved flag or an immediate pivot-frame reset.
        isLockedTd = logical(wlock);
        if ismember("detect_mode", vn)
            isLockedTd = isLockedTd & evt.detect_mode >= 2;
        end
        isConfirmAction = false(height(evt), 1);
        for e = 1:height(evt)
            if ~isLockedTd(e), continue; end
            sameArc = isLockedTd ...
                      & string(evt.satellite) == string(evt.satellite(e)) ...
                      & string(evt.signal)    == string(evt.signal(e));
            dt = seconds(evt.utc(e) - evt.utc);
            if any(sameArc & dt > 0 & dt <= 2.5)
                isConfirmAction(e) = true;
            end
        end

        % Console inventory of every flagged event (confirmation action
        % rows collapse into their flag row for the count).
        fprintf('\n=== Detected MW slip events: %d (repair-eligible: %d) ===\n', ...
                height(evt) - nnz(isConfirmAction), nnz(repairable));
        nShow = min(height(evt), 30);
        for e = 1:nShow
            tags = "";
            if ~inArc(e),           tags = tags + " init";          end
            if isBackstop(e),       tags = tags + " backstop";      end
            if isConfirmAction(e),  tags = tags + " confirm-action"; end
            if repairable(e)
                tags = tags + sprintf(" dN=%+d (frac %.3f)", round(devEvt(e)), frac(e));
            end
            fprintf('  %s  %s %-9s dev=%+9.3f cyc  thresh=%7.3f cyc%s\n', ...
                    string(evt.utc(e)), string(evt.satellite(e)), ...
                    string(evt.signal(e)), devEvt(e), ...
                    evt.threshold_cycles(e), tags);
        end
        if height(evt) > nShow
            fprintf('  ... (%d more)\n', height(evt) - nShow);
        end

        % Detected example: preference cascade for a readable exhibit.
        cand = find(inArc & abs(devEvt) <= 10 & isfinite(ratio));
        if isempty(cand), cand = find(inArc & isfinite(ratio)); end
        if isempty(cand), cand = find(isfinite(ratio)); end
        if ~isempty(cand)
            [~, k] = max(ratio(cand));
            plotSlipExample(Tall, evt(cand(k), :), ...
                'Detected MW cycle slip (example)', NaN, hasLocalRef, gapFactor);
        end

        % Repaired example: most confident integer among eligible events.
        candR = find(repairable);
        if isempty(candR)
            fprintf(['No repair-eligible slips (known-integer gate) in trace -- ' ...
                     'skipping repaired-slip example figure.\n']);
        else
            [~, k] = min(frac(candR));
            pickR = candR(k);
            plotSlipExample(Tall, evt(pickR, :), ...
                'Repaired MW cycle slip (example)', ...
                round(devEvt(pickR)), hasLocalRef, gapFactor);
        end
    end
end

%% --- Gap-bridge adjudication figures (TurboEdit across-gap events) ---
% Joins this MW trace with turbo_edit_trace.csv (same logs directory) to
% exhibit the gap-spanning GF bridge: a tracking outage breaks the TD2
% chain, the pre-gap GF level is stashed, and the post-gap MW flag --
% which on a wl-locked arc arrives one-plus epochs LATE (TD-confirmation
% deferral), after the chain has rebuilt and its own TD2 has gone flat
% and gap-blind -- is adjudicated against the drift-corrected ACROSS-GAP
% jump instead.  The June 2026 G03 field case (+28*lambda2 ~ +6.84 m over
% a 2 s dropout, flag at gap+3) was misclassified as a code event by the
% flat post-gap TD2 before the late-flag re-route existed; this section
% makes that timing visible:
%   Panel 1: raw MW around the gap with the slip-flag epochs starred --
%            shows WHEN the MW evidence actually landed.
%   Panel 2: GF level (m) -- the pre-gap level, the across-gap jump the
%            chain never sees as a TD2, and (for repair/moot rows) the
%            level predicted by the resolved integers overlaid on the
%            post-gap data.
%   Panel 3: TD2 / across-gap jump vs the +/- GF gate -- bridged samples
%            (squares) carry the across-gap jump; chain-owned samples sit
%            flat inside the envelope, which is exactly the blindness the
%            bridge closes.
% Bridged rows are identified by the trailing `bridged` column (newer
% traces); the whole section degrades gracefully on older schemas.
if showGapBridgeFigs
    teFile = fullfile(fileparts(fname), 'turbo_edit_trace.csv');
    if ~isfile(teFile)
        fprintf('\nNo turbo_edit_trace.csv beside the MW trace -- skipping gap-bridge figures.\n');
    else
        TE = readtable(teFile, 'TextType', 'string');
        vnTe = string(TE.Properties.VariableNames);
        needTe = ["week","tow_s","satellite","sig1","sig2","gf_m", ...
                  "td2_gf_m","thresh_m","action"];
        if any(~ismember(needTe, vnTe)) || ~ismember("bridged", vnTe)
            fprintf(['\nturbo_edit_trace.csv schema lacks the gap-bridge ' ...
                     'columns (need %s + bridged) -- skipping gap-bridge ' ...
                     'figures.\n'], strjoin(needTe, ', '));
        else
            for c = ["week","tow_s","gf_m","td2_gf_m","thresh_m","bridged"]
                TE.(c) = double(TE.(c));
            end
            for c = ["lambda1_m","lambda2_m","dN1","dN2","dN_wl","frac"]
                if ismember(c, vnTe), TE.(c) = double(TE.(c)); end
            end
            TE.bridged(isnan(TE.bridged)) = 0;
            TE.utc    = gpsEpoch + seconds(TE.week * 7 * 86400 + TE.tow_s);
            TE.signal = TE.sig1 + "/" + TE.sig2;

            % An adjudication is a bridged row the resolver actioned
            % (repair / moot / code_event / reset).  Un-actioned bridged
            % rows (no MW evidence yet -- the stash waiting out the MW
            % TD-confirmation lag) are kept for the panels but are not
            % exhibit anchors.
            bridgedRows  = TE.bridged > 0;
            adjudicated  = bridgedRows & TE.action ~= "none";
            if ~any(bridgedRows)
                fprintf('\nNo bridged gap-bridge rows in turbo_edit_trace -- skipping gap-bridge figures.\n');
            else
                % Console inventory of every bridged adjudication.
                evIdx = find(adjudicated);
                fprintf('\n=== TurboEdit gap-bridge adjudications: %d (bridged rows total: %d) ===\n', ...
                        numel(evIdx), nnz(bridgedRows));
                for e = evIdx(:)'
                    intsTxt = "";
                    if all(ismember(["dN1","dN2"], vnTe)) && ...
                       (TE.dN1(e) ~= 0 || TE.dN2(e) ~= 0)
                        intsTxt = sprintf('  (dN1=%+d, dN2=%+d)', TE.dN1(e), TE.dN2(e));
                    end
                    fprintf('  %s  %s %-9s dGF=%+8.4f m  gate=%6.4f m  %s%s\n', ...
                            string(TE.utc(e)), string(TE.satellite(e)), ...
                            string(TE.signal(e)), TE.td2_gf_m(e), ...
                            TE.thresh_m(e), string(TE.action(e)), intsTxt);
                end

                % Exhibit anchors: actioned bridged rows, repairs first
                % (the effect under study), then moot / code_event /
                % reset; fall back to the largest un-actioned bridged
                % jump when nothing was adjudicated this run.
                actionRank = @(a) 1 * (a == "repair") + 2 * (a == "moot") ...
                            + 3 * (a == "code_event") + 4 * (a == "reset");
                if ~isempty(evIdx)
                    [~, ord] = sortrows([actionRank(TE.action(evIdx)), ...
                                         -abs(TE.td2_gf_m(evIdx))]);
                    picks = evIdx(ord);
                else
                    bIdx = find(bridgedRows);
                    [~, ord] = sort(-abs(TE.td2_gf_m(bIdx)));
                    picks = bIdx(ord);
                end
                % One exhibit per (satellite, pair) event cluster: bridged
                % rows of the same arc within +/- 30 s collapse into the
                % first-picked (best) row.
                shown = struct('sat', {}, 'sig', {}, 'utc', {});
                nFig = 0;
                for p = picks(:)'
                    dup = false;
                    for s = 1:numel(shown)
                        if shown(s).sat == string(TE.satellite(p)) && ...
                           shown(s).sig == string(TE.signal(p)) && ...
                           abs(seconds(TE.utc(p) - shown(s).utc)) <= 30
                            dup = true; break;
                        end
                    end
                    if dup, continue; end
                    nFig = nFig + 1;
                    shown(nFig) = struct('sat', string(TE.satellite(p)), ...
                                         'sig', string(TE.signal(p)), ...
                                         'utc', TE.utc(p)); %#ok<SAGROW>
                    plotGapBridgeExample(TE, Tall, TE(p, :), gapFactor);
                    if nFig >= maxGapBridgeFigs, break; end
                end
            end
        end
    end
end

%% --- Summary statistics ---
fprintf('\n=== MW WL Statistics by System and Signal Pair ===\n\n');
for i = 1:numel(systemsPresent)
    sysName = systemsPresent{i};
    sigList = sort(unique(string(T.signal(T.system == sysName))));
    for j = 1:numel(sigList)
        sig = sigList(j);
        idx = T.system == sysName & string(T.signal) == sig;
        if any(idx)
            dev = T.dev_from_mean_cycles(idx);
            dev = dev(isfinite(dev));
            if isempty(dev), continue; end
            fprintf('%s %s: N=%d, |dev| mean=%.3f, std=%.3f, max=%.3f cycles\n', ...
                sysName, sig, numel(dev), mean(abs(dev)), std(dev), max(abs(dev)));
        end
    end
end

%% --- Local functions ---
function plotMwwlGrid(T, valueColumn, valueLabel, figName, ...
                      systemsPresent, colors, markerList, plotStyle, ...
                      gapFactor, showLegend, refLines)
% PLOTMWWLGRID  One figure: columns = constellations, rows within each
% column = that constellation's own (sig1/sig2) pairs (sorted).  Mirrors
% plotResidualGrid in measurement_residuals.m: outer tiledlayout(1, nCons)
% with one inner tiledlayout(nSigPairs, 1) per constellation, so
% constellations with more or fewer wide-lane pairs share the figure
% without forcing blank tiles for the others.
%
% refLines: numeric vector of y-values at which to draw dashed reference
% lines (e.g. [0] for a zero line, [-1, 1] for the detector-gate
% boundary, [] for no reference lines).  Lines at y=0 are gray; all
% others are red to flag "this is a threshold".
    if isempty(T)
        fprintf('plotMwwlGrid: empty table, skipping figure ''%s''\n', figName);
        return;
    end

    nSysIn = numel(systemsPresent);

    % Per-constellation signal-pair list, lexicographically sorted so
    % L1C/L2W, L1C/L5Q, ... read top-to-bottom naturally.
    sigsPerCons = cell(1, nSysIn);
    for c = 1:nSysIn
        rows = T.system == systemsPresent{c};
        if any(rows)
            s = sort(unique(string(T.signal(rows))));
            sigsPerCons{c} = s(~ismissing(s) & s ~= "");
        end
    end
    activeCons = find(~cellfun(@isempty, sigsPerCons));
    if isempty(activeCons)
        fprintf('plotMwwlGrid: no populated (signal-pair, system) cells for ''%s''\n', figName);
        return;
    end
    sigsPerCons = sigsPerCons(activeCons);
    consActive  = systemsPresent(activeCons);
    nConsActive = numel(consActive);
    maxSigsPerCons = max(cellfun(@numel, sigsPerCons));

    figWidth  = max(900, 280 * nConsActive + 100);
    figHeight = max(450, 170 * maxSigsPerCons + 120);
    fig = figure('Name', figName, 'Color', 'w', ...
                 'Position', [50 50 figWidth figHeight]);
    fig.Position(4) = fig.Position(4) - 100;  % trim to fit on monitor
    outerTl = tiledlayout(1, nConsActive, 'TileSpacing', 'tight', 'Padding', 'compact');
    title(outerTl, figName);

    for c = 1:nConsActive
        sysName = consActive{c};
        sigs = sigsPerCons{c};
        nSigsForCons = numel(sigs);

        innerTl = tiledlayout(outerTl, nSigsForCons, 1, ...
                              'TileSpacing', 'tight', 'Padding', 'tight');
        innerTl.Layout.Tile = c;
        innerTl.Layout.TileSpan = [1 1];

        % Collect tile axes per column so the inner stack can share its
        % x-axis (only the bottom tile keeps tick labels) and zooming
        % stays synchronized across the column.
        columnAxes = gobjects(0);

        for r = 1:nSigsForCons
            sig = sigs(r);
            ax  = nexttile(innerTl);
            columnAxes(end+1) = ax; %#ok<AGROW>
            hold(ax, 'on');

            idx = T.system == sysName & string(T.signal) == sig;
            T_sub = T(idx, :);

            satsInSubset = categories(removecats(T_sub.satellite));
            for k = 1:numel(satsInSubset)
                sat = satsInSubset{k};
                idx_sat = (T_sub.satellite == sat);

                % Color by PRN (chars 2-3 of the sat ID); fall back to
                % the loop index if the parse fails so we never crash.
                colorRow = str2double(sat(2:3));
                if ~isfinite(colorRow), colorRow = k; end
                if colorRow > size(colors, 1)
                    colorRow = mod(colorRow - 1, size(colors, 1)) + 1;
                end

                if strcmpi(plotStyle, 'lines')
                    [tt, yy] = insertGapBreaks(T_sub.utc(idx_sat), ...
                        T_sub.(valueColumn)(idx_sat), gapFactor);
                    plot(ax, tt, yy, '-', ...
                         'Color', colors(colorRow, :), ...
                         'DisplayName', char(sat));
                else
                    scatter(ax, T_sub.utc(idx_sat), T_sub.(valueColumn)(idx_sat), ...
                            10, colors(colorRow, :), 'filled', ...
                            'DisplayName', char(sat));
                end
            end

            for yref = refLines(:)'
                if yref == 0
                    yline(ax, yref, '--', 'Color', [0.5 0.5 0.5], ...
                          'HandleVisibility', 'off');
                else
                    yline(ax, yref, '--r', 'HandleVisibility', 'off');
                end
            end

            % Per-tile mu/sigma annotation, top-right corner.
            vals = T_sub.(valueColumn);
            vals = vals(isfinite(vals));
            if ~isempty(vals)
                text(ax, 0.98, 0.97, ...
                    sprintf('\\mu=%.3g  \\sigma=%.3g', mean(vals), std(vals)), ...
                    'Units', 'normalized', ...
                    'HorizontalAlignment', 'right', ...
                    'VerticalAlignment', 'top', ...
                    'FontSize', 8, ...
                    'BackgroundColor', 'w', ...
                    'EdgeColor', [0.7 0.7 0.7], ...
                    'Margin', 1);
            end

            grid(ax, 'on');
            title(ax, sprintf('%s - %s', sysName, sig));
            % Axis labels only on the outer edges of each column.  All
            % tiles in this column share the same UTC range, so non-
            % bottom tiles also drop their x-tick LABELS (the bottom
            % tile's labels apply to them via the linkaxes call below).
            if r == nSigsForCons
                xlabel(ax, 'GPS Time');
            else
                ax.XTickLabel = [];
            end
            if c == 1, ylabel(ax, valueLabel); end
            % Legend on the top tile of each column (one entry per
            % satellite in that constellation), gated by showLegend.
            if showLegend && r == 1
                legend(ax, 'Location', 'bestoutside', 'FontSize', 7);
            end

            hold(ax, 'off');
        end

        % Synchronize x-limits across the column so zooming/panning
        % anywhere updates every tile in this constellation's stack.
        if numel(columnAxes) > 1
            linkaxes(columnAxes, 'x');
        end
    end
end

function plotSlipExample(Tall, ev, figName, dN, hasLocalRef, gapFactor)
% PLOTSLIPEXAMPLE  Two-panel zoom on a single slip event.
%
% Top panel: the event arc's raw MW (cycles) with its reference levels
% overlaid -- the Hatch mean and (when traced) the local median.  A real
% slip shows as a step in the raw series that the laggy references take
% epochs to follow.  When dN is finite (repaired-slip exhibit) the
% post-event raw MW is re-plotted with dN removed: the "repaired" series
% should sit flush with the pre-slip level, which is precisely the arc
% continuity the downstream ambiguity shift restores.
%
% Bottom panel: the ACTIVE deviation (the quantity the C++ detector
% gated) against the +/- per-row threshold envelope; the event sample
% pierces the envelope.  For the repaired exhibit, dotted lines mark the
% published integer dN and the dN +/- 0.25-cycle rounding band of the
% repair-confidence gate -- the event sample landing inside the band is
% what made the integer publishable.
%
% Tall : full (pre-hideSlipEpochs) trace table.
% ev   : single-row table -- the event row.
% dN   : known WL repair integer, or NaN for a detection-only exhibit.
    halfWin = 60;   % samples either side of the event on this arc's series

    sat = string(ev.satellite);
    sig = string(ev.signal);
    t0  = ev.utc;

    rows = string(Tall.satellite) == sat & string(Tall.signal) == sig;
    S = sortrows(Tall(rows, :), 'utc');
    i0 = find(S.utc == t0 & logical(S.is_slip), 1);
    if isempty(i0), i0 = find(S.utc == t0, 1); end
    if isempty(i0)
        fprintf('plotSlipExample: event row not found for %s %s -- skipping.\n', sat, sig);
        return;
    end
    lo = max(1, i0 - halfWin);
    hi = min(height(S), i0 + halfWin);
    W  = S(lo:hi, :);
    iEv = i0 - lo + 1;

    fig = figure('Name', figName, 'Color', 'w', 'Position', [80 80 920 640]); %#ok<NASGU>
    tl = tiledlayout(2, 1, 'TileSpacing', 'tight', 'Padding', 'compact');
    if isfinite(dN)
        evtDesc = sprintf('dN_{WL} = %+d cyc', dN);
    else
        evtDesc = 'unknown integer';
    end
    title(tl, sprintf('%s -- %s %s @ %s (%s)', ...
          figName, sat, sig, string(t0), evtDesc), 'Interpreter', 'tex');

    % --- Panel 1: raw MW + reference levels ---
    ax1 = nexttile(tl);
    hold(ax1, 'on'); grid(ax1, 'on');
    [tt, yy] = insertGapBreaks(W.utc, W.mw_cycles, gapFactor);
    plot(ax1, tt, yy, '-o', 'Color', [0 0.35 0.75], ...
         'MarkerSize', 3, 'LineWidth', 0.8, 'DisplayName', 'MW raw');
    if ismember('mw_mean_cycles', string(W.Properties.VariableNames))
        [tt, yy] = insertGapBreaks(W.utc, W.mw_mean_cycles, gapFactor);
        plot(ax1, tt, yy, '-', 'Color', [0.85 0.55 0], ...
             'LineWidth', 1.1, 'DisplayName', 'Hatch mean');
    end
    if hasLocalRef
        [tt, yy] = insertGapBreaks(W.utc, W.local_ref_cycles, gapFactor);
        plot(ax1, tt, yy, '-', 'Color', [0.2 0.6 0.2], ...
             'LineWidth', 1.1, 'DisplayName', 'local median');
    end
    if isfinite(dN) && dN ~= 0
        post = W.utc >= t0;
        [tt, yy] = insertGapBreaks(W.utc(post), W.mw_cycles(post) - dN, gapFactor);
        plot(ax1, tt, yy, '--', ...
             'Color', [0.75 0 0.1], 'LineWidth', 1.2, ...
             'DisplayName', sprintf('repaired: MW - (%+d)', dN));
    end
    xline(ax1, t0, ':k', 'HandleVisibility', 'off');
    plot(ax1, W.utc(iEv), W.mw_cycles(iEv), 'kp', 'MarkerSize', 11, ...
         'MarkerFaceColor', [1 0.85 0], 'DisplayName', 'slip epoch');
    ylabel(ax1, 'MW WL (cycles)');
    title(ax1, 'Raw wide-lane and reference levels');
    legend(ax1, 'Location', 'best', 'FontSize', 8);
    ax1.XTickLabel = [];

    % --- Panel 2: active deviation vs detector gate ---
    ax2 = nexttile(tl);
    hold(ax2, 'on'); grid(ax2, 'on');
    [tt, yy] = insertGapBreaks(W.utc, W.dev_active_cycles, gapFactor);
    plot(ax2, tt, yy, '-o', 'Color', [0 0.35 0.75], ...
         'MarkerSize', 3, 'LineWidth', 0.8, 'DisplayName', 'dev_{active}');
    [tt, yy] = insertGapBreaks(W.utc, W.threshold_cycles, gapFactor);
    plot(ax2, tt, yy, '--r', 'LineWidth', 0.9, ...
         'DisplayName', '\pm threshold');
    [tt, yy] = insertGapBreaks(W.utc, -W.threshold_cycles, gapFactor);
    plot(ax2, tt, yy, '--r', 'LineWidth', 0.9, ...
         'HandleVisibility', 'off');
    if isfinite(dN)
        yline(ax2, dN, ':', sprintf('dN = %+d', dN), 'Color', [0.4 0.4 0.4], ...
              'LabelHorizontalAlignment', 'left', 'HandleVisibility', 'off');
        yline(ax2, dN + 0.25, ':', 'Color', [0.7 0.7 0.7], 'HandleVisibility', 'off');
        yline(ax2, dN - 0.25, ':', 'Color', [0.7 0.7 0.7], 'HandleVisibility', 'off');
    end
    xline(ax2, t0, ':k', 'HandleVisibility', 'off');
    plot(ax2, W.utc(iEv), W.dev_active_cycles(iEv), 'kp', 'MarkerSize', 11, ...
         'MarkerFaceColor', [1 0.85 0], 'DisplayName', 'slip epoch');
    text(ax2, 0.02, 0.95, ...
        sprintf('dev = %+.3f cyc   thresh = %.3f cyc   |dev|/thresh = %.1f', ...
                ev.dev_active_cycles, ev.threshold_cycles, ...
                abs(ev.dev_active_cycles) / ev.threshold_cycles), ...
        'Units', 'normalized', 'HorizontalAlignment', 'left', ...
        'VerticalAlignment', 'top', 'FontSize', 8, ...
        'BackgroundColor', 'w', 'EdgeColor', [0.7 0.7 0.7], 'Margin', 1);
    ylabel(ax2, 'dev_{active} (cycles)');
    xlabel(ax2, 'GPS Time');
    title(ax2, 'Active deviation vs detector gate');
    legend(ax2, 'Location', 'best', 'FontSize', 8);

    linkaxes([ax1 ax2], 'x');
end

function plotGapBridgeExample(TE, Tmw, ev, gapFactor)
% PLOTGAPBRIDGEEXAMPLE  Three-panel zoom on one TurboEdit gap-bridge event.
%
% Panel 1: raw MW (cycles) for the event's (satellite, pair) arc from the
% MW trace, gap break visible, slip-flag epochs starred -- exhibits the MW
% evidence TIMING: on a wl-locked arc the across-gap flag lands one-plus
% epochs after the signal returns (TD-confirmation deferral), i.e. often
% AFTER the GF chain has already rebuilt.
%
% Panel 2: GF level (m).  The pre-gap level (last sample before the
% bridged stretch) is drawn through the window; for repair/moot
% adjudications the level PREDICTED by the resolved integers
% (pre + lambda1*dN1 - lambda2*dN2) overlays the post-gap data -- a
% correct attribution sits flush on it.
%
% Panel 3: the gated quantity vs the +/- GF gate envelope.  On bridged
% samples (squares) td2_gf_m is the drift-corrected ACROSS-GAP jump; on
% chain-owned samples it is the ordinary TD2, which post-gap is flat and
% blind to the jump by construction -- the visual core of the effect.
%
% TE  : full turbo_edit_trace table (utc/signal columns synthesized).
% Tmw : full (pre-hideSlipEpochs) MW trace table.
% ev  : single-row table -- the bridged anchor row.
    halfWinS = 90;   % seconds either side of the event

    sat = string(ev.satellite);
    sig = string(ev.signal);
    t0  = ev.utc;
    vnTe = string(TE.Properties.VariableNames);

    rows = string(TE.satellite) == sat & string(TE.signal) == sig ...
           & abs(seconds(TE.utc - t0)) <= halfWinS;
    W = sortrows(TE(rows, :), 'utc');
    if isempty(W)
        fprintf('plotGapBridgeExample: no TE rows for %s %s -- skipping.\n', sat, sig);
        return;
    end
    isBr = W.bridged > 0;
    iEv  = find(W.utc == t0, 1);

    hasInts = all(ismember(["dN1","dN2","lambda1_m","lambda2_m"], vnTe));
    actTxt = string(ev.action);
    if hasInts && (ev.dN1 ~= 0 || ev.dN2 ~= 0)
        actTxt = actTxt + sprintf('  (dN1=%+d, dN2=%+d)', ev.dN1, ev.dN2);
    end

    fig = figure('Name', 'TurboEdit gap-bridge adjudication (example)', ...
                 'Color', 'w', 'Position', [100 60 940 760]); %#ok<NASGU>
    tl = tiledlayout(3, 1, 'TileSpacing', 'tight', 'Padding', 'compact');
    title(tl, sprintf('Gap-bridge adjudication -- %s %s @ %s -- %s', ...
          sat, sig, string(t0), actTxt), 'Interpreter', 'none');

    % --- Panel 1: raw MW + flag timing ---
    ax1 = nexttile(tl);
    hold(ax1, 'on'); grid(ax1, 'on');
    mwRows = string(Tmw.satellite) == sat & string(Tmw.signal) == sig ...
             & abs(seconds(Tmw.utc - t0)) <= halfWinS;
    M = sortrows(Tmw(mwRows, :), 'utc');
    if isempty(M)
        text(ax1, 0.5, 0.5, 'no MW rows in window', 'Units', 'normalized', ...
             'HorizontalAlignment', 'center');
    else
        [tt, yy] = insertGapBreaks(M.utc, M.mw_cycles, gapFactor);
        plot(ax1, tt, yy, '-o', 'Color', [0 0.35 0.75], 'MarkerSize', 3, ...
             'LineWidth', 0.8, 'DisplayName', 'MW raw');
        if ismember('mw_mean_cycles', string(M.Properties.VariableNames))
            [tt, yy] = insertGapBreaks(M.utc, M.mw_mean_cycles, gapFactor);
            plot(ax1, tt, yy, '-', 'Color', [0.85 0.55 0], ...
                 'LineWidth', 1.1, 'DisplayName', 'Hatch mean');
        end
        if ismember('is_slip', string(M.Properties.VariableNames))
            fl = logical(M.is_slip);
            if any(fl)
                plot(ax1, M.utc(fl), M.mw_cycles(fl), 'rp', 'MarkerSize', 11, ...
                     'MarkerFaceColor', [1 0.3 0.3], 'DisplayName', 'MW flag');
            end
        end
    end
    xline(ax1, t0, ':k', 'HandleVisibility', 'off');
    ylabel(ax1, 'MW WL (cycles)');
    title(ax1, 'Raw wide-lane: when the MW evidence landed');
    legend(ax1, 'Location', 'best', 'FontSize', 8);
    ax1.XTickLabel = [];

    % --- Panel 2: GF level, pre-gap reference, predicted post level ---
    ax2 = nexttile(tl);
    hold(ax2, 'on'); grid(ax2, 'on');
    [tt, yy] = insertGapBreaks(W.utc, W.gf_m, gapFactor);
    plot(ax2, tt, yy, '-o', 'Color', [0 0.35 0.75], 'MarkerSize', 3, ...
         'LineWidth', 0.8, 'DisplayName', 'GF level');
    if any(isBr)
        plot(ax2, W.utc(isBr), W.gf_m(isBr), 'ms', 'MarkerSize', 8, ...
             'LineWidth', 1.2, 'DisplayName', 'bridged epoch');
    end
    % Pre-gap level: last sample strictly before the first bridged epoch
    % (the stash's reference); without bridged rows in the window, the
    % last sample before the largest time gap.
    preIdx = [];
    if any(isBr)
        firstBr = find(isBr, 1);
        if firstBr > 1, preIdx = firstBr - 1; end
    elseif height(W) > 2
        [~, g] = max(seconds(diff(W.utc)));
        preIdx = g;
    end
    if ~isempty(preIdx)
        preLvl = W.gf_m(preIdx);
        yline(ax2, preLvl, '--', 'pre-gap level', 'Color', [0.45 0.45 0.45], ...
              'LabelHorizontalAlignment', 'left', 'HandleVisibility', 'off');
        if hasInts && (ev.dN1 ~= 0 || ev.dN2 ~= 0)
            predLvl = preLvl + ev.lambda1_m * ev.dN1 - ev.lambda2_m * ev.dN2;
            yline(ax2, predLvl, '--r', ...
                  sprintf('pre + \\lambda_1dN_1 - \\lambda_2dN_2 = %.3f m', predLvl), ...
                  'LabelHorizontalAlignment', 'left', 'HandleVisibility', 'off');
        end
    end
    if ~isempty(iEv)
        plot(ax2, W.utc(iEv), W.gf_m(iEv), 'kp', 'MarkerSize', 12, ...
             'MarkerFaceColor', [1 0.85 0], 'DisplayName', 'adjudication epoch');
    end
    xline(ax2, t0, ':k', 'HandleVisibility', 'off');
    ylabel(ax2, 'GF = \lambda_1L_1 - \lambda_2L_2 (m)');
    title(ax2, 'GF level: the across-gap jump the rebuilt TD2 chain cannot see');
    legend(ax2, 'Location', 'best', 'FontSize', 8);
    ax2.XTickLabel = [];

    % --- Panel 3: gated quantity vs GF gate ---
    ax3 = nexttile(tl);
    hold(ax3, 'on'); grid(ax3, 'on');
    chain = ~isBr;
    if any(chain)
        [tt, yy] = insertGapBreaks(W.utc(chain), W.td2_gf_m(chain), gapFactor);
        plot(ax3, tt, yy, '-o', 'Color', [0 0.35 0.75], 'MarkerSize', 3, ...
             'LineWidth', 0.8, 'DisplayName', 'chain TD2');
    end
    if any(isBr)
        stem(ax3, W.utc(isBr), W.td2_gf_m(isBr), 'm', 'filled', ...
             'Marker', 's', 'DisplayName', 'across-gap \DeltaGF');
    end
    [tt, yy] = insertGapBreaks(W.utc, W.thresh_m, gapFactor);
    plot(ax3, tt, yy, '--r', 'LineWidth', 0.9, 'DisplayName', '\pm GF gate');
    [tt, yy] = insertGapBreaks(W.utc, -W.thresh_m, gapFactor);
    plot(ax3, tt, yy, '--r', 'LineWidth', 0.9, 'HandleVisibility', 'off');
    xline(ax3, t0, ':k', 'HandleVisibility', 'off');
    if ~isempty(iEv)
        fracTxt = "";
        if ismember("frac", vnTe) && isfinite(ev.frac)
            fracTxt = sprintf('   frac = %.3f', ev.frac);
        end
        text(ax3, 0.02, 0.95, ...
            sprintf('action = %s   dGF = %+.4f m   gate = %.4f m%s', ...
                    string(ev.action), ev.td2_gf_m, ev.thresh_m, fracTxt), ...
            'Units', 'normalized', 'HorizontalAlignment', 'left', ...
            'VerticalAlignment', 'top', 'FontSize', 8, ...
            'BackgroundColor', 'w', 'EdgeColor', [0.7 0.7 0.7], 'Margin', 1);
    end
    ylabel(ax3, 'TD2 / \DeltaGF_{gap} (m)');
    xlabel(ax3, 'GPS Time');
    title(ax3, 'Gated quantity vs GF gate: flat chain TD2, exceeding across-gap jump');
    legend(ax3, 'Location', 'best', 'FontSize', 8);

    linkaxes([ax1 ax2 ax3], 'x');
end

function [tOut, yOut] = insertGapBreaks(tIn, yIn, gapFactor)
% INSERTGAPBREAKS  Insert NaT/NaN rows where the time series has a gap
% larger than gapFactor x median(dt), so plot() draws a break instead
% of a line crossing the gap.
    tIn = tIn(:);
    yIn = yIn(:);
    if numel(tIn) < 2
        tOut = tIn;
        yOut = yIn;
        return;
    end
    [tSorted, idx] = sort(tIn);
    ySorted = yIn(idx);
    dtSec = seconds(diff(tSorted));
    medDt = median(dtSec);
    if ~isfinite(medDt) || medDt <= 0, medDt = 1; end
    gapIdx = find(dtSec > gapFactor * medDt);
    if isempty(gapIdx)
        tOut = tSorted;
        yOut = ySorted;
        return;
    end
    nOut = numel(tSorted) + numel(gapIdx);
    tOut = NaT(nOut, 1);
    if ~isempty(tSorted.TimeZone), tOut.TimeZone = tSorted.TimeZone; end
    yOut = NaN(nOut, 1);
    w = 1;
    rStart = 1;
    for g = 1:numel(gapIdx)
        gIdx = gapIdx(g);
        chunkLen = gIdx - rStart + 1;
        tOut(w:w+chunkLen-1) = tSorted(rStart:gIdx);
        yOut(w:w+chunkLen-1) = ySorted(rStart:gIdx);
        w = w + chunkLen + 1;  % leave a NaT/NaN row at the gap
        rStart = gIdx + 1;
    end
    chunkLen = numel(tSorted) - rStart + 1;
    if chunkLen > 0
        tOut(w:w+chunkLen-1) = tSorted(rStart:end);
        yOut(w:w+chunkLen-1) = ySorted(rStart:end);
    end
end
