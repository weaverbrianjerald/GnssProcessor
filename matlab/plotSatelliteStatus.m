%% ============================================================
%  Script: plotSatelliteStatus.m
%  Purpose: NRCAN-style per-satellite epoch-status strip chart.
%
%  One figure per constellation.  Each figure renders one row per
%  satellite in that constellation, columns = epochs, cells colored by
%  status:
%
%      0  Clean        green        used in update, no flags
%      1  Pivot        blue         BSSD pivot (frozen at value/var = 0)
%      2  Slip         red          cycle slip detected this epoch
%      3  CodeOutlier  orange       at least one code signal excluded
%      4  BelowMask    dark gray    elevation below the configured mask
%      6  NoProduct    light blue   missing orbit / clock / ephemeris
%      7  NoModel      magenta      signal selection / correction failed
%
%  Code 5 is reserved (formerly "NotUsed" catch-all, retired when the
%  three-way drop_reasons split made it obsolete).  Sats not in the
%  CSV at all (never tracked) produce no row.
%
%  Expected CSV header:
%      week,tow_s,satellite,system,status,status_code,
%      elevation_deg,azimuth_deg,n_signals_used,n_signals_total
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
showElevationOverlay = true;   % thin gray line overlay of elevation
                                % per sat row (0..90 deg -> +/-0.45 lane)
showSummary          = true;   % per-sat fraction-of-status table
showLegend           = true;   % discrete colorbar on each figure

%% --- Load CSV ---
fname = 'C:\GnssProcessor\build\x64-Release\logs\satellite_status_trace.csv';
%fname = 'C:\GnssProcessor\build\x64-Debug\logs\satellite_status_trace.csv';

if ~isfile(fname)
    error('satellite_status_trace.csv not found: %s', fname);
end

T = readtable(fname, 'TextType', 'string');

needed = {'week','tow_s','satellite','status_code'};
for i = 1:numel(needed)
    if ~ismember(needed{i}, T.Properties.VariableNames)
        error('satellite_status_trace.csv missing required column: %s', needed{i});
    end
end

%% --- Convert GPS week + TOW to a datetime on the GPS time scale ---
% Trace files carry pure GPS time so timestamps match RINEX epochs.
gpsEpoch = datetime(1980,1,6,0,0,0);
T.utc = gpsEpoch + seconds(double(T.week)*7*86400 + double(T.tow_s));

%% --- Tag each row with a system (derive if missing) ---
hasSystem = ismember('system', T.Properties.VariableNames);
satStr = string(T.satellite);
if ~hasSystem
    T.system = repmat("Unknown", height(T), 1);
    T.system(startsWith(satStr,'G')) = "GPS";
    T.system(startsWith(satStr,'E')) = "Galileo";
    T.system(startsWith(satStr,'R')) = "GLONASS";
    T.system(startsWith(satStr,'C')) = "BeiDou";
    T.system(startsWith(satStr,'J')) = "QZSS";
    T.system(startsWith(satStr,'S')) = "SBAS";
end
T.system = string(T.system);

%% --- Colormap (discrete, status_code -> RGB) ---
% Indices 0..7 (code 5 is reserved / unused; we render it as the same
% near-white "shouldn't appear" sentinel so any leakage from an older
% trace shows up obviously rather than masquerading as a real category).
statusColors = [
    0.20 0.65 0.30;   % 0 Clean        green
    0.15 0.45 0.85;   % 1 Pivot        blue
    0.85 0.20 0.20;   % 2 Slip         red
    0.95 0.55 0.10;   % 3 CodeOutlier  orange
    0.40 0.40 0.40;   % 4 BelowMask    dark gray
    0.97 0.97 0.97;   % 5 (reserved)   near-white sentinel
    0.55 0.80 0.95;   % 6 NoProduct    light blue
    0.80 0.30 0.70;   % 7 NoModel      magenta
];
statusLabels = {'Clean','Pivot','Slip','CodeOutlier','BelowMask','(reserved)','NoProduct','NoModel'};

%% --- Canonical system render order ---
sysOrder    = {'GPS','Galileo','GLONASS','BeiDou','QZSS','SBAS'};
sysPresent  = {};
for s = 1:numel(sysOrder)
    if any(T.system == string(sysOrder{s}))
        sysPresent{end+1} = sysOrder{s}; %#ok<AGROW>
    end
end
if isempty(sysPresent)
    error('No recognized GNSS systems found in satellite_status_trace.csv');
end

% Shared epoch axis across all figures (so all systems line up in time).
epochs   = unique(T.utc);
nEpochs  = numel(epochs);

%% ================================================================
%  One figure per constellation
%  ================================================================
for s = 1:numel(sysPresent)
    sysName = sysPresent{s};
    rowsSys = T.system == string(sysName);
    T_sys = T(rowsSys, :);
    if isempty(T_sys), continue; end

    % Satellites in this constellation, PRN-sorted (digits 2-3 of the ID;
    % fall back to lex sort if the parse fails so unusual IDs still group).
    satsHere = unique(string(T_sys.satellite));
    prn = nan(numel(satsHere), 1);
    for k = 1:numel(satsHere)
        ch = char(satsHere(k));
        if numel(ch) >= 3, prn(k) = str2double(ch(2:3)); end
    end
    [~, ord] = sortrows([prn (1:numel(satsHere))'], [1 2]);
    satsHere = satsHere(ord);
    nSats = numel(satsHere);

    % Build the (sat x epoch) status grid for this constellation.  NaN
    % means "no row at that epoch" -> rendered transparent below.
    G = nan(nSats, nEpochs);
    [~, satIdxInSys] = ismember(string(T_sys.satellite), satsHere);
    [~, epochIdxInAll] = ismember(T_sys.utc, epochs);
    linIdx = sub2ind([nSats, nEpochs], satIdxInSys, epochIdxInAll);
    G(linIdx) = double(T_sys.status_code);

    fig = figure('Name', sprintf('Sat Status -- %s', sysName), 'Color','w', ...
                 'Position', [60+(s-1)*25 60+(s-1)*25 1400 max(280, 24*nSats + 120)]);
    ax = axes(fig);
    hold(ax,'on');

    imH = imagesc(ax, [epochs(1) epochs(end)], [1 nSats], G);
    set(imH, 'AlphaData', ~isnan(G));   % NaN -> transparent

    colormap(ax, statusColors);
    caxis(ax, [-0.5, 7.5]);  %#ok<CAXIS>
    ax.YDir = 'reverse';
    ax.YTick = 1:nSats;
    ax.YTickLabel = cellstr(satsHere);
    ax.YLim = [0.5 nSats+0.5];
    ax.TickLabelInterpreter = 'none';
    ax.XLim = [epochs(1) epochs(end)];
    xlabel(ax,'GPS Time');
    ylabel(ax,'Satellite');
    title(ax, sprintf('%s satellite status  (epochs=%d, sats=%d)', ...
                      sysName, nEpochs, nSats));
    grid(ax,'on');
    box(ax,'on');

    if showLegend
        % Skip code 5 in the colorbar tick labels -- it's the reserved
        % "(reserved)" sentinel that should never appear in practice.
        % Showing it as a labeled tick would confuse readers; leaving
        % it as a hidden gap between BelowMask and NoProduct subtly
        % communicates "this slot is unused".
        cb = colorbar(ax, 'Ticks', [0 1 2 3 4 6 7], ...
                          'TickLabels', statusLabels([1 2 3 4 5 7 8]));
        cb.Label.String = 'Status';
    end

    % Optional elevation overlay: thin gray line per sat row, mapping
    % 0..90 deg to the +/-0.45 of that row's lane (so the line stays
    % inside the row and never crosses into the next sat).  BelowMask
    % rows carry NaN elevation -> no line plotted for those samples.
    if showElevationOverlay && ismember('elevation_deg', T_sys.Properties.VariableNames)
        for k = 1:nSats
            sat = satsHere(k);
            T_satObs = sortrows(T_sys(T_sys.satellite == sat, :), 'utc');
            if isempty(T_satObs), continue; end
            elev = double(T_satObs.elevation_deg);
            elev(~isfinite(elev)) = NaN;
            yOff = -((elev / 90) - 0.5) * 0.9;
            plot(ax, T_satObs.utc, k + yOff, '-', ...
                'Color', [0.15 0.15 0.15], 'LineWidth', 0.4, ...
                'HandleVisibility','off');
        end
    end

    hold(ax,'off');
end

%% --- Summary statistics ---
if showSummary
    fprintf('\n=== Satellite Status Summary ===\n');
    fprintf('File           : %s\n', fname);
    fprintf('Epochs         : %d\n', nEpochs);
    fprintf('Span (UTC)     : %s -> %s\n', string(epochs(1)), string(epochs(end)));

    fprintf('\n%-9s %-9s %8s %6s %6s %6s %6s %6s %6s %6s\n', ...
        'Satellite','System','Tracked', ...
        'Clean','Pivot','Slip','COutl','BMask','NoProd','NoMod');
    fprintf('%s\n', repmat('-', 1, 86));
    for s = 1:numel(sysPresent)
        sysName = sysPresent{s};
        T_sys = T(T.system == string(sysName), :);
        satsHere = unique(string(T_sys.satellite));
        prn = nan(numel(satsHere), 1);
        for k = 1:numel(satsHere)
            ch = char(satsHere(k));
            if numel(ch) >= 3, prn(k) = str2double(ch(2:3)); end
        end
        [~, ord] = sortrows([prn (1:numel(satsHere))'], [1 2]);
        satsHere = satsHere(ord);
        for k = 1:numel(satsHere)
            sat = satsHere(k);
            codes = double(T_sys.status_code(T_sys.satellite == sat));
            n = numel(codes);
            if n == 0, continue; end
            % Bin into 0..7; code 5 is reserved and should be zero in
            % a well-formed trace.
            cnts = histcounts(codes, -0.5:1:7.5);
            pct = 100 * cnts / n;
            % Print Clean / Pivot / Slip / CodeOutlier / BelowMask /
            % NoProduct / NoModel (skip reserved code 5).
            fprintf('%-9s %-9s %8d %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%%\n', ...
                sat, sysName, n, ...
                pct(1), pct(2), pct(3), pct(4), pct(5), pct(7), pct(8));
        end
    end

    % Epoch-level health, per-constellation: how many sats in each
    % "actionable" state per epoch on average.  NoProduct and NoModel
    % are surfaced explicitly so the user can see at-a-glance whether
    % drop-outs are a product issue or a modelling issue.
    fprintf('\n=== Per-Constellation Epoch Health (averages) ===\n');
    fprintf('%-9s %10s %10s %10s %10s %10s %10s %10s\n', ...
        'System','Clean','Pivot','Slip','COutl','BMask','NoProd','NoMod');
    fprintf('%s\n', repmat('-', 1, 90));
    for s = 1:numel(sysPresent)
        sysName = sysPresent{s};
        rowsSys = T.system == string(sysName);
        if ~any(rowsSys), continue; end
        T_sys = T(rowsSys, :);
        [uEpochs, ~, ix] = unique(T_sys.utc);
        nE = numel(uEpochs);
        nClean=0; nPivot=0; nSlip=0; nOut=0; nBMask=0; nNoProd=0; nNoMod=0;
        for e = 1:nE
            codes = double(T_sys.status_code(ix == e));
            nClean  = nClean  + sum(codes == 0);
            nPivot  = nPivot  + sum(codes == 1);
            nSlip   = nSlip   + sum(codes == 2);
            nOut    = nOut    + sum(codes == 3);
            nBMask  = nBMask  + sum(codes == 4);
            nNoProd = nNoProd + sum(codes == 6);
            nNoMod  = nNoMod  + sum(codes == 7);
        end
        fprintf('%-9s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n', ...
            sysName, nClean/nE, nPivot/nE, nSlip/nE, nOut/nE, ...
            nBMask/nE, nNoProd/nE, nNoMod/nE);
    end
end
