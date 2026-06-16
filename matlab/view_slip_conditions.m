%% ============================================================
%  Script: view_slip_conditions.m
%  Purpose: Demonstrate the four MW x GF cycle-slip detection conditions
%           on real data, as joint four-panel exhibits.  Merges the
%           example-figure machinery of view_mwwl_trace.m (Melbourne-
%           Wubbena wide-lane) and view_turbo_edit_trace.m (TurboEdit
%           geometry-free TD2) into one script keyed on the SAME
%           (satellite, signal-pair, epoch) so the two detectors can be
%           read side by side.
%
%  The 2x2 conditions (each detector's OWN gate decides "detected"):
%     1) MW detected, GF stable   -- WL/code step the phase-GF doesn't see
%     2) MW stable,  GF detected   -- e.g. dN1=dN2 (WL unchanged, GF jumps)
%     3) MW stable,  GF stable     -- clean control window
%     4) MW detected, GF detected  -- unambiguous dual-detector slip
%
%  "MW detected"  == mwwl_trace.is_slip == 1   (|dev_active|/threshold > 1)
%  "GF detected"  == turbo_edit_trace.gf_flagged == 1   (|TD2|/thresh > 1,
%                    past the anti-echo cooldown)
%  These are the two DETECTORS' decisions.  Note turbo_edit's internal
%  mw_flagged is NOT used for classification: it fires on sub-threshold MW
%  wiggle (is_slip=0) and would mislabel a real MW detection as "stable".
%
%  Events are formed by clustering MW and GF flags on the same
%  (sat, pair) that fall within clusterWinS of each other -- the two
%  detectors typically fire an epoch or two apart on the same physical
%  slip, and must be counted as ONE event to land in condition 4.
%
%  Each exhibit is a 2x2 figure:
%     top-left  MW raw wide-lane + reference levels
%     bot-left  MW active deviation vs +/- threshold
%     top-right GF raw geometry-free phase (+repaired overlay)
%     bot-right GF TD2 vs +/- threshold
%  with the slip epoch starred and the two gates annotated.
%  ============================================================

clear all; %clc; %close all;

%% --- Config ---
nPerCondition  = 2;       % up to this many exhibits per condition
nRepairExamples = 1;      % up to this many action="repair" exhibits
windowSec      = 60;      % +/- seconds of arc shown around each event
clusterWinS    = 5;       % merge MW/GF flags within this gap into one event
gapFactor      = 3;       % NaN-break a line when dt > gapFactor*median(dt)
preferInArc    = true;    % prefer arc_count>1 (non-init) MW detections

% Folder resolved via gnssVizLogDir() — set once per session or via view_all.m.
mwFile = gnssTracePath('mwwl_trace.csv');
teFile = gnssTracePath('turbo_edit_trace.csv');

gpsEpoch = datetime(1980,1,6,0,0,0);

%% --- Load MW (Melbourne-Wubbena) trace ---
if ~isfile(mwFile), error('MW trace not found: %s', mwFile); end
Tmw = readtable(mwFile, 'TextType','string');
needMw = ["week","tow_s","satellite","system","sig1","sig2", ...
          "mw_cycles","dev_from_mean_cycles","threshold_cycles"];
miss = needMw(~ismember(needMw, string(Tmw.Properties.VariableNames)));
if ~isempty(miss), error('mwwl_trace.csv missing: %s', strjoin(miss,', ')); end

Tmw.satellite = string(Tmw.satellite);
Tmw.system    = string(Tmw.system);
Tmw.pair      = Tmw.sig1 + "/" + Tmw.sig2;
Tmw.utc       = gpsEpoch + seconds(double(Tmw.week)*7*86400 + double(Tmw.tow_s));
for c = ["mw_cycles","mw_mean_cycles","local_ref_cycles","dev_from_mean_cycles", ...
         "threshold_cycles","active_dev_cycles","is_slip","arc_count"]
    if ismember(c, string(Tmw.Properties.VariableNames)), Tmw.(c) = double(Tmw.(c)); end
end
if ~ismember("is_slip", string(Tmw.Properties.VariableNames))
    error('mwwl_trace.csv has no is_slip column -- cannot classify MW detections.');
end
Tmw.is_slip(isnan(Tmw.is_slip)) = 0;
if ~ismember("arc_count", string(Tmw.Properties.VariableNames))
    Tmw.arc_count = 2*ones(height(Tmw),1);   % unknown -> treat as in-arc
end
% Active deviation the MW detector gated.  Our schema writes it directly;
% fall back to dev_from_mean on the NaN (init/LLI) rows.  (The full
% local-median backstop reconstruction lives in view_mwwl_trace.m; this
% trace carries active_dev_cycles, so the direct path suffices here.)
if ismember("active_dev_cycles", string(Tmw.Properties.VariableNames))
    da = Tmw.active_dev_cycles;
    da(~isfinite(da)) = Tmw.dev_from_mean_cycles(~isfinite(da));
else
    da = Tmw.dev_from_mean_cycles;
end
Tmw.dev_active_cycles = da;

%% --- Load GF (TurboEdit) trace ---
if ~isfile(teFile), error('TurboEdit trace not found: %s', teFile); end
Tte = readtable(teFile, 'TextType','string');
needTe = ["week","tow_s","satellite","system","sig1","sig2", ...
          "gf_m","td2_gf_m","thresh_m","gf_flagged","action"];
miss = needTe(~ismember(needTe, string(Tte.Properties.VariableNames)));
if ~isempty(miss), error('turbo_edit_trace.csv missing: %s', strjoin(miss,', ')); end

Tte.satellite = string(Tte.satellite);
Tte.system    = string(Tte.system);
Tte.pair      = Tte.sig1 + "/" + Tte.sig2;
Tte.utc       = gpsEpoch + seconds(double(Tte.week)*7*86400 + double(Tte.tow_s));
for c = ["gf_m","td2_gf_m","thresh_m","gf_flagged","mw_flagged", ...
         "dN1","dN2","frac","lambda1_m","lambda2_m"]
    if ismember(c, string(Tte.Properties.VariableNames)), Tte.(c) = double(Tte.(c)); end
end
Tte.gf_flagged(isnan(Tte.gf_flagged)) = 0;
Tte.thresh_m(Tte.thresh_m < 0) = NaN;          % -1 sentinel = immature MAD
if ~ismember("action", string(Tte.Properties.VariableNames)), Tte.action = strings(height(Tte),1); end
Tte.action = string(Tte.action);

%% --- Per-detector firing event rows ---
mwSlip = Tmw(Tmw.is_slip==1, :);
gfFlag = Tte(Tte.gf_flagged==1, :);

mwEv = table(mwSlip.satellite, mwSlip.pair, mwSlip.system, mwSlip.utc, ...
             abs(mwSlip.dev_active_cycles)./mwSlip.threshold_cycles, ...
             mwSlip.arc_count, repmat("mw",height(mwSlip),1), ...
             'VariableNames', {'sat','pair','system','utc','ratio','arc','src'});
gfEv = table(gfFlag.satellite, gfFlag.pair, gfFlag.system, gfFlag.utc, ...
             abs(gfFlag.td2_gf_m)./gfFlag.thresh_m, ...
             nan(height(gfFlag),1), repmat("gf",height(gfFlag),1), ...
             'VariableNames', {'sat','pair','system','utc','ratio','arc','src'});
E = [mwEv; gfEv];
E.ratio(~isfinite(E.ratio)) = 0;

%% --- Cluster MW/GF flags on the same (sat,pair) into single events ---
C = clusterEvents(E, clusterWinS);

%% --- Classify clusters into conditions 1/2/4 ---
condNum = zeros(numel(C),1);
for i = 1:numel(C)
    if      C(i).hasMW &&  C(i).hasGF, condNum(i) = 4;
    elseif  C(i).hasMW && ~C(i).hasGF, condNum(i) = 1;
    elseif ~C(i).hasMW &&  C(i).hasGF, condNum(i) = 2;
    end
end

condLabels = strings(4,1);
condLabels(1) = "1: MW detected, GF stable";
condLabels(2) = "2: MW stable, GF detected";
condLabels(4) = "4: MW detected, GF detected";

%% --- Select up to nPerCondition per condition (prefer in-arc, strong) ---
selected = struct('sat',{},'pair',{},'system',{},'t0',{},'condNum',{}, ...
                  'condLabel',{},'rank',{});
fprintf('\n=== Slip-condition inventory ===\n');
for cn = [1 2 4]
    idx = find(condNum == cn);
    if isempty(idx)
        fprintf('Condition %s : 0 found\n', condLabels(cn));
        continue;
    end
    cand = C(idx);
    inArc = arrayfun(@(x) x.inArc, cand);
    rank  = arrayfun(@(x) x.rank,  cand);
    % Sort: in-arc first (if preferred), then by exceedance strength.
    % Force column vectors so the sort key is N-by-2, not a 1-by-2N row.
    if preferInArc
        [~, ord] = sortrows([-double(inArc(:)), -rank(:)]);
    else
        [~, ord] = sort(-rank(:));
    end
    cand = cand(ord);
    pick = pickDistinctSat(cand, nPerCondition);
    fprintf('Condition %s : %d found, showing %d\n', condLabels(cn), numel(cand), numel(pick));
    for p = 1:numel(pick)
        e = pick(p);
        selected(end+1) = struct('sat',e.sat,'pair',e.pair,'system',e.system, ...
            't0',e.t0,'condNum',cn,'condLabel',condLabels(cn),'rank',e.rank); %#ok<SAGROW>
    end
end

%% --- Condition 3: sample clean windows (both detectors quiet) ---
clean = sampleCleanWindows(Tmw, Tte, mwSlip, gfFlag, windowSec, nPerCondition, ...
                           string({selected.sat}));
fprintf('Condition 3: MW stable, GF stable : %d clean window(s) sampled\n', numel(clean));
for k = 1:numel(clean)
    selected(end+1) = struct('sat',clean(k).sat,'pair',clean(k).pair, ...
        'system',clean(k).system,'t0',clean(k).t0,'condNum',3, ...
        'condLabel',"3: MW stable, GF stable",'rank',0); %#ok<SAGROW>
end

%% --- Repair examples (action="repair") ---
% A repair is a TurboEdit OUTCOME, not a detector flag, and it lands a few
% epochs AFTER detection (the WL integer / 2x2 solve has to settle).  Pull
% it on its own axis so a repair is always exhibited, wherever it occurs.
repMask = Tte.action == "repair";
if any(repMask)
    rr = Tte(repMask, :);
    repEv = table(rr.satellite, rr.pair, rr.system, rr.utc, ...
                  abs(rr.td2_gf_m)./rr.thresh_m, nan(height(rr),1), ...
                  repmat("gf",height(rr),1), ...
                  'VariableNames', {'sat','pair','system','utc','ratio','arc','src'});
    repEv.ratio(~isfinite(repEv.ratio)) = 0;
    Crep = clusterEvents(repEv, clusterWinS);
    [~, ord] = sort(-arrayfun(@(x) x.rank, Crep));
    Crep = Crep(ord);
    nShow = min(nRepairExamples, numel(Crep));
    fprintf('Repair (TE action="repair") : %d found, showing %d\n', numel(Crep), nShow);
    for p = 1:nShow
        e = Crep(p);
        selected(end+1) = struct('sat',e.sat,'pair',e.pair,'system',e.system, ...
            't0',e.t0,'condNum',5,'condLabel',"5: Repair (TE action)",'rank',e.rank); %#ok<SAGROW>
    end
else
    fprintf('Repair (TE action="repair") : 0 found\n');
end

%% --- Render exhibits ---
if isempty(selected)
    fprintf('\nNo events to display.\n');
else
    [~, ord] = sort([selected.condNum]);
    selected = selected(ord);
    for i = 1:numel(selected)
        plotConditionExhibit(Tmw, Tte, selected(i), windowSec, gapFactor);
    end
    fprintf('\nRendered %d condition exhibit(s).\n', numel(selected));
end

%% ================================================================
%  Local functions
%  ================================================================
function C = clusterEvents(E, clusterWinS)
% Cluster flag rows per (sat,pair) into events; flags within clusterWinS
% seconds of each other on the same (sat,pair) belong to one event.
    C = struct('sat',{},'pair',{},'system',{},'t0',{},'hasMW',{}, ...
               'hasGF',{},'rank',{},'inArc',{});
    if isempty(E), return; end
    key = E.sat + "|" + E.pair;
    [ukey, ~, gi] = unique(key);
    for g = 1:numel(ukey)
        sub = E(gi==g, :);
        sub = sortrows(sub, 'utc');
        newCluster = [true; seconds(diff(sub.utc)) > clusterWinS];
        cid = cumsum(newCluster);
        for c = 1:max(cid)
            rows  = sub(cid==c, :);
            mwR   = rows(rows.src=="mw", :);
            gfR   = rows(rows.src=="gf", :);
            hasMW = ~isempty(mwR);
            hasGF = ~isempty(gfR);
            rmw = 0; tmw = NaT; if hasMW, [rmw,im]=max(mwR.ratio); tmw=mwR.utc(im); end
            rgf = 0; tgf = NaT; if hasGF, [rgf,ig]=max(gfR.ratio); tgf=gfR.utc(ig); end
            % t0 anchors on the detector that fired strongest; for a
            % dual-detector event prefer the MW step (sharper in time).
            if hasMW, t0 = tmw; else, t0 = tgf; end
            C(end+1) = struct('sat',rows.sat(1),'pair',rows.pair(1), ...
                'system',rows.system(1),'t0',t0,'hasMW',hasMW,'hasGF',hasGF, ...
                'rank',max(rmw,rgf),'inArc',hasMW && any(mwR.arc>1)); %#ok<AGROW>
        end
    end
end

function pick = pickDistinctSat(cand, n)
% Greedily take the highest-ranked candidates, preferring a new satellite
% each pick, then fill any remainder in rank order.
    pick = cand([]);
    usedSat = strings(0);
    for i = 1:numel(cand)
        if numel(pick) >= n, break; end
        if ~any(usedSat == cand(i).sat)
            pick(end+1) = cand(i); %#ok<AGROW>
            usedSat(end+1) = cand(i).sat; %#ok<AGROW>
        end
    end
    if numel(pick) < n
        for i = 1:numel(cand)
            if numel(pick) >= n, break; end
            already = arrayfun(@(x) isequal(x,cand(i)), pick);
            if ~any(already), pick(end+1) = cand(i); end %#ok<AGROW>
        end
    end
end

function clean = sampleCleanWindows(Tmw, Tte, mwSlip, gfFlag, windowSec, n, avoidSats)
% Pick (sat,pair) mid-arc epochs where NEITHER detector fires within
% +/- windowSec, preferring satellites not already used and that have data
% in both traces.  Returns a struct array (possibly shorter than n).
    clean = struct('sat',{},'pair',{},'system',{},'t0',{});
    % Candidate (sat,pair) from the GF trace (mature rows), most-populated first.
    keys = Tte.satellite + "|" + Tte.pair;
    [uk, ~, gi] = unique(keys);
    cnt = accumarray(gi, 1);
    [~, ord] = sort(cnt, 'descend');
    uk = uk(ord);
    usedSat = string(avoidSats);
    for j = 1:numel(uk)
        if numel(clean) >= n, break; end
        parts = split(uk(j), "|");
        sat = parts(1); pair = parts(2);
        if any(usedSat == sat), continue; end
        teRows = Tte(Tte.satellite==sat & Tte.pair==pair, :);
        mwRows = Tmw(Tmw.satellite==sat & Tmw.pair==pair, :);
        if height(teRows) < 30 || height(mwRows) < 30, continue; end
        teRows = sortrows(teRows,'utc');
        t0 = teRows.utc(round(height(teRows)/2));   % mid-arc
        % Require a quiet +/- windowSec window in BOTH detectors.
        win = seconds(windowSec);
        mwBad = any(abs(mwSlip.utc - t0) <= win & mwSlip.satellite==sat & mwSlip.pair==pair);
        gfBad = any(abs(gfFlag.utc - t0) <= win & gfFlag.satellite==sat & gfFlag.pair==pair);
        if mwBad || gfBad, continue; end
        clean(end+1) = struct('sat',sat,'pair',pair, ...
            'system',teRows.system(1),'t0',t0); %#ok<AGROW>
        usedSat(end+1) = sat; %#ok<AGROW>
    end
end

function plotConditionExhibit(Tmw, Tte, ev, windowSec, gapFactor)
% Four-panel MW-vs-GF exhibit for one event.
    sat = ev.sat; pair = ev.pair; t0 = ev.t0;
    win = seconds(windowSec);
    Wmw = sortrows(Tmw(Tmw.satellite==sat & Tmw.pair==pair & abs(Tmw.utc-t0)<=win, :), 'utc');
    Wte = sortrows(Tte(Tte.satellite==sat & Tte.pair==pair & abs(Tte.utc-t0)<=win, :), 'utc');

    % MW detection statistic (from the is_slip sample).
    mwStr = 'MW: n/a';
    if ~isempty(Wmw)
        im = pickFlagRow(Wmw.utc, Wmw.is_slip==1, t0);
        dv = Wmw.dev_active_cycles(im); th = Wmw.threshold_cycles(im);
        mwStr = sprintf('MW |dev|/thr=%.1f (dev=%+.2f, thr=%.2f cyc)', abs(dv)/th, dv, th);
    end
    % GF detection statistic (from the gf_flagged sample).
    gfStr = 'GF: n/a';
    if ~isempty(Wte)
        ig = pickFlagRow(Wte.utc, Wte.gf_flagged==1, t0);
        td = Wte.td2_gf_m(ig); tt = Wte.thresh_m(ig);
        gfStr = sprintf('GF |TD2|/thr=%.1f (TD2=%+.3f, thr=%.3f m)', abs(td)/tt, td, tt);
    end
    % Resolved TurboEdit action -- prefer a real outcome (repair/reset/...)
    % over none/code_event, since it can land a few epochs AFTER detection
    % once the WL integer / 2x2 solve settles.
    act = ""; tAct = NaT; dN1 = NaN; dN2 = NaN; l1 = NaN; l2 = NaN;
    if ~isempty(Wte)
        ia = pickActionRow(Wte.action, Wte.utc, t0);
        act = string(Wte.action(ia)); tAct = Wte.utc(ia);
        if ismember('dN1', string(Wte.Properties.VariableNames)), dN1 = Wte.dN1(ia); dN2 = Wte.dN2(ia); end
        if ismember('lambda1_m', string(Wte.Properties.VariableNames)), l1 = Wte.lambda1_m(ia); l2 = Wte.lambda2_m(ia); end
    end
    % Detection epoch (earliest flag) and detect->action lag in epochs.
    mwFlagT = Wmw.utc(Wmw.is_slip==1);
    gfFlagT = Wte.utc(Wte.gf_flagged==1);
    detTimes = [mwFlagT; gfFlagT];
    if isempty(detTimes), tDetect = t0; else, tDetect = min(detTimes); end
    actStr = 'n/a'; lagStr = "";
    if act ~= ""
        if ~isnat(tAct) && act ~= "none" && tAct > tDetect
            lagEp  = round(seconds(tAct - tDetect) / medianDt([Wmw.utc; Wte.utc]));
            lagStr = sprintf('  +%dep lag', lagEp);
        end
        if isfinite(dN1) && (dN1~=0 || dN2~=0)
            actStr = sprintf('%s (dN1=%+d, dN2=%+d)%s', act, dN1, dN2, lagStr);
        else
            actStr = sprintf('%s%s', act, lagStr);
        end
    end

    fig = figure('Name', sprintf('Slip condition %d', ev.condNum), ...
                 'Color','w','Position',[60 60 1080 720]); %#ok<NASGU>
    tl = tiledlayout(2,2,'TileSpacing','tight','Padding','compact');
    title(tl, sprintf('Condition %s   |   %s %s @ %s', ev.condLabel, sat, pair, string(t0)), ...
          'Interpreter','none','FontWeight','bold');
    subtitle(tl, sprintf('%s     %s     TE: action=%s', mwStr, gfStr, actStr), ...
             'Interpreter','none', 'FontSize', 9);

    % --- TL: MW raw + reference levels ---
    ax1 = nexttile(tl); hold(ax1,'on'); grid(ax1,'on');
    if ~isempty(Wmw)
        gbPlot(ax1, Wmw.utc, Wmw.mw_cycles, gapFactor, '-o', [0 0.35 0.75], 'MW raw');
        if ismember('mw_mean_cycles', string(Wmw.Properties.VariableNames))
            gbPlot(ax1, Wmw.utc, Wmw.mw_mean_cycles, gapFactor, '-', [0.85 0.55 0], 'Hatch mean');
        end
        if ismember('local_ref_cycles', string(Wmw.Properties.VariableNames))
            gbPlot(ax1, Wmw.utc, Wmw.local_ref_cycles, gapFactor, '-', [0.2 0.6 0.2], 'local median');
        end
        starFlags(ax1, Wmw, mwFlagT, 'mw_cycles');
    else, text(0.5,0.5,'no MW data','Units','normalized','Parent',ax1); end
    markEpochs(ax1, tDetect, tAct, act, true); ylabel(ax1,'MW WL (cyc)'); title(ax1,'MW raw wide-lane');
    legend(ax1,'Location','best','FontSize',7); ax1.XTickLabel = [];

    % --- BL: MW gate ---
    ax3 = nexttile(tl,3); hold(ax3,'on'); grid(ax3,'on');
    if ~isempty(Wmw)
        gbPlot(ax3, Wmw.utc, Wmw.dev_active_cycles, gapFactor, '-o', [0 0.35 0.75], 'dev_{active}');
        gbPlot(ax3, Wmw.utc,  Wmw.threshold_cycles, gapFactor, '--', [0.85 0 0], '\pm threshold');
        gbPlot(ax3, Wmw.utc, -Wmw.threshold_cycles, gapFactor, '--', [0.85 0 0], '');
        starFlags(ax3, Wmw, mwFlagT, 'dev_active_cycles');
    end
    markEpochs(ax3, tDetect, tAct, act, false); ylabel(ax3,'dev_{active} (cyc)'); xlabel(ax3,'GPS Time');
    title(ax3,'MW active deviation vs gate');

    % --- TR: GF raw + repaired overlay ---
    ax2 = nexttile(tl,2); hold(ax2,'on'); grid(ax2,'on');
    if ~isempty(Wte)
        gbPlot(ax2, Wte.utc, Wte.gf_m, gapFactor, '-o', [0 0.35 0.75], 'GF raw');
        if act=="repair" && isfinite(dN1) && isfinite(l1) && (dN1~=0 || dN2~=0)
            stepM = l1*dN1 - l2*dN2; post = Wte.utc >= tDetect;
            gbPlot(ax2, Wte.utc(post), Wte.gf_m(post)-stepM, gapFactor, '--', [0.75 0 0.1], ...
                   sprintf('repaired: GF-(%+.3f m)', stepM));
        end
        starFlags(ax2, Wte, gfFlagT, 'gf_m');
    else, text(0.5,0.5,'no GF data','Units','normalized','Parent',ax2); end
    markEpochs(ax2, tDetect, tAct, act, false); ylabel(ax2,'GF (m)'); title(ax2,'GF raw geometry-free phase');
    legend(ax2,'Location','best','FontSize',7); ax2.XTickLabel = [];

    % --- BR: GF gate ---
    ax4 = nexttile(tl,4); hold(ax4,'on'); grid(ax4,'on');
    if ~isempty(Wte)
        gbPlot(ax4, Wte.utc, Wte.td2_gf_m, gapFactor, '-o', [0 0.35 0.75], 'TD2_{GF}');
        gbPlot(ax4, Wte.utc,  Wte.thresh_m, gapFactor, '--', [0.85 0 0], '\pm threshold');
        gbPlot(ax4, Wte.utc, -Wte.thresh_m, gapFactor, '--', [0.85 0 0], '');
        starFlags(ax4, Wte, gfFlagT, 'td2_gf_m');
    end
    markEpochs(ax4, tDetect, tAct, act, false); ylabel(ax4,'TD2_{GF} (m)'); xlabel(ax4,'GPS Time');
    title(ax4,'GF second time-difference vs gate');

    linkaxes([ax1 ax3 ax2 ax4], 'x');
    if ~isempty(Wmw), xlim(ax1, [min(Wmw.utc) max(Wmw.utc)]); end
end

function gbPlot(ax, t, y, gapFactor, spec, color, name)
% Gap-broken line plot; empty name -> hidden from legend.
    [tt, yy] = insertGapBreaks(t, y, gapFactor);
    if isempty(name)
        plot(ax, tt, yy, spec, 'Color', color, 'MarkerSize', 3, ...
             'LineWidth', 0.9, 'HandleVisibility','off');
    else
        plot(ax, tt, yy, spec, 'Color', color, 'MarkerSize', 3, ...
             'LineWidth', 0.9, 'DisplayName', name);
    end
end

function starFlags(ax, W, flagTimes, col)
% Star the flagged samples of column col within window W.
    for k = 1:numel(flagTimes)
        r = find(W.utc==flagTimes(k), 1);
        if isempty(r), continue; end
        plot(ax, W.utc(r), W.(col)(r), 'kp', 'MarkerSize', 11, ...
             'MarkerFaceColor', [1 0.85 0], 'HandleVisibility','off');
    end
end

function markEpochs(ax, tDet, tAct, act, doLabel)
% Dotted line at the detection epoch; a solid, action-colored line at the
% resolved-action epoch (only when it is a real action a measurable lag
% after detection).  Labels the action line on the first panel only.
    xline(ax, tDet, ':', 'Color', [0.15 0.15 0.15], 'HandleVisibility','off');
    if ~isnat(tAct) && act ~= "none" && act ~= "" && abs(seconds(tAct - tDet)) > 0.001
        col = actionColor(act);
        if doLabel
            xline(ax, tAct, '-', char(act), 'Color', col, 'LineWidth', 1.3, ...
                  'FontSize', 8, 'LabelVerticalAlignment','top', ...
                  'LabelHorizontalAlignment','left', 'HandleVisibility','off');
        else
            xline(ax, tAct, '-', 'Color', col, 'LineWidth', 1.3, 'HandleVisibility','off');
        end
    end
end

function i = pickActionRow(actions, utc, t0)
% Index of the resolved-action row nearest t0, preferring a real outcome
% (repair/reset/defer/moot/code_event) over the 'none' default.
    pri = ["repair","reset","defer","moot","code_event","none"];
    for p = pri
        c = find(actions == p);
        if ~isempty(c), [~,j] = min(abs(utc(c) - t0)); i = c(j); return; end
    end
    [~, i] = min(abs(utc - t0));
end

function md = medianDt(utc)
    u = unique(utc); d = seconds(diff(sort(u))); d = d(d > 0);
    md = median(d); if ~isfinite(md) || md <= 0, md = 1; end
end

function c = actionColor(act)
    switch string(act)
        case "repair",     c = [0.00 0.60 0.20];
        case "reset",      c = [0.85 0.00 0.85];
        case "defer",      c = [0.90 0.50 0.00];
        case "moot",       c = [0.50 0.50 0.50];
        case "code_event", c = [0.20 0.40 0.70];
        otherwise,         c = [0.30 0.30 0.30];
    end
end

function i = pickFlagRow(utc, flagMask, t0)
% Index of the flagged row nearest t0; if none flagged, the nearest row.
% Keeps the title statistic on the actual detection epoch rather than a
% relinearization-duplicate that happens to be closest in time.
    if any(flagMask)
        fi = find(flagMask);
        [~, j] = min(abs(utc(fi) - t0));
        i = fi(j);
    else
        [~, i] = min(abs(utc - t0));
    end
end

function [tOut, yOut] = insertGapBreaks(tIn, yIn, gapFactor)
% Insert NaT/NaN at gaps > gapFactor*median(dt) so plot() breaks the line.
    tIn = tIn(:); yIn = yIn(:);
    if numel(tIn) < 2, tOut = tIn; yOut = yIn; return; end
    [tS, idx] = sort(tIn); yS = yIn(idx);
    dt = seconds(diff(tS)); md = median(dt);
    if ~isfinite(md) || md <= 0, md = 1; end
    gaps = find(dt > gapFactor*md);
    if isempty(gaps), tOut = tS; yOut = yS; return; end
    tOut = NaT(numel(tS)+numel(gaps),1);
    if ~isempty(tS.TimeZone), tOut.TimeZone = tS.TimeZone; end
    yOut = NaN(numel(tS)+numel(gaps),1);
    w = 1; r0 = 1;
    for g = 1:numel(gaps)
        gi = gaps(g); n = gi - r0 + 1;
        tOut(w:w+n-1) = tS(r0:gi); yOut(w:w+n-1) = yS(r0:gi);
        w = w + n + 1; r0 = gi + 1;
    end
    n = numel(tS) - r0 + 1;
    if n > 0, tOut(w:w+n-1) = tS(r0:end); yOut(w:w+n-1) = yS(r0:end); end
end
