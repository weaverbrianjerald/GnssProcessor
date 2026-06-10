%% ============================================================
%  Script: plot_state_trace.m
%  Purpose: Read and visualize PPP state trace (position, clock, tropo)
%           + ENU conversion + position error magnitude
%  ============================================================

clear all; clc; %close all;

%% --- Load CSV file ---
fname = 'C:\GnssProcessor\logs\state_trace.csv';
%fname = 'C:\GnssProcessor\batch_results_GE_Dual\JLPM_week_2025_200_215\2025-200\JPLM\logs\state_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Release\logs\state_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\state_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Release\logs\smoother_trace.csv';
T = readtable(fname);

%% --- Convert GPS week + TOW to UTC datetime ---
gpsEpoch = datetime(1980,1,6,0,0,0);
try
    gpsSeconds = T.week * 7 * 86400 + T.tow_s;
catch
    gpsSeconds = T.week * 7 * 86400 + T.tow;
end
% Trace files carry pure GPS time so timestamps match the RINEX epochs.
T.utc = gpsEpoch + seconds(gpsSeconds);

%% --- Extract variables ---
% Core
t = T.utc;

try
    x  = T.x_m;     y  = T.y_m;     z  = T.z_m;
catch
    x  = T.x;     y  = T.y;     z  = T.z;
end
try
    xs = T.x_std_m; ys = T.y_std_m; zs = T.z_std_m;
catch
    xs = T.sigma_x; ys = T.sigma_y; zs = T.sigma_z;
end


try
    clk  = T.clock_m;
catch
    clk  = [];
end

try
    clks = T.clock_std_m;
catch
    try
        clks = T.clock_m;
    catch
        clks = [];
    end
end

try
    trop  = T.tropo_m;
    trops = T.tropo_std_m;
catch
    trop  = T.tropo_zwd_m;
    trops = T.sigma_tropo_m;
end

% Extras
%%% Galileo clock
try
    galClk = T.("Galileo_m");
    galClk_std = T.("Galileo_std_m");
catch
    galClk = []; galClk_std = [];
end

%%% Tropo grad north
try
    gradN = T.("gn_m");
    gradN_std = T.("gn_std_m");
catch
    gradN = []; gradN_std = [];
end

%%% Tropo grad east
try
    gradE = T.("ge_m");
    gradE_std = T.("ge_std_m");
catch
    gradE = []; gradE_std = [];
end

%%% Phase clock
try
    phasClk = T.("phase_clock_m");
    phasClk_std = T.("phase_clock_std_m");
catch
    phasClk = []; phasClk_std = [];
end

%% ============================================================
%  ENU Conversion
% ============================================================

% Reference point for ENU
lxyz = min([length(x),length(y),length(z)]);
x0 = median(x(lxyz-round(lxyz/2,0):end),'omitmissing');  y0 = median(y(lxyz-round(lxyz/2,0):end),'omitmissing');  z0 = median(z(lxyz-round(lxyz/2,0):end),'omitmissing'); % median of post-converged
%x0 = median(x(120:end),'omitmissing');  y0 = median(y(120:end),'omitmissing');  z0 = median(z(120:end),'omitmissing'); % median of post-converged
%x0 = x(end-1);  y0 = y(end-1); z0 = z(end-1); % end epoch
%x0 = -2281621.777;  y0 = -1453585.151;  z0 = 5756964.848; % GCGO (gnss-qc)
%x0 = -1248596.4806;  y0 = -4819428.1948;  z0 = 3976505.8949; % AMC4 (NRCAN PPP)
%x0 = -2493304.6796;  y0 = -4655215.1032;  z0 = 3565497.5918; % JPLM (IGS)

% Convert reference ECEF → geodetic
[llh] = ecef2lla([x0 y0 z0]);
wgs84_lat = llh(1);
wgs84_lon = llh(2);
wgs84_h   = llh(3);

% Convert all ECEF → ENU
[E,N,U] = ecef2enu(x, y, z, wgs84_lat, wgs84_lon, wgs84_h,referenceEllipsoid('wgs84'),"degrees");

%% ============================================================
%  Position Error Magnitude
% ============================================================

pos_err = sqrt(E.^2 + N.^2 + U.^2);

%% ============================================================
%  Position Components with ±1σ
% ============================================================

% figure('Name','Position Components','Color','w');
% 
% subplot(3,1,1); hold on;
% plot(t, x, 'b', 'LineWidth', 1.2);
% plot(t, x + xs, '--r');
% plot(t, x - xs, '--r');
% ylabel('X (m)');
% title('Position Components with 1σ Uncertainty');
% grid on;
% 
% subplot(3,1,2); hold on;
% plot(t, y, 'b', 'LineWidth', 1.2);
% plot(t, y + ys, '--r');
% plot(t, y - ys, '--r');
% ylabel('Y (m)');
% grid on;
% 
% subplot(3,1,3); hold on;
% plot(t, z, 'b', 'LineWidth', 1.2);
% plot(t, z + zs, '--r');
% plot(t, z - zs, '--r');
% ylabel('Z (m)');
% xlabel('GPS Time');
% grid on;

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
xlabel('GPS Time');
grid on;

%% ============================================================
%  Position Error: E / N / U / 3D combined
% ============================================================

cE = [0 0 1];     % blue
cN = [0 0.6 0];   % green
cU = [1 0 0];     % red
c3 = [0 0 0];     % black

figure('Name','Position Error','Color','w'); hold on; grid on; box on;
plot(t, E,       'Color', cE, 'LineWidth', 1.2);
plot(t, N,       'Color', cN, 'LineWidth', 1.2);
plot(t, U,       'Color', cU, 'LineWidth', 1.2);
plot(t, pos_err, 'Color', c3, 'LineWidth', 1.4);
xlabel('GPS Time');
ylabel('Error (m)');
title('ENU & 3D Position Error');
legend({'E','N','U','3D'}, 'Location','best');

convergedIdx = 120;
E=E(convergedIdx:end); N=N(convergedIdx:end); U=U(convergedIdx:end);
mE = mean(E,'omitnan')*100;       sE = std(E,'omitnan')*100;
mN = mean(N,'omitnan')*100;       sN = std(N,'omitnan')*100;
mU = mean(U,'omitnan')*100;       sU = std(U,'omitnan')*100;
m3 = mean(pos_err,'omitnan')*100; s3 = std(pos_err,'omitnan')*100;

txt = @(y, c, lbl, mu, sg) text(0.02, y, ...
    sprintf('%3s: mean = %4.1f cm; sigma = %4.1f cm', lbl, mu, sg), ...
    'Units','normalized', 'Color', c, ...
    'FontWeight','bold', 'VerticalAlignment','top','FontName','courier');
txt(0.97, cE, 'E',  mE, sE);
txt(0.93, cN, 'N',  mN, sN);
txt(0.89, cU, 'U',  mU, sU);
txt(0.85, c3, '3D', m3, s3);

%% ============================================================
%  Clock Bias with ±1σ
% ============================================================
% GPS
if ~isempty(clk)
    figure('Name','GPS Clock Bias','Color','w'); hold on;
    plot(t, clk, 'k', 'LineWidth', 1.2);
    plot(t, clk + clks, '--r');
    plot(t, clk - clks, '--r');
    xlabel('GPS Time');
    ylabel('Clock Bias (m)');
    title('Receiver Clock Bias with 1σ Uncertainty');
    grid on;
end

% Galileo
if ~isempty(galClk)
    figure('Name','GAL Clock Bias','Color','w'); hold on;
    plot(t, galClk, 'k', 'LineWidth', 1.2);
    plot(t, galClk + galClk_std, '--r');
    plot(t, galClk - galClk_std, '--r');
    xlabel('GPS Time');
    ylabel('Clock Bias (m)');
    title('Galileo Clock Bias with 1σ Uncertainty');
    grid on;
end

% Phase clock
if ~isempty(phasClk)
    figure('Name','Phase Clock Bias','Color','w'); hold on;
    plot(t, phasClk, 'k', 'LineWidth', 1.2);
    plot(t, phasClk + phasClk_std, '--r');
    plot(t, phasClk - phasClk_std, '--r');
    xlabel('GPS Time');
    ylabel('Phase Clock Bias (m)');
    title('Phase Clock Bias with 1σ Uncertainty');
    grid on;
end

%% ============================================================
%  Troposphere Estimate with ±1σ
% ============================================================

figure('Name','Troposphere','Color','w'); hold on;
plot(t, trop, 'g', 'LineWidth', 1.2);
plot(t, trop + trops, '--r');
plot(t, trop - trops, '--r');
xlabel('GPS Time');
ylabel('Tropo Delay (m)');
title('Tropospheric Zenith Delay with 1σ Uncertainty');
grid on;

% North gradient
if ~isempty(gradN) && ~isempty(gradE)
    figure('Name','Tropo gradient','Color','w');
    title('Tropospheric azimuthal gradients');
    tiledlayout(2,1);
    nexttile;
    plot(t, gradN, 'g', 'LineWidth', 1.2); hold on;
    plot(t, gradN + gradN_std, '--r');
    plot(t, gradN - gradN_std, '--r');
    xlabel('GPS Time');
    ylabel('North (m)');
    grid on; box on;

    nexttile;
    plot(t, gradE, 'b', 'LineWidth', 1.2); hold on;
    plot(t, gradE + gradE_std, '--r');
    plot(t, gradE - gradE_std, '--r');
    xlabel('GPS Time');
    ylabel('East (m)');
    grid on; box on;
end

%% ============================================================
%  Optional: 3D Trajectory
% ============================================================

figure('Name','3D Position','Color','w');
plot3(x, y, z, '-b', 'LineWidth', 1.2);
xlabel('X (m)'); ylabel('Y (m)'); zlabel('Z (m)');
title('3D Receiver Trajectory');
grid on; axis equal;
