%% ============================================================
%  Script: plotDops.m
%  Purpose: Visualize per-epoch satellite counts (per-system + total)
%           and dilution-of-precision indicators.
%
%  Reads dop_trace.csv (one row per epoch) and produces three figures:
%
%    Figure 1: Sat counts -- stacked area per-constellation + bold
%              "total" overlay.  Quick read on geometry availability
%              and constellation-mix evolution.
%
%    Figure 2: Position DOPs (PDOP, HDOP, VDOP, TDOP, GDOP) over time
%              with a horizontal reference at PDOP=6 (the conventional
%              "good geometry" threshold).
%
%    Figure 3: Ambiguity DOP (Teunissen ADOP) on log y-axis, with the
%              number-of-ambiguities overlaid on a secondary y-axis
%              so the user can correlate ADOP drops with reseeds.
%
%  Expected CSV header:
%      week,tow_s,n_used_total,
%      n_used_gps,n_used_galileo,n_used_glonass,
%      n_used_beidou,n_used_qzss,n_used_sbas,
%      pdop,hdop,vdop,tdop,gdop,
%      adop_m,n_ambiguities
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
showSatCounts = true;
showPosDops   = true;
showADop      = true;
showSummary   = true;

% PDOP threshold line on Figure 2.  6.0 is the conventional "good"
% geometry boundary; values above suggest weak geometry.
pdopThreshold = 6.0;

%% --- Load CSV ---
fname = 'C:\GnssProcessor\logs\dop_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Release\logs\dop_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\dop_trace.csv';

if ~isfile(fname)
    error('dop_trace.csv not found: %s', fname);
end

T = readtable(fname, 'TextType', 'string');

needed = {'week','tow_s','n_used_total','pdop','adop_m'};
for i = 1:numel(needed)
    if ~ismember(needed{i}, T.Properties.VariableNames)
        error('dop_trace.csv missing required column: %s', needed{i});
    end
end

%% --- Convert GPS week + TOW to a datetime on the GPS time scale ---
% Trace files carry pure GPS time so timestamps match RINEX epochs.
gpsEpoch = datetime(1980,1,6,0,0,0);
T.utc = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));
T = sortrows(T, 'utc');

%% --- Canonical system colors (match plotSatelliteStatus.m where useful) ---
% Order matches the column layout in the CSV.
sysCols = {
    'GPS',     'n_used_gps',     [0.15 0.45 0.85];   % blue
    'Galileo', 'n_used_galileo', [0.20 0.65 0.30];   % green
    'GLONASS', 'n_used_glonass', [0.85 0.20 0.20];   % red
    'BeiDou',  'n_used_beidou',  [0.55 0.30 0.75];   % purple
    'QZSS',    'n_used_qzss',    [0.95 0.55 0.10];   % orange
    'SBAS',    'n_used_sbas',    [0.50 0.50 0.50];   % gray
};

% Filter to systems that ever had a non-zero used-sat count.
sysPresent = false(size(sysCols,1), 1);
for s = 1:size(sysCols,1)
    col = sysCols{s,2};
    if ismember(col, T.Properties.VariableNames) && any(double(T.(col)) > 0)
        sysPresent(s) = true;
    end
end
if ~any(sysPresent)
    warning('plotDops: no per-system sat-count columns found; sat-count figure will be empty.');
end
sysCols = sysCols(sysPresent, :);
nSys = size(sysCols, 1);

%% ================================================================
%  Figure 1: Satellite counts -- stacked area + total overlay
%  ================================================================
if showSatCounts
    figure('Name','Used satellites per epoch','Color','w', ...
           'Position',[60 60 1400 460]);
    ax = axes; hold(ax,'on'); grid(ax,'on'); box(ax,'on');

    % Build the per-system matrix in canonical column order so the
    % stacked area is reproducible across runs.
    counts = zeros(height(T), nSys);
    for s = 1:nSys
        counts(:, s) = double(T.(sysCols{s,2}));
    end

    % area() handles stacking automatically.  Use 'EdgeColor','none' for
    % a clean look; per-band colour comes from sysCols.
    h = area(ax, T.utc, counts, 'EdgeColor','none', 'FaceAlpha', 0.85);
    for s = 1:nSys
        h(s).FaceColor = sysCols{s,3};
    end

    % Bold black line for the total (== sum of stacks, but explicit so a
    % future schema change can't desync the area and the overlay).
    plot(ax, T.utc, double(T.n_used_total), '-k', 'LineWidth', 1.5, ...
        'DisplayName','Total');

    legendEntries = [sysCols(:,1); {'Total'}];
    legend(ax, legendEntries, 'Location','bestoutside', 'Interpreter','none');

    xlabel(ax,'GPS Time');
    ylabel(ax,'Number of used satellites');
    title(ax, 'Used satellites per epoch (per system, stacked) + total');
    ax.YLim = [0 max([max(double(T.n_used_total)) + 2, 8])];
    hold(ax,'off');
end

%% ================================================================
%  Figure 2: Position DOPs (PDOP/HDOP/VDOP/TDOP/GDOP)
%  ================================================================
if showPosDops
    figure('Name','Position DOPs','Color','w', ...
           'Position',[80 80 1400 460]);
    ax = axes; hold(ax,'on'); grid(ax,'on'); box(ax,'on');

    dops = {
        'pdop', 'PDOP', [0.85 0.20 0.20], 1.6;    % red, thick
        'gdop', 'GDOP', [0.45 0.10 0.10], 1.2;    % dark red
        'hdop', 'HDOP', [0.15 0.45 0.85], 1.0;    % blue
        'vdop', 'VDOP', [0.20 0.65 0.30], 1.0;    % green
        'tdop', 'TDOP', [0.50 0.50 0.50], 0.8;    % gray
    };

    for d = 1:size(dops,1)
        col = dops{d,1};
        if ~ismember(col, T.Properties.VariableNames), continue; end
        vals = double(T.(col));
        plot(ax, T.utc, vals, '-', ...
            'Color', dops{d,3}, 'LineWidth', dops{d,4}, ...
            'DisplayName', dops{d,2});
    end

    % "Good geometry" reference line.  Use yline if available; fall
    % back to a constant plot otherwise.
    yline(ax, pdopThreshold, '--', sprintf('PDOP = %.1f', pdopThreshold), ...
        'Color', [0.6 0.6 0.6], 'LabelHorizontalAlignment','left', ...
        'HandleVisibility','off');

    legend(ax, 'Location','bestoutside');
    xlabel(ax,'GPS Time');
    ylabel(ax,'DOP (unitless)');
    title(ax, 'Position dilution-of-precision indicators');

    % Tight bound at the bottom; the top auto-scales so transient
    % weak-geometry epochs aren't clipped.
    ax.YLim(1) = 0;
    hold(ax,'off');
end

%% ================================================================
%  Figure 3: Ambiguity DOP (Teunissen ADOP), log y, with N-amb overlay
%  ================================================================
if showADop
    figure('Name','Ambiguity DOP (ADOP)','Color','w', ...
           'Position',[100 100 1400 460]);
    ax1 = axes; hold(ax1,'on'); grid(ax1,'on'); box(ax1,'on');

    adop = double(T.adop_m);
    adop(~isfinite(adop) | adop <= 0) = NaN;
    plot(ax1, T.utc, adop, '-', ...
        'Color', [0.15 0.35 0.75], 'LineWidth', 1.2, ...
        'DisplayName','ADOP (m)');

    set(ax1, 'YScale','log');
    xlabel(ax1,'GPS Time');
    ylabel(ax1, 'ADOP (m, log scale)');
    title(ax1, 'Teunissen ambiguity DOP \cdot (det Q_a)^{1/(2n)} \cdot meters');

    % Secondary y-axis: number of ambiguities (counts, linear scale).
    % yyaxis if available (R2016a+); otherwise stick with one axis.
    if exist('yyaxis','file')
        yyaxis(ax1,'right');
        if ismember('n_ambiguities', T.Properties.VariableNames)
            nAmb = double(T.n_ambiguities);
            plot(ax1, T.utc, nAmb, '-', ...
                'Color', [0.7 0.7 0.7], 'LineWidth', 0.7, ...
                'DisplayName','# ambiguities (ADOP basis)');
        end
        ylabel(ax1,'# ambiguities (right axis)');
        yyaxis(ax1,'left');
    end

    legend(ax1,'Location','bestoutside');
    hold(ax1,'off');
end

%% --- Summary statistics ---
if showSummary
    fprintf('\n=== DOP / Sat-Count Trace Summary ===\n');
    fprintf('File           : %s\n', fname);
    fprintf('Epochs         : %d\n', height(T));
    fprintf('Span (UTC)     : %s -> %s\n', string(T.utc(1)), string(T.utc(end)));

    fprintf('\n%-9s %8s %8s %8s\n', 'Metric','mean','median','max');
    fprintf('%s\n', repmat('-', 1, 40));
    metrics = {
        'n_used_total', 'Total sats';
        'pdop',         'PDOP';
        'hdop',         'HDOP';
        'vdop',         'VDOP';
        'tdop',         'TDOP';
        'gdop',         'GDOP';
        'adop_m',       'ADOP (m)';
    };
    for m = 1:size(metrics,1)
        col = metrics{m,1};
        if ~ismember(col, T.Properties.VariableNames), continue; end
        vals = double(T.(col));
        vals = vals(isfinite(vals));
        if isempty(vals), continue; end
        fprintf('%-12s %8.3f %8.3f %8.3f\n', metrics{m,2}, ...
            mean(vals), median(vals), max(vals));
    end

    % Per-constellation mean sat count.
    fprintf('\n=== Mean used sats per constellation ===\n');
    for s = 1:nSys
        col = sysCols{s,2};
        if ~ismember(col, T.Properties.VariableNames), continue; end
        vals = double(T.(col));
        fprintf('  %-8s : %.2f\n', sysCols{s,1}, mean(vals));
    end

    % Geometry-quality flags.
    if ismember('pdop', T.Properties.VariableNames)
        pdop = double(T.pdop);
        nWeak = sum(pdop > pdopThreshold & isfinite(pdop));
        nGood = sum(pdop <= pdopThreshold & isfinite(pdop));
        if nWeak + nGood > 0
            fprintf('\nPDOP <= %.1f : %d epochs (%.1f%%)\n', ...
                pdopThreshold, nGood, 100*nGood/(nWeak+nGood));
            fprintf('PDOP >  %.1f : %d epochs (%.1f%%)\n', ...
                pdopThreshold, nWeak, 100*nWeak/(nWeak+nGood));
        end
    end
end
