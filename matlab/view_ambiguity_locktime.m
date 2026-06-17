%% ============================================================
%  Script: view_ambiguity_locktime.m
%  Purpose: Per-satellite phase-ambiguity LOCKTIME strip chart -- how
%           long (minutes) each carrier-phase ambiguity has held since
%           it was last (re)initialized.  Styled after
%           plotResidualStrip.m: one figure per constellation, one
%           imagesc tile per phase signal, (satellite x epoch) cells
%           colored by locktime.  Fresh resets glow HOT, mature locks
%           recede COOL -- so a cycle-slip / outlier path that resets
%           ambiguities too often paints the strip with hot stripes,
%           while a healthy run is long cool sweeps.
%
%  ---------------------------------------------------------------
%  WHY THIS IS RECONSTRUCTED FROM STATE, NOT FROM THE SLIP TRACES
%  ---------------------------------------------------------------
%  An ambiguity is reinitialized by TWO independent code paths:
%    (1) the cycle-slip pipeline (MwSlipDetector / TurboEdit ->
%        CycleSlipManager::resetAmbiguityBssdAware ->
%        StateManager::resetAmbiguity), and
%    (2) the Kalman filter's post-fit measurement-residual outlier
%        check (KalmanFilter::handlePhaseOutlier), which fires with no
%        slip flag at all.
%  Reconstructing resets from mwwl_trace / turbo_edit_trace alone would
%  therefore SILENTLY MISS every KF-driven reset -- precisely the failure
%  the user flagged.  The robust observable is the state itself: BOTH
%  paths do the identical covariance surgery
%        P.row(i)=0; P.col(i)=0; P(i,i)=reset_std^2
%  so a reset ALWAYS shows up in ambiguity_trace.csv as a sharp UP-STEP
%  (reinflation) in std_m.  Within an arc the ambiguity variance is
%  monotonically non-increasing (ambiguities carry ~no process noise),
%  so any meaningful std up-step is unambiguously a reinitialization,
%  regardless of which path triggered it.  That single signature
%  captures both sources uniformly.  (A covariance-preserving cycle-slip
%  REPAIR shifts value_m but NOT std_m -- correctly NOT counted as a
%  locktime reset, because the arc continuity is preserved.)
%
%  Reset taxonomy reconstructed per (satellite, signal) series:
%    arc-start  : first appearance, or reacquisition after a time gap
%                 (a rising/setting satellite -- expected, not pathological).
%    in-arc reset: a std reinflation mid-arc -- the diagnostic of interest.
%                 Optionally classified by source (see classifyResetSource):
%                   "slip" -- a slip flag exists for this sat near this epoch
%                             in mwwl_trace / turbo_edit_trace,
%                   "kf"   -- no slip flag -> KF post-fit residual reset.
%    bridge held: a soft-LLI gap hold (turbo_edit_bridge_lli) whose live
%                 state parks at the reset sigma but is then RESTORED to
%                 the pre-gap snapshot + dN*lambda by the bridge repair
%                 ("Soft-LLI bridge REPAIR" in cycle_slip_manager.log).
%                 Lock is logically continuous -- locktime keeps counting,
%                 and the event is marked with a triangle, not a reset.
%                 A hold that TIMES OUT never restores and remains a true
%                 in-arc reset.  See the resetParams hold/restore knobs.
%
%  Frozen BSSD pivots (value=0, std=0 for their whole tenure) are held
%  constant by construction; they accumulate locktime continuously and
%  only reset when they LOSE the pivot (std jumps 0 -> reset_std), which
%  the up-step rule catches for free.
%
%  Expected CSV header (TraceLogger::logAmbiguities):
%      week,tow_s,key,value_m,std_m         (key = "<sat>_<signal>")
%  ============================================================

clear all; %clc; %close all;

%% --- Content switches ---
% Which quantity to paint in the strip cells:
%   'locktime' : minutes since last (re)init  (PRIMARY -- reset diagnostic)
%   'std'      : ambiguity std (m) -- the raw reinflation signal itself, a
%                direct cross-check that the locktime resets are real
%                (every locktime drop coincides with an std spike here).
plotValue = 'locktime';

% Color saturation cap.  Locktime cells beyond capMinutes all read as the
% "fully mature / locked" cold extreme, spending the whole color range on
% the young (just-reset) region you actually want to scrutinize.  Raise it
% to compress fresh resets, lower it to make even brief locks pop cold.
capMinutes = 20;
% std-mode color cap (m): std at/above this reads as the hot "just reset /
% uncertain" extreme; 0 reads cold/converged.
stdCapM    = 0.60;

% Mark in-arc resets with a glyph at the (epoch, satellite) cell, colored
% by reconstructed source (when classifyResetSource is on): slip vs KF.
showResetMarkers = true;

% Cross-reference mwwl_trace / turbo_edit_trace to attribute each in-arc
% reset to the cycle-slip pipeline ("slip") vs the KF post-fit residual
% check ("kf").  Auto-disabled if neither slip trace is present.
classifyResetSource = true;
classifyWindowS     = 5;   % a reset within +/- this many seconds of a slip
                           % flag for the SAME satellite is "slip"-attributed
                           % (the reset can lag the flag by an epoch or two).

% Colormap for the magnitude ramp (resolved by the shared stripColormap
% library).  Perceptually-uniform maps only; fresh resets render at the
% dark/low end -- salient against the white background -- maturing to the
% bright/high end.  Options: 'viridis' (default), 'cividis' (colorblind-
% safe), 'turbo', 'parula'.  The std view uses the reversed map so high
% (just-reset / uncertain) sigma is the salient dark end.
colormapMode = 'viridis';

showSummary  = true;

%% --- Reset-detection parameters (faithful to the C++ covariance surgery) ---
% A mid-arc std UP-step is a reinitialization.  Detected when the std rises
% AND either the absolute jump exceeds absJumpM, or the ratio exceeds
% relFactor while already above the noise floor.  Defaults are calibrated
% against the observed converged std (~0.01 m) vs reset std (~0.55 m).
%
% SOFT-LLI BRIDGE HOLDS (turbo_edit_bridge_lli): a phase-gap hold PARKS the
% live ambiguity at the reset sigma while the snapshot is kept aside and
% the measurements are KF-excluded; at bridge adjudication the pre-gap
% value + dN*lambda is RESTORED with the pre-gap variance
% (SoftLliBridge::resolve, "Soft-LLI bridge REPAIR" in
% cycle_slip_manager.log).  That is logically CONTINUOUS lock -- so a
% reinflation that returns to ~pre-gap sigma in a SINGLE step within the
% hold cap is classified "held" (resetType 3), and locktime keeps
% accumulating across it.  The signature is unambiguous: a snapshot
% restore lands at <= restoreFactor x pre-gap sigma in one epoch (e.g.
% 1000 -> 0.0104 -> update -> 0.0069), whereas a genuine fresh reset
% re-converges gradually (1000 -> ~0.5 -> ...).  A hold that TIMES OUT
% ("fresh reset, hold released") never restores, so it stays a true
% in-arc reset (type 2) -- exactly the C++ semantics.
resetParams = struct( ...
    'relFactor',     1.5, ...  % std(i) / std(i-1) ratio trigger
    'absJumpM',      0.10, ... % std(i) - std(i-1) absolute trigger (m)
    'minResetStd',   0.05, ... % ignore sub-floor jitter in deep convergence
    'pivotStd',      1e-5, ... % std <= this is a frozen BSSD pivot (m)
    'gapFactor',     3.0, ...  % dt > gapFactor*median(dt) => reacquisition
    'holdCapS',      90, ...   % look-ahead for a snapshot restore (C++ cap
                          ...  % is 60 s; margin for cadence jitter)
    'restoreFactor', 2.0);     % restored sigma <= this x pre-hold sigma
                               % (in ONE step) => bridged repair, not reset

%% --- Load CSV ---
% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
fname = gnssTracePath('ambiguity_trace.csv');
if ~isfile(fname)
    error('ambiguity_trace.csv not found: %s', fname);
end
T = readtable(fname, 'TextType', 'string');

need = ["week","tow_s","key","value_m","std_m"];
miss = need(~ismember(need, string(T.Properties.VariableNames)));
if ~isempty(miss)
    error('ambiguity_trace.csv missing columns: %s', strjoin(miss, ', '));
end
T.week    = double(T.week);
T.tow_s   = double(T.tow_s);
T.value_m = double(T.value_m);
T.std_m   = double(T.std_m);

%% --- Parse key -> satellite + signal ---
% key is "<sat>_<signal>", e.g. "G03_L1C", "E04_L7Q".  Split on the FIRST
% underscore so multi-token signals (none today, but be defensive) stay
% intact.  Keys without an underscore (e.g. a baseline-DD "DD|..." key)
% fall through with an empty signal and are dropped from the phase grid.
% Vectorized split on the FIRST underscore -- a per-row loop assigning into
% table columns (T.satellite(i)=...) is O(N) per element on a table and
% hangs on the ~150k-row ambiguity trace; do it on the whole string array.
keyStr = string(T.key);
T.satellite = extractBefore(keyStr, "_");
T.signal    = extractAfter(keyStr, "_");
noUS = ismissing(T.satellite);          % keys with no "_" (e.g. baseline DD)
T.satellite(noUS) = "";
T.signal(noUS)    = "";
T = T(T.signal ~= "" & startsWith(T.signal, "L"), :);   % phase ambiguities only
if isempty(T)
    error('No phase (L*) ambiguities found in %s', fname);
end

%% --- System tag ---
satStr = string(T.satellite);
T.system = repmat("Unknown", height(T), 1);
T.system(startsWith(satStr,'G')) = "GPS";
T.system(startsWith(satStr,'E')) = "Galileo";
T.system(startsWith(satStr,'R')) = "GLONASS";
T.system(startsWith(satStr,'C')) = "BeiDou";
T.system(startsWith(satStr,'J')) = "QZSS";
T.system(startsWith(satStr,'S')) = "SBAS";

%% --- GPS time axis (trace files carry pure GPS time) ---
gpsEpoch = datetime(1980,1,6,0,0,0);
T.utc    = gpsEpoch + seconds(T.week*7*86400 + T.tow_s);

%% --- De-duplicate (key, epoch) ---
% The IEKF relinearization 2nd pass can emit a (key, epoch) twice; keep the
% LAST (post-relinearization) row so the std series is the one the epoch
% actually closed on.
T = sortrows(T, {'key','utc'});
dupMask = [false; (T.key(2:end) == T.key(1:end-1)) & (T.utc(2:end) == T.utc(1:end-1))];
% keep last of each run: a row is a duplicate-to-drop if the NEXT row is the
% same (key,utc).
dropMask = [ (T.key(1:end-1) == T.key(2:end)) & (T.utc(1:end-1) == T.utc(2:end)); false];
if any(dropMask)
    fprintf('Dropped %d duplicate (key,epoch) row(s) (kept last).\n', nnz(dropMask));
    T = T(~dropMask, :);
end

%% --- Reconstruct locktime + reset taxonomy per key ---
% resetType: 0 = continuing, 1 = arc-start (init / reacquire), 2 = in-arc reset.
[grp, ~] = findgroups(T.key);
T.lockMin   = nan(height(T),1);
T.resetType = zeros(height(T),1);
nGroups = max(grp);
for g = 1:nGroups
    ridx = find(grp == g);                 % ascending in utc (table pre-sorted)
    tsec = seconds(T.utc(ridx) - T.utc(ridx(1)));
    [lockSec, rtype] = reconstructLocktime(tsec, T.std_m(ridx), resetParams);
    T.lockMin(ridx)   = lockSec / 60;
    T.resetType(ridx) = rtype;
end

% Survived-lock duration: how long the lock held BEFORE each in-arc reset.
% lockMin is 0 AT the reset epoch by construction, so the survived time is
% the accumulated locktime one epoch earlier (same arc -- a type-2 reset is
% never the first row of a key group, so row-1 is the same key).
T.survivedMin = nan(height(T),1);
resetIdx = find(T.resetType == 2);
T.survivedMin(resetIdx) = T.lockMin(resetIdx - 1);

% Bridge-held span starts (one marker per hold-and-restore event): a type-3
% row whose predecessor is a different key or not itself type-3.
T.heldStart = T.resetType == 3 & ...
    ([true; diff(grp) ~= 0] | [true; T.resetType(1:end-1) ~= 3]);

%% --- Optional: classify each in-arc reset as slip- vs KF-driven ---
T.resetSrc = strings(height(T),1);     % "" / "slip" / "kf"
slipEvents = table('Size',[0 2], 'VariableTypes',{'string','datetime'}, ...
                   'VariableNames',{'satellite','utc'});
if classifyResetSource
    slipEvents = loadSlipEvents(gpsEpoch);
    if isempty(slipEvents)
        fprintf(['No slip traces found (mwwl_trace / turbo_edit_trace) -- ' ...
                 'reset-source classification disabled.\n']);
        classifyResetSource = false;
    end
end
if classifyResetSource
    resetRows = find(T.resetType == 2);
    win = seconds(classifyWindowS);
    for ii = resetRows'
        sat = T.satellite(ii);
        t0  = T.utc(ii);
        sm  = slipEvents.satellite == sat & abs(slipEvents.utc - t0) <= win;
        if any(sm)
            T.resetSrc(ii) = "slip";
        else
            T.resetSrc(ii) = "kf";
        end
    end
end

%% --- Resolve render quantity, caxis, colormap ---
switch lower(plotValue)
    case 'locktime'
        valueCol = 'lockMin';
        valueLbl = 'Locktime (min since last reinit)';
        caxisLim = [0 capMinutes];
        cmap     = stripColormap(colormapMode, 256);          % fresh = dark/low end
    case 'std'
        valueCol = 'std_m';
        valueLbl = 'Ambiguity \sigma (m)';
        caxisLim = [0 stdCapM];
        cmap     = stripColormap([colormapMode '_r'], 256);   % high sigma = dark end
    otherwise
        error('plotValue must be ''locktime'' or ''std''');
end

%% --- Canonical system order + shared epoch axis ---
sysOrder   = {'GPS','Galileo','GLONASS','BeiDou','QZSS','SBAS'};
sysPresent = sysOrder(cellfun(@(s) any(T.system == string(s)), sysOrder));
if isempty(sysPresent)
    error('No recognized GNSS systems in %s', fname);
end
epochs  = unique(T.utc);
nEpochs = numel(epochs);

%% ================================================================
%  One figure per constellation, one imagesc tile per phase signal.
%  ================================================================
figIdx = 0;
for s = 1:numel(sysPresent)
    sysName = sysPresent{s};
    T_sys = T(T.system == string(sysName), :);
    if isempty(T_sys), continue; end

    sigs = sort(unique(string(T_sys.signal)));
    sigs = sigs(~ismissing(sigs) & sigs ~= "");
    nSigs = numel(sigs);
    if nSigs == 0, continue; end

    figHeight = min(950, 175*nSigs + 150);
    figIdx = figIdx + 1;
    fig = figure('Name', sprintf('Ambiguity locktime -- %s', sysName), ...
                 'Color','w', ...
                 'Position', [60+(figIdx-1)*25 60+(figIdx-1)*25 1400 figHeight]);
    tl = tiledlayout(fig, nSigs, 1, 'TileSpacing','tight','Padding','compact');
    title(tl, sprintf('%s phase-ambiguity %s', sysName, valueLbl), ...
          'Interpreter','tex');
    subtitle(tl, ['fresh reset = dark (low)    mature lock = bright (high)' ...
        '    (\circ slip-driven reset,  \times non-slip reset (KF residual / hold timeout),' ...
        '  \Delta bridge hold+repair: lock continuous)'], ...
        'Interpreter','tex', 'FontSize', 8);

    columnAxes = gobjects(0);
    for r = 1:nSigs
        sig = sigs(r);
        T_sub = T_sys(string(T_sys.signal) == sig, :);
        if isempty(T_sub), continue; end

        % PRN-sorted satellite rows.
        satsHere = unique(string(T_sub.satellite));
        prn = nan(numel(satsHere),1);
        for k = 1:numel(satsHere)
            ch = char(satsHere(k));
            if numel(ch) >= 3, prn(k) = str2double(ch(2:3)); end
        end
        [~, ord] = sortrows([prn (1:numel(satsHere))'], [1 2]);
        satsHere = satsHere(ord);
        nSats = numel(satsHere);

        % (sat x epoch) value grid; NaN -> transparent gap.
        G = nan(nSats, nEpochs);
        [~, satIdx]   = ismember(string(T_sub.satellite), satsHere);
        [~, epochIdx] = ismember(T_sub.utc, epochs);
        keep = satIdx > 0 & epochIdx > 0;
        linIdx = sub2ind([nSats nEpochs], satIdx(keep), epochIdx(keep));
        G(linIdx) = double(T_sub.(valueCol)(keep));

        ax = nexttile(tl); hold(ax,'on');
        columnAxes(end+1) = ax; %#ok<AGROW>

        imH = imagesc(ax, [epochs(1) epochs(end)], [1 nSats], G);
        set(imH, 'AlphaData', ~isnan(G));
        colormap(ax, cmap);
        caxis(ax, caxisLim); %#ok<CAXIS>
        ax.YDir = 'reverse';
        ax.YTick = 1:nSats;
        ax.YTickLabel = cellstr(satsHere);
        ax.YLim = [0.5 nSats+0.5];
        ax.XLim = [epochs(1) epochs(end)];
        ax.TickLabelInterpreter = 'none';
        grid(ax,'on'); box(ax,'on');

        % --- Reset markers (in-arc resets only -- the diagnostic) ---
        nSlip = 0; nKf = 0;
        if showResetMarkers
            rs = T_sub(T_sub.resetType == 2, :);
            for e = 1:height(rs)
                row = find(satsHere == string(rs.satellite(e)), 1);
                if isempty(row), continue; end
                src = rs.resetSrc(e);
                if src == "slip"
                    plot(ax, rs.utc(e), row, 'o', 'MarkerSize', 6, ...
                        'MarkerEdgeColor','k', 'LineWidth', 1.0);
                    nSlip = nSlip + 1;
                elseif src == "kf"
                    plot(ax, rs.utc(e), row, 'x', 'MarkerSize', 8, ...
                        'MarkerEdgeColor',[0.85 0 0.85], 'LineWidth', 1.4);
                    nKf = nKf + 1;
                else
                    plot(ax, rs.utc(e), row, '+', 'MarkerSize', 7, ...
                        'MarkerEdgeColor',[0.1 0.1 0.1], 'LineWidth', 1.0);
                end
            end
            % Bridge holds restored by a snapshot+dN repair: one triangle
            % per hold-and-restore event (lock continuous -- not a reset).
            hs = T_sub(T_sub.heldStart, :);
            for e = 1:height(hs)
                row = find(satsHere == string(hs.satellite(e)), 1);
                if isempty(row), continue; end
                plot(ax, hs.utc(e), row, '^', 'MarkerSize', 6, ...
                    'MarkerEdgeColor', [0 0.55 0.25], 'LineWidth', 1.2);
            end
        end

        if r == nSigs, xlabel(ax,'GPS Time'); else, ax.XTickLabel = []; end
        ylabel(ax, sig);

        % Per-tile reset tally, top-right.
        nReset = nnz(T_sub.resetType == 2);
        nArc   = nnz(T_sub.resetType == 1);
        nHeld  = nnz(T_sub.heldStart);
        if classifyResetSource
            annot = sprintf('in-arc resets: %d (slip %d, kf %d)   held+repaired: %d   arcs: %d', ...
                            nReset, nSlip, nKf, nHeld, nArc);
        else
            annot = sprintf('in-arc resets: %d   held+repaired: %d   arcs: %d', ...
                            nReset, nHeld, nArc);
        end
        text(ax, 0.995, 0.06, annot, 'Units','normalized', ...
            'HorizontalAlignment','right', 'VerticalAlignment','bottom', ...
            'FontSize',8, 'BackgroundColor','w', 'EdgeColor',[0.7 0.7 0.7], ...
            'Margin',1);

        hold(ax,'off');
    end

    if ~isempty(columnAxes)
        cb = colorbar(columnAxes(end), 'Location','eastoutside');
        cb.Label.String = valueLbl;
        cb.Label.Interpreter = 'tex';
    end
    if numel(columnAxes) > 1, linkaxes(columnAxes, 'x'); end
end

%% --- Summary statistics ---
if showSummary
    fprintf('\n=== Ambiguity Locktime Summary ===\n');
    fprintf('File        : %s\n', fname);
    fprintf('Epochs      : %d   Span (GPS): %s -> %s\n', ...
            nEpochs, string(epochs(1)), string(epochs(end)));
    spanHr = hours(epochs(end) - epochs(1));
    if spanHr <= 0, spanHr = NaN; end

    totReset = nnz(T.resetType == 2);
    totSlip  = nnz(T.resetType == 2 & T.resetSrc == "slip");
    totKf    = nnz(T.resetType == 2 & T.resetSrc == "kf");
    totHeld  = nnz(T.heldStart);
    if classifyResetSource
        fprintf(['In-arc resets: %d total  (slip-driven %d, ' ...
                 'non-slip %d -- KF residual or soft-LLI hold timeout)\n'], ...
                totReset, totSlip, totKf);
    else
        fprintf('In-arc resets: %d total\n', totReset);
    end
    fprintf(['Bridge hold+repair events: %d (soft-LLI snapshot restored -- ' ...
             'lock counted CONTINUOUS)\n'], totHeld);

    % "surv" columns report how long locks SURVIVED before being reset
    % (minutes); a small mean/min surv next to a high reset count is the
    % signature of a path thrashing an ambiguity.
    fprintf('\n%-9s %-6s %6s %8s %8s %10s %9s\n', ...
        'System','Signal','N','resets','rst/hr','mean surv','min surv');
    fprintf('%s\n', repmat('-',1,64));
    for s = 1:numel(sysPresent)
        sysName = sysPresent{s};
        T_sys = T(T.system == string(sysName), :);
        if isempty(T_sys), continue; end
        sigs = sort(unique(string(T_sys.signal)));
        sigs = sigs(~ismissing(sigs) & sigs ~= "");
        for k = 1:numel(sigs)
            sig = sigs(k);
            sub = T_sys(string(T_sys.signal) == sig, :);
            if isempty(sub), continue; end
            nR = nnz(sub.resetType == 2);
            surv = sub.survivedMin(sub.resetType == 2);
            surv = surv(isfinite(surv));
            if isempty(surv)
                meanSurv = NaN; minSurv = NaN;
            else
                meanSurv = mean(surv); minSurv = min(surv);
            end
            rstHr = nR / spanHr;
            fprintf('%-9s %-6s %6d %8d %8.2f %9.1fm %8.2fm\n', ...
                sysName, sig, height(sub), nR, rstHr, meanSurv, minSurv);
        end
    end

    % Loudest offenders: satellites with the most in-arc resets.
    rs = T(T.resetType == 2, :);
    if ~isempty(rs)
        [gk, gsat, gsig] = findgroups(rs.satellite, rs.signal);
        cnt = splitapply(@numel, rs.resetType, gk);
        [cnt, o] = sort(cnt, 'descend');
        gsat = gsat(o); gsig = gsig(o);
        fprintf('\nTop reset offenders (satellite/signal):\n');
        for i = 1:min(10, numel(cnt))
            fprintf('  %s %-4s : %d in-arc reset(s)\n', gsat(i), gsig(i), cnt(i));
        end
    end
end

%% ================================================================
%  Local functions
%  ================================================================
function [lockSec, resetType] = reconstructLocktime(tsec, stdv, p)
% RECONSTRUCTLOCKTIME  Per (satellite, signal) arc, derive seconds-since-
% reinit and a reset taxonomy from the ambiguity std series.
%   resetType: 0 continuing, 1 arc-start (init/reacquire), 2 in-arc reset,
%              3 bridge-held span (soft-LLI hold restored by a snapshot +
%                dN*lambda repair -- locktime CONTINUES across it).
% tsec must be ascending (seconds, arc-relative); stdv the matching std (m).
    n = numel(tsec);
    lockSec   = zeros(n,1);
    resetType = zeros(n,1);
    if n == 0, return; end
    resetType(1) = 1;                       % first appearance = arc start

    dt = diff(tsec);
    medDt = median(dt(dt > 0));
    if ~isfinite(medDt) || medDt <= 0, medDt = 1; end

    i = 2;
    while i <= n
        gap = tsec(i) - tsec(i-1);
        s0  = stdv(i-1); s1 = stdv(i);

        isGap = gap > p.gapFactor * medDt;

        % Mid-arc covariance reinflation (== a C++ resetAmbiguity /
        % handlePhaseOutlier did P(i,i)=reset_std^2 this epoch).
        dStd  = s1 - s0;
        ratio = (s0 > 0) * (s1 / max(s0, eps));
        reinfl = (s1 > p.minResetStd) & ...
                 ( dStd > p.absJumpM | (s0 > p.minResetStd & ratio > p.relFactor) );

        % Former BSSD pivot losing the pivot: frozen (std~0) -> re-estimated.
        formerPivot = (s0 <= p.pivotStd) & (s1 > p.minResetStd);

        if isGap
            resetType(i) = 1; lockSec(i) = 0;
            i = i + 1;
        elseif reinfl || formerPivot
            % Reinflation: bridged hold-and-restore, or a true reset?
            % A soft-LLI bridge repair RESTORES the pre-hold sigma in one
            % step within the hold cap; scan ahead for that signature.
            % (formerPivot spans are excluded -- a pivot's pre-event sigma
            % is 0, so "restore to pre-hold sigma" is undefined.)
            j = 0;
            if reinfl && s0 > p.pivotStd
                k = i + 1;
                while k <= n && (tsec(k) - tsec(i-1)) <= p.holdCapS
                    if stdv(k) <= p.restoreFactor * s0 && ...
                       stdv(k-1) > p.restoreFactor * s0
                        j = k;             % single-step restore epoch
                        break;
                    end
                    k = k + 1;
                end
            end
            if j > 0
                % Held span i..j-1: lock logically continuous.  Locktime
                % keeps accumulating through the hold and the restore.
                for m = i:j
                    lockSec(m) = lockSec(m-1) + (tsec(m) - tsec(m-1));
                end
                resetType(i:j-1) = 3;       % parked/held samples
                i = j + 1;
            else
                resetType(i) = 2; lockSec(i) = 0;
                i = i + 1;
            end
        else
            lockSec(i) = lockSec(i-1) + gap;
            i = i + 1;
        end
    end
end

function ev = loadSlipEvents(gpsEpoch)
% LOADSLIPEVENTS  Gather (satellite, utc) slip flags from the MW and
% TurboEdit traces for reset-source attribution.  Returns an empty table
% if neither trace is present.
    ev = table('Size',[0 2], 'VariableTypes',{'string','datetime'}, ...
               'VariableNames',{'satellite','utc'});
    logDir = gnssVizLogDir();

    mwFile = fullfile(logDir, 'mwwl_trace.csv');
    if isfile(mwFile)
        M = readtable(mwFile, 'TextType','string');
        if all(ismember(["satellite","week","tow_s","is_slip"], ...
                        string(M.Properties.VariableNames)))
            isSlip = double(M.is_slip); isSlip(isnan(isSlip)) = 0;
            m = isSlip == 1;
            if any(m)
                utc = gpsEpoch + seconds(double(M.week(m))*7*86400 + double(M.tow_s(m)));
                ev = [ev; table(string(M.satellite(m)), utc, ...
                                'VariableNames',{'satellite','utc'})];
            end
        end
    end

    teFile = fullfile(logDir, 'turbo_edit_trace.csv');
    if isfile(teFile)
        E = readtable(teFile, 'TextType','string');
        vn = string(E.Properties.VariableNames);
        if all(ismember(["satellite","week","tow_s","mw_flagged","gf_flagged"], vn))
            mwF = double(E.mw_flagged); mwF(isnan(mwF)) = 0;
            gfF = double(E.gf_flagged); gfF(isnan(gfF)) = 0;
            flagged = (mwF == 1) | (gfF == 1);
            % Drop "none" resolutions (a GF false alarm that resolved to no
            % slip never touched the state).
            if ismember("action", vn)
                flagged = flagged & E.action ~= "none";
            end
            if any(flagged)
                utc = gpsEpoch + seconds(double(E.week(flagged))*7*86400 + ...
                                         double(E.tow_s(flagged)));
                ev = [ev; table(string(E.satellite(flagged)), utc, ...
                                'VariableNames',{'satellite','utc'})];
            end
        end
    end
    ev = unique(ev, 'rows');
end

