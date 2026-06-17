% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('graphic_slip_trace.csv');

plot_graphic_trace(fname);

%%

% plot_graphic_trace.m
% Read and visualize graphic_slip_trace.csv (GPS time axis)
%
% Expected CSV header (written by GraphicSlipDetector):
%   week,tow_s,satellite,system,elevation_deg,
%   signal,graphic_m,dg_m,
%   mad_sigma_m,threshold_m,arc_count,
%   slip_cycles,fractional,is_slip
%
% Produces three figures:
%   1. dG (between-epoch GRAPHIC difference) with �threshold envelope
%   2. MAD sigma and arc count diagnostics
%   3. Slip events with estimated integer magnitudes
%
% Usage:
%   plot_graphic_trace();       % default Release path
%   plot_graphic_trace(fname);
function plot_graphic_trace(fname)

if nargin < 1 || isempty(fname)
    fname = gnssTracePath('graphic_slip_trace.csv');
end

if ~isfile(fname)
    error('GRAPHIC trace CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

% Require the columns we need
vars = lower(string(T.Properties.VariableNames));
need = ["week","tow_s","satellite","system","signal",...
        "graphic_m","dg_m","threshold_m","is_slip","slip_cycles","fractional"];
missing = need(~ismember(need, vars));
if ~isempty(missing)
    error('CSV missing columns: %s', strjoin(missing,', '));
end

% Convert numerics
T.week          = double(T.week);
T.tow_s         = double(T.tow_s);
T.elevation_deg = double(T.elevation_deg);
T.graphic_m     = double(T.graphic_m);
T.dg_m          = double(T.dg_m);
T.mad_sigma_m   = double(T.mad_sigma_m);
T.threshold_m   = double(T.threshold_m);
T.arc_count     = double(T.arc_count);
T.slip_cycles   = double(T.slip_cycles);
T.fractional    = double(T.fractional);
T.is_slip       = double(T.is_slip);

% Replace sentinel values (-1) with NaN for cleaner plots
T.mad_sigma_m(T.mad_sigma_m < 0) = NaN;
T.threshold_m(T.threshold_m < 0) = NaN;

%% --- Build a per-arc key: "G01_L1C" ---
T.key = T.satellite + "_" + T.signal;
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
%%  Figure 1: dG with �threshold � one tile per constellation
%% =====================================================================
figure('Name','GRAPHIC: dG + Threshold','Color','w','Position',[80 400 1400 450]);
tl1 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl1, 'GRAPHIC Between-Epoch Difference  dG = G(t) - G(t-1)  with Threshold');

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

        % dG values
        scatter(T_sys.utc(idx_key), T_sys.dg_m(idx_key), 14, ...
            'Marker', m, 'MarkerEdgeColor','none', 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.5);

        % �Threshold envelope
        clite = min(c + 0.4, 1.0);
        plot(T_sys.utc(idx_key),  T_sys.threshold_m(idx_key), ...
            '-', 'Color', [clite 0.3], 'LineWidth', 0.5, 'HandleVisibility','off');
        plot(T_sys.utc(idx_key), -T_sys.threshold_m(idx_key), ...
            '-', 'Color', [clite 0.3], 'LineWidth', 0.5, 'HandleVisibility','off');

        % Mark slips with a red X
        idx_slip = idx_key & (T_sys.is_slip == 1);
        if any(idx_slip)
            scatter(T_sys.utc(idx_slip), T_sys.dg_m(idx_slip), 120, ...
                'rx', 'LineWidth', 2, 'HandleVisibility','off');
        end

        h = plot(T_sys.utc(1), NaN, 'Marker', m, 'LineStyle','none', ...
            'Color', c, 'DisplayName', thisKey);
        set(h,'Visible','off');
        lh(end+1) = h; %#ok<AGROW>
    end

    xlabel('GPS Time'); ylabel('dG  (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none','FontSize',7);
    datetick('x','keeplimits');
    hold off;
end

%% =====================================================================
%%  Figure 2: MAD sigma and arc count
%% =====================================================================
figure('Name','GRAPHIC: MAD Sigma & Arc Count','Color','w','Position',[80 100 1400 450]);
tl2 = tiledlayout(2, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl2, 'GRAPHIC Detector Diagnostics');

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
    title([sysName ' � MAD \sigma']);
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
    title([sysName ' � Arc Count']);
    datetick('x','keeplimits');
    hold off;
end

%% =====================================================================
%%  Figure 3: Slip events with integer estimates
%% =====================================================================
figure('Name','GRAPHIC: Slip Events','Color','w','Position',[80 -200 1400 350]);
tl3 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl3, 'GRAPHIC Detected Slips � Integer Estimate  dN = round(2 dG / \lambda)');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    idx_slip = (T_sys.is_slip == 1);
    if ~any(idx_slip)
        text(0.5,0.5,'No Slips','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    T_slip = T_sys(idx_slip, :);
    keysInSlip = unique(T_slip.key);
    lh = gobjects(0);
    for k = 1:numel(keysInSlip)
        thisKey = keysInSlip(k);
        idx_key = (T_slip.key == thisKey);
        [c, m] = key_style(thisKey, satCats, colors, markerList, k);

        scatter(T_slip.utc(idx_key), T_slip.slip_cycles(idx_key), 60, ...
            'Marker', m, 'MarkerEdgeColor', c, 'MarkerFaceColor', c, ...
            'MarkerFaceAlpha', 0.7);

        h = plot(T_slip.utc(1), NaN, 'Marker', m, 'LineStyle','none', ...
            'Color', c, 'DisplayName', thisKey);
        set(h,'Visible','off');
        lh(end+1) = h; %#ok<AGROW>
    end

    xlabel('GPS Time'); ylabel('dN  (cycles)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none','FontSize',7);
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
