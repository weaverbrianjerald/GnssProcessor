%% ============================================================
%  Script: plot_wls_trace.m
%  Purpose: Read and visualize WLS trace output
%           + ENU conversion + position error magnitude
%  ============================================================

clear; clc; close all;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\wls_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\wls_trace.csv';
T = readtable(fname);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = T.week * 7 * 86400 + T.tow_s;
T.utc = gpsEpoch + seconds(gpsSeconds);

t = T.utc;

%% --- Extract variables ---
x = T.x_m;   y = T.y_m;   z = T.z_m;

clk = T.clock_m;
trop = T.tropo_m;

rms_m = T.rms_m;
max_abs = T.max_abs_m;

wls_clk = T.wls_clock_m;
kf_clk  = T.kf_clock_m;
dclk    = T.delta_clock_m;

jump = logical(T.jump_detected);
qgood = logical(T.wls_quality_good);
reasonable = logical(T.delta_reasonable);
seed = logical(T.should_seed);

%% ============================================================
%  ENHANCEMENT 1: ENU Conversion
% ============================================================

% Reference point for ENU (first epoch)
x0 = x(1);  y0 = y(1);  z0 = z(1);

% Convert reference ECEF → geodetic
llh = ecef2lla([x0 y0 z0]);
wgs84_lat = llh(1);
wgs84_lon = llh(2);
wgs84_h   = llh(3);

% Convert all ECEF → ENU
[E, N, U] = ecef2enu(x, y, z, ...
    wgs84_lat, wgs84_lon, wgs84_h, ...
    referenceEllipsoid('wgs84'), "degrees");

%% ============================================================
%  ENHANCEMENT 2: Position Error Magnitude
% ============================================================

pos_err = sqrt(E.^2 + N.^2 + U.^2);

%% ============================================================
%  Position Components (ECEF)
% ============================================================

figure('Name','WLS Position Components (ECEF)','Color','w');

subplot(3,1,1); hold on;
plot(t, x, 'b', 'LineWidth', 1.2);
ylabel('X (m)');
title('WLS Position Components (ECEF)');
grid on;

subplot(3,1,2); hold on;
plot(t, y, 'b', 'LineWidth', 1.2);
ylabel('Y (m)');
grid on;

subplot(3,1,3); hold on;
plot(t, z, 'b', 'LineWidth', 1.2);
ylabel('Z (m)');
xlabel('UTC Time');
grid on;

%% ============================================================
%  ENU Components
% ============================================================

figure('Name','WLS ENU Components','Color','w');

subplot(3,1,1); hold on;
plot(t, E, 'm', 'LineWidth', 1.2);
ylabel('East (m)');
title('WLS ENU Position Components');
grid on;

subplot(3,1,2); hold on;
plot(t, N, 'm', 'LineWidth', 1.2);
ylabel('North (m)');
grid on;

subplot(3,1,3); hold on;
plot(t, U, 'm', 'LineWidth', 1.2);
ylabel('Up (m)');
xlabel('UTC Time');
grid on;

%% ============================================================
%  Position Error Magnitude
% ============================================================

figure('Name','WLS Position Error Magnitude','Color','w'); hold on;
plot(t, pos_err, 'k', 'LineWidth', 1.4);
xlabel('UTC Time');
ylabel('Error (m)');
title('3D Position Error Magnitude (ENU)');
grid on;

%% ============================================================
%  Clock Estimates
% ============================================================

figure('Name','Clock Estimates','Color','w'); hold on;
plot(t, wls_clk, 'b', 'LineWidth', 1.2, 'DisplayName','WLS Clock');
plot(t, kf_clk,  'g', 'LineWidth', 1.2, 'DisplayName','KF Clock');
plot(t, dclk,    'r', 'LineWidth', 1.2, 'DisplayName','Δ Clock');
xlabel('UTC Time');
ylabel('Clock (m)');
title('Clock Estimates (WLS, KF, Δ)');
legend('Location','best');
grid on;

%% ============================================================
%  Tropo Estimate
% ============================================================

figure('Name','Tropo Estimate','Color','w'); hold on;
plot(t, trop, 'm', 'LineWidth', 1.2);
xlabel('UTC Time');
ylabel('Tropo (m)');
title('WLS Troposphere Estimate');
grid on;

%% ============================================================
%  RMS and Max Abs Residuals
% ============================================================

figure('Name','Residual Metrics','Color','w'); hold on;
plot(t, rms_m, 'b', 'LineWidth', 1.2, 'DisplayName','RMS (m)');
plot(t, max_abs, 'r', 'LineWidth', 1.2, 'DisplayName','Max Abs (m)');
xlabel('UTC Time');
ylabel('Residual (m)');
title('WLS Residual Metrics');
legend('Location','best');
grid on;

%% ============================================================
%  Boolean Flags
% ============================================================

figure('Name','WLS Flags','Color','w');

subplot(4,1,1); hold on;
stem(t, jump, 'r', 'filled');
ylabel('Jump');
title('WLS Boolean Flags');
grid on;

subplot(4,1,2); hold on;
stem(t, qgood, 'g', 'filled');
ylabel('Quality');

subplot(4,1,3); hold on;
stem(t, reasonable, 'b', 'filled');
ylabel('Reasonable');

subplot(4,1,4); hold on;
stem(t, seed, 'k', 'filled');
ylabel('Seed');
xlabel('UTC Time');
grid on;
