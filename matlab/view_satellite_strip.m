%% ============================================================
%  Script: view_satellite_strip.m
%  Purpose: (satellite x epoch) strip charts of per-satellite status from
%           satellite_status_trace.csv -- elevation, the categorical
%           processing status, or the count of signals used.  One tile per
%           constellation (these quantities have no signal dimension).
%           Built on the shared renderer plotSatEpochStrip.m.
%
%  Elevation is the cheap context layer for every other strip: the low-
%  elevation band is where slips, residual blow-ups and multipath cluster,
%  so reading the locktime / gate rasters against this one is the point.
%
%  plotValue : 'elevation'    -> elevation_deg            [0..90]
%              'status'        -> categorical status      (Clean/Pivot/...)
%              'signals_used'  -> n_signals_used
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
plotValue = 'status';   % 'elevation' | 'status' | 'signals_used'

% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('satellite_status_trace.csv');
if ~isfile(fname), error('satellite status trace not found: %s', fname); end
R = readtable(fname, 'TextType','string');

need = ["week","tow_s","satellite","system"];
miss = need(~ismember(need, string(R.Properties.VariableNames)));
if ~isempty(miss)
    error('satellite_status_trace.csv missing columns: %s', strjoin(miss, ', '));
end

gpsEpoch = datetime(1980,1,6,0,0,0);
utc = gpsEpoch + seconds(double(R.week)*7*86400 + double(R.tow_s));

opts = struct();
opts.figTag = 'sat';
opts.facetLabelFallback = '';      % single tile per constellation
opts.markers = [];

switch lower(plotValue)
    case 'elevation'
        value = double(R.elevation_deg);
        opts.figTitle   = 'Satellite elevation';
        opts.valueLabel = 'Elevation (deg)';
        opts.colormap   = 'viridis';   % low elevation = dark/low end
        opts.caxis      = [0 90];
        opts.subtitle   = 'low elevation = dark (slip / multipath danger band)';
        mode = 'continuous';

    case 'signals_used'
        value = double(R.n_signals_used);
        opts.figTitle   = 'Signals used per satellite';
        opts.valueLabel = 'n signals used';
        opts.colormap   = 'viridis';
        opts.caxis      = 'auto';
        mode = 'continuous';

    case 'status'
        % Categorical raster.  Map each status to a stable index + color.
        statusStr = string(R.status);
        catDef = { ...   % name          color
            'Clean',      [0.20 0.65 0.30]; ...
            'Pivot',      [0.15 0.45 0.80]; ...
            'BelowMask',  [0.70 0.70 0.70]; ...
            'NoProduct',  [0.95 0.60 0.15]; ...
            'NoModel',    [0.80 0.15 0.15]; ...
            'Excluded',   [0.55 0.10 0.55]};
        names  = string(catDef(:,1));
        % Keep only statuses actually present, preserving the defined order.
        present = arrayfun(@(nm) any(statusStr==nm), names);
        names   = names(present);
        colors  = cell2mat(catDef(present,2));
        % Any unexpected status strings -> append as gray cats so nothing
        % is silently dropped.
        extra = setdiff(unique(statusStr), names, 'stable');
        extra = extra(extra ~= "" & ~ismissing(extra));
        if ~isempty(extra)
            names  = [names; extra];
            colors = [colors; repmat([0.4 0.4 0.4], numel(extra), 1)];
        end
        value = zeros(height(R),1);
        for k = 1:numel(names)
            value(statusStr==names(k)) = k;
        end
        value(value==0) = NaN;     % unknown -> transparent
        opts.figTitle   = 'Satellite processing status';
        opts.valueLabel = 'status';
        opts.mode       = 'categorical';
        opts.cat        = struct('labels', names, 'colors', colors);
        mode = 'categorical';

    otherwise
        error('plotValue must be ''elevation'', ''status'', or ''signals_used''');
end
opts.mode = mode;

D = table(string(R.system), string(R.satellite), strings(height(R),1), utc, value, ...
          'VariableNames', {'system','satellite','facet','utc','value'});
D = D(isfinite(D.value), :);

plotSatEpochStrip(D, opts);
