clear all; clc; %close all;

fname = 'C:\GnssProcessor\build\x64-Release\logs\if_slip_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\if_slip_trace.csv';

plot_if_slip(fname);

%%

% plot_if_slip.m
% Read and visualize if_slip_trace.csv (UTC time axis)
% Expected CSV header:
% week,tow_s,satellite,system,elevation_deg,
% phase_if_key,phi_if_omc_m,
% grad_phi_if_m,delta_grad_m,
% ref_satellite,ref_changed,
% lambda_if_m,is_slip,slip_reason,
% pos_sigma_3d_m,pos_gated
%
% Usage:
%   plot_if_slip(); % default path
%   plot_if_slip(fname);
function plot_if_slip(fname)

if nargin < 1 || isempty(fname)
    fname = fullfile('C:','GnssProcessor','build','x64-Release','logs','if_slip_trace.csv');
end

if ~isfile(fname)
    error('if_slip_trace CSV not found: %s', fname);
end

%% --- Load CSV file ---
T = readtable(fname, 'TextType','string');

% Require the columns we need
vars = lower(string(T.Properties.VariableNames));
need = ["week","tow_s","satellite","system","elevation_deg",...
"phase_if_key","delta_grad_m","ref_satellite","is_slip"];
if any(~ismember(need, vars))
    error('CSV must contain at least columns: %s', strjoin(need,', '));
end

% Convert numerics
T.week          = double(T.week);
T.tow_s         = double(T.tow_s);
T.elevation_deg = double(T.elevation_deg);
T.phi_if_omc_m  = double(T.phi_if_omc_m);
T.grad_phi_if_m = double(T.grad_phi_if_m);
T.delta_grad_m  = double(T.delta_grad_m);
T.lambda_if_m   = double(T.lambda_if_m);
T.is_slip       = double(T.is_slip);
T.ref_changed   = double(T.ref_changed);

% New position-uncertainty gating columns (may be absent in old files)
if ismember("pos_sigma_3d_m", vars)
    T.pos_sigma_3d_m = double(T.pos_sigma_3d_m);
else
    T.pos_sigma_3d_m = zeros(height(T), 1);
end
if ismember("pos_gated", vars)
    T.pos_gated = double(T.pos_gated);
else
    T.pos_gated = zeros(height(T), 1);
end

%% --- Build a "key" like the MWWL script ---
% Example: "G01_L1C_L2W"
T.key = T.satellite + "_" + T.phase_if_key;
T.key = string(T.key);

% Normalize system as categorical
T.system = categorical(string(T.system));

%% --- Convert GPS week + TOW to datetime (UTC) ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = T.week * 7 * 86400 + T.tow_s;
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Detect nominal epoch interval for arc-breaking ---
% Use 1.5x the median time step as the gap threshold.
allTow = sort(unique(T.tow_s));
if numel(allTow) > 1
    dt_nominal = median(diff(allTow));
else
    dt_nominal = 30;
end
gap_thresh_s = 1.5 * dt_nominal;

%% --- Separate slip / no-slip / pos-gated rows ---
T_slip   = T(T.is_slip == 1, :);
T_gated  = T(T.is_slip == 0 & T.pos_gated == 1, :);
T_clean  = T(T.is_slip == 0 & T.pos_gated == 0, :);

%% --- Color setup ---
systemList = categories(T.system);

satCats = unique(categorical(T.satellite));
N = numel(satCats);
try
    colors = distinguishable_colors(max(8,N));
catch
    colors = lines(max(8,N));
end

%% ========================================================================
%  FIGURE 1 : Between-epoch double difference (delta_grad) by System
%  ========================================================================
figure('Name','IF phase double-diff (delta_grad) by System','Color','w', ...
       'Position',[100 100 1400 500]);
tl1 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl1, '\Delta\nabla\phi_{IF} (between-satellite, between-epoch) [m]');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    T_sys = T_clean(T_clean.system == sysName, :);
    T_sys_slip = T_slip(T_slip.system == sysName, :);
    T_sys_gated = T_gated(T_gated.system == sysName, :);

    if isempty(T_sys) && isempty(T_sys_slip) && isempty(T_sys_gated)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sprintf('%s', sysName));
        continue;
    end

    keysInSys = unique(T_sys.key);
    lh = gobjects(0);

    for k = 1:numel(keysInSys)
        thisKey = keysInSys(k);
        idx_key = (T_sys.key == thisKey);

        % Choose color by satellite index
        satName = split(thisKey, "_"); satName = satName(1);
        satIdx = find(string(satCats) == satName, 1);
        if isempty(satIdx), satIdx = k; end

        c = colors(mod(satIdx-1,size(colors,1))+1,:);

        T_key = sortrows(T_sys(idx_key, :), 'utc');
        [t_arc, y_arc] = break_arcs(T_key.utc, T_key.delta_grad_m, gap_thresh_s);

        h = plot(t_arc, y_arc, '-', 'Color', c, 'LineWidth', 0.8, 'DisplayName', thisKey);
        lh(end+1) = h; %#ok<AGROW>
    end

    % Overlay slips as large red markers
    if ~isempty(T_sys_slip)
        plot(T_sys_slip.utc, T_sys_slip.delta_grad_m, 'rx', ...
            'MarkerSize', 10, 'LineWidth', 2, 'HandleVisibility','off');
        h_slip = plot(NaN, NaN, 'rx', 'MarkerSize', 10, 'LineWidth', 2, 'DisplayName', 'SLIP');
        lh(end+1) = h_slip; %#ok<AGROW>
    end

    % Overlay pos-gated epochs as small grey markers
    if ~isempty(T_sys_gated)
        plot(T_sys_gated.utc, T_sys_gated.delta_grad_m, '.', ...
            'Color', [0.7 0.7 0.7], 'MarkerSize', 4, 'HandleVisibility','off');
        h_gate = plot(NaN, NaN, '.', 'Color', [0.7 0.7 0.7], 'MarkerSize', 8, 'DisplayName', 'pos-gated');
        lh(end+1) = h_gate; %#ok<AGROW>
    end

    % Draw +/- lambda_IF reference lines using median wavelength
    lambda_median = median(T.lambda_if_m(T.system == sysName), 'omitnan');
    if ~isnan(lambda_median) && lambda_median > 0
        yline( lambda_median, '--', sprintf('+\\lambda_{IF} %.3f m', lambda_median), ...
            'Color',[0.5 0.5 0.5], 'LineWidth', 0.8, 'LabelHorizontalAlignment','left');
        yline(-lambda_median, '--', sprintf('-\\lambda_{IF} %.3f m', lambda_median), ...
            'Color',[0.5 0.5 0.5], 'LineWidth', 0.8, 'LabelHorizontalAlignment','left');
    end

    xlabel('UTC Time');
    ylabel('\Delta\nabla\phi_{IF} (m)');
    title(sprintf('%s', sysName));
    legend(lh, 'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

%% ========================================================================
%  FIGURE 2 : IF phase OMC (phi_if_omc) by System
%  ========================================================================
figure('Name','IF phase OMC by System','Color','w', ...
       'Position',[100 150 1400 500]);
tl2 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl2, '\phi_{IF} observed-minus-computed [m]');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    T_sys = T(T.system == sysName, :);

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

        satName = split(thisKey, "_"); satName = satName(1);
        satIdx = find(string(satCats) == satName, 1);
        if isempty(satIdx), satIdx = k; end

        c = colors(mod(satIdx-1,size(colors,1))+1,:);

        T_key = sortrows(T_sys(idx_key, :), 'utc');
        [t_arc, y_arc] = break_arcs(T_key.utc, T_key.phi_if_omc_m, gap_thresh_s);

        h = plot(t_arc, y_arc, '-', 'Color', c, 'LineWidth', 0.8, 'DisplayName', thisKey);
        lh(end+1) = h; %#ok<AGROW>
    end

    % Overlay slips
    T_sys_slip = T_sys(T_sys.is_slip == 1, :);
    if ~isempty(T_sys_slip)
        plot(T_sys_slip.utc, T_sys_slip.phi_if_omc_m, 'rx', ...
            'MarkerSize', 10, 'LineWidth', 2, 'HandleVisibility','off');
        h_slip = plot(NaN, NaN, 'rx', 'MarkerSize', 10, 'LineWidth', 2, 'DisplayName', 'SLIP');
        lh(end+1) = h_slip; %#ok<AGROW>
    end

    xlabel('UTC Time');
    ylabel('\phi_{IF} OMC (m)');
    title(sprintf('%s', sysName));
    legend(lh, 'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

%% ========================================================================
%  FIGURE 3 : Between-satellite single difference (grad_phi_if) by System
%  ========================================================================
figure('Name','IF phase gradient (grad) by System','Color','w', ...
       'Position',[100 200 1400 500]);
tl3 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl3, '\nabla\phi_{IF} between-satellite single difference [m]');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    T_sys = T(T.system == sysName, :);

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

        satName = split(thisKey, "_"); satName = satName(1);
        satIdx = find(string(satCats) == satName, 1);
        if isempty(satIdx), satIdx = k; end

        c = colors(mod(satIdx-1,size(colors,1))+1,:);

        T_key = sortrows(T_sys(idx_key, :), 'utc');
        [t_arc, y_arc] = break_arcs(T_key.utc, T_key.grad_phi_if_m, gap_thresh_s);

        h = plot(t_arc, y_arc, '-', 'Color', c, 'LineWidth', 0.8, 'DisplayName', thisKey);
        lh(end+1) = h; %#ok<AGROW>
    end

    % Overlay slips
    T_sys_slip = T_sys(T_sys.is_slip == 1, :);
    if ~isempty(T_sys_slip)
        plot(T_sys_slip.utc, T_sys_slip.grad_phi_if_m, 'rx', ...
            'MarkerSize', 10, 'LineWidth', 2, 'HandleVisibility','off');
        h_slip = plot(NaN, NaN, 'rx', 'MarkerSize', 10, 'LineWidth', 2, 'DisplayName', 'SLIP');
        lh(end+1) = h_slip; %#ok<AGROW>
    end

    xlabel('UTC Time');
    ylabel('\nabla\phi_{IF} (m)');
    title(sprintf('%s', sysName));
    legend(lh, 'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

%% ========================================================================
%  FIGURE 4 : Reference satellite elevation over time
%  ========================================================================
figure('Name','IF slip reference satellite','Color','w', ...
       'Position',[100 250 1400 400]);
tl4 = tiledlayout(1, numel(systemList), 'TileSpacing','compact','Padding','compact');
title(tl4, 'Reference satellite elevation & changes');

for col = 1:numel(systemList)
    sysName = systemList{col};
    nexttile; hold on; grid on;

    T_sys = T(T.system == sysName, :);
    if isempty(T_sys)
        text(0.5,0.5,'No Data','HorizontalAlignment','center','Units','normalized');
        title(sprintf('%s', sysName));
        continue;
    end

    % Get one row per epoch (all rows in same epoch share the same ref_satellite)
    [~, ia] = unique(T_sys.tow_s, 'stable');
    T_epoch = T_sys(ia, :);

    % Look up the ref satellite's elevation from the full table
    ref_elev = NaN(height(T_epoch), 1);
    for r = 1:height(T_epoch)
        idx_match = T_sys.tow_s == T_epoch.tow_s(r) & ...
                    T_sys.satellite == T_epoch.ref_satellite(r);
        if any(idx_match)
            ref_elev(r) = T_sys.elevation_deg(find(idx_match, 1));
        end
    end

    refSats = unique(T_epoch.ref_satellite);
    lh = gobjects(0);
    for rs = 1:numel(refSats)
        idx_rs = (T_epoch.ref_satellite == refSats(rs));
        satIdx = find(string(satCats) == refSats(rs), 1);
        if isempty(satIdx), satIdx = rs; end
        c = colors(mod(satIdx-1,size(colors,1))+1,:);

        T_ref = sortrows(T_epoch(idx_rs, :), 'utc');
        elev_sorted = ref_elev(idx_rs);
        [~, si] = sortrows(T_epoch(idx_rs, :), 'utc');
        elev_sorted = elev_sorted(si);

        [t_arc, y_arc] = break_arcs(T_ref.utc, elev_sorted, gap_thresh_s);

        h = plot(t_arc, y_arc, '-', 'Color', c, 'LineWidth', 1.0, 'DisplayName', refSats(rs));
        lh(end+1) = h; %#ok<AGROW>
    end

    % Mark ref changes
    idx_change = T_epoch.ref_changed == 1;
    if any(idx_change)
        xline_times = T_epoch.utc(idx_change);
        for ix = 1:numel(xline_times)
            xline(xline_times(ix), ':', 'Color', [0.8 0.2 0.2], 'LineWidth', 0.8);
        end
        h_chg = plot(NaN, NaN, ':', 'Color', [0.8 0.2 0.2], 'DisplayName', 'ref change');
        lh(end+1) = h_chg; %#ok<AGROW>
    end

    xlabel('UTC Time');
    ylabel('Ref Elevation (deg)');
    title(sprintf('%s', sysName));
    legend(lh, 'Location','bestoutside','Interpreter','none');
    datetick('x','keeplimits');
    hold off;
end

%% --- Summary printout ---
fprintf('\n=== IF Phase Slip Trace Summary ===\n');
fprintf('File       : %s\n', fname);
fprintf('Rows       : %d\n', height(T));
fprintf('Epochs     : %d\n', numel(unique(T.tow_s)));
fprintf('Satellites : %d\n', numel(unique(T.satellite)));
fprintf('Systems    : %s\n', strjoin(string(systemList), ', '));
fprintf('Slips      : %d\n', sum(T.is_slip == 1));
fprintf('Pos-gated  : %d epochs\n', numel(unique(T.tow_s(T.pos_gated == 1))));

if sum(T.is_slip == 1) > 0
    fprintf('\n--- Slip details ---\n');
    for r = 1:height(T_slip)
        fprintf('  TOW %10.1f  %s  %-12s  delta_grad=%+.4f m  reason=%s\n', ...
            T_slip.tow_s(r), T_slip.satellite(r), T_slip.phase_if_key(r), ...
            T_slip.delta_grad_m(r), T_slip.slip_reason(r));
    end
end
fprintf('\n');

end

%% ========================================================================
%  break_arcs  -  insert NaT/NaN at time gaps so plot() does not connect
%                 points across separate visibility arcs.
%  ========================================================================
function [t_out, y_out] = break_arcs(t, y, gap_thresh_s)
% t : datetime column vector (sorted)
% y : double column vector (same length)
% gap_thresh_s : maximum gap in seconds before inserting a break
%
% Returns t_out, y_out with NaT/NaN rows inserted at every gap.

    n = numel(t);
    if n < 2
        t_out = t;
        y_out = y;
        return;
    end

    dt_sec = seconds(diff(t));
    gap_idx = find(dt_sec > gap_thresh_s);   % indices where gap follows

    if isempty(gap_idx)
        t_out = t;
        y_out = y;
        return;
    end

    % Pre-allocate output with room for NaN break rows
    m = n + numel(gap_idx);
    t_out = NaT(m, 1, 'TimeZone', t.TimeZone);
    y_out = NaN(m, 1);

    src = 1;   % read pointer into t/y
    dst = 1;   % write pointer into t_out/y_out
    for gi = 1:numel(gap_idx)
        seg_end = gap_idx(gi);
        seg_len = seg_end - src + 1;
        t_out(dst:dst+seg_len-1) = t(src:seg_end);
        y_out(dst:dst+seg_len-1) = y(src:seg_end);
        dst = dst + seg_len;
        % Insert NaT/NaN break
        % t_out(dst) stays NaT, y_out(dst) stays NaN
        dst = dst + 1;
        src = seg_end + 1;
    end
    % Copy remaining tail
    tail_len = n - src + 1;
    if tail_len > 0
        t_out(dst:dst+tail_len-1) = t(src:end);
        y_out(dst:dst+tail_len-1) = y(src:end);
    end
end
