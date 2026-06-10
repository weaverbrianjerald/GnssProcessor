%% ============================================================
%  Script: plotResidualStrip.m
%  Purpose: Per-satellite measurement-residual strip chart, styled
%           after plotSatelliteStatus.m.
%
%  One figure per (constellation, code/phase) -- e.g.:
%      "GPS Code"     : C1W, C2W, C5Q, ...
%      "GPS Phase"    : L1C, L2W, L5Q, ...
%      "Galileo Code" : C1C, C5Q, C7Q, ...
%      ...
%  Each figure stacks one strip-chart tile per signal of that
%  measurement type.  Each tile is an imagesc grid of
%  (satellite x epoch) cells colored by the residual value.  Sats
%  not seen on that (cons, signal) at a given epoch appear as
%  transparent gaps; BSSD pivots show as the neutral mid-colour
%  (zero residual) so their tenure is visible alongside the
%  non-pivot rows.
%
%  Splitting code and phase keeps each tile tall enough to read
%  individual satellite rows -- the previous one-figure-per-cons
%  layout stacked ~8-10 tiles and buried the per-cell colour.
%
%  Expected CSV header (matches plot_measurement_residuals.m):
%      week,tow_s,satellite,signal,innovation_m,residual_m,
%      residual_std_m,standardized_residual
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
% Which column to render in the strip:
%   'standardized' : standardized_residual (sigma units, naturally
%                    normalized -- best default for cross-signal view)
%   'residual'     : residual_m  (raw post-fit residual in meters)
%   'innovation'   : innovation_m (pre-fit residual in meters)
plotValue = 'standardized';
%plotValue = 'residual';

% caxis bounds for each mode.  Bigger limit -> finer colour resolution
% near zero; smaller limit -> more saturation on outliers.
caxisLimitStandardized = 5.0;     % +/- sigma
caxisLimitMeters       = 0.010;    % +/- m   (auto-overridden by signal type)
% When in 'residual' or 'innovation' mode, code rows get a 10x looser
% limit than phase rows so the visual scale is roughly comparable.
codeScaleFactor        = 10.0;

% Mark BSSD pivot rows: synthesize residual=0 cells so pivots show as
% neutral mid-colour during their tenure, not transparent gaps.  Set
% false to inspect the raw post-BSSD measurement set without pivots.
showPivotRows = true;

% Discrete vs. continuous colorbar.  The continuous diverging map is
% better for spotting outliers; discrete is occasionally preferred to
% match the look of plotSatelliteStatus.m.  Default continuous.
useDiscreteColors = false;

showSummary = true;

%% --- Load CSV ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\measurement_residuals.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\measurement_residuals.csv';

if ~isfile(fname)
    error('measurement_residuals.csv not found: %s', fname);
end

T = readtable(fname, 'TextType', 'string');

needed = {'week','tow_s','satellite','signal','residual_m','standardized_residual'};
for i = 1:numel(needed)
    if ~ismember(needed{i}, T.Properties.VariableNames)
        error('measurement_residuals.csv missing required column: %s', needed{i});
    end
end

%% --- BSSD compound ID handling ---
% C++ residual logger writes "<primary>-<pivot>" for BSSD rows (see
% MeasurementModel.cpp:2114 and KalmanFilter::splitBssdSatId).  Strip
% to the primary so the same physical sat reads as one row across
% pivot handovers; keep the pivot in a sidecar column for the
% optional pivot-row synthesis below.
satRaw = string(T.satellite);
hasDash = contains(satRaw, '-');
T.pivot = strings(height(T), 1);
T.pivot(hasDash) = extractAfter(satRaw(hasDash), '-');
satRaw(hasDash) = extractBefore(satRaw(hasDash), '-');
T.satellite = satRaw;

%% --- Synthesize 0-residual rows for active pivots ---
% Without this, the pivot's row in the strip chart goes transparent
% during its tenure (it has no SD residual against itself).  Inserting
% one synthetic row per unique (week, tow_s, pivot, signal) at
% residual=0 keeps the pivot's row continuous and immediately
% distinguishes "pivot at this epoch" from "not tracked at this
% epoch" (which stays transparent).
if showPivotRows
    bssdMask = T.pivot ~= "";
    if any(bssdMask)
        Tb = T(bssdMask, :);
        [G, ~, ~, pivot_g, ~] = findgroups( ...
            Tb.week, Tb.tow_s, string(Tb.pivot), string(Tb.signal));
        firstIdx = splitapply(@(x) x(1), (1:height(Tb))', G);
        syn = Tb(firstIdx, :);
        syn.satellite = pivot_g;
        syn.residual_m(:) = 0;
        if ismember('innovation_m', syn.Properties.VariableNames)
            syn.innovation_m(:) = 0;
        end
        if ismember('standardized_residual', syn.Properties.VariableNames)
            syn.standardized_residual(:) = 0;
        end
        if ismember('residual_std_m', syn.Properties.VariableNames)
            syn.residual_std_m(:) = NaN;
        end
        syn.pivot = strings(height(syn), 1);
        T = [T; syn];
        T = sortrows(T, {'week', 'tow_s'});
    end
end

%% --- Convert GPS week + TOW to a datetime on the GPS time scale ---
% Trace files carry pure GPS time so timestamps match RINEX epochs.
gpsEpoch = datetime(1980,1,6,0,0,0);
T.utc = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));

%% --- Tag each row with a system ---
satStr = string(T.satellite);
T.system = repmat("Unknown", height(T), 1);
T.system(startsWith(satStr,'G')) = "GPS";
T.system(startsWith(satStr,'E')) = "Galileo";
T.system(startsWith(satStr,'R')) = "GLONASS";
T.system(startsWith(satStr,'C')) = "BeiDou";
T.system(startsWith(satStr,'J')) = "QZSS";
T.system(startsWith(satStr,'S')) = "SBAS";

%% --- Resolve which CSV column to render + axis label ---
switch lower(plotValue)
    case 'standardized'
        valueCol  = 'standardized_residual';
        valueLbl  = 'Standardized residual (\sigma)';
        caxisLim  = caxisLimitStandardized;
        perSigCax = false;
    case 'residual'
        valueCol  = 'residual_m';
        valueLbl  = 'Residual (m)';
        caxisLim  = caxisLimitMeters;
        perSigCax = true;   % code vs phase use different scales
    case 'innovation'
        if ~ismember('innovation_m', T.Properties.VariableNames)
            error('innovation_m column not present in this CSV');
        end
        valueCol  = 'innovation_m';
        valueLbl  = 'Innovation (m)';
        caxisLim  = caxisLimitMeters;
        perSigCax = true;
    otherwise
        error('plotValue must be ''standardized'', ''residual'', or ''innovation''');
end

%% --- Diverging colormap (blue -> white -> red) ---
% Symmetric around zero so the sign of the residual is obvious at a
% glance.  256 entries gives smooth gradient for continuous values;
% the optional discrete version snaps to a coarser palette that
% mimics plotSatelliteStatus.m's discrete legend.
if useDiscreteColors
    statusColors = [
        0.10 0.30 0.75;   % strong negative
        0.40 0.60 0.90;
        0.75 0.85 0.95;
        0.95 0.95 0.95;   % near zero (white)
        0.95 0.80 0.75;
        0.90 0.40 0.40;
        0.75 0.10 0.10;   % strong positive
    ];
else
    n = 256;
    half = floor(n/2);
    bluePart = [linspace(0.10,1.00,half)' linspace(0.30,1.00,half)' linspace(0.75,1.00,half)'];
    redPart  = [linspace(1.00,0.75,n-half)' linspace(1.00,0.10,n-half)' linspace(1.00,0.10,n-half)'];
    statusColors = [bluePart; redPart];
end

%% --- Canonical system render order ---
sysOrder   = {'GPS','Galileo','GLONASS','BeiDou','QZSS','SBAS'};
sysPresent = {};
for s = 1:numel(sysOrder)
    if any(T.system == string(sysOrder{s}))
        sysPresent{end+1} = sysOrder{s}; %#ok<AGROW>
    end
end
if isempty(sysPresent)
    error('No recognized GNSS systems found in measurement_residuals.csv');
end

% Shared epoch axis across all figures (lets the user stack windows
% and compare time slices across constellations).
epochs = unique(T.utc);
nEpochs = numel(epochs);

%% ================================================================
%  One figure per (constellation, code/phase), tiled per signal.
%
%  Splitting code and phase into separate figures keeps each tile
%  large enough to read individual satellite rows -- the previous
%  combined layout had ~8-10 tiles per constellation, each ~80 px
%  tall, which buried the per-cell colour.
%
%  Side benefit: per-figure caxis becomes uniform (each figure is
%  single-type), so the right-hand colorbar reads correctly across
%  every tile in the figure and the per-signal caxis hack from the
%  combined version goes away.
%  ================================================================
measTypes = { ...
    'Code',  'C', codeScaleFactor;   % code signals start with 'C'
    'Phase', 'L', 1.0;               % phase signals start with 'L'
};
figIdx = 0;
for s = 1:numel(sysPresent)
    sysName = sysPresent{s};
    rowsSys = T.system == string(sysName);
    T_sys = T(rowsSys, :);
    if isempty(T_sys), continue; end

    for m = 1:size(measTypes,1)
        measLabel  = measTypes{m,1};
        measPrefix = measTypes{m,2};
        measScale  = measTypes{m,3};

        % Filter to (constellation, code/phase) and grab the signals
        % present, lex-sorted so the typical L1*/L2*/L5* (or C1*/C2*/
        % C5*) ordering reads top-to-bottom inside the figure.
        sigMask = startsWith(string(T_sys.signal), measPrefix);
        T_meas = T_sys(sigMask, :);
        if isempty(T_meas), continue; end
        sigs = sort(unique(string(T_meas.signal)));
        sigs = sigs(~ismissing(sigs) & sigs ~= "");
        nSigs = numel(sigs);
        if nSigs == 0, continue; end

        % Figure sized for the actual tile count.  Each tile gets a
        % generous slice of vertical space (~170 px) so satellite-row
        % stripes are legible; cap total height for monitor fit.
        figHeight = min(950, 170*nSigs + 140);
        figIdx = figIdx + 1;
        fig = figure('Name', sprintf('Residuals -- %s %s', sysName, measLabel), ...
                     'Color','w', ...
                     'Position', [60+(figIdx-1)*25 60+(figIdx-1)*25 1400 figHeight]);
        tl = tiledlayout(fig, nSigs, 1, 'TileSpacing','tight','Padding','compact');
        title(tl, sprintf('%s %s residuals (%s)', sysName, measLabel, valueLbl));

        % Per-figure caxis.  Standardized mode uses one global sigma
        % limit; raw residual / innovation modes scale by code vs
        % phase (single multiplier, since the whole figure is one
        % meas type).
        if perSigCax
            figCaxis = caxisLim * measScale;
        else
            figCaxis = caxisLim;
        end

        columnAxes = gobjects(0);

        for r = 1:nSigs
            sig = sigs(r);
            idx = string(T_meas.signal) == sig;
            T_sub = T_meas(idx, :);
            if isempty(T_sub), continue; end

            % Satellites in this (sys, sig) tile, PRN-sorted.
            satsHere = unique(string(T_sub.satellite));
            prn = nan(numel(satsHere),1);
            for k = 1:numel(satsHere)
                ch = char(satsHere(k));
                if numel(ch) >= 3, prn(k) = str2double(ch(2:3)); end
            end
            [~, ord] = sortrows([prn (1:numel(satsHere))'], [1 2]);
            satsHere = satsHere(ord);
            nSats = numel(satsHere);

            % Build (sat x epoch) value grid.  NaN -> transparent.
            % Duplicate (sat, sig, epoch) rows (e.g. relinearization
            % pass) take the last value -- harmless for visualization.
            G = nan(nSats, nEpochs);
            [~, satIdx]   = ismember(string(T_sub.satellite), satsHere);
            [~, epochIdx] = ismember(T_sub.utc, epochs);
            keep = satIdx > 0 & epochIdx > 0;
            linIdx = sub2ind([nSats nEpochs], satIdx(keep), epochIdx(keep));
            vals = double(T_sub.(valueCol)(keep));
            G(linIdx) = vals;

            ax = nexttile(tl); hold(ax,'on');
            columnAxes(end+1) = ax; %#ok<AGROW>

            imH = imagesc(ax, [epochs(1) epochs(end)], [1 nSats], G);
            set(imH, 'AlphaData', ~isnan(G));

            colormap(ax, statusColors);
            caxis(ax, [-figCaxis, figCaxis]);  %#ok<CAXIS>
            ax.YDir = 'reverse';
            ax.YTick = 1:nSats;
            ax.YTickLabel = cellstr(satsHere);
            ax.YLim = [0.5 nSats+0.5];
            ax.XLim = [epochs(1) epochs(end)];
            ax.TickLabelInterpreter = 'none';
            grid(ax,'on');
            box(ax,'on');

            if r == nSigs
                xlabel(ax,'GPS Time');
            else
                ax.XTickLabel = [];
            end
            ylabel(ax, sig);

            % Per-tile mu/sigma annotation, top-right.  Excludes
            % synthetic pivot rows (residual_std_m is NaN there) so
            % stats reflect real BSSD residuals only.
            if ismember('residual_std_m', T_sub.Properties.VariableNames)
                realMask = ~isnan(T_sub.residual_std_m);
            else
                realMask = true(height(T_sub),1);
            end
            statVals = double(T_sub.(valueCol)(realMask));
            statVals = statVals(isfinite(statVals));
            if ~isempty(statVals)
                text(ax, 0.995, 0.95, ...
                    sprintf('\\mu=%.3g  \\sigma=%.3g  N=%d', ...
                            mean(statVals), std(statVals), numel(statVals)), ...
                    'Units','normalized', 'HorizontalAlignment','right', ...
                    'VerticalAlignment','top','FontSize',8, ...
                    'BackgroundColor','w', 'EdgeColor',[0.7 0.7 0.7], ...
                    'Margin',1);
            end

            hold(ax,'off');
        end

        % One shared colorbar on the right.  caxis is uniform across
        % all tiles in this figure (single meas type), so the legend
        % is universally meaningful.
        if ~isempty(columnAxes)
            cb = colorbar(columnAxes(end), 'Location','eastoutside');
            cb.Label.String = valueLbl;
            if strcmpi(plotValue,'standardized')
                cb.Ticks = [-figCaxis -3 0 3 figCaxis];
            end
        end

        if numel(columnAxes) > 1
            linkaxes(columnAxes, 'x');
        end
    end
end

%% --- Summary statistics ---
if showSummary
    fprintf('\n=== Residual Strip Summary ===\n');
    fprintf('File           : %s\n', fname);
    fprintf('Epochs         : %d\n', nEpochs);
    fprintf('Span (GPS)     : %s -> %s\n', string(epochs(1)), string(epochs(end)));
    fprintf('Mode           : %s (%s)\n', plotValue, valueCol);

    % Per (system, signal) summary -- mirror the tile layout in text.
    fprintf('\n%-9s %-6s %8s %10s %10s %8s\n', ...
        'System','Signal','N','Mean','Sigma','|sres|>3 %');
    fprintf('%s\n', repmat('-', 1, 60));
    for s = 1:numel(sysPresent)
        sysName = sysPresent{s};
        T_sys = T(T.system == string(sysName), :);
        if isempty(T_sys), continue; end
        sigs = sort(unique(string(T_sys.signal)));
        sigs = sigs(~ismissing(sigs) & sigs ~= "");
        for k = 1:numel(sigs)
            sig = sigs(k);
            mask = string(T_sys.signal) == sig;
            if ismember('residual_std_m', T_sys.Properties.VariableNames)
                mask = mask & ~isnan(T_sys.residual_std_m);
            end
            if ~any(mask), continue; end
            vals = double(T_sys.(valueCol)(mask));
            sres = double(T_sys.standardized_residual(mask));
            vals = vals(isfinite(vals));
            sres = sres(isfinite(sres));
            if isempty(vals), continue; end
            pctOutlier = 0;
            if ~isempty(sres)
                pctOutlier = 100 * sum(abs(sres) > 3) / numel(sres);
            end
            fprintf('%-9s %-6s %8d %10.4f %10.4f %7.2f%%\n', ...
                sysName, sig, numel(vals), mean(vals), std(vals), pctOutlier);
        end
    end
end
