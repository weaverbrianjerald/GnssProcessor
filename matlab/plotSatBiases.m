%% ============================================================
%  Script: plotSatBiases.m
%  Purpose: Read and visualize sat_bias_trace.csv (GPS time axis)
%           Per-(satellite, signal) hardware code-bias estimates
%           from the PPP / PPP-RTK uncombined Kalman filter.
%
%  Expected CSV header:
%      week,tow_s,satellite,signal,system,value_m,std_m,
%      elevation_deg,is_pivot,pivot_satellite
%
%  Layout mirrors plot_measurement_residuals.m and plotIonosphere.m:
%  per-system tiled figures with NaN-gap-aware lines so cache restore,
%  pruning, and pivot handover read as breaks rather than bridges.
%
%  BSSD handover handling (matches plotIonosphere.m): in PPP-RTK the
%  sat-bias state for non-pivot sat X on signal s is b_X^s - b_pivot^s.
%  Every pivot rotation produces a synthetic per-(sat,signal) jump of
%  b_old_pivot^s - b_new_pivot^s that is NOT real hardware behaviour.
%  By default this script re-references each (constellation, signal)
%  time series to the first pivot seen per constellation (continuous
%  view, jumps removed).  Set biasReferenceMode = 'rawBssd' below to
%  see the raw filter state.  Handover events are marked with thin
%  vertical lines in either mode.
%  ============================================================

clear all; %clc; %close all;

%% --- Plot style switch ---
% 'lines'   : one line per (sat, signal) with NaN breaks at gaps; color
%             by PRN, line style by signal.  Same as measurement_residuals
%             per-signal layout.
% 'scatter' : markers only.
plotStyle = 'lines';

% A gap is declared whenever dt > gapFactor * median(dt) for that series.
gapFactor = 3;

%% --- Plot layout switch ---
% 'per_signal' : (recommended) outer tiledlayout columns = constellations,
%                inner stack rows = signals (C1W, C2W, C5Q, ...).  Each
%                tile holds one (system, signal) so lines stay solid and
%                satellites are distinguished by color alone.
% 'compact'    : 1 x nSys grid with all signals stacked in one tile per
%                constellation, distinguished by line style.
plotLayout = 'per_signal';

%% --- Content switches ---
showValue     = true;   % Figure 1 : sat-bias value
showSigma     = true;   % Figure 2 : sat-bias sigma (convergence, log y)
showPerKeyBand= true;   % Figure 3 : per-(sat,signal) +/- 1 sigma band
showSummary   = true;   % Console summary

% Pivot rings on the value figure (BSSD-frozen rows: value=0, sigma=0).
showPivotMarkers = true;

% Tile legends (off by default; PPP-RTK 40+ rows make legends overflow).
showLegend = false;

%% --- BSSD pivot re-reference mode ---
% In PPP-RTK BSSD, each non-pivot sat's bias state on signal sig is
% b_X^sig - b_pivot^sig.  When the pivot rotates from A to B, every
% non-pivot bias on the affected constellation jumps by
% b_A^sig - b_B^sig (a per-handover, per-signal constant -- not real
% hardware behaviour).  The pivot row snaps to zero on both sides.
% Two display modes are offered, identical in concept to plotIonosphere:
%
%   'firstPivot' (recommended, default) -- accumulate a per-handover,
%     per-signal offset so each (constellation, signal) reads as if a
%     single global pivot had been used throughout.  The pivot row,
%     originally frozen at 0, becomes the running offset
%     b_pivot^sig - b_globalpivot^sig and reads as a continuous line.
%     Note: std_m is NOT re-propagated -- displayed sigma is a lower
%     bound for absolute-frame uncertainty (chain accumulation).
%
%   'rawBssd' -- show the raw filter state with handover jumps.  Use
%     when auditing pivot-handover behaviour or BSSD freeze math.
biasReferenceMode = 'firstPivot';   % 'firstPivot' | 'rawBssd'

% Draw a thin vertical line at each pivot handover so the events stay
% visible even when the value time series is continuous.  Applies to
% the value and sigma per-tile figures.
showHandoverLines = true; % true for PPP-RTK (BSSD)

%% --- Load CSV file ---
% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('sat_bias_trace.csv');

if ~isfile(fname)
    error('sat-bias trace CSV not found: %s', fname);
end

T = readtable(fname, 'TextType', 'string');

%% --- Validate header ---
needed = {'week','tow_s','satellite','signal','value_m','std_m'};
for i = 1:numel(needed)
    if ~ismember(needed{i}, T.Properties.VariableNames)
        error('sat_bias_trace.csv missing required column: %s', needed{i});
    end
end

hasSystem  = ismember('system',          T.Properties.VariableNames);
hasElev    = ismember('elevation_deg',   T.Properties.VariableNames);
hasPivot   = ismember('is_pivot',        T.Properties.VariableNames);
hasPivSat  = ismember('pivot_satellite', T.Properties.VariableNames);

%% --- Derive system if older trace ---
satStr = string(T.satellite);
T.satellite = categorical(satStr);

if ~hasSystem
    T.system = repmat("Unknown", height(T), 1);
    T.system(startsWith(satStr,'G')) = "GPS";
    T.system(startsWith(satStr,'E')) = "Galileo";
    T.system(startsWith(satStr,'R')) = "GLONASS";
    T.system(startsWith(satStr,'C')) = "BeiDou";
    T.system(startsWith(satStr,'J')) = "QZSS";
    T.system(startsWith(satStr,'S')) = "SBAS";
end
T.system = categorical(string(T.system));
T.signal = categorical(string(T.signal));

if ~hasElev,   T.elevation_deg   = nan(height(T),1);          end
if ~hasPivot,  T.is_pivot        = zeros(height(T),1);        end
if ~hasPivSat, T.pivot_satellite = strings(height(T),1);      end

%% --- Convert GPS week + TOW to a datetime on the GPS time scale ---
% Trace files carry pure GPS time so timestamps match RINEX epochs.
gpsEpoch  = datetime(1980,1,6,0,0,0);
T.utc = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));

%% --- Apply BSSD pivot re-reference (eliminate handover jumps) ---
% Same pattern as plotIonosphere.m, but keyed by (constellation, signal)
% rather than just (constellation) -- each signal carries its own
% hardware bias state with its own per-handover jump.
handoverEpochs = collectPivotHandovers(T);   % per-constellation event log
nHandoversTotal = 0;
kHo = keys(handoverEpochs);
for ii = 1:numel(kHo)
    nHandoversTotal = nHandoversTotal + numel(handoverEpochs(kHo{ii}));
end
if strcmpi(biasReferenceMode, 'firstPivot') && nHandoversTotal > 0
    fprintf(['Re-referencing %d pivot handover(s) across %d constellation(s) ' ...
             'to the first pivot seen per (constellation, signal) ' ...
             '(mode=''firstPivot'').\n' ...
             'Switch biasReferenceMode to ''rawBssd'' to see raw BSSD jumps.\n'], ...
            nHandoversTotal, numel(kHo));
    T = reReferenceBiasToFirstPivot(T);
elseif strcmpi(biasReferenceMode, 'rawBssd') && nHandoversTotal > 0
    fprintf(['Showing raw BSSD sat-bias states (mode=''rawBssd''); %d handover ' ...
             'jump(s) will be visible.\n'], nHandoversTotal);
end

%% --- Color and marker setup ---
satList = categories(T.satellite);
N = numel(satList);
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

try
    addpath(genpath('C:\temp\gnss-qc\matlab\visualization'));
    colors = distinguishable_colors(max(8, 1e3));
catch
    colors = lines(max(8, 1e3));
end

% Color row per satellite: PRN (chars 2-3), wrap if needed.  Same
% convention used in plotIonosphere.m and plot_measurement_residuals.m so
% the three scripts agree on satellite color for a given run.
satColorRow = @(sat) localColorRow(sat, satList, size(colors,1));

%% --- Per-signal line style map (used when plotStyle = 'lines' and
%%     plotLayout = 'compact') ---
% Each distinct signal gets a line style; cycles if more signals than
% entries.  Stable across every subplot because it's built over the full
% table.  In per_signal layout the line style is irrelevant (one signal
% per tile) so it's only consulted in compact layout.
lineStyleList = {'-','--',':','-.'};
allSignals    = string(categories(T.signal));
allSignals    = allSignals(~ismissing(allSignals) & allSignals ~= "");
sigStyleFor   = @(sig) lineStyleList{ ...
    mod(find(allSignals == string(sig), 1) - 1, numel(lineStyleList)) + 1};

%% --- Active systems present in the data ---
systemList = categories(T.system);
systemList = systemList(~strcmp(systemList,'Unknown'));
systemsPresent = {};
for i = 1:numel(systemList)
    if any(T.system == systemList{i})
        systemsPresent{end+1} = systemList{i};
    end
end
nSys = numel(systemsPresent);
if nSys == 0
    error('No recognized GNSS systems found in sat_bias_trace');
end

%% ================================================================
%  Figure 1: sat-bias value
%  ================================================================
if showValue
    if strcmpi(plotLayout, 'per_signal')
        plotBiasGrid(T, 'value_m', 'Sat bias (m)', ...
            'Per-(satellite,signal) hardware bias by system and signal', ...
            systemsPresent, colors, satColorRow, satList, markerList, ...
            plotStyle, gapFactor, showPivotMarkers, showLegend, ...
            'YScale','linear', ...
            'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
    else
        plotBiasBySystem(T, 'value_m', 'Sat bias (m)', ...
            'Per-(satellite,signal) hardware bias by system', ...
            systemsPresent, colors, satColorRow, satList, sigStyleFor, ...
            plotStyle, gapFactor, showPivotMarkers, showLegend, ...
            'YScale','linear', ...
            'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
    end
end

%% ================================================================
%  Figure 2: sat-bias sigma (convergence, log y-axis)
%  ================================================================
if showSigma
    if strcmpi(plotLayout, 'per_signal')
        plotBiasGrid(T, 'std_m', '\sigma (m)', ...
            'Sat-bias state \sigma (m) - convergence monitor', ...
            systemsPresent, colors, satColorRow, satList, markerList, ...
            plotStyle, gapFactor, false, showLegend, ...
            'YScale','log', ...
            'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
    else
        plotBiasBySystem(T, 'std_m', '\sigma (m)', ...
            'Sat-bias state \sigma (m) - convergence monitor', ...
            systemsPresent, colors, satColorRow, satList, sigStyleFor, ...
            plotStyle, gapFactor, false, showLegend, ...
            'YScale','log', ...
            'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
    end
end

%% ================================================================
%  Figure 3: Per-(sat,signal) +/- 1 sigma band, small multiples
%  ================================================================
if showPerKeyBand
    keys = unique(string(T.satellite) + "|" + string(T.signal));
    nKeys = numel(keys);
    maxPerFig = 36;
    nPlot = min(nKeys, maxPerFig);
    rows  = ceil(sqrt(nPlot));
    cols  = ceil(nPlot / rows);

    figure('Name','Sat-bias +/-1 sigma (per key)','Color','w', ...
           'Position', [80 80 min(1600, 220*cols+120) min(900, 170*rows+120)]);
    tl = tiledlayout(rows, cols, 'TileSpacing','tight','Padding','compact');
    title(tl, 'Per-(sat,signal) hardware bias with \pm1\sigma envelope');

    for k = 1:nPlot
        ax = nexttile; hold(ax,'on'); grid(ax,'on');
        key = keys(k);
        parts = split(key, '|');
        sat = string(parts(1));
        sig = string(parts(2));
        idx = string(T.satellite) == sat & string(T.signal) == sig;
        T_sub = sortrows(T(idx,:), 'utc');
        if isempty(T_sub)
            text(0.5,0.5,'No Data','Units','normalized', ...
                'HorizontalAlignment','center', 'Parent', ax);
            title(ax, key,'Interpreter','none'); continue;
        end

        times = T_sub.utc;
        vals  = double(T_sub.value_m);
        err   = double(T_sub.std_m);
        c     = colors(satColorRow(sat), :);
        bandRgb = 1 - 0.4*(1 - c);

        % Gap-aware fill
        [tt, lo] = insertGapBreaks(times, vals - err, gapFactor);
        [~,  hi] = insertGapBreaks(times, vals + err, gapFactor);
        nanMask = isnat(tt) | isnan(lo) | isnan(hi);
        segStart = find(diff([true; nanMask]) == -1);
        segEnd   = find(diff([nanMask; true]) ==  1);
        for s = 1:numel(segStart)
            si = segStart(s); ei = segEnd(s);
            if ei <= si, continue; end
            xx = [tt(si:ei); flipud(tt(si:ei))];
            yy = [lo(si:ei); flipud(hi(si:ei))];
            fill(ax, xx, yy, bandRgb, 'EdgeColor','none', 'FaceAlpha',0.4, ...
                'HandleVisibility','off');
        end

        [tt2, vv2] = insertGapBreaks(times, vals, gapFactor);
        plot(ax, tt2, vv2, '-', 'Color', c, 'LineWidth', 0.9);

        % Pivot ring overlay
        if showPivotMarkers && any(double(T_sub.is_pivot) > 0)
            pmask = double(T_sub.is_pivot) > 0;
            scatter(ax, T_sub.utc(pmask), vals(pmask), 18, ...
                'MarkerEdgeColor','k', 'MarkerFaceColor','none', ...
                'HandleVisibility','off');
        end

        finite = isfinite(vals);
        if any(finite)
            text(ax, 0.98, 0.95, ...
                sprintf('\\mu=%.3g  \\sigma=%.3g', mean(vals(finite)), std(vals(finite))), ...
                'Units','normalized', 'HorizontalAlignment','right', ...
                'VerticalAlignment','top','FontSize',7, ...
                'BackgroundColor','w','EdgeColor',[0.7 0.7 0.7],'Margin',1);
        end

        title(ax, key, 'Interpreter','none');
        if mod(k-1, cols) == 0, ylabel(ax,'bias (m)'); end
        if k > nPlot - cols, xlabel(ax,'GPS Time'); end
        hold(ax,'off');
    end

    if nKeys > maxPerFig
        warning('plotSatBiases:tooManyKeys', ...
            'Only first %d / %d (sat,signal) keys shown in per-key band figure.', ...
            maxPerFig, nKeys);
    end
end

%% ================================================================
%  Summary statistics
%  ================================================================
if showSummary
    fprintf('\n=== Satellite-Bias Trace Summary ===\n');
    fprintf('File           : %s\n', fname);
    fprintf('Epochs         : %d unique\n', numel(unique(T.utc)));
    fprintf('Sat-signal keys: %d\n', numel(unique(string(T.satellite) + "|" + string(T.signal))));
    fprintf('Span (UTC)     : %s -> %s\n', string(min(T.utc)), string(max(T.utc)));

    fprintf('\n%-9s %-7s %-9s %6s %10s %10s %10s\n', ...
        'Satellite','Signal','System','N', ...
        'Mean(m)','Std(m)','MeanSigma');
    fprintf('%s\n', repmat('-', 1, 70));
    keys = unique(string(T.satellite) + "|" + string(T.signal));
    for k = 1:numel(keys)
        parts = split(keys(k), '|');
        sat = string(parts(1)); sig = string(parts(2));
        idx = string(T.satellite) == sat & string(T.signal) == sig;
        if ~any(idx), continue; end
        vals = double(T.value_m(idx));
        sigs = double(T.std_m(idx));
        sys  = string(T.system(find(idx,1,'first')));
        fprintf('%-9s %-7s %-9s %6d %10.4f %10.4f %10.4f\n', ...
            sat, sig, sys, sum(idx), ...
            mean(vals,'omitnan'), std(vals,'omitnan'), ...
            mean(sigs,'omitnan'));
    end

    if any(double(T.is_pivot) > 0)
        fprintf('\n=== BSSD Pivot Tenure (sat-bias rows) ===\n');
        pivotSats = unique(T.satellite(T.is_pivot > 0));
        for k = 1:numel(pivotSats)
            sat = pivotSats(k);
            tp  = T.utc(T.satellite == sat & T.is_pivot > 0);
            fprintf('  %s pivot for %d epoch-rows : %s -> %s\n', ...
                string(sat), numel(tp), string(min(tp)), string(max(tp)));
        end
    end
end


%% ======================== LOCAL FUNCTIONS =======================

function plotBiasGrid(T, valueColumn, valueLabel, figTitle, ...
                      systemsPresent, colors, satColorRow, satList, ...
                      markerList, plotStyle, gapFactor, ...
                      showPivotMarkers, showLegend, varargin) %#ok<INUSD>
% PLOTBIASGRID  per_signal layout, mirrored on plotResidualGrid in
% plot_measurement_residuals.m.  Outer tiledlayout(1, nCons) and one
% inner tiledlayout(nSigsForCons, 1) per constellation -- different
% constellations can carry different signal sets and no cross-column
% alignment by signal name is enforced.
%
% Optional name-value pairs:
%   'YScale'            : 'linear' (default) or 'log' (sigma figure).
%   'Handovers'         : containers.Map (system -> datetime[]) of pivot
%                         handover epochs.  Empty = no annotations.
%   'ShowHandoverLines' : true to draw a thin vertical line at each
%                         handover in every tile for the affected system.
    p = inputParser; p.KeepUnmatched = true;
    addParameter(p,'YScale','linear');
    addParameter(p,'Handovers', containers.Map());
    addParameter(p,'ShowHandoverLines', false);
    parse(p, varargin{:});
    yScale            = p.Results.YScale;
    handovers         = p.Results.Handovers;
    showHandoverLines = p.Results.ShowHandoverLines;

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
        fprintf('plotBiasGrid: no populated (signal, system) pairs\n');
        return;
    end
    sigsPerCons = sigsPerCons(activeCons);
    consActive  = systemsPresent(activeCons);
    nConsActive = numel(consActive);
    maxSigsPerCons = max(cellfun(@numel, sigsPerCons));

    figWidth  = max(900, 320*nConsActive + 120);
    figHeight = max(420, 160*maxSigsPerCons + 120);
    fig = figure('Name', figTitle, 'Color','w', ...
                 'Position', [40 40 figWidth figHeight]);
    fig.Position(4) = fig.Position(4) - 100;
    outerTl = tiledlayout(1, nConsActive, 'TileSpacing','tight','Padding','compact');
    title(outerTl, figTitle);

    for c = 1:nConsActive
        sysName = consActive{c};
        sigs = sigsPerCons{c};
        nSigsForCons = numel(sigs);

        innerTl = tiledlayout(outerTl, nSigsForCons, 1, ...
                              'TileSpacing','tight','Padding','tight');
        innerTl.Layout.Tile = c;
        innerTl.Layout.TileSpan = [1 1];

        columnAxes = gobjects(0);

        for r = 1:nSigsForCons
            sig = sigs(r);
            ax = nexttile(innerTl); hold(ax,'on'); grid(ax,'on');
            columnAxes(end+1) = ax; %#ok<AGROW>

            idx = T.system == sysName & string(T.signal) == sig;
            T_sub = T(idx, :);

            satsHere = categories(removecats(T_sub.satellite));
            for k = 1:numel(satsHere)
                sat = satsHere{k};
                idx_sat = T_sub.satellite == sat;
                T_satObs = sortrows(T_sub(idx_sat, :), 'utc');
                if isempty(T_satObs), continue; end

                c_rgb = colors(satColorRow(sat), :);
                if strcmpi(plotStyle, 'lines')
                    [tt, yy] = insertGapBreaks(T_satObs.utc, ...
                        double(T_satObs.(valueColumn)), gapFactor);
                    plot(ax, tt, yy, '-', 'Color', c_rgb, ...
                        'LineWidth', 0.9, 'DisplayName', sat);
                else
                    scatter(ax, T_satObs.utc, ...
                        double(T_satObs.(valueColumn)), 14, ...
                        'Marker', markerList{mod(k-1,numel(markerList))+1}, ...
                        'MarkerEdgeColor', c_rgb, 'MarkerFaceColor','none', ...
                        'DisplayName', sat);
                end

                if showPivotMarkers && strcmp(valueColumn,'value_m') ...
                                    && any(double(T_satObs.is_pivot) > 0)
                    pmask = double(T_satObs.is_pivot) > 0;
                    scatter(ax, T_satObs.utc(pmask), ...
                        double(T_satObs.(valueColumn)(pmask)), 22, ...
                        'MarkerEdgeColor','k', 'MarkerFaceColor','none', ...
                        'LineWidth', 0.8, 'HandleVisibility','off');
                end
            end

            % Pivot-handover vertical lines for this constellation.
            % Drawn AFTER the per-sat traces but with HandleVisibility off
            % so the legend stays clean.
            if showHandoverLines && isKey(handovers, sysName)
                evs = handovers(sysName);
                for he = 1:numel(evs)
                    xline(ax, evs(he), '-', 'Color', [0.3 0.3 0.3], ...
                        'LineWidth', 0.5, 'Alpha', 0.5, ...
                        'HandleVisibility', 'off');
                end
            end

            % Per-tile mu/sigma, top-right.
            vals = double(T_sub.(valueColumn));
            vals = vals(isfinite(vals));
            if ~isempty(vals)
                text(ax, 0.98, 0.97, ...
                    sprintf('\\mu=%.3g  \\sigma=%.3g  N=%d', ...
                        mean(vals), std(vals), numel(vals)), ...
                    'Units','normalized', 'HorizontalAlignment','right', ...
                    'VerticalAlignment','top','FontSize',8, ...
                    'BackgroundColor','w','EdgeColor',[0.7 0.7 0.7],'Margin',1);
            end

            title(ax, sprintf('%s - %s', sysName, sig));
            if r == nSigsForCons
                xlabel(ax,'GPS Time');
            else
                ax.XTickLabel = [];
            end
            if c == 1, ylabel(ax, valueLabel); end
            set(ax, 'YScale', yScale);

            if showLegend && r == 1
                legend(ax,'Location','bestoutside','FontSize',7);
            end
            hold(ax,'off');
        end

        if numel(columnAxes) > 1
            linkaxes(columnAxes, 'x');
        end
    end
end


function plotBiasBySystem(T, valueColumn, valueLabel, figTitle, ...
                          systemsPresent, colors, satColorRow, satList, ...
                          sigStyleFor, plotStyle, gapFactor, ...
                          showPivotMarkers, showLegend, varargin) %#ok<INUSD>
% PLOTBIASBYSYSTEM  Compact layout: 1 x nSys grid, all signals stacked in
% one tile per constellation, distinguished by line style.
%
% Optional name-value pairs:
%   'YScale'            : 'linear' (default) or 'log' (sigma figure).
%   'Handovers'         : containers.Map (system -> datetime[]) of pivot
%                         handover epochs.  Empty = no annotations.
%   'ShowHandoverLines' : true to draw a thin vertical line at each
%                         handover in the tile for the affected system.
    p = inputParser; p.KeepUnmatched = true;
    addParameter(p,'YScale','linear');
    addParameter(p,'Handovers', containers.Map());
    addParameter(p,'ShowHandoverLines', false);
    parse(p, varargin{:});
    yScale            = p.Results.YScale;
    handovers         = p.Results.Handovers;
    showHandoverLines = p.Results.ShowHandoverLines;

    nSys = numel(systemsPresent);
    figWidth  = max(900, 360*nSys + 120);
    figHeight = 480;
    fig = figure('Name', figTitle, 'Color','w', ...
                 'Position', [60 60 figWidth figHeight]);
    tl = tiledlayout(fig, 1, nSys, 'TileSpacing','compact','Padding','compact');
    title(tl, figTitle);

    columnAxes = gobjects(0);
    for col = 1:nSys
        sysName = systemsPresent{col};
        ax = nexttile(tl); hold(ax,'on'); grid(ax,'on');
        columnAxes(end+1) = ax; %#ok<AGROW>

        idx = T.system == sysName;
        T_sub = T(idx, :);
        if isempty(T_sub)
            text(0.5,0.5,'No Data','Units','normalized', ...
                'HorizontalAlignment','center', 'Parent', ax);
            title(ax, sysName); continue;
        end

        satsHere = categories(removecats(T_sub.satellite));
        for k = 1:numel(satsHere)
            sat = satsHere{k};
            sigsForSat = unique(string(T_sub.signal(T_sub.satellite == sat)));
            c_rgb = colors(satColorRow(sat), :);
            for s = 1:numel(sigsForSat)
                sig = sigsForSat(s);
                idx_ss = T_sub.satellite == sat & string(T_sub.signal) == sig;
                T_obs = sortrows(T_sub(idx_ss, :), 'utc');
                if isempty(T_obs), continue; end
                if strcmpi(plotStyle, 'lines')
                    [tt, yy] = insertGapBreaks(T_obs.utc, ...
                        double(T_obs.(valueColumn)), gapFactor);
                    plot(ax, tt, yy, ...
                        'LineStyle', sigStyleFor(sig), ...
                        'Color', c_rgb, ...
                        'DisplayName', sprintf('%s/%s', sat, sig));
                else
                    scatter(ax, T_obs.utc, double(T_obs.(valueColumn)), ...
                        12, c_rgb, 'filled', ...
                        'DisplayName', sprintf('%s/%s', sat, sig));
                end
                if showPivotMarkers && strcmp(valueColumn,'value_m') ...
                                    && any(double(T_obs.is_pivot) > 0)
                    pmask = double(T_obs.is_pivot) > 0;
                    scatter(ax, T_obs.utc(pmask), ...
                        double(T_obs.(valueColumn)(pmask)), 22, ...
                        'MarkerEdgeColor','k','MarkerFaceColor','none', ...
                        'LineWidth',0.8,'HandleVisibility','off');
                end
            end
        end

        % Pivot-handover vertical lines for this constellation.
        if showHandoverLines && isKey(handovers, sysName)
            evs = handovers(sysName);
            for he = 1:numel(evs)
                xline(ax, evs(he), '-', 'Color', [0.3 0.3 0.3], ...
                    'LineWidth', 0.5, 'Alpha', 0.5, ...
                    'HandleVisibility', 'off');
            end
        end

        v_all = double(T_sub.(valueColumn));
        v_all = v_all(isfinite(v_all));
        if ~isempty(v_all)
            text(ax, 0.98, 0.97, ...
                sprintf('\\mu=%.3g  \\sigma=%.3g  N=%d', ...
                    mean(v_all), std(v_all), numel(v_all)), ...
                'Units','normalized', 'HorizontalAlignment','right', ...
                'VerticalAlignment','top','FontSize',8, ...
                'BackgroundColor','w','EdgeColor',[0.7 0.7 0.7],'Margin',1);
        end

        title(ax, sysName);
        xlabel(ax,'GPS Time');
        if col == 1, ylabel(ax, valueLabel); end
        set(ax, 'YScale', yScale);

        if showLegend
            legend(ax,'Location','bestoutside','FontSize',7,'Interpreter','none');
        end
        hold(ax,'off');
    end

    if numel(columnAxes) > 1
        linkaxes(columnAxes, 'x');
    end
end


function [tOut, yOut] = insertGapBreaks(tIn, yIn, gapFactor)
% INSERTGAPBREAKS  Insert NaT/NaN rows where the time series has a gap.
% Cloned from plot_measurement_residuals.m so this script is self-contained.
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
        w = w + chunkLen + 1;
        rStart = gIdx + 1;
    end
    chunkLen = numel(tSorted) - rStart + 1;
    if chunkLen > 0
        tOut(w:w+chunkLen-1) = tSorted(rStart:end);
        yOut(w:w+chunkLen-1) = ySorted(rStart:end);
    end
end


function row = localColorRow(sat, satList, nColors)
% LOCALCOLORROW  PRN-keyed color row, matches plotIonosphere.m and
% plot_measurement_residuals.m so the same satellite renders in the same
% color across the three scripts when viewing the same dataset.
    satStr = char(sat);
    row = NaN;
    if numel(satStr) >= 3
        row = str2double(satStr(2:3));
    end
    if ~isfinite(row)
        row = find(strcmp(satList, sat), 1);
        if isempty(row), row = 1; end
    end
    if row > nColors
        row = mod(row-1, nColors) + 1;
    end
    if row < 1, row = 1; end
end


function handovers = collectPivotHandovers(T)
% COLLECTPIVOTHANDOVERS  Build a containers.Map from constellation name to
% the UTC datetime of every BSSD pivot handover for that constellation.
% A handover is declared when pivot_satellite changes between two
% successive unique epochs.  Returns an empty map for traces that lack
% pivot info (PPP, baseline DD, older traces).  Signal-agnostic --
% handovers are a per-constellation property and apply to every signal.
    handovers = containers.Map();
    if ~ismember('pivot_satellite', T.Properties.VariableNames)
        return;
    end
    pivStr = string(T.pivot_satellite);
    if all(pivStr == "")
        return;
    end
    systems = categories(T.system);
    systems = systems(~strcmp(systems,'Unknown'));
    for s = 1:numel(systems)
        sysName = systems{s};
        rows = T.system == sysName;
        if ~any(rows), continue; end
        T_sys = sortrows(T(rows, :), 'utc');
        [uEpochs, ~, ix] = unique(T_sys.utc);
        if numel(uEpochs) < 2, continue; end
        pivPerEpoch = strings(numel(uEpochs), 1);
        for e = 1:numel(uEpochs)
            firstRow = find(ix == e, 1, 'first');
            pivPerEpoch(e) = string(T_sys.pivot_satellite(firstRow));
        end
        changeMask = false(numel(uEpochs), 1);
        for e = 2:numel(uEpochs)
            if pivPerEpoch(e) ~= pivPerEpoch(e-1) ...
                    && pivPerEpoch(e) ~= "" ...
                    && pivPerEpoch(e-1) ~= ""
                changeMask(e) = true;
            end
        end
        if any(changeMask)
            handovers(sysName) = uEpochs(changeMask);
        end
    end
end


function T = reReferenceBiasToFirstPivot(T)
% RE-REFERENCE BSSD sat-bias states to the first pivot seen per
% (constellation, signal).
%
% In BSSD, the sat-bias state for non-pivot sat X on signal s is
% b_X^s - b_pivot^s.  When the pivot rotates from A to B, every non-pivot
% row on that constellation/signal jumps by b_A^s - b_B^s, and the pivot
% row snaps from one sat to another.  These are presentation artefacts
% of the moving reference, not hardware events.
%
% Algorithm (per (constellation, signal)): walk epochs in chronological
% order, accumulate a running offset O = b_pivot^s - b_globalpivot^s.
% At each handover P_old -> P_new, look up the new pivot's bias value
% for THIS signal at the LAST epoch in P_old's frame (= b_P_new - b_P_old
% for signal s), and add to O.  Then apply O to every row at this epoch
% for this (cons, signal) -- the pivot row's raw value is 0, so the
% result is exactly O, i.e. the pivot's true bias relative to the global
% pivot for this signal.
%
% Per-signal because each signal carries its own bias state and the
% jump magnitude differs across signals.  If the new pivot doesn't carry
% the same signal at the previous epoch, that signal's offset can't be
% bridged across the handover and a small residual jump remains -- the
% handover line still flags where it happened.
%
% Caveat: std_m is NOT re-propagated -- each chained offset adds a small
% variance term not carried in the trace.  Displayed sigma is a lower
% bound for absolute-frame uncertainty.  Documented in script header.
    if ~ismember('pivot_satellite', T.Properties.VariableNames)
        return;
    end
    pivStr = string(T.pivot_satellite);
    if all(pivStr == "")
        return;
    end

    systems = categories(T.system);
    systems = systems(~strcmp(systems,'Unknown'));

    value_out = double(T.value_m);

    for s = 1:numel(systems)
        sysName = systems{s};
        rowsSys = find(T.system == sysName);
        if isempty(rowsSys), continue; end

        % Per-signal loop within this constellation.  T.signal is
        % categorical (set during validation); iterate distinct signals
        % present in this constellation only.
        sigsHere = unique(string(T.signal(rowsSys)));
        sigsHere = sigsHere(~ismissing(sigsHere) & sigsHere ~= "");

        for ig = 1:numel(sigsHere)
            sigName = sigsHere(ig);

            % Subset: this (constellation, signal).
            rowsSubset = rowsSys(string(T.signal(rowsSys)) == sigName);
            if isempty(rowsSubset), continue; end

            T_sub = T(rowsSubset, :);
            [T_sub, sortIdx] = sortrows(T_sub, 'utc');
            rowsSubsetSorted = rowsSubset(sortIdx);

            [uEpochs, ~, ixEpoch] = unique(T_sub.utc);
            nE = numel(uEpochs);

            % Pivot at each unique epoch.  pivot_satellite is per-row
            % but consistent across rows of the same (epoch, system),
            % so any row of the subset at epoch e gives the right value.
            pivPerEpoch = strings(nE,1);
            for e = 1:nE
                firstRow = find(ixEpoch == e, 1, 'first');
                pivPerEpoch(e) = string(T_sub.pivot_satellite(firstRow));
            end

            if all(pivPerEpoch == ""), continue; end

            O = 0.0;
            for e = 1:nE
                if e > 1 ...
                        && pivPerEpoch(e) ~= pivPerEpoch(e-1) ...
                        && pivPerEpoch(e)   ~= "" ...
                        && pivPerEpoch(e-1) ~= ""
                    % Pivot changed.  Look up the new pivot's bias for
                    % THIS signal at the previous epoch.  If the new
                    % pivot didn't carry this signal there, O stays put
                    % and a residual jump remains (handover line marks
                    % the spot).
                    prevRows = ixEpoch == (e-1) ...
                               & string(T_sub.satellite) == pivPerEpoch(e);
                    if any(prevRows)
                        O = O + double(T_sub.value_m(find(prevRows, 1, 'first')));
                    end
                end

                thisEpochMaskSorted = (ixEpoch == e);
                origIdx = rowsSubsetSorted(thisEpochMaskSorted);
                value_out(origIdx) = double(T.value_m(origIdx)) + O;
                % Pivot row: raw value_m is 0 (BSSD-frozen) -> becomes O,
                % the pivot's bias delta vs the global pivot for this
                % signal.  Continuous across handovers.
            end
        end
    end

    T.value_m = value_out;
end
