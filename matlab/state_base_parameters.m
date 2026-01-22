%% ============================================================
%  Script: plot_state_trace.m
%  Purpose: Read and visualize PPP state trace (position, clock, tropo)
%           + ENU conversion + position error magnitude
%  ============================================================

clear all; clc; close all;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\state_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\state_trace.csv';
T = readtable(fname);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0,'TimeZone','UTC');
gpsSeconds = T.week * 7 * 86400 + T.tow_s;
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Extract variables ---
t = T.utc;

x  = T.x_m;     y  = T.y_m;     z  = T.z_m;
xs = T.x_std_m; ys = T.y_std_m; zs = T.z_std_m;

clk  = T.clock_m;
clks = T.clock_std_m;

trop  = T.tropo_m;
trops = T.tropo_std_m;

%% ============================================================
%  ENHANCEMENT 1: ENU Conversion
% ============================================================

% Reference point for ENU (use first epoch)
x0 = x(end);  y0 = y(end);  z0 = z(end);

% Convert reference ECEF → geodetic
[llh] = ecef2lla([x0 y0 z0]);
wgs84_lat = llh(1);
wgs84_lon = llh(2);
wgs84_h   = llh(3);

% Convert all ECEF → ENU
[E,N,U] = ecef2enu(x, y, z, wgs84_lat, wgs84_lon, wgs84_h,referenceEllipsoid('wgs84'),"degrees");

%% ============================================================
%  ENHANCEMENT 2: Position Error Magnitude
% ============================================================

pos_err = sqrt(E.^2 + N.^2 + U.^2);

%% ============================================================
%  Position Components with ±1σ
% ============================================================

figure('Name','Position Components','Color','w');

subplot(3,1,1); hold on;
plot(t, x, 'b', 'LineWidth', 1.2);
plot(t, x + xs, '--r');
plot(t, x - xs, '--r');
ylabel('X (m)');
title('Position Components with 1σ Uncertainty');
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
xlabel('UTC Time');
grid on;

%% ============================================================
%  ENU Components
% ============================================================

figure('Name','ENU Components','Color','w');

subplot(3,1,1); hold on;
plot(t, E, 'm', 'LineWidth', 1.2);
ylabel('East (m)');
title('ENU Position Components');
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

figure('Name','Position Error Magnitude','Color','w'); hold on;
plot(t, pos_err, 'k', 'LineWidth', 1.4);
xlabel('UTC Time');
ylabel('Error (m)');
title('3D Position Error Magnitude (ENU)');
grid on;

%% ============================================================
%  Clock Bias with ±1σ
% ============================================================

figure('Name','Clock Bias','Color','w'); hold on;
plot(t, clk, 'k', 'LineWidth', 1.2);
plot(t, clk + clks, '--r');
plot(t, clk - clks, '--r');
xlabel('UTC Time');
ylabel('Clock Bias (m)');
title('Receiver Clock Bias with 1σ Uncertainty');
grid on;

%% ============================================================
%  Troposphere Estimate with ±1σ
% ============================================================

figure('Name','Troposphere','Color','w'); hold on;
plot(t, trop, 'g', 'LineWidth', 1.2);
plot(t, trop + trops, '--r');
plot(t, trop - trops, '--r');
xlabel('UTC Time');
ylabel('Tropo Delay (m)');
title('Tropospheric Zenith Delay with 1σ Uncertainty');
grid on;

%% ============================================================
%  Optional: 3D Trajectory
% ============================================================

figure('Name','3D Position','Color','w');
plot3(x, y, z, '-b', 'LineWidth', 1.2);
xlabel('X (m)'); ylabel('Y (m)'); zlabel('Z (m)');
title('3D Receiver Trajectory');
grid on; axis equal;
