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
showZScorePlot         = true;   % dev_from_mean / mw_sigma -- approximate
                                 % "sigmas off arc mean" gauge
showThresholdRatioPlot = true;   % dev_from_mean / threshold_cycles --
                                 % EXACT detector gate: |y|>1 iff is_slip=1
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
fname = 'C:\GnssProcessor\logs\mwwl_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Release\logs\mwwl_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\mwwl_trace.csv';
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

%% --- Optional: drop detector-flagged slip rows ---
% Hide rows the C++ MwSlipDetector already flagged is_slip=1 so the
% remaining plot autoscales on the in-arc noise instead of being
% dominated by 100+ cycle slip steps.  Filter once at the top so every
% downstream artefact (z-score column, threshold-ratio column, per-tile
% mu/sigma annotation, summary statistics) sees the same row set.
% Guarded against missing column for back-compat with older traces.
if hideSlipEpochs && ismember('is_slip', T.Properties.VariableNames)
    T.is_slip(isnan(T.is_slip)) = 0;
    slipMask  = logical(double(T.is_slip));   % cast handles int/bool/string
    nDropped  = nnz(slipMask);
    if nDropped > 0
        T = T(~slipMask, :);
        fprintf(['Dropped %d slip-flagged row(s) from mwwl_trace ' ...
                 '(toggle hideSlipEpochs=false to keep them).\n'], nDropped);
    end
end

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

%% --- Threshold-ratio column (synthesized, faithful to detector) ---
% dev / thresh, where thresh = threshold_cycles is the per-row threshold
% the C++ MwSlipDetector actually compared |dev| against
% (computeAdaptiveThreshold -- post-warmup it's kMadSigmaMultiplier x
% max(sigma_mad, kMinSigmaMeters); pre-warmup it's initial_threshold_cycles
% or 3 x sigma_pred under stochastic-aware mode).  By construction
% |y| > 1 corresponds exactly to the rows where is_slip = 1 was written.
T.mw_threshold_ratio = T.dev_from_mean_cycles ./ T.threshold_cycles;
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
