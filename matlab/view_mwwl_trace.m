clear all; clc; %close all;

fname = 'C:\GnssProcessor\build\x64-Release\logs\mwwl_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\mwwl_trace.csv';

plot_mwwl(fname);

%%

% plot_mwwl.m
% Read and visualize mwwl_trace.csv (UTC time axis)
% Expected CSV header:
% week,tow_s,satellite,system,elevation_deg,sig1,sig2,mw_m,mw_cycles,...,is_slip,is_lli
%
% Usage:
%   plot_mwwl(); % default path
%   plot_mwwl(fname);
function plot_mwwl(fname)

if nargin < 1 || isempty(fname)
    fname = fullfile('C:','GnssProcessor','build','x64-Release','logs','mwwl_trace.csv');
end

if ~isfile(fname)
    error('mwwl CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

% Require the simple columns we need
vars = lower(string(T.Properties.VariableNames));
need = ["week","tow_s","satellite","system","sig1","sig2","mw_cycles"];
if any(~ismember(need, vars))
    error('CSV must contain at least columns: week,tow_s,satellite,system,sig1,sig2,mw_cycles');
end

% Convert numerics
T.week      = double(T.week);
T.tow_s     = double(T.tow_s);
T.mw_cycles = double(T.mw_cycles);

%% --- Build a "key" like your ambiguity script ---
% Example: "C14_L2I_L6I"
T.key = T.satellite + "_" + T.sig1 + "_" + T.sig2;
T.key = string(T.key);

% Normalize system as categorical
T.system = categorical(string(T.system));

%% --- Convert GPS week + TOW to datetime (UTC) ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = T.week * 7 * 86400 + T.tow_s;
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Plot: MW/WL traces by System (scatter only) ---
systemList = categories(T.system);

figure('Name','MWWL traces by System','Color','w','Position',[100 100 1400 500]);
tl = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl, 'MW/WL (cycles) by System');

% Color by satellite (simple + stable)
satCats = unique(categorical(T.satellite));
N = numel(satCats);
try
    colors = distinguishable_colors(max(8,N));
catch
    colors = lines(max(8,N));
end

markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    idx_sys = (T.system == sysName);
    T_sys = T(idx_sys, :);

    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sprintf('%s', sysName));
        continue;
    end

    keysInSys = unique(T_sys.key);
    lh = gobjects(0);

    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);

        % Choose color by satellite index (first token before "_")
        satName = split(thisKey, "_"); satName = satName(1);
        satIdx = find(string(satCats) == satName, 1);
        if isempty(satIdx), satIdx = k; end

        c = colors(mod(satIdx-1,size(colors,1))+1,:);
        m = markerList{mod(k-1,numel(markerList))+1};

        % MWWL
        %scatter(T_sys.utc(idx_key), T_sys.mw_cycles(idx_key), 36, ...
        %    'Marker', m, 'MarkerEdgeColor', c, 'MarkerFaceColor', 'none');

        % MWWL Delta
        scatter(T_sys.utc(idx_key), T_sys.delta_mw_m(idx_key), 36, ...
            'Marker', m, 'MarkerEdgeColor', c, 'MarkerFaceColor', 'none');

        % Legend entry (invisible helper like your script)
        h = plot(T_sys.utc(find(idx_key,1)), 0, ...
            'Marker', m, 'LineStyle','none', 'Color', c, 'DisplayName', thisKey);
        set(h, 'Visible','off');
        lh(end+1) = h; %#ok<AGROW>
    end

    xlabel('UTC Time');
    ylabel('MW/WL (cycles)');
    title(sprintf('%s', sysName));
    legend(lh, 'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

end
