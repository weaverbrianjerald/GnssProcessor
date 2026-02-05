clear all; clc; close all;

fname = 'C:\GnssProcessor\build\x64-Release\logs\ambiguity_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\ambiguity_trace.csv';

plot_ambiguities(fname);

%%

% plot_ambiguities.m
% Read and visualize ambiguity_trace.csv (UTC time axis)
% Expected CSV header: week,tow_s,key,value_m,std_m
%
% Usage:
%   plot_ambiguities(); % default path
%   plot_ambiguities(fname);
function plot_ambiguities(fname)

if nargin < 1 || isempty(fname)
    % Adjust this path to your build/logs location if different
    fname = fullfile('C:','GnssProcessor','build','x64-Release','logs','ambiguity_trace.csv');
    % Alternate debug path example:
    % fname = fullfile('C:','GnssProcessor','build','x64-Debug','logs','ambiguity_trace.csv');
end

if ~isfile(fname)
    error('ambiguity CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

% Normalize columns if names vary slightly
vars = lower(T.Properties.VariableNames);
needed = {'week','tow_s','key','value_m','std_m'};
for i = 1:numel(needed)
    if ~any(strcmp(vars, needed{i}))
        error('CSV must contain columns: week,tow_s,key,value_m,std_m');
    end
end

%% --- Parse key into satellite and signal(s) ---
% Expected key examples: "G01_L1C", "G01_L1C_L2W", "E11_L1C_L5Q" etc.
keyStr = string(T.key);
% Satellite token is first underscore-separated token
splitKeys = split(keyStr, '_');
satCol = splitKeys(:,1);
% Signal part is the remainder (joined with underscore if multiple)
sigCol = strings(size(keyStr));
for i=1:size(splitKeys,1)
    if size(splitKeys,2) >= 2
        sigCol(i) = strjoin(splitKeys(i,2:end), '_');
    else
        sigCol(i) = "";
    end
end

T.satellite = categorical(satCol);

% Determine system from satellite first character
T.system = categorical(repmat("Unknown", height(T), 1));
satStr = string(T.satellite);
T.system(startsWith(satStr, 'G')) = categorical("GPS");
T.system(startsWith(satStr, 'E')) = categorical("Galileo");
T.system(startsWith(satStr, 'R')) = categorical("GLONASS");
T.system(startsWith(satStr, 'C')) = categorical("BeiDou");
T.system(startsWith(satStr, 'J')) = categorical("QZSS");
T.system(startsWith(satStr, 'S')) = categorical("SBAS");

T.signal = categorical(sigCol);

%% --- Convert GPS week + TOW to datetime (UTC axis like residual script) ---
% GPS epoch 1980-01-06
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = double(T.week) * 7 * 86400 + double(T.tow_s);
% Note: residual example used gpsEpoch + seconds(gpsSeconds)
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Prepare plotting settings ---
uniqueSats = categories(T.satellite);
N = numel(uniqueSats);
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

try
    colors = distinguishable_colors(max(8,N));
catch
    colors = lines(max(8,N));
end

%% --- Plot ambiguities grouped by system ---
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
    error('No recognized GNSS systems found in ambiguity CSV');
end

figure('Name','Ambiguity traces by System','Color','w','Position',[100 100 1400 500]);
t = tiledlayout(1, nSys, 'TileSpacing','compact','Padding','compact');
title(t, 'Ambiguity estimates (meters) by System');
lh = [];
for col = 1:nSys
    sysName = systemsPresent{col};
    nexttile;
    hold on;
    grid on;

    idx_sys = T.system == sysName;
    T_sys = T(idx_sys, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sprintf('%s', sysName));
        continue;
    end

    % Unique keys within this system (full key string)
    keysInSys = unique(string(T_sys.key));
    for k = 1:numel(keysInSys)
        thisKey = keysInSys{k};
        idx_key = string(T_sys.key) == thisKey;
        % Determine a consistent color by satellite index if possible
        sat = split(thisKey, '_');
        satName = sat{1};
        globalIdx = find(strcmp(uniqueSats, satName));
        if isempty(globalIdx)
            globalIdx = k;
        end
        err = double(T_sys.std_m(idx_key));
        vals = double(T_sys.value_m(idx_key));
        times = T_sys.utc(idx_key);

        % Scatter markers and draw vertical error bars only (no connecting lines)
        c = colors(mod(globalIdx-1,size(colors,1))+1,:);
        s = markerList{mod(k-1,numel(markerList))+1};
        scatter(times, vals, 36, 'Marker', s, 'MarkerEdgeColor', c, 'MarkerFaceColor', 'none');
        % Draw vertical errorbars without markers or connecting lines
        if ~isempty(times)
            % Use errorbar with no marker to draw vertical bars only
            he = errorbar(times, vals, err, 'LineStyle', 'none', 'Color', c, 'CapSize', 8);
            set(he, 'Marker', 'none');
        end
        % Provide display name for legend
        % Create an invisible line object to show in legend (avoids duplicate entries)
        plt_temp_lng = plot(times(1), 0, 'Marker', s, 'LineStyle', 'none', 'Color', c, 'DisplayName', thisKey);
        lh = [lh; plt_temp_lng];
        set(lh, 'Visible', 'off');
    end

    xlabel('UTC Time');
    ylabel('Ambiguity (m)');
    title(sprintf('%s', sysName));
    % Show legend outside
    legend(lh,'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

%% --- Tiled layout: per-satellite ambiguity keys (if many keys) ---
% If number of unique keys is moderately small, create a tiled view with one
% subplot per key for detail (errorbars + std shading).
allKeys = unique(string(T.key));
nKeys = numel(allKeys);
maxKeysSingleFig = 16;
if nKeys > 1 && nKeys <= maxKeysSingleFig
    figure('Name','Ambiguity per Key','Color','w','Position',[100 100 1200 700]);
    rows = ceil(sqrt(nKeys));
    cols = ceil(nKeys/rows);
    t2 = tiledlayout(rows, cols, 'TileSpacing','compact','Padding','compact');
    title(t2, 'Ambiguity time-series per key');

    for k = 1:nKeys
        nexttile;
        thisKey = allKeys{k};
        idx_key = string(T.key) == thisKey;
        times = T.utc(idx_key);
        vals = double(T.value_m(idx_key));
        err = double(T.std_m(idx_key));

        if isempty(times)
            text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
            title(thisKey, 'Interpreter','none');
            continue;
        end

        % Scatter markers
        scatter(times, vals, 36, 'Marker', 'o', 'MarkerEdgeColor', [0 0.4470 0.7410], 'MarkerFaceColor', 'none');
        hold on;
        % shaded area for 1-sigma
        xnum = datenum(times);
        x = [xnum; flipud(xnum)];
        y = [vals - err; flipud(vals + err)];
        fill(x, y, [0.9 0.9 1], 'EdgeColor','none', 'FaceAlpha', 0.5);
        % draw vertical error bars (no connecting lines)
        he = errorbar(times, vals, err, 'LineStyle', 'none', 'Color', [0 0.4470 0.7410], 'CapSize', 8);
        set(he, 'Marker', 'none');
        datetick('x','keeplimits');
        title(thisKey, 'Interpreter','none');
        xlabel('UTC Time'); ylabel('Ambiguity (m)');
        grid on;
        hold off;
    end
end

%% --- Summary Statistics ---
fprintf('\n=== Ambiguity Summary ===\n');
counts = zeros(nKeys,1);
means = zeros(nKeys,1);
stds = zeros(nKeys,1);
for k = 1:nKeys
    idx = string(T.key) == allKeys{k};
    vals = double(T.value_m(idx));
    counts(k) = numel(vals);
    if ~isempty(vals)
        means(k) = mean(vals);
        stds(k) = std(vals);
    else
        means(k) = NaN;
        stds(k) = NaN;
    end
end
summaryTbl = table(allKeys, counts, means, stds, 'VariableNames', {'Key','Count','Mean_m','Std_m'});
disp(summaryTbl);

end