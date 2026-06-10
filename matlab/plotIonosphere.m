%% ============================================================
%  Script: plotIonosphere.m
%  Purpose: Read and visualize ionosphere_trace.csv (GPS time axis)
%           Per-satellite slant ionosphere delay estimates from
%           the PPP-RTK uncombined Kalman filter.
%  Expected CSV header (extended in 2026):
%      week,tow_s,satellite,system,iono_m,std_m,elevation_deg,
%      azimuth_deg,mapping_factor,vtec_m,is_pivot,pivot_satellite
%
%  Layout mirrors plot_measurement_residuals.m: per-system tiled
%  figures with NaN-gap-aware lines so handover and reseed events
%  read as breaks instead of bridges.
%
%  BSSD handover handling: in PPP-RTK the slant iono state for non-pivot
%  sat X is I_X - I_pivot.  Every pivot rotation produces a synthetic
%  per-sat jump of I_old_pivot - I_new_pivot that is NOT real iono.  By
%  default this script re-references the time series to the first pivot
%  seen per constellation (continuous view, jumps removed).  Set
%  ionoReferenceMode = 'rawBssd' below to see the raw filter state.
%  Handover events are marked with thin vertical lines in either mode.
%  ============================================================

clear all; %clc; %close all;

%% --- Plot style switch ---
% 'lines'   : one line per satellite with NaN breaks at gaps; color by
%             PRN, gap detection identical to measurement_residuals.
% 'scatter' : markers only.  Useful when sample spacing varies wildly.
plotStyle = 'lines';

% A gap is declared whenever dt > gapFactor * median(dt) for that
% series; NaN is inserted so plot() draws a break instead of a bridge.
gapFactor = 3;

%% --- Content switches ---
% Set to false to skip any figure you don't want to render.  All figures
% share the per-system tiled layout convention from
% plot_measurement_residuals.m.
showSlant        = true;   % Figure 1 : slant iono delay per system
showVtec         = true;   % Figure 2 : VTEC (vertical iono) per system
showSigma        = true;   % Figure 3 : iono state sigma (convergence)
showPerSatBand   = true;   % Figure 4 : per-sat slant +/- 1 sigma band
showSkyHeatmap   = true;   % Figure 5 : sky plot colored by slant iono
showSummary      = true;   % Console summary statistics

% Mark pivot rows with a heavy ring so the BSSD reference is visible.
showPivotMarkers = true;

% Show legends inside per-tile plots (off by default; large constellations
% with 10+ sats make the legend dominate the tile area).
showLegend       = false;

%% --- BSSD pivot re-reference mode ---
% In PPP-RTK BSSD, each non-pivot sat's iono state is Î_X = I_X - I_pivot.
% When the pivot rotates from A to B, every non-pivot sat's stored value
% jumps by I_A - I_B (a per-handover constant, not real iono).  The pivot
% row snaps to zero on both sides.  Two display modes are offered:
%
%   'firstPivot' (recommended, default) -- accumulate a per-handover
%     offset so the time series reads as if a single global pivot
%     (the first one seen per constellation) had been used throughout.
%     The pivot's row, originally frozen at 0, becomes the running
%     offset I_pivot - I_globalpivot and reads as a continuous line.
%     Note: std_m is NOT re-propagated across the chain -- displayed
%     sigma is a lower bound (the chained offset adds small variance
%     terms that the trace doesn't carry).
%
%   'rawBssd' -- show the raw filter state with handover jumps.  Useful
%     when debugging the BSSD math itself or auditing pivot-handover
%     behaviour.
ionoReferenceMode = 'firstPivot';   % 'firstPivot' | 'rawBssd'

% Draw a thin vertical line at each pivot handover so the events stay
% visible even when the value time series is continuous.  Applies to
% figures 1, 2, and 3 (the per-system tiled views).
showHandoverLines = true;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\logs\ionosphere_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Release\logs\ionosphere_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\ionosphere_trace.csv';

if ~isfile(fname)
    error('ionosphere trace CSV not found: %s', fname);
end

T = readtable(fname, 'TextType', 'string');

%% --- Validate header (tolerate old short-schema traces) ---
needed = {'week','tow_s','satellite','iono_m','std_m','elevation_deg'};
for i = 1:numel(needed)
    if ~ismember(needed{i}, T.Properties.VariableNames)
        error('ionosphere_trace.csv missing required column: %s', needed{i});
    end
end

% Optional columns from the 2026 schema; fall back to derived values when
% reading an older trace.
hasAzimuth   = ismember('azimuth_deg',     T.Properties.VariableNames);
hasMapping   = ismember('mapping_factor',  T.Properties.VariableNames);
hasVtec      = ismember('vtec_m',          T.Properties.VariableNames);
hasPivotFlag = ismember('is_pivot',        T.Properties.VariableNames);
hasPivotSat  = ismember('pivot_satellite', T.Properties.VariableNames);
hasSystem    = ismember('system',          T.Properties.VariableNames);

%% --- Derive system / mapping factor / vtec if the trace is older ---
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

el_deg = double(T.elevation_deg);
if ~hasMapping
    T.mapping_factor = thinShellMappingFactor(el_deg);
end
if ~hasVtec
    T.vtec_m = double(T.iono_m) ./ double(T.mapping_factor);
end
if ~hasAzimuth
    T.azimuth_deg = nan(height(T), 1);
end
if ~hasPivotFlag
    T.is_pivot = zeros(height(T), 1);
end
if ~hasPivotSat
    T.pivot_satellite = strings(height(T), 1);
end

%% --- Convert GPS week + TOW to a datetime on the GPS time scale ---
% Trace files carry pure GPS time (week + tow_s) so timestamps line up
% exactly with the RINEX epoch records the engine consumed.  No leap-
% second adjustment is applied: the displayed datetime IS GPS time.
gpsEpoch  = datetime(1980,1,6,0,0,0);
T.utc = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));

%% --- Apply BSSD pivot re-reference (eliminate handover jumps) ---
% Run BEFORE building handover-line metadata so the line locations come
% from the pre-modification table, and after UTC conversion so the
% chronological sort inside the helper is meaningful.  No-op for traces
% without pivot info (PPP, baseline, older traces).
handoverEpochs = collectPivotHandovers(T);   % per-constellation event log
nHandoversTotal = 0;
k = keys(handoverEpochs);
for ii = 1:numel(k)
    nHandoversTotal = nHandoversTotal + numel(handoverEpochs(k{ii}));
end
if strcmpi(ionoReferenceMode, 'firstPivot') && nHandoversTotal > 0
    fprintf(['Re-referencing %d pivot handover(s) across %d constellation(s) ' ...
             'to the first pivot seen per constellation (mode=''firstPivot'').\n' ...
             'Switch ionoReferenceMode to ''rawBssd'' to see raw BSSD jumps.\n'], ...
            nHandoversTotal, numel(k));
    T = reReferenceIonoToFirstPivot(T);
elseif strcmpi(ionoReferenceMode, 'rawBssd') && nHandoversTotal > 0
    fprintf(['Showing raw BSSD iono states (mode=''rawBssd''); %d handover ' ...
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

% Color row per satellite: by PRN number where the sat ID parses, otherwise
% by global index.  Matches the convention used by
% plot_measurement_residuals.m so the two scripts agree on satellite color
% when looking at the same dataset.
satColorRow = @(sat) localColorRow(sat, satList, size(colors,1));

systemList = categories(T.system);
systemList = systemList(~strcmp(systemList,'Unknown'));
systemsPresent = {};
for i = 1:numel(systemList)
    if any(T.system == systemList{i})
        systemsPresent{end+1} = systemList{i}; %#ok<AGROW>
    end
end
nSys = numel(systemsPresent);
if nSys == 0
    error('No recognized GNSS systems found in ionosphere CSV');
end

%% ================================================================
%  Figure 1: Slant ionosphere delay (m) -- one tile per system
%  ================================================================
if showSlant
    plotIonoBySystem(T, 'iono_m', 'Slant iono delay (m)', ...
        'Slant ionosphere delay by system', ...
        systemsPresent, colors, satColorRow, satList, ...
        markerList, plotStyle, gapFactor, showPivotMarkers, showLegend, ...
        'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
end

%% ================================================================
%  Figure 2: VTEC (vertical iono, m) -- one tile per system
%  ================================================================
if showVtec
    plotIonoBySystem(T, 'vtec_m', 'Vertical iono delay (m, L1)', ...
        'Vertical ionosphere delay (slant / mapping factor) by system', ...
        systemsPresent, colors, satColorRow, satList, ...
        markerList, plotStyle, gapFactor, showPivotMarkers, showLegend, ...
        'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
end

%% ================================================================
%  Figure 3: State sigma (m) -- convergence monitor, log y-axis
%  ================================================================
if showSigma
    plotIonoBySystem(T, 'std_m', '\sigma_{iono} (m)', ...
        'Ionosphere state \sigma (m) - convergence monitor', ...
        systemsPresent, colors, satColorRow, satList, ...
        markerList, plotStyle, gapFactor, false, showLegend, ...
        'YScale','log', ...
        'Handovers', handoverEpochs, 'ShowHandoverLines', showHandoverLines);
end

%% ================================================================
%  Figure 4: Per-satellite slant + 1-sigma band (small multiples)
%  ================================================================
if showPerSatBand
    allSats = string(satList);
    nSats   = numel(allSats);
    maxPerFig = 36;
    nPlot = min(nSats, maxPerFig);
    rows  = ceil(sqrt(nPlot));
    cols  = ceil(nPlot / rows);

    figure('Name','Iono per Satellite (+/- 1 sigma)','Color','w', ...
           'Position', [80 80 min(1600, 220*cols+120) min(900, 170*rows+120)]);
    tl = tiledlayout(rows, cols, 'TileSpacing','tight','Padding','compact');
    title(tl, 'Per-satellite slant iono delay with \pm1\sigma envelope');

    for k = 1:nPlot
        ax = nexttile; hold(ax,'on'); grid(ax,'on');
        sat = allSats(k);
        idx = T.satellite == sat;
        T_sub = T(idx, :);
        T_sub = sortrows(T_sub, 'utc');
        if isempty(T_sub)
            text(0.5,0.5,'No Data','Units','normalized', ...
                'HorizontalAlignment','center', 'Parent', ax);
            title(ax, sat,'Interpreter','none'); continue;
        end

        times = T_sub.utc;
        vals  = double(T_sub.iono_m);
        err   = double(T_sub.std_m);

        % Gap-aware fill for the +/- 1 sigma band so handover holes appear
        % as breaks in the shading rather than long bridges.
        [tt, lo] = insertGapBreaks(times, vals - err, gapFactor);
        [~,  hi] = insertGapBreaks(times, vals + err, gapFactor);
        % Split fill by NaN segments
        nanMask = isnat(tt) | isnan(lo) | isnan(hi);
        segStart = find(diff([true; nanMask]) == -1);
        segEnd   = find(diff([nanMask; true]) ==  1);
        c        = colors(satColorRow(sat), :);
        bandRgb  = 1 - 0.4*(1 - c);  % desaturated band color
        for s = 1:numel(segStart)
            si = segStart(s); ei = segEnd(s);
            if ei <= si, continue; end
            xx = [tt(si:ei); flipud(tt(si:ei))];
            yy = [lo(si:ei); flipud(hi(si:ei))];
            fill(ax, xx, yy, bandRgb, 'EdgeColor','none', 'FaceAlpha', 0.4, ...
                'HandleVisibility','off');
        end

        % Mean line, also gap-aware.
        [tt2, vv2] = insertGapBreaks(times, vals, gapFactor);
        plot(ax, tt2, vv2, '-', 'Color', c, 'LineWidth', 0.9);

        % Pivot marker overlay: a hollow black ring on every epoch where
        % this satellite was BSSD pivot.
        if showPivotMarkers && any(double(T_sub.is_pivot) > 0)
            pmask = double(T_sub.is_pivot) > 0;
            scatter(ax, T_sub.utc(pmask), vals(pmask), 20, ...
                'MarkerEdgeColor','k', 'MarkerFaceColor','none', ...
                'HandleVisibility','off');
        end

        % Compact stats annotation, top-right.
        finite = isfinite(vals);
        if any(finite)
            text(ax, 0.98, 0.95, ...
                sprintf('\\mu=%.3g  \\sigma=%.3g', mean(vals(finite)), std(vals(finite))), ...
                'Units','normalized', 'HorizontalAlignment','right', ...
                'VerticalAlignment','top','FontSize',7, ...
                'BackgroundColor','w','EdgeColor',[0.7 0.7 0.7],'Margin',1);
        end

        title(ax, sat, 'Interpreter','none');
        if mod(k-1, cols) == 0, ylabel(ax,'Iono (m)'); end
        if k > nPlot - cols, xlabel(ax,'GPS Time'); end
        hold(ax,'off');
    end

    if nSats > maxPerFig
        warning('plotIonosphere:tooManySats', ...
            'Only first %d / %d satellites shown in per-sat band figure.', ...
            maxPerFig, nSats);
    end
end

%% ================================================================
%  Figure 5: Sky plot per system colored by slant iono
%  ================================================================
if showSkyHeatmap && any(~isnan(double(T.azimuth_deg)))
    figure('Name','Sky plot - iono','Color','w','Position',[160 160 320*nSys+120 380]);
    tl = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
    title(tl, 'Sky plot colored by slant iono (m)');

    % Shared color scale across constellations for visual comparability.
    cMin = quantile(double(T.iono_m), 0.02);
    cMax = quantile(double(T.iono_m), 0.98);

    for col = 1:nSys
        sysName = systemsPresent{col};
        ax = polaraxes(tl);
        ax.Layout.Tile = col;
        hold(ax,'on');

        T_sys = T(T.system == sysName, :);
        if isempty(T_sys)
            title(ax, sysName);
            continue;
        end

        % Polar conventions: theta = 0 at North, increasing clockwise to
        % match the sky-plot convention used in GNSS texts.
        az_rad = deg2rad(double(T_sys.azimuth_deg));
        rho    = 90 - double(T_sys.elevation_deg);  % zenith angle, 0 at zenith
        c      = double(T_sys.iono_m);

        % Filter out rows with NaN geometry (older traces or missing az).
        ok = isfinite(az_rad) & isfinite(rho) & isfinite(c);
        polarscatter(ax, az_rad(ok), rho(ok), 14, c(ok), 'filled', ...
            'MarkerFaceAlpha', 0.55);

        ax.ThetaZeroLocation = 'top';
        ax.ThetaDir          = 'clockwise';
        ax.RLim              = [0 90];
        ax.RTick             = [0 30 60 90];
        ax.RTickLabel        = {'90','60','30','0'};  % elevation labels
        ax.ThetaTick         = 0:45:315;
        ax.ThetaTickLabel    = {'N','NE','E','SE','S','SW','W','NW'};
        title(ax, sysName);
        if isfinite(cMin) && isfinite(cMax) && cMax > cMin
            caxis(ax, [cMin cMax]); %#ok<CAXIS>
        end
    end
    cb = colorbar(ax,'Location','eastoutside');
    cb.Label.String = 'Slant iono (m)';
end

%% ================================================================
%  Summary statistics
%  ================================================================
if showSummary
    fprintf('\n=== Ionosphere Trace Summary ===\n');
    fprintf('File           : %s\n', fname);
    fprintf('Epochs         : %d unique\n', numel(unique(T.utc)));
    fprintf('Satellites     : %d (%s)\n', N, strjoin(satList, ', '));
    fprintf('Span (UTC)     : %s -> %s\n', ...
        string(min(T.utc)), string(max(T.utc)));

    fprintf('\n%-9s %-9s %6s %10s %10s %10s %10s %8s\n', ...
        'Satellite','System','N', ...
        'Mean(m)','Std(m)','MeanVTEC','MeanSigma','MeanEl');
    fprintf('%s\n', repmat('-', 1, 80));
    for k = 1:N
        sat = satList{k};
        idx = T.satellite == sat;
        if ~any(idx), continue; end
        vals = double(T.iono_m(idx));
        vtec = double(T.vtec_m(idx));
        sig  = double(T.std_m(idx));
        el   = double(T.elevation_deg(idx));
        sys  = string(T.system(find(idx,1,'first')));
        fprintf('%-9s %-9s %6d %10.3f %10.3f %10.3f %10.4f %8.2f\n', ...
            sat, sys, sum(idx), ...
            mean(vals,'omitnan'), std(vals,'omitnan'), ...
            mean(vtec,'omitnan'), mean(sig,'omitnan'), ...
            mean(el,'omitnan'));
    end

    % BSSD pivot summary: which satellites served as pivot, when.
    if any(double(T.is_pivot) > 0)
        fprintf('\n=== BSSD Pivot Tenure ===\n');
        pivotSats = unique(T.satellite(T.is_pivot > 0));
        for k = 1:numel(pivotSats)
            sat = pivotSats(k);
            tp  = T.utc(T.satellite == sat & T.is_pivot > 0);
            fprintf('  %s pivot for %d epochs : %s -> %s\n', ...
                string(sat), numel(tp), ...
                string(min(tp)), string(max(tp)));
        end
    end
end


%% ======================== LOCAL FUNCTIONS =======================

function plotIonoBySystem(T, valueColumn, valueLabel, figTitle, ...
                          systemsPresent, colors, satColorRow, satList, ...
                          markerList, plotStyle, gapFactor, ...
                          showPivotMarkers, showLegend, varargin)
% PLOTIONOBYSYSTEM  One tile per constellation, NaN-gap aware lines, one
% line per satellite colored by PRN.  Optional name-value pairs supported:
%   'YScale'            : 'linear' (default) or 'log' (sigma figure).
%   'Handovers'         : containers.Map (system -> datetime[]) of pivot
%                         handover epochs.  Empty = no annotations.
%   'ShowHandoverLines' : true to draw a thin vertical line at each
%                         handover in this system's tile.
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
    figHeight = 450;
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
        lh = gobjects(0);
        for k = 1:numel(satsHere)
            sat = satsHere{k};
            idx_sat = T_sub.satellite == sat;
            T_satObs = T_sub(idx_sat, :);
            T_satObs = sortrows(T_satObs, 'utc');

            times = T_satObs.utc;
            vals  = double(T_satObs.(valueColumn));
            if isempty(times), continue; end

            c = colors(satColorRow(sat), :);
            mk = markerList{mod(k-1, numel(markerList))+1};

            if strcmpi(plotStyle, 'lines')
                [tt, yy] = insertGapBreaks(times, vals, gapFactor);
                h = plot(ax, tt, yy, '-', 'Color', c, ...
                    'LineWidth', 0.9, 'DisplayName', sat);
            else
                h = scatter(ax, times, vals, 18, 'Marker', mk, ...
                    'MarkerEdgeColor', c, 'MarkerFaceColor', 'none', ...
                    'DisplayName', sat);
            end
            lh(end+1) = h; %#ok<AGROW>

            % Pivot marker overlay -- only applies to the slant figure
            % where rendering the ring on the *value* line is meaningful.
            if showPivotMarkers && strcmp(valueColumn, 'iono_m') ...
                                && any(double(T_satObs.is_pivot) > 0)
                pmask = double(T_satObs.is_pivot) > 0;
                scatter(ax, T_satObs.utc(pmask), vals(pmask), 22, ...
                    'MarkerEdgeColor','k', 'MarkerFaceColor','none', ...
                    'LineWidth', 0.8, 'HandleVisibility','off');
            end
        end

        % Pivot-handover vertical lines for this constellation.  Drawn
        % AFTER the per-sat traces so they sit on top, but with
        % HandleVisibility off so legend stays clean.
        if showHandoverLines && isKey(handovers, sysName)
            evs = handovers(sysName);
            for he = 1:numel(evs)
                xline(ax, evs(he), '-', 'Color', [0.3 0.3 0.3], ...
                    'LineWidth', 0.5, 'Alpha', 0.5, ...
                    'HandleVisibility', 'off');
            end
        end

        % Per-tile mu/sigma annotation, top-right.
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

        if showLegend && ~isempty(lh)
            legend(ax, lh, 'Location','bestoutside','FontSize',7, ...
                'Interpreter','none');
        end
        hold(ax,'off');
    end

    % Link x-axes so panning/zooming in one tile syncs the others.
    if numel(columnAxes) > 1
        linkaxes(columnAxes, 'x');
    end
end


function [tOut, yOut] = insertGapBreaks(tIn, yIn, gapFactor)
% INSERTGAPBREAKS  Insert NaT/NaN rows where the time series has a gap so
% that plot() draws a break rather than a line crossing the gap.  Cloned
% from plot_measurement_residuals.m so this script remains self-contained.
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


function mf = thinShellMappingFactor(el_deg)
% THINSHELLMAPPINGFACTOR  Single-layer ionosphere mapping factor
% (slant/vertical) at the IPP, matching the convention used by
% GlobalIonosphereMap.cpp in the C++ engine.  Earth radius 6371.23 km,
% shell height 450 km.
    Re = 6371230.0;
    H  = 450.0e3;
    el = el_deg(:) * pi/180;
    sin_zp = (Re / (Re + H)) * cos(el);
    cos_zp = sqrt(max(0.0, 1 - sin_zp.^2));
    mf = 1.0 ./ cos_zp;
    mf(~isfinite(el_deg(:)) | el_deg(:) <= 0) = NaN;
    mf = reshape(mf, size(el_deg));
end


function row = localColorRow(sat, satList, nColors)
% LOCALCOLORROW  Map a satellite ID to a color row, matching the
% convention in plot_measurement_residuals.m: parse PRN from chars 2-3,
% fall back to global index, wrap into the palette.
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
% pivot info (PPP, baseline DD, older traces).
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
        % A handover marks the FIRST epoch under the new pivot.  Skip the
        % implicit "first pivot" transition where the previous value was
        % the empty string (no pivot established yet).
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


function T = reReferenceIonoToFirstPivot(T)
% RE-REFERENCE BSSD iono states to the first pivot seen per constellation.
%
% In BSSD, the iono state for non-pivot sat X is Î_X = I_X - I_pivot.
% When the pivot rotates from A to B, every non-pivot row's value jumps
% by I_A - I_B and the pivot row snaps from one sat to another.  Both
% are presentation artefacts of the moving reference, not real iono.
%
% Algorithm: walk epochs in chronological order per constellation,
% accumulate a running offset O = I_pivot(now) - I_pivot(first).  At
% each handover P_old -> P_new, look up the new pivot's iono state at
% the LAST epoch in P_old's frame -- that value is I_P_new - I_P_old,
% which is exactly the increment to O.  Then apply O to every row at
% this epoch (the pivot row's raw value is 0, so it becomes O, i.e.
% I_pivot relative to the first-seen pivot).
%
% Caveat: std_m is NOT re-propagated.  Each chained offset adds a small
% variance term that the input table doesn't carry, so the displayed
% sigma is a lower bound for the absolute-frame uncertainty.  Acceptable
% for a diagnostic plot; documented in the script header.
    if ~ismember('pivot_satellite', T.Properties.VariableNames)
        return;
    end
    pivStr = string(T.pivot_satellite);
    if all(pivStr == "")
        return;
    end

    systems = categories(T.system);
    systems = systems(~strcmp(systems,'Unknown'));

    iono_out = double(T.iono_m);

    for s = 1:numel(systems)
        sysName = systems{s};
        rowsSys = find(T.system == sysName);
        if isempty(rowsSys), continue; end

        T_sys = T(rowsSys, :);
        [T_sys, sortIdx] = sortrows(T_sys, 'utc');
        rowsSysSorted = rowsSys(sortIdx);   % back-map sorted -> T row index

        [uEpochs, ~, ixEpoch] = unique(T_sys.utc);
        nE = numel(uEpochs);

        % Pivot at each unique epoch (assume consistent across rows).
        pivPerEpoch = strings(nE,1);
        for e = 1:nE
            firstRow = find(ixEpoch == e, 1, 'first');
            pivPerEpoch(e) = string(T_sys.pivot_satellite(firstRow));
        end

        % Skip constellations that never carried a non-empty pivot
        % (e.g. a non-BSSD constellation in a partial-BSSD run).
        if all(pivPerEpoch == "")
            continue;
        end

        % Walk epochs, accumulate offset, apply to rows.
        O = 0.0;
        for e = 1:nE
            if e > 1 ...
                    && pivPerEpoch(e) ~= pivPerEpoch(e-1) ...
                    && pivPerEpoch(e)   ~= "" ...
                    && pivPerEpoch(e-1) ~= ""
                % Pivot changed.  Look up the new pivot's iono value at
                % the previous epoch (it was a non-pivot then, so its
                % iono_m carries I_newpivot - I_oldpivot in the old
                % frame).  Add to running offset.
                prevRows = ixEpoch == (e-1) ...
                           & string(T_sys.satellite) == pivPerEpoch(e);
                if any(prevRows)
                    O = O + double(T_sys.iono_m(find(prevRows, 1, 'first')));
                end
                % If the new pivot wasn't visible at the previous epoch
                % we can't bridge the gap; O stays where it was and a
                % residual jump remains.  This is rare in practice
                % since pivot selection requires current visibility.
            end

            % Apply O to every row at this epoch (in T_sys index space).
            thisEpochMaskSorted = (ixEpoch == e);
            origIdx = rowsSysSorted(thisEpochMaskSorted);
            iono_out(origIdx) = double(T.iono_m(origIdx)) + O;
            % Note: the pivot row's raw iono_m is 0 (BSSD-frozen), so
            % the result is exactly O -- the pivot's iono delay in the
            % global reference frame, which now reads as a continuous
            % line across the handover instead of snapping to the new
            % pivot's row.
        end
    end

    T.iono_m = iono_out;

    % VTEC mirrors iono_m through the mapping factor; recompute so the
    % VTEC figure also reads as continuous.
    if ismember('mapping_factor', T.Properties.VariableNames) ...
            && ismember('vtec_m', T.Properties.VariableNames)
        T.vtec_m = T.iono_m ./ double(T.mapping_factor);
    end
end
