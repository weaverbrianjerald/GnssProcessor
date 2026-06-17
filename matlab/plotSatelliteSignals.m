%% ============================================================
%  Script: plotSatelliteSignals.m
%  Purpose: Per-epoch, per-satellite SIGNAL-usage strip chart.
%
%  Companion to plotSatelliteStatus.m.  Where that script answers
%  "was the satellite used?" (one row per sat, binary raster), this one
%  resolves the usage into the actual signals tracked -- which frequency
%  BANDS and which tracking CHANNELS (RINEX attributes) each satellite
%  contributed at every epoch.
%
%  DATA SOURCE
%  -----------
%  measurement_residuals.csv -- the same file measurement_residuals.m
%  reads.  It carries one row per (epoch, satellite, signal) for every
%  signal the filter actually USED in its update, so a signal's presence
%  in the file == "this signal was used this epoch".
%
%  Required columns (read by name; order irrelevant):
%      week, tow_s     GPS week + time-of-week  -> epoch axis
%      satellite       e.g. "G01"  (BSSD "E02-E36" -> primary "E02")
%      signal          RINEX-3 obs code, e.g. "L1C", "C2W", "L5X"
%
%  Each obs code decomposes as  <type><band><attribute>:
%      type      L/C/D/S      carrier phase / code / doppler / SNR
%      band      the digit    1,2,5,6,7,8,9   -> drives COLOR
%      channel   attribute    C,W,Q,X,I,L,... -> the "many of them" dim
%  L1C and C1C are the SAME signal (band 1, channel C) measured two ways,
%  so we collapse on (band, channel) and treat type as a sub-dimension.
%
%  VISUAL ENCODING -- the channel problem
%  --------------------------------------
%  There are far too many tracking channels to give each its own color,
%  so color is reserved for the (few) frequency bands and the channel
%  dimension is layered on without cluttering the raster.  Each satellite
%  is split into one thin sub-row per band it tracks; a cell is filled
%  when that (sat, band) was used at that epoch.
%
%  Each satellite is split into one thin sub-row per band it tracks, and
%  the band is named in the ROW LABEL ("G01 L1" / "G01 L2"), which frees
%  the cell COLOR to carry the channel dimension.  How a cell is colored
%  is selectable via channelEncoding below:
%
%    'count'   (default) DISCRETE color = how many distinct tracking
%              channels were used in that band this epoch:
%                  1 = yellow   2 = orange   3 = blue   4 = cyan
%                  5+ = purple        (all editable in countColors)
%              You do not see WHICH channels, but you instantly see e.g.
%              "G01 L1 = blue = 3 channels (L1C + L1W + L1L)", and any
%              drop to a lower count shows up as a hard color change
%              rather than a subtle shading difference.
%    'shade'   COLOR = frequency band, brightness = #channels (light = 1).
%              Keeps the band visible in color too, at the cost of the
%              subtler count cue.
%    'obstype' COLOR = band, brightness = phase-only / code-only / both --
%              highlights where code dropped out (CodeOutlier epochs).
%    'letters' COLOR = band, attribute letters (C/W/Q/X...) drawn inside
%              each cell (auto-hidden above maxLabelEpochs epochs).
%
%  In EVERY mode, CLICK any cell (data tips) to read the exact codes,
%  e.g.  "G01 L1 -> L1C  L1W  L1L".  A per-code tally is also printed to
%  the command window.
%
%  Expected to be run as a script (edit the switches below, press Run),
%  matching the style of the other matlab/ tools.
%  ============================================================

clear all; %clc; %close all;

%% --- Data file ---
% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('measurement_residuals.csv');

%% --- Layout switch ---
% 'per_system' : one figure per constellation (mirrors plotSatelliteStatus).
% 'single'     : every constellation stacked into a single figure.
layout = 'per_system';

%% --- Channel-encoding switch (see header) ---
% 'count'   : discrete color = # distinct channels used (band in label) [default]
% 'shade'   : color = band, brightness = # channels
% 'obstype' : color = band, brightness = phase / code / both
% 'letters' : color = band, attribute letters drawn in each cell
channelEncoding = 'count';
maxLabelEpochs  = 150;   % 'letters' overlay auto-hides above this many epochs

%% --- Content switches ---
showLegend       = true;   % band-color legend on each figure
showChannelTally = true;   % print band|channel usage table to the console

%% --- Load ---
if ~isfile(fname)
    error('plotSatelliteSignals:fileNotFound', ...
          'measurement_residuals.csv not found: %s', fname);
end
T = readtable(fname, 'TextType', 'string');
T.Properties.VariableNames = lower(T.Properties.VariableNames);

need = ["week","tow_s","satellite","signal"];
miss = need(~ismember(need, string(T.Properties.VariableNames)));
if ~isempty(miss)
    error('plotSatelliteSignals:missingColumns', ...
          'CSV missing required column(s): %s', strjoin(miss, ', '));
end

%% --- Strip BSSD compound IDs ("E02-E36" / "G05/G03" -> primary) ---
% Same reasoning as measurement_residuals.m: keep the physical satellite
% as one categorical value so a mid-run pivot handover does not split its
% rows.  The engine writes the SD pair as "<primary><sep><pivot>"; it has
% used both '-' (current) and '/' (older) as the separator, so strip
% either.  (Pivot satellites have no SD residual against themselves, so
% they are simply absent on epochs where they ARE the pivot -- rendered
% transparent here, exactly like a not-tracked gap.)  A bare id like
% "G01" contains neither separator and passes through untouched.
satRaw = string(T.satellite);
for sep = ["-", "/"]
    hasSep = contains(satRaw, sep);
    satRaw(hasSep) = extractBefore(satRaw(hasSep), sep);
end
T.satellite = satRaw;

%% --- Decompose the signal code into type / band / channel ---
% RINEX-3 obs codes are exactly <type><band><attr>: type is one letter
% (L/C/D/S), band is one digit, attr is the remainder.
sig         = string(T.signal);
T.obstype   = extractBefore(sig, 2);     % "L" / "C" / "D" / "S"
T.band      = extractBetween(sig, 2, 2); % the frequency digit
T.chan      = extractAfter(sig, 2);      % tracking attribute (may be "")

goodSig = ~isnan(str2double(T.band));    % band must be a digit
if ~all(goodSig)
    warning('plotSatelliteSignals:badSignal', ...
        'Dropping %d row(s) whose signal code is not <type><band><attr>.', ...
        sum(~goodSig));
    T   = T(goodSig, :);
    sig = sig(goodSig);
end
T.code    = sig;                         % full code, for data tips
T.sysChar = extractBefore(T.satellite, 2);

%% --- GPS-time epoch axis (trace files carry pure GPS time) ---
gpsEpoch = datetime(1980,1,6,0,0,0);
T.utc    = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));
epochsAll = unique(T.utc);
[~, T.epochIdx] = ismember(T.utc, epochsAll);
nEpochAll = numel(epochsAll);

%% --- Per-band color palette (by RINEX band digit) ---
% Harmonized by frequency where the constellations agree (band 5 ~1176
% MHz for GPS L5 / Galileo E5a / BeiDou B2a, etc.).  Distinct, roughly
% colorblind-aware hues; '0' is the gray fallback for anything unexpected.
bandColors = containers.Map( ...
    {'1','2','3','4','5','6','7','8','9','0'}, ...
    {[0.00 0.45 0.74], [0.85 0.33 0.10], [0.49 0.18 0.56], [0.80 0.30 0.65], ...
     [0.20 0.63 0.17], [0.64 0.08 0.18], [0.93 0.69 0.13], [0.20 0.70 0.90], ...
     [0.55 0.35 0.20], [0.50 0.50 0.50]});

%% --- Discrete channel-COUNT palette (used by channelEncoding='count') ---
% Row N (1-based) is the color for "N distinct channels used in this band
% this epoch".  Counts at or above the last row clamp to it, so a rare
% 6-channel band still renders (as the 5+ color) instead of vanishing.
% Order/colors are yours to retune; the legend is generated from this.
countColors = [
    0.95 0.85 0.15;   % 1 channel  -> yellow
    0.95 0.55 0.10;   % 2 channels -> orange
    0.15 0.45 0.85;   % 3 channels -> blue
    0.30 0.78 0.90;   % 4 channels -> cyan
    0.55 0.25 0.70];  % 5+ channels-> purple

%% --- Constellations present, in canonical render order ---
sysDefs = {'G','GPS'; 'E','Galileo'; 'R','GLONASS'; 'C','BeiDou'; ...
           'J','QZSS'; 'I','NavIC'; 'S','SBAS'};
present = false(size(sysDefs,1),1);
for i = 1:size(sysDefs,1)
    present(i) = any(T.sysChar == sysDefs{i,1});
end
sysDefs = sysDefs(present, :);
if isempty(sysDefs)
    error('plotSatelliteSignals:noSystems', 'No recognized constellations in CSV.');
end

cfg = struct('channelEncoding', lower(string(channelEncoding)), ...
             'maxLabelEpochs',  maxLabelEpochs, ...
             'showLegend',      showLegend, ...
             'countColors',     countColors);

%% --- Build figure(s) ---
if strcmpi(layout, 'single')
    buildSignalFigure(T, 'All constellations', '', epochsAll, cfg, bandColors, sysDefs);
else
    for i = 1:size(sysDefs,1)
        Tsys = T(T.sysChar == sysDefs{i,1}, :);
        if isempty(Tsys), continue; end
        buildSignalFigure(Tsys, sysDefs{i,2}, sysDefs{i,1}, epochsAll, cfg, bandColors, sysDefs);
    end
end

%% --- Console tally ---
if showChannelTally
    fprintf('\n=== Signal usage tally ===\n');
    fprintf('File   : %s\n', fname);
    fprintf('Epochs : %d   Span: %s -> %s\n', nEpochAll, ...
            string(epochsAll(1)), string(epochsAll(end)));
    [uc, ~, ix] = unique(T.code);
    cnt = accumarray(ix, 1);
    fprintf('\n%-6s %-10s %s\n', 'code', 'obs', 'band|channel');
    fprintf('%s\n', repmat('-', 1, 34));
    for k = 1:numel(uc)
        bc = extractBetween(uc(k),2,2) + "|" + extractAfter(uc(k),2);
        fprintf('%-6s %-10d %s\n', uc(k), cnt(k), bc);
    end
    fprintf('\nTip: click any cell in the figures to read the exact codes used.\n');
end

%% ================================================================
%  Local functions
%  ================================================================
function buildSignalFigure(Tsys, sysName, sysChar, epochsAll, cfg, bandColors, sysDefs)
% BUILDSIGNALFIGURE  Render one signal-usage raster.  Rows are grouped by
% satellite, with one thin sub-row per frequency band that satellite uses.
    nEpoch = numel(epochsAll);

    % Satellites, PRN-sorted within each constellation (so the 'single'
    % layout still groups G.., E.., ... blocks in canonical order).
    sats = unique(Tsys.satellite);
    sysRank = containers.Map(sysDefs(:,1), num2cell(1:size(sysDefs,1)));
    sc = extractBefore(sats, 2);
    sr = ones(numel(sats),1);
    for k = 1:numel(sats)
        if isKey(sysRank, char(sc(k))), sr(k) = sysRank(char(sc(k))); end
    end
    prn = str2double(extractAfter(sats, 1));
    prn(isnan(prn)) = 0;
    [~, ord] = sortrows([sr, prn]);
    sats = sats(ord);

    % One display row per (satellite, band-it-uses).
    rowSat = strings(0,1); rowBand = strings(0,1); rowLabel = strings(0,1);
    for k = 1:numel(sats)
        bset = sort(unique(Tsys.band(Tsys.satellite == sats(k))));
        for j = 1:numel(bset)
            rowSat(end+1,1)   = sats(k);                                   %#ok<AGROW>
            rowBand(end+1,1)  = bset(j);                                   %#ok<AGROW>
            rowLabel(end+1,1) = sats(k) + " " + ...
                                bandLabel(char(extractBefore(sats(k),2)), bset(j)); %#ok<AGROW>
        end
    end
    nRow = numel(rowSat);
    if nRow == 0, return; end

    % Per-cell aggregates: distinct-channel count, obs-type code (1 phase /
    % 2 code / 3 both), and the exact-codes / attribute strings for tips.
    countGrid = zeros(nRow, nEpoch);   % # distinct channels
    typeGrid  = zeros(nRow, nEpoch);   % 1 / 2 / 3
    tipGrid   = strings(nRow, nEpoch); % "L1C C1C  L1W"
    attrGrid  = strings(nRow, nEpoch); % "CW" (for the letters overlay)
    for r = 1:nRow
        m = Tsys.satellite == rowSat(r) & Tsys.band == rowBand(r);
        if ~any(m), continue; end
        e  = Tsys.epochIdx(m);
        ch = Tsys.chan(m);
        ot = Tsys.obstype(m);
        cd = Tsys.code(m);
        g  = findgroups(e);
        % splitapply returns column vectors; transpose to rows so the
        % grid-row assignments below (1 x k = 1 x k) line up.
        ue   = splitapply(@(x) x(1),               e,  g)';
        cCnt = splitapply(@(c) numel(unique(c)),   ch, g)';
        hasL = splitapply(@(t) any(t == "L"),      ot, g)';
        hasC = splitapply(@(t) any(t == "C"),      ot, g)';
        cdsG = splitapply(@(s) strjoin(unique(s)', '  '), cd, g)';
        atG  = splitapply(@(s) strjoin(unique(s)', ''),   ch, g)';
        countGrid(r, ue) = cCnt;
        typeGrid(r, ue)  = 1*(hasL & ~hasC) + 2*(~hasL & hasC) + 3*(hasL & hasC);
        tipGrid(r, ue)   = cdsG;
        attrGrid(r, ue)  = atG;
    end
    used = countGrid > 0;   % AlphaData mask: empties stay transparent

    % Per-band shade cap (so a band that maxes at 2 channels still spans
    % the full light->dark range instead of looking uniformly dim).
    capMap = containers.Map('KeyType','char','ValueType','double');
    for b = unique(rowBand)'
        rows = rowBand == b;
        capMap(char(b)) = max(1, max(countGrid(rows, :), [], 'all'));
    end

    nCountLvl = size(cfg.countColors, 1);   % last row = "N+" clamp level
    switch char(cfg.channelEncoding)
        case 'count',   encDesc = 'color = # channels';
        case 'letters', encDesc = 'color = band, attribute letters';
        case 'obstype', encDesc = 'color = band, shade = phase/code/both';
        otherwise,      encDesc = 'color = band, shade = # channels';
    end

    % Compose the truecolor image, blended over white (empties alpha'd out
    % via the AlphaData mask below).  'count' colors each cell by its
    % channel count from the discrete palette; the other modes keep the
    % band color and vary brightness.
    IMG = ones(nRow, nEpoch, 3);
    if strcmp(char(cfg.channelEncoding), 'count')
        for r = 1:nRow
            cnt = countGrid(r, :);
            for e = find(used(r, :))
                lvl = min(cnt(e), nCountLvl);     % clamp 5,6,... into the top level
                IMG(r, e, :) = cfg.countColors(lvl, :);
            end
        end
    else
        for r = 1:nRow
            c  = getBandColor(bandColors, rowBand(r));
            nz = used(r, :);
            a  = zeros(1, nEpoch);
            switch char(cfg.channelEncoding)
                case 'obstype'
                    v = typeGrid(r, :);
                    a(v > 0) = 0.45 + 0.55 * (v(v > 0) - 1) / 2;   % 1->.45 2->.725 3->1
                case 'letters'
                    a(nz) = 0.85;
                otherwise % 'shade'
                    cap = capMap(char(rowBand(r)));
                    cnt = countGrid(r, :);
                    a(nz) = 0.5 + 0.5 * min(cnt(nz) - 1, cap - 1) / max(cap - 1, 1);
            end
            a = max(0, min(1, a));
            for ch = 1:3
                IMG(r, :, ch) = (1 - a) * 1 + a * c(ch);
            end
        end
    end

    fig = figure('Name', sprintf('Sat Signals -- %s', sysName), 'Color', 'w', ...
                 'Position', [60 60 1400 max(280, 16*nRow + 140)]);
    ax = axes(fig); hold(ax, 'on');

    im = image(ax, 1:nEpoch, 1:nRow, IMG);
    set(im, 'AlphaData', used);
    set(ax, 'YDir', 'reverse');
    ax.YTick = 1:nRow;
    ax.YTickLabel = cellstr(rowLabel);
    ax.YLim = [0.5 nRow + 0.5];
    ax.TickLabelInterpreter = 'none';

    nt = min(nEpoch, 10);
    xt = unique(round(linspace(1, nEpoch, nt)));
    ax.XTick = xt;
    ax.XTickLabel = string(epochsAll(xt), 'HH:mm');
    ax.XLim = [0.5 nEpoch + 0.5];
    xlabel(ax, 'GPS Time');
    ylabel(ax, 'Satellite / band');
    title(ax, sprintf('%s signals used   (%s)   [%d sat, %d epochs]', ...
                      sysName, encDesc, numel(sats), nEpoch));
    grid(ax, 'on'); box(ax, 'on');
    ax.Layer = 'top';     % keep grid + separators visible over the image
    ax.FontSize = 8;

    % Faint separators between satellite groups.
    bnd = find(rowSat(1:end-1) ~= rowSat(2:end));
    for bb = bnd(:)'
        plot(ax, [0.5 nEpoch+0.5], [bb+0.5 bb+0.5], '-', ...
             'Color', [0.82 0.82 0.82], 'LineWidth', 0.5, 'HandleVisibility', 'off');
    end

    % Legend (dummy patches carry the colors).  In 'count' mode the key is
    % the channel-count palette (band lives in the row label); otherwise it
    % is the band-color key.
    if cfg.showLegend
        if strcmp(char(cfg.channelEncoding), 'count')
            hL = gobjects(nCountLvl, 1);
            nm = cell(nCountLvl, 1);
            for i = 1:nCountLvl
                hL(i) = patch(ax, 'XData', [NaN NaN NaN], 'YData', [NaN NaN NaN], ...
                              'FaceColor', cfg.countColors(i, :), 'EdgeColor', 'none');
                if i == nCountLvl
                    nm{i} = sprintf('%d+ channels', i);   % top level is a clamp
                elseif i == 1
                    nm{i} = '1 channel';
                else
                    nm{i} = sprintf('%d channels', i);
                end
            end
            legend(ax, hL, nm, 'Location', 'eastoutside', 'Interpreter', 'none');
        else
            bs = sort(unique(rowBand));
            hL = gobjects(numel(bs), 1);
            nm = cell(numel(bs), 1);
            for i = 1:numel(bs)
                hL(i) = patch(ax, 'XData', [NaN NaN NaN], 'YData', [NaN NaN NaN], ...
                              'FaceColor', getBandColor(bandColors, bs(i)), 'EdgeColor', 'none');
                if isempty(sysChar)
                    nm{i} = char("Band " + bs(i));   % mixed systems: label by digit
                else
                    nm{i} = char(bandLabel(sysChar, bs(i)));
                end
            end
            legend(ax, hL, nm, 'Location', 'eastoutside', 'Interpreter', 'none');
        end
    end

    % Attribute-letter overlay (short runs only).
    if strcmpi(char(cfg.channelEncoding), 'letters')
        if nEpoch <= cfg.maxLabelEpochs
            for r = 1:nRow
                for e = find(used(r, :))
                    text(ax, e, r, char(attrGrid(r, e)), 'FontSize', 7, ...
                         'HorizontalAlignment', 'center', 'VerticalAlignment', 'middle', ...
                         'Color', [0 0 0], 'Clipping', 'on');
                end
            end
        else
            fprintf(['[plotSatelliteSignals] %s: letters overlay skipped ' ...
                     '(%d epochs > maxLabelEpochs=%d).\n'], sysName, nEpoch, cfg.maxLabelEpochs);
        end
    end

    % Click-to-inspect data tips: exact codes for the (sat, band, epoch).
    % (Dot-assign rather than struct(): a 2-D array passed to struct()
    % would spawn a struct array, not a scalar struct holding the grid.)
    d.tip      = tipGrid;
    d.rowLabel = rowLabel;
    d.epochs   = epochsAll;
    setappdata(fig, 'sigdata', d);
    dcm = datacursormode(fig);
    set(dcm, 'UpdateFcn', @signalDatatip);

    hold(ax, 'off');
end

function txt = signalDatatip(~, evt)
% SIGNALDATATIP  Map a clicked cell back to its exact signal codes.
    fig = ancestor(evt.Target, 'figure');
    d   = getappdata(fig, 'sigdata');
    pos = evt.Position;
    e = min(max(round(pos(1)), 1), numel(d.epochs));
    r = min(max(round(pos(2)), 1), numel(d.rowLabel));
    codes = d.tip(r, e);
    if strlength(codes) == 0, codes = "(none)"; end
    txt = { char(d.rowLabel(r)), ...
            char("epoch: " + string(d.epochs(e), 'yyyy-MM-dd HH:mm:ss')), ...
            char("signals: " + codes) };
end

function c = getBandColor(map, bandDigit)
% GETBANDCOLOR  Band-digit -> RGB, gray fallback for unknown digits.
    k = char(bandDigit);
    if isKey(map, k), c = map(k); else, c = [0.5 0.5 0.5]; end
end

function lbl = bandLabel(sysChar, bandDigit)
% BANDLABEL  Constellation-aware band name for a RINEX band digit.
% Falls back to "B<digit>" for combinations not in the table.
    d = char(bandDigit);
    switch sysChar
        case 'G', m = struct('x1','L1','x2','L2','x5','L5');
        case 'R', m = struct('x1','G1','x2','G2','x3','G3','x4','G1a','x6','G2a');
        case 'E', m = struct('x1','E1','x5','E5a','x6','E6','x7','E5b','x8','E5');
        case 'C', m = struct('x1','B1','x2','B1I','x5','B2a','x6','B3','x7','B2b','x8','B2');
        case 'J', m = struct('x1','L1','x2','L2','x5','L5','x6','L6');
        case 'I', m = struct('x5','L5','x9','S');
        case 'S', m = struct('x1','L1','x5','L5');
        otherwise, m = struct();
    end
    f = ['x' d];
    if isfield(m, f), lbl = string(m.(f)); else, lbl = "B" + string(d); end
end
