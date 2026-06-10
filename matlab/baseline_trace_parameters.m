%% ============================================================
%  Script: baseline_trace_parameters.m
%  Purpose: Read and visualize DD baseline trace (rover position,
%           1σ uncertainty, DD throughput, ambiguity count)
%           emitted by Application::runBaseline().
%
%  CSV columns expected (set by Application.cpp ~line 1409):
%    week, tow, x, y, z, sigma_x, sigma_y, sigma_z,
%    dds_applied, n_dd_total, n_ambs
%
%  ENU plots are referenced to the master reference station ECEF,
%  so the rover trace shows the baseline vector directly.  Override
%  the reference point below if running against a different dataset.
%  ============================================================

clear all; clc; %close all;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\baseline_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\baseline_trace.csv';
T = readtable(fname);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0);
try
    gpsSeconds = T.week * 7 * 86400 + T.tow_s;
catch
    gpsSeconds = T.week * 7 * 86400 + T.tow;
end
% Trace files carry pure GPS time so timestamps match RINEX epochs.
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Extract variables ---
t  = T.utc;

try
    x  = T.x_m;     y  = T.y_m;     z  = T.z_m;
catch
    x  = T.x;       y  = T.y;       z  = T.z;
end
try
    xs = T.x_std_m; ys = T.y_std_m; zs = T.z_std_m;
catch
    xs = T.sigma_x; ys = T.sigma_y; zs = T.sigma_z;
end

% DD throughput counters — optional; older trace formats may omit them.
% The pre-rebuild-loop format used "n_dd_total" for the post-update count;
% the current format uses "dds_built" for the pre-update built count.  Try
% the new name first, fall back to the legacy name so older traces still load.
try   ddsApplied = T.dds_applied; catch, ddsApplied = []; end
try   ddsBuilt   = T.dds_built;
catch
    try   ddsBuilt = T.n_dd_total;
    catch, ddsBuilt = []; end
end
try   nAmbs      = T.n_ambs;      catch, nAmbs      = []; end

%% ============================================================
%  Reference station ECEF (origin for ENU baseline view)
%
%  Default = OSU C103 (matches src/core/config/gnss_config.json).
%  Replace these with the reference station coordinates configured
%  for your run if you swap datasets.  A bad reference point doesn't
%  break the plots — it just shifts the ENU origin.
% ============================================================

ref_x = -2493304.6796;  ref_y = -4655215.1032;  ref_z =  3565497.5918;  % JPLM (IGS_Pasadena)
   
%ref_x = -2497386.1104;  ref_y = -3805407.1957;  ref_z =  4453040.9909;  % C103 (OSU campus)
%ref_x = -2281621.777;   ref_y = -1453585.151;   ref_z =  5756964.848;  % GCGO (gnss-qc)



% Per-epoch baseline length (rover-to-reference distance, meters)
baseline_len = sqrt((x - ref_x).^2 + (y - ref_y).^2 + (z - ref_z).^2);

%% ============================================================
%  Convergence summary (last 25% of run)
% ============================================================

n      = numel(t);
i_tail = max(1, round(0.75 * n)) : n;
xf     = mean(x(i_tail));
yf     = mean(y(i_tail));
zf     = mean(z(i_tail));

% Convert reference ECEF -> geodetic
llh = ecef2lla([xf yf zf]);
ref_lat = llh(1);
ref_lon = llh(2);
ref_h   = llh(3);

% Convert all rover ECEF -> ENU relative to ending epochs
[E, N, U] = ecef2enu(x, y, z, ref_lat, ref_lon, ref_h, ...
                     referenceEllipsoid('wgs84'), 'degrees');

% Distance of each epoch's solution from the late-arc mean — a quick
% empirical "convergence curve" without needing external truth.
conv_err = sqrt((x - xf).^2 + (y - yf).^2 + (z - zf).^2);

fprintf('\n=== Baseline trace summary ===\n');
fprintf('  epochs:           %d\n', n);
fprintf('  time span:        %s  ->  %s\n', string(t(1)), string(t(end)));
fprintf('  late-arc mean ECEF: (%.4f, %.4f, %.4f) m\n', xf, yf, zf);
fprintf('  late-arc baseline length: %.4f km\n', mean(baseline_len(i_tail))/1000);
fprintf('  late-arc 3D RMS vs late-arc mean: %.4f m\n', ...
        sqrt(mean(conv_err(i_tail).^2)));
fprintf('  late-arc 1σ (x, y, z) [m]: (%.4f, %.4f, %.4f)\n', ...
        mean(xs(i_tail)), mean(ys(i_tail)), mean(zs(i_tail)));
if ~isempty(ddsApplied)
    fprintf('  late-arc dds_applied/epoch: mean=%.1f, min=%d, max=%d\n', ...
            mean(ddsApplied(i_tail)), min(ddsApplied(i_tail)), max(ddsApplied(i_tail)));
end
if ~isempty(ddsApplied) && ~isempty(ddsBuilt)
    rebuilt = sum(ddsBuilt > ddsApplied & ddsApplied > 0);
    dropped = sum(ddsApplied == 0 & ddsBuilt > 0);
    fprintf('  epochs with rebuild-applied (built > applied > 0): %d\n', rebuilt);
    fprintf('  epochs dropped (applied==0 with built>0): %d\n', dropped);
end
fprintf('==============================\n\n');

%% ============================================================
%  ECEF Position Components with ±1σ
% ============================================================

figure('Name','ECEF Position','Color','w');

subplot(3,1,1); hold on;
plot(t, x, 'b', 'LineWidth', 1.2);
plot(t, x + xs, '--r');
plot(t, x - xs, '--r');
ylabel('X (m)');
title('Rover ECEF Position with 1σ Uncertainty');
grid on;

subplot(3,1,2); hold on;
plot(t, y, 'b', 'LineWidth', 1.2);
plot(t, y + ys, '--r');
plot(t, y - ys, '--r');
ylabel('Y (m)');
grid on;

subplot(3,1,3); hold on;
plot(t, z, 'b', 'LineWidth', 1.2);
plot(t, z + zs, '--r');
plot(t, z - zs, '--r');
ylabel('Z (m)');
xlabel('GPS Time');
grid on;

%% ============================================================
%  ENU Position w.r.t. Reference (= baseline vector)
% ============================================================

figure('Name','ENU Baseline Vector','Color','w');

subplot(3,1,1); hold on;
plot(t, E, 'm', 'LineWidth', 1.2);
ylabel('East (m)');
title('Rover ENU Position (relative to reference station)');
grid on;

subplot(3,1,2); hold on;
plot(t, N, 'm', 'LineWidth', 1.2);
ylabel('North (m)');
grid on;

subplot(3,1,3); hold on;
plot(t, U, 'm', 'LineWidth', 1.2);
ylabel('Up (m)');
xlabel('GPS Time');
grid on;

%% ============================================================
%  Empirical convergence curve (3D distance from late-arc mean)
% ============================================================

figure('Name','Convergence to late-arc mean','Color','w'); hold on;
plot(t, conv_err, 'k', 'LineWidth', 1.4);
xlabel('GPS Time');
ylabel('Distance to late-arc mean (m)');
title('3D Position Error vs. Late-Arc Mean Position');
set(gca, 'YScale', 'log');
grid on;

%% ============================================================
%  1σ Uncertainty Evolution
% ============================================================

figure('Name','Position 1σ','Color','w'); hold on;
plot(t, xs, 'b', 'LineWidth', 1.2);
plot(t, ys, 'g', 'LineWidth', 1.2);
plot(t, zs, 'r', 'LineWidth', 1.2);
xlabel('GPS Time');
ylabel('1σ (m)');
title('Position Uncertainty (Filter Covariance Diagonal)');
legend('σ_x','σ_y','σ_z','Location','best');
set(gca, 'YScale', 'log');
grid on;

%% ============================================================
%  DD Throughput per Epoch
% ============================================================

if ~isempty(ddsApplied) || ~isempty(ddsBuilt)
    figure('Name','DD Throughput','Color','w'); hold on;
    if ~isempty(ddsBuilt)
        plot(t, ddsBuilt,   '-', 'Color', [0.6 0.6 0.6], 'LineWidth', 1.0, ...
             'DisplayName','DDs built (pre-update)');
    end
    if ~isempty(ddsApplied)
        plot(t, ddsApplied, '-b', 'LineWidth', 1.2, ...
             'DisplayName','DDs applied (post-rebuild)');
    end
    xlabel('GPS Time');
    ylabel('DD count / epoch');
    title('Double-Difference Measurement Throughput');
    legend('Location','best');
    grid on;
end

%% ============================================================
%  DD Phase-Ambiguity State Count
% ============================================================

if ~isempty(nAmbs) && any(nAmbs > 0)
    figure('Name','DD Ambiguity States','Color','w'); hold on;
    plot(t, nAmbs, 'k', 'LineWidth', 1.2);
    xlabel('GPS Time');
    ylabel('# DD ambiguity states');
    title('Active DD Phase Ambiguities (state vector size minus position)');
    grid on;
end

%% ============================================================
%  Baseline Length Time Series
%
%  Should be roughly constant across the run (it's a static
%  geometry).  Drift here indicates rover-position drift in the
%  filter — useful sanity check independent of ENU framing.
% ============================================================

figure('Name','Baseline Length','Color','w'); hold on;
plot(t, baseline_len/1000, 'b', 'LineWidth', 1.2);
xlabel('GPS Time');
ylabel('Baseline length (km)');
title('Rover-to-Reference Baseline Length per Epoch');
grid on;

%% ============================================================
%  3D Trajectory
% ============================================================

figure('Name','3D Trajectory','Color','w');
plot3(x, y, z, '-b', 'LineWidth', 1.2); hold on;
plot3(ref_x, ref_y, ref_z, 'r^', 'MarkerSize', 10, 'MarkerFaceColor','r', ...
      'DisplayName','reference station');
plot3(x(end), y(end), z(end), 'go', 'MarkerSize', 8, 'MarkerFaceColor','g', ...
      'DisplayName','final rover');
xlabel('X (m)'); ylabel('Y (m)'); zlabel('Z (m)');
title('3D Trajectory (rover blue, reference red, final green)');
grid on; axis equal;
legend('Location','best');
