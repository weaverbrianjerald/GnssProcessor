clear all; %clc; %close all;




fname = 'C:\GnssProcessor\build\x64-Release\logs\gf_slip_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\gf_slip_trace.csv';

plot_gf_trace(fname);

%%

% plot_gf_trace.m
% Read and visualize gf_slip_trace.csv (GPS time axis)
%
% Expected CSV header (written by GfSlipDetector):
%   week,tow_s,satellite,system,elevation_deg,
%   sig1,sig2,gf_m,td2_gf_m,
%   mad_sigma_m,threshold_m,arc_count,
%   ivi_m,iono_stable,
%   is_slip,suppressed
%
% Produces four figures:
%   1. Raw GF observable (L1-L2) [meters]
%   2. TD2 GF (second-order time-differenced GF) with ±threshold,
%      slip markers, and suppressed markers
%   3. MAD sigma and arc count diagnostics
%   4. Ionospheric Variability Index (IVI)
%
% Usage:
%   plot_gf_trace();       % default Release path
%   plot_gf_trace(fname);
function plot_gf_trace(fname)

if nargin < 1 || isempty(fname)
    fname = fullfile('C:','GnssProcessor','build','x64-Release','logs','gf_slip_trace.csv');
end

if ~isfile(fname)
    error('GF trace CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

% Require the columns we need
vars = lower(string(T.Properties.VariableNames));
need = ["week","tow_s","satellite","system","sig1","sig2",...
        "gf_m","td2_gf_m","threshold_m","is_slip","suppressed",...
        "ivi_m","iono_stable"];
missing = need(~ismember(need, vars));
if ~isempty(missing)
    error('CSV missing columns: %s', strjoin(missing,', '));
end

% Convert numerics
T.week          = double(T.week);
T.tow_s         = double(T.tow_s);
T.elevation_deg = double(T.elevation_deg);
T.gf_m          = double(T.gf_m);
T.td2_gf_m      = double(T.td2_gf_m);
T.mad_sigma_m   = double(T.mad_sigma_m);
T.threshold_m   = double(T.threshold_m);
T.arc_count     = double(T.arc_count);
T.ivi_m         = double(T.ivi_m);
T.iono_stable   = double(T.iono_stable);
T.is_slip       = double(T.is_slip);
T.suppressed    = double(T.suppressed);

% Replace sentinel values (-1) with NaN for cleaner plots
T.mad_sigma_m(T.mad_sigma_m < 0)   = NaN;
T.threshold_m(T.threshold_m < 0)   = NaN;

%% --- Build a per-arc key: "G01_L1C_L2W" ---
T.key = T.satellite + "_" + T.sig1 + "_" + T.sig2;
T.key = string(T.key);

% Normalize system as categorical
T.system = categorical(string(T.system));

%% --- Convert GPS week + TOW to datetime (UTC) ---
gpsEpoch   = datetime(1980,1,6,0,0,0);
gpsSeconds = T.week * 7 * 86400 + T.tow_s;
T.utc      = gpsEpoch + seconds(gpsSeconds);

%% --- Shared color/marker setup ---
satCats = unique(categorical(T.satellite));
N = numel(satCats);
try
    colors = distinguishable_colors(max(8,N));
catch
    colors = lines(max(8,N));
end
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

systemList = categories(T.system);

%% =====================================================================
%%  Figure 1: Raw GF time series (meters) — one tile per constellation
%% =====================================================================
figure('Name','GF: Raw GF (L1-L2)','Color','w','Position',[80 400 1400 450]);
tl1 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl1, 'Geometry-Free Observable  GF = L1 - L2  (m)');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    keysInSys = unique(T_sys.key);
    lh = gobjects(0);
    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);

        [c, m] = key_style(thisKey, satCats, colors, markerList, k);

        scatter(T_sys.utc(idx_key), T_sys.gf_m(idx_key), 18, ...
            'Marker', m, 'MarkerEdgeColor','none', 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.6);

        h = plot(T_sys.utc(1), NaN, 'Marker', m, 'LineStyle','none', ...
            'Color', c, 'DisplayName', thisKey);
        set(h,'Visible','off');
        lh(end+1) = h; %#ok<AGROW>
    end

    xlabel('GPS Time'); ylabel('GF  (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none','FontSize',7);
    datetick('x','keeplimits');
    hold off;
end

%% =====================================================================
%%  Figure 2: TD2 GF with ±threshold, slips, and suppressed detections
%% =====================================================================
figure('Name','GF: TD2 GF + Threshold','Color','w','Position',[80 100 1400 450]);
tl2 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl2, '2nd-Order Time-Differenced GF  (TD2\_GF)  with Adaptive Threshold');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    keysInSys = unique(T_sys.key);
    lh = gobjects(0);
    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);

        [c, m] = key_style(thisKey, satCats, colors, markerList, k);

        % TD2 GF values
        scatter(T_sys.utc(idx_key), T_sys.td2_gf_m(idx_key), 18, ...
            'Marker', m, 'MarkerEdgeColor','none', 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.6);

        % ±Threshold envelope (dashed lines, same color but lighter)
        clite = min(c + 0.4, 1.0);
        plot(T_sys.utc(idx_key),  T_sys.threshold_m(idx_key), ...
            '-', 'Color', [clite 0.4], 'LineWidth', 0.5, 'HandleVisibility','off');
        plot(T_sys.utc(idx_key), -T_sys.threshold_m(idx_key), ...
            '-', 'Color', [clite 0.4], 'LineWidth', 0.5, 'HandleVisibility','off');

        % Mark confirmed slips with a red X
        idx_slip = idx_key & (T_sys.is_slip == 1);
        if any(idx_slip)
            scatter(T_sys.utc(idx_slip), T_sys.td2_gf_m(idx_slip), 120, ...
                'rx', 'LineWidth', 2, 'HandleVisibility','off');
        end

        % Mark suppressed detections with orange diamond
        idx_supp = idx_key & (T_sys.suppressed == 1);
        if any(idx_supp)
            scatter(T_sys.utc(idx_supp), T_sys.td2_gf_m(idx_supp), 80, ...
                'd', 'MarkerEdgeColor',[1 0.6 0], 'MarkerFaceColor','none', ...
                'LineWidth', 1.5, 'HandleVisibility','off');
        end

        h = plot(T_sys.utc(1), NaN, 'Marker', m, 'LineStyle','none', ...
            'Color', c, 'DisplayName', thisKey);
        set(h,'Visible','off');
        lh(end+1) = h; %#ok<AGROW>
    end

    xlabel('GPS Time'); ylabel('TD2\_GF  (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none','FontSize',7);
    datetick('x','keeplimits');
    hold off;
end

%% =====================================================================
%%  Figure 3: MAD sigma and arc count (diagnostics)
%% =====================================================================
figure('Name','GF: MAD Sigma & Arc Count','Color','w','Position',[80 -200 1400 450]);
tl3 = tiledlayout(2, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl3, 'GF Detector Diagnostics');

% Row 1: MAD sigma
for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile(col); hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title([sysName ' MAD \sigma']); continue;
    end

    keysInSys = unique(T_sys.key);
    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);
        [c, m] = key_style(thisKey, satCats, colors, markerList, k);
        scatter(T_sys.utc(idx_key), T_sys.mad_sigma_m(idx_key), 12, ...
            'Marker', m, 'MarkerEdgeColor','none', 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.5);
    end

    xlabel('GPS Time'); ylabel('MAD \sigma  (m)');
    title([sysName ' — MAD \sigma']);
    datetick('x','keeplimits');
    hold off;
end

% Row 2: Arc count
for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile(numel(systemList) + col); hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title([sysName ' Arc Count']); continue;
    end

    keysInSys = unique(T_sys.key);
    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);
        [c, m] = key_style(thisKey, satCats, colors, markerList, k);
        scatter(T_sys.utc(idx_key), T_sys.arc_count(idx_key), 12, ...
            'Marker', m, 'MarkerEdgeColor','none', 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.5);
    end

    xlabel('GPS Time'); ylabel('Arc Count');
    title([sysName ' — Arc Count']);
    datetick('x','keeplimits');
    hold off;
end

%% =====================================================================
%%  Figure 4: Ionospheric Variability Index (IVI) — one tile per system
%% =====================================================================
figure('Name','GF: Ionospheric Variability Index','Color','w','Position',[80 -500 1400 350]);
tl4 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl4, 'Ionospheric Variability Index  (IVI)');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    % Take one IVI value per epoch (they are the same for all sats in a
    % constellation at a given epoch — use the first satellite per epoch)
    [~, ia] = unique(T_sys.utc, 'stable');
    T_epoch = T_sys(ia, :);

    scatter(T_epoch.utc, T_epoch.ivi_m, 20, 'k', 'filled', ...
        'MarkerFaceAlpha', 0.5);

    % Shade epochs where iono is unstable
    idx_unstable = (T_epoch.iono_stable == 0);
    if any(idx_unstable)
        scatter(T_epoch.utc(idx_unstable), T_epoch.ivi_m(idx_unstable), 50, ...
            'r', 'filled', 'MarkerFaceAlpha', 0.3);
    end

    % IVI threshold line
    yline(0.06, 'r--', 'IVI threshold', 'LineWidth', 1, ...
        'LabelHorizontalAlignment','left', 'FontSize', 8);

    xlabel('GPS Time'); ylabel('IVI  (m)');
    title([sysName ' — Iono Variability Index']);
    datetick('x','keeplimits');
    hold off;
end

end

%% =====================================================================
%%  Helper: consistent color & marker for a key
%% =====================================================================
function [c, m] = key_style(thisKey, satCats, colors, markerList, fallbackIdx)
    parts   = split(thisKey, "_");
    satName = parts(1);
    satIdx  = find(string(satCats) == satName, 1);
    if isempty(satIdx), satIdx = fallbackIdx; end

    c = colors(mod(satIdx-1, size(colors,1)) + 1, :);
    m = markerList{mod(fallbackIdx-1, numel(markerList)) + 1};
end
