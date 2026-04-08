%% ============================================================
%  Script: plotIonosphere.m
%  Purpose: Read and visualize ionosphere_trace.csv (UTC time axis)
%           Per-satellite slant ionosphere delay estimates from PPP-RTK
%  Expected CSV header: week,tow_s,satellite,iono_m,std_m,elevation_deg
%  ============================================================

clear all; clc; %close all;

fname = 'C:\GnssProcessor\build\x64-Release\logs\ionosphere_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\ionosphere_trace.csv';

plot_ionosphere(fname);

%%
function plot_ionosphere(fname)

if nargin < 1 || isempty(fname)
    fname = fullfile('C:','GnssProcessor','build','x64-Release','logs','ionosphere_trace.csv');
end

if ~isfile(fname)
    error('ionosphere CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

vars = lower(T.Properties.VariableNames);
needed = {'week','tow_s','satellite','iono_m','std_m','elevation_deg'};
for i = 1:numel(needed)
    if ~any(strcmp(vars, needed{i}))
        error('CSV must contain columns: %s', strjoin(needed, ', '));
    end
end

%% --- Determine system from satellite ID ---
T.system = categorical(repmat("Unknown", height(T), 1));
satStr = string(T.satellite);
T.system(startsWith(satStr, 'G')) = categorical("GPS");
T.system(startsWith(satStr, 'E')) = categorical("Galileo");
T.system(startsWith(satStr, 'R')) = categorical("GLONASS");
T.system(startsWith(satStr, 'C')) = categorical("BeiDou");
T.system(startsWith(satStr, 'J')) = categorical("QZSS");
T.system(startsWith(satStr, 'S')) = categorical("SBAS");

T.satellite = categorical(T.satellite);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = double(T.week) * 7 * 86400 + double(T.tow_s);
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Plotting setup ---
uniqueSats = categories(T.satellite);
N = numel(uniqueSats);
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

try
    colors = distinguishable_colors(max(8,N));
catch
    colors = lines(max(8,N));
end

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
%  Figure 1: Slant ionosphere delay time-series by system
%  ================================================================
figure('Name','Ionosphere Delay by System','Color','w','Position',[80 80 1400 500]);
t1 = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
title(t1, 'Slant ionosphere delay (meters) by system');

for col = 1:nSys
    sysName = systemsPresent{col};
    nexttile; hold on; grid on;

    idx_sys = T.system == sysName;
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    satsInSys = unique(string(T_sys.satellite));
    lh = gobjects(0);
    for k = 1:numel(satsInSys)
        sat = satsInSys(k);
        idx_sat = T_sys.satellite == sat;
        times = T_sys.utc(idx_sat);
        vals  = double(T_sys.iono_m(idx_sat));

        globalIdx = find(strcmp(uniqueSats, sat));
        if isempty(globalIdx), globalIdx = k; end
        c = colors(mod(globalIdx-1, size(colors,1))+1, :);
        mk = markerList{mod(k-1, numel(markerList))+1};

        scatter(times, vals, 20, 'Marker', mk, 'MarkerEdgeColor', c, 'MarkerFaceColor', 'none');
        % Invisible handle for legend
        h = plot(times(1), NaN, 'Marker', mk, 'LineStyle','none', 'Color', c, 'DisplayName', sat);
        lh = [lh; h]; %#ok<AGROW>
    end

    xlabel('UTC Time'); ylabel('Iono delay (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none');
    hold off;
end

%% ================================================================
%  Figure 2: Ionosphere delay with ±1? shading, per satellite
%  ================================================================
allSats = unique(string(T.satellite));
nSats = numel(allSats);
maxPerFig = 16;
nPlot = min(nSats, maxPerFig);

figure('Name','Ionosphere per Satellite','Color','w','Position',[100 100 1200 700]);
rows = ceil(sqrt(nPlot));
cols_t = ceil(nPlot / rows);
t2 = tiledlayout(rows, cols_t, 'TileSpacing','compact','Padding','compact');
title(t2, 'Per-satellite slant iono delay with \pm1\sigma');

for k = 1:nPlot
    nexttile; hold on; grid on;
    sat = allSats(k);
    idx = T.satellite == sat;
    times = T.utc(idx);
    vals  = double(T.iono_m(idx));
    err   = double(T.std_m(idx));

    if isempty(times)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sat,'Interpreter','none'); continue;
    end

    % 1-sigma shaded band
    tNum = datenum(times);
    xfill = [tNum; flipud(tNum)];
    yfill = [vals - err; flipud(vals + err)];
    fill(xfill, yfill, [0.85 0.85 1], 'EdgeColor','none', 'FaceAlpha',0.5);

    plot(times, vals, '-', 'Color', [0 0.4470 0.7410], 'LineWidth', 0.8);

    xlabel('UTC'); ylabel('Iono (m)');
    title(sat,'Interpreter','none');
    hold off;
end

%% ================================================================
%  Figure 3: Iono delay vs elevation (scatter, color-coded by sat)
%  ================================================================
figure('Name','Iono vs Elevation','Color','w','Position',[120 120 900 500]);
t3 = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
title(t3, 'Slant iono delay vs satellite elevation');

for col = 1:nSys
    sysName = systemsPresent{col};
    nexttile; hold on; grid on;

    idx_sys = T.system == sysName;
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    satsInSys = unique(string(T_sys.satellite));
    lh = gobjects(0);
    for k = 1:numel(satsInSys)
        sat = satsInSys(k);
        idx_sat = T_sys.satellite == sat;
        elev = double(T_sys.elevation_deg(idx_sat));
        vals = double(T_sys.iono_m(idx_sat));

        globalIdx = find(strcmp(uniqueSats, sat));
        if isempty(globalIdx), globalIdx = k; end
        c = colors(mod(globalIdx-1, size(colors,1))+1, :);
        mk = markerList{mod(k-1, numel(markerList))+1};

        scatter(elev, vals, 20, 'Marker', mk, 'MarkerEdgeColor', c, 'MarkerFaceColor', 'none');
        h = plot(NaN, NaN, 'Marker', mk, 'LineStyle','none', 'Color', c, 'DisplayName', sat);
        lh = [lh; h]; %#ok<AGROW>
    end

    xlabel('Elevation (deg)'); ylabel('Iono delay (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none');
    hold off;
end

%% ================================================================
%  Figure 4: Ionosphere standard deviation time-series (convergence)
%  ================================================================
figure('Name','Iono Std Dev','Color','w','Position',[140 140 1400 400]);
t4 = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
title(t4, 'Ionosphere state \sigma (m) — convergence monitor');

for col = 1:nSys
    sysName = systemsPresent{col};
    nexttile; hold on; grid on;

    idx_sys = T.system == sysName;
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    satsInSys = unique(string(T_sys.satellite));
    lh = gobjects(0);
    for k = 1:numel(satsInSys)
        sat = satsInSys(k);
        idx_sat = T_sys.satellite == sat;
        times = T_sys.utc(idx_sat);
        err   = double(T_sys.std_m(idx_sat));

        globalIdx = find(strcmp(uniqueSats, sat));
        if isempty(globalIdx), globalIdx = k; end
        c = colors(mod(globalIdx-1, size(colors,1))+1, :);

        plot(times, err, '-', 'Color', c, 'LineWidth', 0.8);
        h = plot(NaT, NaN, '-', 'Color', c, 'DisplayName', sat);
        lh = [lh; h]; %#ok<AGROW>
    end

    xlabel('UTC Time'); ylabel('\sigma (m)');
    title(sysName);
    legend(lh, 'Location','bestoutside','Interpreter','none');
    set(gca, 'YScale', 'log');
    hold off;
end

%% ================================================================
%  Figure 5: Sky-plot style — iono delay mapped to elevation/time
%  ================================================================
figure('Name','Iono Elevation Heatmap','Color','w','Position',[160 160 1000 450]);
t5 = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
title(t5, 'Iono delay (m) colour-mapped by elevation and time');

for col = 1:nSys
    sysName = systemsPresent{col};
    nexttile; hold on; grid on;

    idx_sys = T.system == sysName;
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sysName); continue;
    end

    scatter(T_sys.utc, double(T_sys.elevation_deg), 18, double(T_sys.iono_m), 'filled', 'MarkerFaceAlpha', 0.6);
    cb = colorbar; cb.Label.String = 'Iono delay (m)';
    xlabel('UTC Time'); ylabel('Elevation (deg)');
    title(sysName);
    hold off;
end

%% --- Summary Statistics ---
fprintf('\n=== Ionosphere Summary ===\n');
counts = zeros(nSats,1);
means  = zeros(nSats,1);
stds_v = zeros(nSats,1);
mean_el = zeros(nSats,1);
for k = 1:nSats
    idx = T.satellite == allSats(k);
    vals = double(T.iono_m(idx));
    el   = double(T.elevation_deg(idx));
    counts(k) = numel(vals);
    if ~isempty(vals)
        means(k)  = mean(vals);
        stds_v(k) = std(vals);
        mean_el(k) = mean(el);
    else
        means(k)  = NaN;
        stds_v(k) = NaN;
        mean_el(k) = NaN;
    end
end
summaryTbl = table(allSats, counts, means, stds_v, mean_el, ...
    'VariableNames', {'Satellite','Count','Mean_m','Std_m','MeanElev_deg'});
disp(summaryTbl);

end
