%% ============================================================
%  Script: view_turbo_edit_trace.m
%  Purpose: Plot TurboEdit (MW+GF joint slip resolver) traces for
%           cycle-slip repair diagnostics.  Mirrors the look of
%           view_mwwl_trace.m: per-constellation columns, signal pairs
%           stacked within each column, one colored line per satellite,
%           mu/sigma annotation in the top-right corner.
%
%  Row semantics (one row per mature (satellite, phase-pair) per epoch;
%  warm-up epochs -- the first TWO after any arc (re)start -- emit no
%  rows at all):
%    gf_m         geometry-free phase Phi1-Phi2 (m); per-arc level is
%                 ambiguity-dominated and arbitrary -- only steps matter.
%    td2_gf_m     uniform second time-difference of GF (m); the detector
%                 statistic.  A slip (dN1,dN2) spikes it by
%                 lambda1*dN1 - lambda2*dN2 at the slip epoch.
%    thresh_m     the GF-only gate, ALWAYS finite (June 2026 schema):
%                 mature arc -> max(k*MAD(arc TD2), 0.04 m floor); warm-up
%                 arc -> max(k*sigma_xsat, 0.04), where sigma_xsat is the
%                 cross-satellite TD2 noise scale resolved through the
%                 breadth cascade (constellation x band-pair) group ->
%                 constellation -> global non-GLONASS (zenith-normalized,
%                 EWMA-smoothed, elevation re-expanded -- the same breadth
%                 machinery as the MW detector's cold start); bare 0.04 m
%                 only at the true cold start (first ~2 epochs of a run)
%                 and for GLONASS when its own tiers are thin.  Older
%                 traces carry -1 during warm-up instead (mapped to NaN
%                 below).  On clean arcs the floor governs
%                 BY DESIGN: the smallest physically meaningful GF event
%                 is the dN1=dN2=+/-1 quantum |lambda1-lambda2| (~5.4 cm
%                 GPS L1/L2), so the adaptive terms exist to LIFT the
%                 gate on noisy arcs / disturbed iono, never to sharpen
%                 quiet ones below slip-physical scale.
%    thresh_unfloored_m  (June 2026 schema) the active adaptive term with
%                 NO floor: k*MAD(arc) when mature, k*sigma_xsat during
%                 warm-up; -1 only when neither exists (true cold start).
%                 Compare against the 0.04 floor to see the per-arc
%                 margin.  Guarded below for older traces.
%    ivi_m        per-constellation cross-satellite ionospheric
%                 variability index: median(|TD2|) + MAD-sigma(|TD2|).
%                 Above 0.06 m the iono is deemed active and GF jumps
%                 are not trusted (iono_stable = 0).
%    mw_flagged / gf_flagged   which detector(s) flagged this pair.
%    dN_wl / wl_known          the MW wide-lane integer input.
%    dN1 / dN2 / frac          the joint 2x2 solve output (per-signal
%                 integers and the rounding residual of the float dN2).
%    action       "repair" (covariance-preserving per-signal integer
%                 shift), "reset" (ambiguity reset), "code_event"
%                 (MW-only flag with affirmatively stable GF: classified
%                 as a code-domain artifact -- multipath / NLOS / DLL
%                 stress -- and the phase reset was SKIPPED; dN1/dN2
%                 record the implied-but-rejected null-line pair),
%                 "moot" (gap-bridge attribution onto a signal whose
%                 ambiguity was pruned-and-reseeded during the gap: the
%                 slip is already absorbed, mandate suppressed, nothing
%                 touched), or "none" (resolved to no slip, e.g. a
%                 GF-only false alarm).
%    bridged      (June 2026, optional) 1 when td2_gf_m is NOT a TD2 but
%                 the drift-corrected ACROSS-GAP GF jump from a stashed
%                 pre-gap level (gap-spanning bridge; thresh_m carries
%                 the sqrt(dt/dt0)-widened gate).  Guarded below for
%                 older traces.
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
showTd2Plot        = true;   % td2_gf_m vs time (primary statistic, meters)
showRatioPlot      = false;   % td2 / thresh -- |y|>1 iff gf_exceeds (see
                             % caveat: the cooldown can suppress the FLAG
                             % on an exceeding row for 3 epochs after a
                             % slip, so |y|>1 with gf_flagged=0 is the
                             % anti-echo cooldown, not an inconsistency)
showThresholdPlot  = false;   % thresh_unfloored_m vs time with the 0.04
                             % floor as a red reference -- shows how far
                             % under (quantum-limited) or over (noise-
                             % limited) the adaptive term sits per arc
showRawGfPlot      = false;  % raw gf_m vs time (per-arc level arbitrary)
showIviPlot        = true;   % per-constellation IVI vs time + 0.06 gate
showEventExamples  = true;   % zoomed two-panel example of one repair and
                             % one reset event (if present)
showLegend         = false;  % per-tile satellite legend on top tile

% Drop rows the resolver flagged (mw_flagged or gf_flagged) from the
% GRID figures so they autoscale on the in-arc noise instead of being
% dominated by multi-decimeter slip TD2s.  The event inventory and the
% example figures below are built from the pre-filter snapshot (Tall),
% so nothing is lost.  Set false to keep slip spikes visible.
hideFlaggedEpochs  = true;

%% --- Load CSV file ---
% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('turbo_edit_trace.csv');
if ~isfile(fname)
    error('TurboEdit CSV not found: %s', fname);
end
T = readtable(fname, 'TextType', 'string');

% Required columns -- abort early with a useful message if the CSV
% schema drifts from what the C++ logger writes.
need = ["week","tow_s","satellite","system","sig1","sig2", ...
        "gf_m","td2_gf_m","thresh_m","ivi_m","iono_stable", ...
        "mw_flagged","gf_flagged","dN_wl","wl_known", ...
        "dN1","dN2","frac","action"];
miss = need(~ismember(need, string(T.Properties.VariableNames)));
if ~isempty(miss)
    error('turbo_edit_trace.csv missing columns: %s', strjoin(miss, ', '));
end

% Coerce numerics in case readtable picked the wrong type.
for numCol = ["week","tow_s","elevation_deg","lambda1_m","lambda2_m", ...
              "gf_m","td2_gf_m","thresh_m","ivi_m","frac", ...
              "dN_wl","dN1","dN2"]
    if ismember(numCol, string(T.Properties.VariableNames))
        T.(numCol) = double(T.(numCol));
    end
end
for flagCol = ["iono_stable","mw_flagged","gf_flagged","wl_known"]
    T.(flagCol) = double(T.(flagCol));
    T.(flagCol)(isnan(T.(flagCol))) = 0;
end

% -1 is the C++ sentinel for "no threshold yet" (immature MAD window);
% thresholds are physically nonnegative, so map sentinels to NaN.
T.thresh_m(T.thresh_m < 0) = NaN;

% Optional column (June 2026 schema): the raw k*MAD with no floor.
hasUnfloored = ismember("thresh_unfloored_m", string(T.Properties.VariableNames));
if hasUnfloored
    T.thresh_unfloored_m = double(T.thresh_unfloored_m);
    T.thresh_unfloored_m(T.thresh_unfloored_m < 0) = NaN;
else
    fprintf(['Note: thresh_unfloored_m column absent (pre-June-2026 trace); ' ...
             'threshold-margin figure will be skipped.\n']);
end

% Optional column (June 2026 gap bridge): td2_gf_m on bridged rows is the
% drift-corrected ACROSS-GAP GF jump, not a TD2.
hasBridged = ismember("bridged", string(T.Properties.VariableNames));
if hasBridged
    T.bridged = double(T.bridged);
    T.bridged(isnan(T.bridged)) = 0;
else
    T.bridged = zeros(height(T), 1);
    fprintf('Note: bridged column absent (pre-gap-bridge trace); assuming 0.\n');
end

%% --- Signal-pair composition ---
% The GF "signal" axis is the (sig1, sig2) phase pair -- e.g. L1C/L2W --
% matching the MW detector's pair keys so the two trace viewers tile
% identically.
T.signal = T.sig1 + "/" + T.sig2;

%% --- GPS time axis (trace files carry pure GPS time) ---
gpsEpoch  = datetime(1980, 1, 6, 0, 0, 0);
T.utc     = gpsEpoch + seconds(T.week * 7 * 86400 + T.tow_s);

%% --- Categoricals for filtering ---
T.system    = categorical(string(T.system));
T.satellite = categorical(string(T.satellite));

%% --- Derived columns ---
% Detector-faithful gate ratio: |y| > 1 corresponds to gf_exceeds in the
% C++ (|TD2| > thresh).  gf_flagged additionally requires the anti-echo
% cooldown to have expired, so an |y|>1 row with gf_flagged=0 within ~3
% epochs of a slip is the cooldown working, not a logging bug.  NaN where
% the threshold is immature.
T.gf_ratio = T.td2_gf_m ./ T.thresh_m;
T.gf_ratio(~isfinite(T.gf_ratio)) = NaN;

%% --- Full-row snapshot, then optionally drop flagged rows ---
% Tall keeps EVERY row (flags included) -- the event inventory and the
% example figures are built from it.  The grid figures then optionally
% hide flagged rows so they autoscale on in-arc noise.
Tall = T;
if hideFlaggedEpochs
    flagMask = logical(T.mw_flagged) | logical(T.gf_flagged);
    nDropped = nnz(flagMask);
    if nDropped > 0
        T = T(~flagMask, :);
        fprintf(['Dropped %d flagged row(s) from turbo_edit_trace ' ...
                 '(toggle hideFlaggedEpochs=false to keep them).\n'], nDropped);
    end
end

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
    error('No usable constellations in turbo_edit_trace.csv');
end

%% --- Figures ---
if showTd2Plot
    plotTeGrid(T, 'td2_gf_m', 'TD2_{GF} (m)', ...
        'TurboEdit GF Second Time-Difference by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, 0);
end

if showRatioPlot
    plotTeGrid(T, 'gf_ratio', 'TD2 / thresh_{C++}', ...
        'TurboEdit GF Gate (TD2 / threshold) by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, [-1, 1]);
end

if showThresholdPlot && hasUnfloored
    % The 0.04 m reference is kMinThresholdM: where a satellite's line
    % sits BELOW it, the floor governs (quantum-limited regime -- normal
    % for clean arcs); where it climbs above, the arc is noise-limited
    % and the adaptive MAD is doing its job.
    plotTeGrid(T, 'thresh_unfloored_m', 'k\sigma\cdotMAD (m)', ...
        'TurboEdit Unfloored Threshold (vs 0.04 m floor) by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, 0.04);
end

if showRawGfPlot
    plotTeGrid(T, 'gf_m', 'GF (m)', ...
        'TurboEdit Raw Geometry-Free Phase by Signal Pair and Constellation', ...
        systemsPresent, colors, markerList, plotStyle, gapFactor, ...
        showLegend, []);
end

% IVI: cross-satellite per constellation, so one line per constellation
% on a single axes rather than the per-satellite grid.
if showIviPlot
    figure('Name', 'TurboEdit IVI', 'Color', 'w', 'Position', [80 80 900 360]);
    ax = axes; hold(ax, 'on'); grid(ax, 'on');
    for i = 1:numel(systemsPresent)
        sysName = systemsPresent{i};
        rows = Tall.system == sysName;
        % IVI is identical across a constellation's rows each epoch --
        % collapse to unique epochs for a clean line.
        sub = unique(Tall(rows, {'utc','ivi_m'}));
        [tt, yy] = insertGapBreaks(sub.utc, sub.ivi_m, gapFactor);
        plot(ax, tt, yy, '-', 'DisplayName', sysName, 'LineWidth', 1.0);
    end
    yline(ax, 0.06, '--r', 'IVI gate (0.06 m)', 'HandleVisibility', 'off');
    ylabel(ax, 'IVI (m)'); xlabel(ax, 'GPS Time');
    title(ax, 'Cross-satellite ionospheric variability index (median|TD2| + MAD\sigma|TD2|)');
    legend(ax, 'Location', 'best');
    hold(ax, 'off');
end

%% --- Event inventory (from the pre-filter snapshot) ---
evt = Tall(logical(Tall.mw_flagged) | logical(Tall.gf_flagged), :);
nRepair = nnz(evt.action == "repair");
nReset  = nnz(evt.action == "reset");
nCode   = nnz(evt.action == "code_event");
nMoot   = nnz(evt.action == "moot");
nNone   = nnz(evt.action == "none");
nOther  = height(evt) - nRepair - nReset - nCode - nMoot - nNone;
nJoint  = nnz(logical(evt.mw_flagged) & logical(evt.gf_flagged));
nMwOnly = nnz(logical(evt.mw_flagged) & ~logical(evt.gf_flagged));
nGfOnly = nnz(~logical(evt.mw_flagged) & logical(evt.gf_flagged));
nBridged = nnz(logical(evt.bridged));
fprintf(['\n=== TurboEdit flagged events: %d (repair: %d, reset: %d, ' ...
         'code_event: %d, moot: %d, none: %d; bridged: %d) ===\n'], ...
        height(evt), nRepair, nReset, nCode, nMoot, nNone, nBridged);
if nOther > 0
    fprintf('    WARNING: %d event(s) carry an unrecognized action value\n', nOther);
end
fprintf('    flag sources: joint MW+GF: %d, MW-only: %d, GF-only: %d\n', ...
        nJoint, nMwOnly, nGfOnly);
nShow = min(height(evt), 30);
for e = 1:nShow
    src = "";
    if evt.mw_flagged(e), src = src + "MW"; end
    if evt.gf_flagged(e)
        if strlength(src) > 0, src = src + "+"; end
        src = src + "GF";
    end
    fprintf(['  %s  %s %-9s %-5s td2=%+8.4f m  thresh=%7.4f m  ' ...
             'dN_wl=%+d(k=%d)  dN1=%+d dN2=%+d frac=%.3f  -> %s\n'], ...
            string(evt.utc(e)), string(evt.satellite(e)), ...
            string(evt.signal(e)), src, evt.td2_gf_m(e), evt.thresh_m(e), ...
            evt.dN_wl(e), evt.wl_known(e), evt.dN1(e), evt.dN2(e), ...
            evt.frac(e), evt.action(e));
end
if height(evt) > nShow
    fprintf('  ... (%d more)\n', height(evt) - nShow);
end

%% --- Example event figures (one repair, one reset) ---
% evt is populated by MW-only slips
if showEventExamples && ~isempty(evt)
    rep = evt(evt.action == "repair", :);
    if isempty(rep)
        fprintf('No repair events in trace -- skipping repair example figure.\n');
    else
        % Most confident integer split = smallest rounding residual.
        [~, k] = min(rep.frac);
        plotTeEventExample(Tall, rep(k, :), ...
            'TurboEdit repair (example)', true, gapFactor);
    end
    rst = evt(evt.action == "reset", :);
    if isempty(rst)
        fprintf('No reset events in trace -- skipping reset example figure.\n');
    else
        % Clearest exceedance among resets with a usable ratio.
        ratio = abs(rst.td2_gf_m ./ rst.thresh_m);
        ratio(~isfinite(ratio)) = -inf;
        [~, k] = max(ratio);
        plotTeEventExample(Tall, rst(k, :), ...
            'TurboEdit reset (example)', false, gapFactor);
    end
    cev = evt(evt.action == "code_event", :);
    if isempty(cev)
        fprintf('No code_event vetoes in trace -- skipping code-event example figure.\n');
    else
        % Stillest GF among the vetoes: the exemplar of "MW moved, phase
        % did not" (td2 smallest relative to the arc's gate).
        ratio = abs(cev.td2_gf_m ./ cev.thresh_m);
        ratio(~isfinite(ratio)) = inf;
        [~, k] = min(ratio);
        plotTeEventExample(Tall, cev(k, :), ...
            'TurboEdit code-domain event (example: MW flagged, GF stable, phase preserved)', ...
            false, gapFactor);
    end
end

%% --- Summary statistics ---
fprintf('\n=== TurboEdit GF Statistics by System and Signal Pair ===\n\n');
for i = 1:numel(systemsPresent)
    sysName = systemsPresent{i};
    sigList = sort(unique(string(Tall.signal(Tall.system == sysName))));
    for j = 1:numel(sigList)
        sig = sigList(j);
        idx = Tall.system == sysName & string(Tall.signal) == sig;
        if ~any(idx), continue; end
        td2 = Tall.td2_gf_m(idx); td2 = td2(isfinite(td2));
        th  = Tall.thresh_m(idx);
        nMature = nnz(isfinite(th));
        if hasUnfloored
            un = Tall.thresh_unfloored_m(idx);
            un = un(isfinite(un));
            pctFloor = 100 * nnz(un < 0.04) / max(numel(un), 1);
            unStr = sprintf('  median k\\sigmaMAD=%.4f m  floor-bound=%.0f%%', ...
                            median(un), pctFloor);
        else
            unStr = '';
        end
        fprintf('%s %s: N=%d (mature %d), |TD2| med=%.4f max=%.4f m%s\n', ...
                sysName, sig, nnz(idx), nMature, ...
                median(abs(td2)), max([abs(td2); 0]), unStr);
    end
end

%% --- Local functions ---
function plotTeGrid(T, valueColumn, valueLabel, figName, ...
                    systemsPresent, colors, markerList, plotStyle, ...
                    gapFactor, showLegend, refLines)
% PLOTTEGRID  One figure: columns = constellations, rows within each
% column = that constellation's own (sig1/sig2) pairs (sorted).  Mirrors
% plotMwwlGrid in view_mwwl_trace.m: outer tiledlayout(1, nCons) with one
% inner tiledlayout(nSigPairs, 1) per constellation.
%
% refLines: numeric vector of y-values at which to draw dashed reference
% lines (0 -> gray zero line; anything else -> red threshold line; []
% for none).
    if isempty(T)
        fprintf('plotTeGrid: empty table, skipping figure ''%s''\n', figName);
        return;
    end

    nSysIn = numel(systemsPresent);

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
        fprintf('plotTeGrid: no populated (signal-pair, system) cells for ''%s''\n', figName);
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
            if r == nSigsForCons
                xlabel(ax, 'GPS Time');
            else
                ax.XTickLabel = [];
            end
            if c == 1, ylabel(ax, valueLabel); end
            if showLegend && r == 1
                legend(ax, 'Location', 'bestoutside', 'FontSize', 7);
            end

            hold(ax, 'off');
        end

        if numel(columnAxes) > 1
            linkaxes(columnAxes, 'x');
        end
    end
end

function plotTeEventExample(Tall, ev, figName, isRepair, gapFactor)
% PLOTTEEVENTEXAMPLE  Two-panel zoom on a single TurboEdit event.
%
% Top panel: the event arc's raw GF (m).  A real slip shows as a step of
% lambda1*dN1 - lambda2*dN2.  For a repair exhibit the post-event GF is
% re-plotted with that step removed -- the "repaired" series sits flush
% with the pre-slip level, which is the arc continuity the per-signal
% integer shift restores.
%
% Bottom panel: TD2 against the +/- per-row threshold envelope, with the
% event sample starred and the solve quantities annotated.
    halfWin = 60;   % samples either side of the event on this arc's series

    sat = string(ev.satellite);
    sig = string(ev.signal);
    t0  = ev.utc;

    rows = string(Tall.satellite) == sat & string(Tall.signal) == sig;
    S = sortrows(Tall(rows, :), 'utc');
    i0 = find(S.utc == t0, 1);
    if isempty(i0)
        fprintf('plotTeEventExample: event row not found for %s %s -- skipping.\n', sat, sig);
        return;
    end
    lo = max(1, i0 - halfWin);
    hi = min(height(S), i0 + halfWin);
    W  = S(lo:hi, :);
    iEv = i0 - lo + 1;

    figure('Name', figName, 'Color', 'w', 'Position', [80 80 920 640]);
    tl = tiledlayout(2, 1, 'TileSpacing', 'tight', 'Padding', 'compact');
    title(tl, sprintf('%s -- %s %s @ %s (dN1=%+d, dN2=%+d, frac=%.3f)', ...
          figName, sat, sig, string(t0), ev.dN1, ev.dN2, ev.frac), ...
          'Interpreter', 'tex');

    % --- Panel 1: raw GF with optional repaired overlay ---
    ax1 = nexttile(tl);
    hold(ax1, 'on'); grid(ax1, 'on');
    [tt, yy] = insertGapBreaks(W.utc, W.gf_m, gapFactor);
    plot(ax1, tt, yy, '-o', 'Color', [0 0.35 0.75], ...
         'MarkerSize', 3, 'LineWidth', 0.8, 'DisplayName', 'GF raw');
    if isRepair && (ev.dN1 ~= 0 || ev.dN2 ~= 0)
        stepM = ev.lambda1_m * ev.dN1 - ev.lambda2_m * ev.dN2;
        post  = W.utc >= t0;
        [tt2, yy2] = insertGapBreaks(W.utc(post), W.gf_m(post) - stepM, gapFactor);
        plot(ax1, tt2, yy2, '--', 'Color', [0.75 0 0.1], 'LineWidth', 1.2, ...
             'DisplayName', sprintf('repaired: GF - (%+.4f m)', stepM));
    end
    xline(ax1, t0, ':k', 'HandleVisibility', 'off');
    plot(ax1, W.utc(iEv), W.gf_m(iEv), 'kp', 'MarkerSize', 11, ...
         'MarkerFaceColor', [1 0.85 0], 'DisplayName', 'event epoch');
    ylabel(ax1, 'GF (m)');
    title(ax1, 'Raw geometry-free phase');
    legend(ax1, 'Location', 'best', 'FontSize', 8);
    ax1.XTickLabel = [];

    % --- Panel 2: TD2 vs threshold envelope ---
    ax2 = nexttile(tl);
    hold(ax2, 'on'); grid(ax2, 'on');
    [tt, yy] = insertGapBreaks(W.utc, W.td2_gf_m, gapFactor);
    plot(ax2, tt, yy, '-o', 'Color', [0 0.35 0.75], ...
         'MarkerSize', 3, 'LineWidth', 0.8, 'DisplayName', 'TD2_{GF}');
    [tt, yy] = insertGapBreaks(W.utc, W.thresh_m, gapFactor);
    plot(ax2, tt, yy, '--r', 'LineWidth', 0.9, 'DisplayName', '\pm threshold');
    [tt, yy] = insertGapBreaks(W.utc, -W.thresh_m, gapFactor);
    plot(ax2, tt, yy, '--r', 'LineWidth', 0.9, 'HandleVisibility', 'off');
    xline(ax2, t0, ':k', 'HandleVisibility', 'off');
    plot(ax2, W.utc(iEv), W.td2_gf_m(iEv), 'kp', 'MarkerSize', 11, ...
         'MarkerFaceColor', [1 0.85 0], 'DisplayName', 'event epoch');
    src = "";
    if ev.mw_flagged, src = src + "MW"; end
    if ev.gf_flagged
        if strlength(src) > 0, src = src + "+"; end
        src = src + "GF";
    end
    text(ax2, 0.02, 0.95, ...
        sprintf('flag: %s   dN_{WL}=%+d (known=%d)   td2=%+.4f m   thresh=%.4f m   action: %s', ...
                src, ev.dN_wl, ev.wl_known, ev.td2_gf_m, ev.thresh_m, ev.action), ...
        'Units', 'normalized', 'HorizontalAlignment', 'left', ...
        'VerticalAlignment', 'top', 'FontSize', 8, ...
        'BackgroundColor', 'w', 'EdgeColor', [0.7 0.7 0.7], 'Margin', 1);
    ylabel(ax2, 'TD2_{GF} (m)');
    xlabel(ax2, 'GPS Time');
    title(ax2, 'GF second time-difference vs detector gate');
    legend(ax2, 'Location', 'best', 'FontSize', 8);

    linkaxes([ax1 ax2], 'x');
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
