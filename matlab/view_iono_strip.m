%% ============================================================
%  Script: view_iono_strip.m
%  Purpose: (satellite x epoch) strip charts of the per-satellite slant
%           ionosphere state from ionosphere_trace.csv -- the estimate,
%           its standard deviation, or the vertical TEC.  One tile per
%           constellation (no signal dimension).  Built on the shared
%           renderer plotSatEpochStrip.m.
%
%  The iono-sigma raster reinflates at slips exactly like the ambiguity
%  sigma did -- an independent witness to the reset story -- and the iono
%  raster shows the active-ionosphere periods that gate GF slip trust
%  (the TurboEdit IVI cut).  PPP-RTK (uncombined) runs only.
%
%  plotValue : 'iono' -> slant iono delay (m), signed about 0
%              'std'  -> iono estimate sigma (m), high = hot
%              'vtec' -> vertical TEC mapped to range (m)
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
plotValue = 'std';   % 'iono' | 'std' | 'vtec'

% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('ionosphere_trace.csv');
if ~isfile(fname), error('ionosphere trace not found: %s', fname); end
R = readtable(fname, 'TextType','string');

need = ["week","tow_s","satellite","system","iono_m","std_m"];
miss = need(~ismember(need, string(R.Properties.VariableNames)));
if ~isempty(miss)
    error('ionosphere_trace.csv missing columns: %s', strjoin(miss, ', '));
end

gpsEpoch = datetime(1980,1,6,0,0,0);
utc = gpsEpoch + seconds(double(R.week)*7*86400 + double(R.tow_s));

opts = struct();
opts.figTag = 'iono';
opts.facetLabelFallback = '';
opts.markers = [];

switch lower(plotValue)
    case 'iono'
        value = double(R.iono_m);
        opts.figTitle   = 'Slant ionosphere estimate';
        opts.valueLabel = 'Slant iono (m)';
        opts.colormap   = 'vik';
        opts.caxis      = 'autosym';
        opts.diverging  = true;
    case 'std'
        value = double(R.std_m);
        opts.figTitle   = 'Ionosphere estimate \sigma';
        opts.valueLabel = 'Iono \sigma (m)';
        opts.colormap   = 'viridis_r';   % high sigma (just reset/uncertain) = dark end
        opts.caxis      = 'auto';
    case 'vtec'
        if ~ismember('vtec_m', string(R.Properties.VariableNames))
            error('vtec_m column not present in ionosphere_trace.csv');
        end
        value = double(R.vtec_m);
        opts.figTitle   = 'Vertical TEC (range)';
        opts.valueLabel = 'VTEC (m)';
        opts.colormap   = 'viridis';
        opts.caxis      = 'auto';
    otherwise
        error('plotValue must be ''iono'', ''std'', or ''vtec''');
end

D = table(string(R.system), string(R.satellite), strings(height(R),1), utc, value, ...
          'VariableNames', {'system','satellite','facet','utc','value'});
D = D(isfinite(D.value), :);

plotSatEpochStrip(D, opts);
