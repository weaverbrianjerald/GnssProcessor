%% ============================================================
%  Script: view_slip_gate_strip.m
%  Purpose: (satellite x epoch) strip charts of the cycle-slip detector
%           gate -- the deviation, the adaptive threshold it is tested
%           against, and their ratio -- across the whole constellation at
%           once.  Complements the per-arc line plots in
%           view_mwwl_trace.m / view_turbo_edit_trace.m: the raster shows
%           whether a near-gate epoch is one satellite or a constellation-
%           wide event, and where each detector runs hot.
%
%  Built on the shared renderer plotSatEpochStrip.m.  One tile per signal
%  pair (e.g. L1C/L2W); slip-flag epochs overlaid as glyphs.
%
%  source    : 'mw' -> mwwl_trace.csv      (Melbourne-Wubbena wide-lane)
%              'gf' -> turbo_edit_trace.csv (TurboEdit geometry-free TD2)
%  plotValue : 'ratio'     -> deviation / threshold  (|y|>1 == over gate)
%              'deviation' -> the raw statistic tested (cyc for MW, m for GF)
%              'threshold' -> the adaptive threshold itself
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
source    = 'mw';        % 'mw' | 'gf'
plotValue = 'threshold';     % 'ratio' | 'deviation' | 'threshold'
ratioCap  = 3;           % +/- color saturation for ratio mode (gate at +/-1)
showSlipMarkers = true;  % overlay slip-flag epochs

% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
logDir = gnssVizLogDir();

%% --- Load + melt the chosen trace into the long-form table ---
switch lower(source)
    case 'mw'
        fname = fullfile(logDir, 'mwwl_trace.csv');
        if ~isfile(fname), error('MW trace not found: %s', fname); end
        R = readtable(fname, 'TextType','string');
        R.facet = R.sig1 + "/" + R.sig2;
        % Active deviation the detector gated; fall back to dev_from_mean
        % on init/LLI rows that carry no active_dev.
        dev = getNum(R, 'active_dev_cycles');
        if all(isnan(dev)), dev = getNum(R, 'dev_from_mean_cycles'); end
        nanrows = ~isfinite(dev);
        dev(nanrows) = getNum(R, 'dev_from_mean_cycles', nanrows);
        thr = getNum(R, 'threshold_cycles');
        devLbl = 'MW active dev (cyc)';
        thrLbl = 'MW threshold (cyc)';
        ratLbl = 'MW dev / threshold';
        % Markers: detector-flagged slip epochs (LLI distinguished).
        isSlip = getNum(R,'is_slip')==1;
        isLli  = getNum(R,'is_lli')==1;
        mkKind = strings(height(R),1);
        mkKind(isSlip) = "slip";
        mkKind(isLli & ~isSlip) = "lli";
        mkMask = mkKind ~= "";
    case 'gf'
        fname = fullfile(logDir, 'turbo_edit_trace.csv');
        if ~isfile(fname), error('TurboEdit trace not found: %s', fname); end
        R = readtable(fname, 'TextType','string');
        R.facet = R.sig1 + "/" + R.sig2;
        dev = getNum(R, 'td2_gf_m');
        thr = getNum(R, 'thresh_m'); thr(thr < 0) = NaN;   % -1 sentinel = immature
        devLbl = 'GF TD2 (m)';
        thrLbl = 'GF threshold (m)';
        ratLbl = 'GF TD2 / threshold';
        % Markers: any flagged event that touched the state (action ~= none).
        flagged = (getNum(R,'mw_flagged')==1 | getNum(R,'gf_flagged')==1);
        act = string(R.action);
        flagged = flagged & act ~= "none";
        mkKind = strings(height(R),1);
        mkKind(flagged) = act(flagged);          % "repair" / "reset"
        mkMask = flagged;
    otherwise
        error('source must be ''mw'' or ''gf''');
end

%% --- Resolve plotValue -> value column + render options ---
switch lower(plotValue)
    case 'deviation'
        value = dev;  valLbl = devLbl;  cmapName = 'vik';
        cax = 'autosym';  divr = true;  refTicks = [];
    case 'threshold'
        value = thr;  valLbl = thrLbl;  cmapName = 'viridis';
        cax = 'auto';     divr = false; refTicks = [];
    case 'ratio'
        value = dev ./ thr;  valLbl = ratLbl;  cmapName = 'vik';
        cax = [-ratioCap ratioCap];  divr = true;  refTicks = [-ratioCap -1 0 1 ratioCap];
    otherwise
        error('plotValue must be ''ratio'', ''deviation'', or ''threshold''');
end

%% --- Long-form table for the renderer ---
gpsEpoch = datetime(1980,1,6,0,0,0);
utc = gpsEpoch + seconds(double(R.week)*7*86400 + double(R.tow_s));
D = table(string(R.system), string(R.satellite), R.facet, utc, value, ...
          'VariableNames', {'system','satellite','facet','utc','value'});
D = D(isfinite(D.value), :);

%% --- Markers table ---
markers = [];
if showSlipMarkers && any(mkMask)
    markers = table(string(R.system(mkMask)), string(R.satellite(mkMask)), ...
                    R.facet(mkMask), utc(mkMask), mkKind(mkMask), ...
                    'VariableNames', {'system','satellite','facet','utc','kind'});
end

%% --- Render ---
opts = struct();
opts.figTitle   = sprintf('%s slip-detector gate (%s)', upper(source), lower(plotValue));
opts.figTag     = sprintf('%s-gate', lower(source));
opts.valueLabel = valLbl;
opts.colormap   = cmapName;
opts.caxis      = cax;
opts.diverging  = divr;
opts.refTicks   = refTicks;
opts.markers    = markers;
opts.markerStyles = struct( ...
    'slip',   struct('marker','o','color',[0 0 0],        'size',6), ...
    'lli',    struct('marker','s','color',[0.2 0.2 0.2],  'size',6), ...
    'repair', struct('marker','o','color',[0 0 0],        'size',6), ...
    'reset',  struct('marker','x','color',[0.85 0 0.85],  'size',8));
opts.subtitle = 'tile = signal pair    markers = detector-flagged slip epochs';

plotSatEpochStrip(D, opts);

%% --- local helper ---
function v = getNum(T, col, mask)
% Numeric column fetch with missing-column tolerance; optional row mask.
    if ~ismember(col, string(T.Properties.VariableNames))
        v = nan(height(T),1);
    else
        v = double(T.(col));
    end
    if nargin >= 3, v = v(mask); end
end
