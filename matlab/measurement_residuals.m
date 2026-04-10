%% ============================================================
%  Script: plot_measurement_residuals.m
%  Purpose: Read and visualize PPP measurement residuals (UTC time axis)
%           Categorized by system (GPS, Galileo, etc.) and type (code/phase)
%  ============================================================

clear all; %clc; %close all;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\measurement_residuals.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\measurement_residuals.csv';
T = readtable(fname);

%% --- Categorize measurements by type (code/phase) and system ---
% Determine measurement type from signal column (C* = code, L* = phase)
T.meas_type = categorical(repmat("unknown", height(T), 1));
isCode = startsWith(T.signal, 'C');
isPhase = startsWith(T.signal, 'L');
T.meas_type(isCode) = categorical("Code");
T.meas_type(isPhase) = categorical("Phase");

% Determine system from satellite ID (first character)
T.system = categorical(repmat("Unknown", height(T), 1));
satStr = string(T.satellite);
T.system(startsWith(satStr, 'G')) = categorical("GPS");
T.system(startsWith(satStr, 'E')) = categorical("Galileo");
T.system(startsWith(satStr, 'R')) = categorical("GLONASS");
T.system(startsWith(satStr, 'C')) = categorical("BeiDou");
T.system(startsWith(satStr, 'J')) = categorical("QZSS");
T.system(startsWith(satStr, 'S')) = categorical("SBAS");

% Convert satellite column to categorical
T.satellite = categorical(T.satellite);
satList = categories(T.satellite);
N = numel(satList);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = T.week*7*86400 + T.tow_s;
T.utc = gpsEpoch + seconds(gpsSeconds) + seconds(18); % Add 18 second offset between GPS and UTC Time

%% --- Define markers and colors ---
markerList = {'o','s','d','^','v','>','<','p','h','x','+'};

try
    addpath(genpath('C:\temp\gnss-qc\matlab\visualization'));
    colors = distinguishable_colors(1e3);
catch
    colors = lines(1e3);
end

%% --- Get unique systems and measurement types ---
systemList = categories(T.system);
systemList = systemList(~strcmp(systemList, 'Unknown')); % Remove unknown
measTypeList = {'Code', 'Phase'};

% Filter to only systems present in data
systemsPresent = {};
for i = 1:numel(systemList)
    if any(T.system == systemList{i})
        systemsPresent{end+1} = systemList{i};
    end
end
nSys = numel(systemsPresent);
nTypes = numel(measTypeList);

%% --- Tiledlayout: Residuals by System and Type ---
% Rows = measurement type (Code, Phase), Columns = system (GPS, Galileo, etc.)
figure('Name','Residuals by System and Type','Color','w','Position',[100 100 1400 600]);
t = tiledlayout(nTypes, nSys, 'TileSpacing', 'compact', 'Padding', 'compact');
title(t, 'Measurement Residuals by System and Type');

for row = 1:nTypes
    measType = measTypeList{row};
    for col = 1:nSys
        sysName = systemsPresent{col};

        nexttile;
        hold on;

        % Filter data for this system and measurement type
        idx_sys_type = T.system == sysName & T.meas_type == measType;
        T_sub = T(idx_sys_type, :);

        if isempty(T_sub)
            text(0.5, 0.5, 'No Data', 'HorizontalAlignment', 'center', ...
                'Units', 'normalized', 'FontSize', 12);
            title(sprintf('%s - %s', sysName, measType));
            hold off;
            continue;
        end

        satsInSubset = categories(removecats(T_sub.satellite));

        for k = 1:numel(satsInSubset)
            sat = satsInSubset{k};
            idx_sat = (T_sub.satellite == sat);

            colorRow = str2double(sat(2:3));

            globalIdx = find(strcmp(satList, sat), 1);
            if isempty(globalIdx), globalIdx = k; end
            if globalIdx > size(colors,1), globalIdx = mod(globalIdx-1, size(colors,1)) + 1; end

            scatter(T_sub.utc(idx_sat), T_sub.residual_m(idx_sat), ...
                10, colors(colorRow,:), 'filled', ...
                'DisplayName', sat);
        end

        xlabel('UTC Time');
        ylabel('Residual (m)');
        title(sprintf('%s - %s', sysName, measType));
        grid on;

        hold off;

    end

end

%% --- Tiledlayout: Standardized Residuals by System and Type ---
figure('Name','Standardized Residuals by System and Type','Color','w','Position',[100 100 1400 600]);
t2 = tiledlayout(nTypes, nSys, 'TileSpacing', 'compact', 'Padding', 'compact');
title(t2, 'Standardized Residuals by System and Type');

for row = 1:nTypes
    measType = measTypeList{row};
    for col = 1:nSys
        sysName = systemsPresent{col};
        
        nexttile;
        hold on;
        
        % Filter data for this system and measurement type
        idx_sys_type = T.system == sysName & T.meas_type == measType;
        T_sub = T(idx_sys_type, :);
        
        if isempty(T_sub)
            text(0.5, 0.5, 'No Data', 'HorizontalAlignment', 'center', ...
                'Units', 'normalized', 'FontSize', 12);
            title(sprintf('%s - %s', sysName, measType));
            continue;
        end
        
        % Get satellites in this subset
        satsInSubset = categories(removecats(T_sub.satellite));
        
        for k = 1:numel(satsInSubset)
            sat = satsInSubset{k};
            idx_sat = T_sub.satellite == sat;
            
            % Find global satellite index for consistent coloring
            globalIdx = find(strcmp(satList, sat));
            if isempty(globalIdx), globalIdx = k; end
            
            plot(T_sub.utc(idx_sat), T_sub.standardized_residual(idx_sat), ...
                'LineStyle', 'none', ...
                'Marker', markerList{mod(globalIdx-1, numel(markerList))+1}, ...
                'MarkerSize', 5, ...
                'Color', colors(globalIdx,:), ...
                'DisplayName', sat);
        end
        
        yline(3, '--r', '+3\sigma', 'HandleVisibility', 'off');
        yline(-3, '--r', '-3\sigma', 'HandleVisibility', 'off');
        
        xlabel('UTC Time');
        ylabel('Std. Residual');
        title(sprintf('%s - %s', sysName, measType));
        grid on;
        
        % Only show legend for first column
        if col == 1
            legend('Location', 'bestoutside', 'FontSize', 7);
        end
        
        hold off;
    end
end

%% --- Summary Statistics Table ---
fprintf('\n=== Residual Statistics by System and Type ===\n\n');
for i = 1:nSys
    for j = 1:nTypes
        idx = T.system == systemsPresent{i} & T.meas_type == measTypeList{j};
        if any(idx)
            res = T.residual_m(idx);
            sres = T.standardized_residual(idx);
            fprintf('%s %s: N=%d, RMS=%.4f m, Mean=%.4f m, Std=%.4f m, |Std.Res|>3: %d (%.1f%%)\n', ...
                systemsPresent{i}, measTypeList{j}, ...
                sum(idx), rms(res), mean(res), std(res), ...
                sum(abs(sres) > 3), 100*sum(abs(sres) > 3)/sum(idx));
        end
    end
end
