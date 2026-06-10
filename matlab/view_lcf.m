%% ============================================================
%  Script: view_lcf.m
%  Purpose: View an NGS LASER Constraint File (LCF | *.lcf) used for
%           national / global least-squares adjustments.  Parses the
%           constrained (C) and unconstrained (U) geometric coordinate
%           records, transforms each station's ECEF position and full
%           covariance into geodetic lat/lon/height and local East-
%           North-Up, then renders:
%
%             Fig 1  Global station map, colored by horizontal 1-sigma,
%                    constrained (CX/CC) vs unconstrained (UX/UC) marked
%                    distinctly, with per-station data tips.
%             Fig 2  Precision dashboard: sigma_H and sigma_U histograms,
%                    sigma_H-vs-sigma_U scatter, and the worst-constrained
%                    stations ranked.
%             Fig 3  Error-ellipse gallery (1-sigma and 95%) for the
%                    least-precise constrained stations, in a mm ENU frame.
%
%           A console summary reports the datum/epoch metadata, the record
%           inventory (by 3-char code), and horizontal/vertical sigma
%           statistics, plus any unparsed lines.
%
%  LCF format (NGS "LASER Constraint File v1", 2024-10-16).  Every record
%  begins with a '#' (comment) or a 3-char code: char1 = C/U/M, char2 =
%  quantity (X=ECEF, C=geodetic, O=ortho ht, A=azimuth, ...), char3 =
%  format digit.  Fields are space-delimited; a trailing " # NOTE" is a
%  free-form comment.  This file's records are CX4 / UX4:
%
%    CX4 ID X Y Z COVXX COVYY COVZZ COVXY COVYZ COVXZ  # <STA> epoch <yr> ...
%
%  X,Y,Z in m; the six COV terms in m^2; off-diagonals in the order
%  XY, YZ, XZ (NOT XY, XZ, YZ -- token 5 is YZ, token 6 is XZ).  The NOTE
%  carries the 9-char IGS station name and the reference epoch (e.g.
%  "ABMF00GLP,epoch 2020.0000 from file .../igs20_SIF.json").
%
%  Toolbox-free: ECEF<->geodetic via Bowring closed-form on GRS80/WGS84;
%  covariance rotated to ENU as C_enu = R*C_ecef*R'.  Coastlines are drawn
%  if a 'coastlines.mat' (Mapping Toolbox) is on the path, otherwise a
%  graticule stands in.
%  ============================================================

clearvars -except lcfFile; %clc; %close all;   % keep an externally-set lcfFile

%% --- Content switches ---
showMap            = true;   % Fig 1 -- global station map
showDashboard      = true;   % Fig 2 -- precision histograms / scatter / ranking
showEllipseGallery = true;   % Fig 3 -- worst-N error-ellipse small multiples
showUnconstrained  = true;   % include UX/UC ("a-priori") records on the map
nWorst             = 12;     % how many least-precise stations to highlight
sigmaMetric        = 'major';% ranking/coloring metric for "horizontal sigma":
                             %   'major' = error-ellipse semi-major axis (mm)
                             %   'rss'   = sqrt(sigE^2 + sigN^2)        (mm)
k95                = 2.4477; % sqrt(chi2inv(0.95,2)); 1-sigma -> 95% ellipse scale

%% --- LCF file path ---
% Default points at the attached national-adjustment constraint file.
% Swap in a local copy or another *.lcf as needed.
if ~exist('lcfFile','var') || isempty(lcfFile)
    lcfFile = '\\depot\cce_projects\gcap\Software\LASER\LCF_Constraints\constraints.nonlin.20260130.lcf';
    %lcfFile = 'C:\GnssProcessor\data\constraints.nonlin.20260130.lcf';
end
if ~isfile(lcfFile)
    error('LCF file not found: %s', lcfFile);
end

%% --- Read + parse ---
fid = fopen(lcfFile, 'r', 'n', 'UTF-8');
if fid < 0, error('Could not open LCF file: %s', lcfFile); end
raw = textscan(fid, '%s', 'Delimiter', '\n', 'Whitespace', '');
fclose(fid);
lines = raw{1};

[R, meta] = parse_lcf(lines);   % R: coordinate-station struct array; meta: counts/metadata

if isempty(R)
    error('No plottable coordinate records (CX/UX/CC/UC) found in %s', lcfFile);
end

% Convenient column arrays (struct array -> vectors).
code  = string({R.code}');
isCon = [R.isConstrained]';           % true for C* (constrained) records
isFix = [R.isFixed]';                 % true if covariance is all-zero (held fixed)
lat   = [R.lat]';   lon = [R.lon]';   h = [R.h]';
sigE  = [R.sigE]';  sigN = [R.sigN]'; sigU = [R.sigU]';
smaj  = [R.semiMajor]';                % horizontal error-ellipse semi-major (m)
smin  = [R.semiMinor]';                % horizontal error-ellipse semi-minor (m)
name  = string({R.name}');
pid   = string({R.id}');

% "Horizontal sigma" per the selected metric, reported in mm.
switch lower(sigmaMetric)
    case 'major', sigH = smaj * 1000;            metricLbl = 'ellipse semi-major';
    case 'rss',   sigH = hypot(sigE,sigN)*1000;  metricLbl = 'RSS(\sigma_E,\sigma_N)';
    otherwise,    error('Unknown sigmaMetric: %s', sigmaMetric);
end
sigV3d = sigU * 1000;                  % vertical sigma (mm)
sig3d  = hypot(hypot(sigE,sigN), sigU) * 1000;   % 3-D sigma (mm)

%% --- Console summary ---
fprintf('\n=== LCF Viewer ===\n\n');
fprintf('File   : %s\n', lcfFile);
fprintf('Datum  : %s\n', tern(strlength(meta.datum)>0, meta.datum, '(none / not in file)'));
fprintf('Epoch  : %s\n', tern(~isnan(meta.epoch), sprintf('%.4f', meta.epoch), '(per-record; see notes)'));
fprintf('Lines  : %d total, %d blank, %d comment, %d unparsed\n', ...
        meta.nLines, meta.nBlank, meta.nComment, numel(meta.warnings));

fprintf('\n--- Record inventory (by 3-char code) ---\n');
ckeys = keys(meta.counts);
[~, ord] = sort(ckeys);
for i = ord
    fprintf('  %-5s %6d\n', ckeys{i}, meta.counts(ckeys{i}));
end

nCon = nnz(isCon); nUnc = nnz(~isCon);
fprintf('\n--- Coordinate stations ---\n');
fprintf('  plotted        : %d  (%d constrained, %d unconstrained)\n', numel(R), nCon, nUnc);
fprintf('  held fixed     : %d  (zero covariance)\n', nnz(isFix));

stoch = ~isFix;   % stations that carry an uncertainty
fprintf('\n--- Sigma statistics (mm, stochastic records only) ---\n');
fprintf('  horizontal (%s):  min %.2f   median %.2f   max %.2f\n', ...
        metricLbl, minf(sigH(stoch)), medf(sigH(stoch)), maxf(sigH(stoch)));
fprintf('  vertical   (\\sigma_U)       :  min %.2f   median %.2f   max %.2f\n', ...
        minf(sigV3d(stoch)), medf(sigV3d(stoch)), maxf(sigV3d(stoch)));

% Worst-constrained constrained stations.
conIdx = find(isCon & stoch);
[~, srt] = sort(sig3d(conIdx), 'descend');
worstIdx = conIdx(srt(1:min(nWorst, numel(srt))));
fprintf('\n--- %d least-precise constrained stations ---\n', numel(worstIdx));
fprintf('  %-10s %-8s %9s %9s %8s %8s %8s\n', 'name','id','lat','lon','sE(mm)','sN(mm)','sU(mm)');
fprintf('  %s\n', repmat('-', 1, 70));
for j = worstIdx'
    fprintf('  %-10s %-8s %9.4f %9.4f %8.2f %8.2f %8.2f\n', ...
        name(j), pid(j), lat(j), lon(j), sigE(j)*1000, sigN(j)*1000, sigU(j)*1000);
end
fprintf('\n');

% ----- mask for what goes on the map -----
mapIdx = true(numel(R),1);
if ~showUnconstrained, mapIdx = isCon; end

%% --- Fig 1: global station map ---
if showMap
    f1 = figure('Name', 'LCF -- station map', 'Color', 'w', ...
                'Position', [60 80 1180 620]);
    ax = axes(f1); hold(ax, 'on'); box(ax, 'on');
    draw_world(ax);

    % Stochastic records colored by horizontal sigma (log color scale).
    % Constrained = filled circles, unconstrained = open circles.
    cmap = parula(256);  colormap(ax, cmap);
    cdat = sigH;  % mm
    cLo = max(pctl(cdat(stoch), 2),  1e-3);
    cHi = max(pctl(cdat(stoch), 98), cLo*1.01);

    mC = mapIdx & isCon  & stoch;
    mU = mapIdx & ~isCon & stoch;
    hC = scatter(ax, lon(mC), lat(mC), 34, cdat(mC), 'filled', ...
                 'MarkerEdgeColor', [0.25 0.25 0.25], 'LineWidth', 0.3, ...
                 'MarkerFaceAlpha', 0.95, 'DisplayName', 'constrained (CX/CC)');
    hU = scatter(ax, lon(mU), lat(mU), 30, cdat(mU), 'o', ...
                 'LineWidth', 1.1, 'DisplayName', 'unconstrained (UX/UC)');

    % Held-fixed stations (no uncertainty) as a distinct symbol.
    mF = mapIdx & isFix;
    if any(mF)
        scatter(ax, lon(mF), lat(mF), 60, [0 0 0], 'p', 'filled', ...
                'DisplayName', 'held fixed (\sigma=0)');
    end

    if isfinite(cLo) && isfinite(cHi) && cHi > cLo
        set(ax, 'ColorScale', 'log'); caxis(ax, [cLo cHi]);   % caxis: works on all releases (clim is R2022a+)
    end
    cb = colorbar(ax); cb.Label.String = sprintf('horizontal 1\\sigma -- %s (mm)', metricLbl);

    attach_tips(hC, name(mC), pid(mC), h(mC), sigE(mC), sigN(mC), sigU(mC));
    attach_tips(hU, name(mU), pid(mU), h(mU), sigE(mU), sigN(mU), sigU(mU));

    xlim(ax, [-180 180]); ylim(ax, [-90 90]);
    xticks(ax, -180:30:180); yticks(ax, -90:30:90);
    daspect(ax, [1 1 1]);
    xlabel(ax, 'Longitude (deg)'); ylabel(ax, 'Latitude (deg)');
    legend(ax, 'Location', 'southwest', 'Color', 'w', 'FontSize', 8);
    title(ax, sprintf('%s  --  %d stations  (%d constrained, %d unconstrained)', ...
          shortname(lcfFile), numel(R), nCon, nUnc), 'Interpreter', 'none');
end

%% --- Fig 2: precision dashboard ---
if showDashboard
    f2 = figure('Name', 'LCF -- precision dashboard', 'Color', 'w', ...
                'Position', [120 60 1100 760]);
    tl = tiledlayout(f2, 2, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
    title(tl, sprintf('%s  --  formal precision', shortname(lcfFile)), 'Interpreter', 'none');

    cH = sigH(stoch);  vH = sigV3d(stoch);  conMask = isCon(stoch);

    % (a) horizontal sigma histogram (log x), constrained vs unconstrained
    ax1 = nexttile(tl); hold(ax1, 'on'); grid(ax1, 'on'); box(ax1, 'on');
    eH = log_edges(cH, 40);
    histogram(ax1, cH(conMask),  'BinEdges', eH, 'FaceColor', [0.15 0.45 0.85], ...
              'FaceAlpha', 0.6, 'DisplayName', 'constrained');
    histogram(ax1, cH(~conMask), 'BinEdges', eH, 'FaceColor', [0.85 0.55 0.10], ...
              'FaceAlpha', 0.5, 'DisplayName', 'unconstrained');
    set(ax1, 'XScale', 'log');
    xlabel(ax1, sprintf('horizontal 1\\sigma -- %s (mm)', metricLbl));
    ylabel(ax1, 'count'); legend(ax1, 'Location', 'northeast', 'FontSize', 8);
    title(ax1, sprintf('Horizontal \\sigma   (median %.2f mm)', medf(cH)));

    % (b) vertical sigma histogram (log x)
    ax2 = nexttile(tl); hold(ax2, 'on'); grid(ax2, 'on'); box(ax2, 'on');
    eV = log_edges(vH, 40);
    histogram(ax2, vH(conMask),  'BinEdges', eV, 'FaceColor', [0.15 0.45 0.85], ...
              'FaceAlpha', 0.6, 'DisplayName', 'constrained');
    histogram(ax2, vH(~conMask), 'BinEdges', eV, 'FaceColor', [0.85 0.55 0.10], ...
              'FaceAlpha', 0.5, 'DisplayName', 'unconstrained');
    set(ax2, 'XScale', 'log');
    xlabel(ax2, 'vertical 1\sigma -- \sigma_U (mm)');
    ylabel(ax2, 'count'); legend(ax2, 'Location', 'northeast', 'FontSize', 8);
    title(ax2, sprintf('Vertical \\sigma   (median %.2f mm)', medf(vH)));

    % (c) horizontal vs vertical sigma (log-log), with 1:1 line
    ax3 = nexttile(tl); hold(ax3, 'on'); grid(ax3, 'on'); box(ax3, 'on');
    scatter(ax3, cH(conMask),  vH(conMask),  16, [0.15 0.45 0.85], 'filled', ...
            'MarkerFaceAlpha', 0.45, 'DisplayName', 'constrained');
    scatter(ax3, cH(~conMask), vH(~conMask), 16, [0.85 0.55 0.10], 'filled', ...
            'MarkerFaceAlpha', 0.40, 'DisplayName', 'unconstrained');
    pos  = [cH(cH > 0); vH(vH > 0)];   % log axis needs strictly-positive limits
    lo   = tern(isempty(pos), 0.1, min(pos)*0.8);
    hi   = tern(isempty(pos), 1.0, max([cH;vH])*1.2);
    lims = [lo hi];
    plot(ax3, lims, lims, '--', 'Color', [0.6 0.6 0.6], 'DisplayName', '1:1');
    set(ax3, 'XScale', 'log', 'YScale', 'log'); xlim(ax3, lims); ylim(ax3, lims);
    xlabel(ax3, 'horizontal 1\sigma (mm)'); ylabel(ax3, 'vertical 1\sigma (mm)');
    legend(ax3, 'Location', 'northwest', 'FontSize', 8);
    title(ax3, 'Horizontal vs vertical \sigma');

    % (d) worst-N constrained stations, ranked by 3-D sigma
    ax4 = nexttile(tl); hold(ax4, 'on'); grid(ax4, 'on'); box(ax4, 'on');
    wq = sig3d(worstIdx);
    barh(ax4, wq, 'FaceColor', [0.55 0.30 0.75], 'FaceAlpha', 0.8);
    set(ax4, 'YDir', 'reverse');
    yticks(ax4, 1:numel(worstIdx)); yticklabels(ax4, name(worstIdx));
    set(ax4, 'TickLabelInterpreter', 'none', 'FontSize', 8);
    xlabel(ax4, '3-D 1\sigma (mm)');
    title(ax4, sprintf('%d least-precise constrained stations', numel(worstIdx)));
end

%% --- Fig 3: error-ellipse gallery ---
if showEllipseGallery && ~isempty(worstIdx)
    nG = numel(worstIdx);
    nc = ceil(sqrt(nG)); nr = ceil(nG/nc);
    f3 = figure('Name', 'LCF -- error ellipses', 'Color', 'w', ...
                'Position', [180 40 1080 760]);
    tl3 = tiledlayout(f3, nr, nc, 'TileSpacing', 'compact', 'Padding', 'compact');
    title(tl3, sprintf('%s  --  horizontal error ellipses (1\\sigma fill, 95%% outline) for the %d least-precise constrained stations', ...
          shortname(lcfFile), nG), 'Interpreter', 'none');

    for j = worstIdx'
        ax = nexttile(tl3); hold(ax, 'on'); grid(ax, 'on'); box(ax, 'on');
        S2 = R(j).Cen(1:2,1:2) * 1e6;            % E-N block in mm^2
        [ex1, ny1] = ellipse_pts(S2, 1.0);       % 1-sigma
        [ex2, ny2] = ellipse_pts(S2, k95);       % 95%
        fill(ax, ex1, ny1, [0.15 0.45 0.85], 'FaceAlpha', 0.30, 'EdgeColor', [0.10 0.30 0.65]);
        plot(ax, ex2, ny2, '-', 'Color', [0.55 0.30 0.75], 'LineWidth', 1.2);
        plot(ax, 0, 0, 'k+', 'MarkerSize', 7, 'LineWidth', 1);
        daspect(ax, [1 1 1]);
        r = k95 * sqrt(max(eig((S2 + S2')/2))) * 1.15 + eps;   % exact 95% semi-major bound
        xlim(ax, [-r r]); ylim(ax, [-r r]);
        xlabel(ax, 'East (mm)'); ylabel(ax, 'North (mm)');
        title(ax, sprintf('%s  \\sigma_U=%.1f mm', name(j), sigU(j)*1000), ...
              'Interpreter', 'tex', 'FontSize', 9);
        set(ax, 'FontSize', 7);
    end
end

% =====================================================================
%  Local functions
% =====================================================================

function [R, meta] = parse_lcf(lines)
% PARSE_LCF  Parse LCF lines into a coordinate-station struct array R plus
% a metadata struct.  Plottable records (CX/UX/CC/UC) are fully reduced to
% lat/lon/h + ENU covariance; every other record type is counted in
% meta.counts.  Unparsable non-comment lines are logged in meta.warnings.
    R = struct('code',{}, 'type2',{}, 'fmt',{}, 'id',{}, 'name',{}, ...
               'lat',{}, 'lon',{}, 'h',{}, 'x',{}, 'y',{}, 'z',{}, ...
               'sigE',{}, 'sigN',{}, 'sigU',{}, 'semiMajor',{}, ...
               'semiMinor',{}, 'azFromN',{}, 'Cen',{}, ...
               'isConstrained',{}, 'isFixed',{}, 'epoch',{}, 'note',{});
    meta = struct('datum','', 'epoch',NaN, 'counts',containers.Map('KeyType','char','ValueType','double'), ...
                  'comments',{{}}, 'warnings',{{}}, 'nLines',numel(lines), ...
                  'nBlank',0, 'nComment',0);

    for k = 1:numel(lines)
        ln = strtrim(lines{k});
        if isempty(ln),  meta.nBlank = meta.nBlank + 1;  continue; end
        if ln(1) == '#'
            meta.nComment = meta.nComment + 1;
            meta.comments{end+1} = ln; %#ok<AGROW>
            continue;
        end

        % Split off any trailing free-form note (" # ...").
        hashPos = find(ln == '#', 1);
        if ~isempty(hashPos)
            note = strtrim(ln(hashPos+1:end));
            body = strtrim(ln(1:hashPos-1));
        else
            note = '';  body = ln;
        end
        tok = strsplit(body);            % collapses runs of whitespace
        code = upper(tok{1});
        if ~meta.counts.isKey(code), meta.counts(code) = 0; end
        meta.counts(code) = meta.counts(code) + 1;

        if numel(code) < 2, meta.warnings{end+1} = ln; continue; end %#ok<AGROW>
        type2 = code(1:2);

        % Metadata records.
        if strcmp(type2, 'ME')
            if numel(tok) >= 2, meta.epoch = str2double(tok{2}); end
            continue;
        elseif strcmp(type2, 'MD')
            if numel(tok) >= 2, meta.datum = tok{2}; end
            continue;
        end

        % Only the geometric coordinate records carry a plottable position.
        isECEF = any(strcmp(type2, {'CX','UX'}));
        isGEO  = any(strcmp(type2, {'CC','UC'}));
        if ~(isECEF || isGEO)
            continue;   % CO/UO/CA/CH/CS/CZ/CG/UG/CR -- counted, not plotted
        end

        try
            fmt = str2double(code(3));
            sta = note2name(note);
            ep  = note2epoch(note);
            id  = tok{2};
            v   = str2double(tok(3:end));   % numeric payload

            if isECEF
                x = v(1); y = v(2); z = v(3);
                [latd, lond, hgt] = ecef2geo(x, y, z);
                switch fmt
                    case 1                       % no stats -> fixed
                        Cecef = zeros(3);
                    case 2                       % SDE SDN SDU (ENU sigmas)
                        Cenu = diag([v(4) v(5) v(6)].^2);
                        Cecef = enu_rot(latd,lond)' * Cenu * enu_rot(latd,lond);
                    case 3                       % SDX SDY SDZ (ECEF sigmas)
                        Cecef = diag([v(4) v(5) v(6)].^2);
                    case 4                       % full covariance
                        % Order: XX YY ZZ XY YZ XZ  (off-diag XY,YZ,XZ).
                        Cecef = [v(4) v(7) v(9); v(7) v(5) v(8); v(9) v(8) v(6)];
                    otherwise
                        meta.warnings{end+1} = ln; continue; %#ok<AGROW>
                end
                Rrot = enu_rot(latd, lond);
                Cen  = Rrot * Cecef * Rrot';

            else  % geodetic CC/UC
                switch fmt
                    case 1                       % LAT LON EHT (decimal deg)
                        latd = v(1); lond = v(2); hgt = v(3); Cen = zeros(3);
                    case 2                       % LAT LON EHT SDN SDE SDU
                        latd = v(1); lond = v(2); hgt = v(3);
                        Cen = diag([v(5) v(4) v(6)].^2);   % store as [E N U]
                    case 3                       % LATH..LATS LONH..LONS EHT
                        latd = dms2deg(tok{3}, v(2), v(3), v(4));
                        lond = dms2deg(tok{7}, v(6), v(7), v(8));
                        hgt  = v(9); Cen = zeros(3);
                    case 4                       % DMS + SDN SDE SDU
                        latd = dms2deg(tok{3}, v(2), v(3), v(4));
                        lond = dms2deg(tok{7}, v(6), v(7), v(8));
                        hgt  = v(9);
                        Cen = diag([v(11) v(10) v(12)].^2);
                    otherwise
                        meta.warnings{end+1} = ln; continue; %#ok<AGROW>
                end
                [x, y, z] = geo2ecef(latd, lond, hgt);
            end

            Cen = (Cen + Cen')/2;                % defensive symmetrization
            [sE, sN, sU, smj, smn, azN] = enu_ellipse(Cen);

            R(end+1) = struct( ...
                'code', code, 'type2', type2, 'fmt', fmt, 'id', id, 'name', sta, ...
                'lat', latd, 'lon', lond, 'h', hgt, 'x', x, 'y', y, 'z', z, ...
                'sigE', sE, 'sigN', sN, 'sigU', sU, ...
                'semiMajor', smj, 'semiMinor', smn, 'azFromN', azN, 'Cen', Cen, ...
                'isConstrained', code(1)=='C', ...
                'isFixed', trace(Cen) < 1e-12, 'epoch', ep, 'note', note); %#ok<AGROW>
        catch
            meta.warnings{end+1} = ln; %#ok<AGROW>
        end
    end
end

function [latd, lond, h] = ecef2geo(x, y, z)
% ECEF2GEO  ECEF (m) -> geodetic lat/lon (deg) + ellipsoidal height (m) on
% GRS80/WGS84 via the Bowring closed-form (sub-mm for terrestrial heights).
    a = 6378137.0; f = 1/298.257223563; b = a*(1-f);
    e2 = f*(2-f); ep2 = e2/(1-e2);
    p = hypot(x, y);
    th = atan2(z*a, p*b);
    lat = atan2(z + ep2*b*sin(th).^3, p - e2*a*cos(th).^3);
    lon = atan2(y, x);
    if p > 1   % away from the poles
        N = a / sqrt(1 - e2*sin(lat)^2);
        h = p/cos(lat) - N;
    else       % polar fallback (cos(lat) -> 0)
        N = a / sqrt(1 - e2*sin(lat)^2);
        h = abs(z)/abs(sin(lat)) - N*(1-e2);
    end
    latd = rad2deg(lat); lond = rad2deg(lon);
end

function [x, y, z] = geo2ecef(latd, lond, h)
% GEO2ECEF  Geodetic lat/lon (deg) + ellipsoidal height (m) -> ECEF (m).
    a = 6378137.0; f = 1/298.257223563; e2 = f*(2-f);
    lat = deg2rad(latd); lon = deg2rad(lond);
    N = a / sqrt(1 - e2*sin(lat)^2);
    x = (N + h)*cos(lat)*cos(lon);
    y = (N + h)*cos(lat)*sin(lon);
    z = (N*(1-e2) + h)*sin(lat);
end

function Rm = enu_rot(latd, lond)
% ENU_ROT  Rotation mapping an ECEF vector/covariance to local ENU at
% (lat,lon); rows are [E; N; U].  d_enu = Rm * d_ecef.
    la = deg2rad(latd); lo = deg2rad(lond);
    sl = sin(la); cl = cos(la); so = sin(lo); co = cos(lo);
    Rm = [ -so,      co,     0; ...
           -sl*co,  -sl*so,  cl; ...
            cl*co,   cl*so,  sl];
end

function [sE, sN, sU, smaj, smin, azFromN] = enu_ellipse(Cen)
% ENU_ELLIPSE  From a 3x3 ENU covariance: per-axis sigmas, horizontal
% error-ellipse semi-axes, and major-axis geodetic azimuth (deg from N).
    sE = sqrt(max(Cen(1,1), 0));
    sN = sqrt(max(Cen(2,2), 0));
    sU = sqrt(max(Cen(3,3), 0));
    Sxx = Cen(1,1); Syy = Cen(2,2); Sxy = Cen(1,2);
    tr = Sxx + Syy;
    disc = sqrt(max(((Sxx - Syy)/2)^2 + Sxy^2, 0));
    smaj = sqrt(max(tr/2 + disc, 0));
    smin = sqrt(max(tr/2 - disc, 0));
    if tr < 1e-18
        azFromN = NaN;                       % degenerate (fixed) -- undefined
    else
        phiE = 0.5*atan2(2*Sxy, Sxx - Syy);  % CCW from East
        azFromN = mod(90 - rad2deg(phiE), 180);
    end
end

function [ex, ny] = ellipse_pts(S2, scale)
% ELLIPSE_PTS  Polyline of the (scale*sigma) ellipse for 2x2 covariance S2,
% via eigendecomposition.  Returns East (ex) and North (ny) coordinates.
    [V, D] = eig((S2 + S2')/2);
    r = scale * sqrt(max(diag(D), 0));
    t = linspace(0, 2*pi, 181);
    xy = V * [r(1)*cos(t); r(2)*sin(t)];
    ex = xy(1,:); ny = xy(2,:);
end

function draw_world(ax)
% DRAW_WORLD  Light graticule + coastlines if a coastlines.mat is available.
    % 'HandleVisibility','off' keeps these reference lines out of the legend.
    for yy = -60:30:60, yline(ax, yy, '-', 'Color', [0.92 0.92 0.92], 'HandleVisibility', 'off'); end
    for xx = -150:30:150, xline(ax, xx, '-', 'Color', [0.92 0.92 0.92], 'HandleVisibility', 'off'); end
    yline(ax, 0, '-', 'Color', [0.80 0.80 0.80], 'HandleVisibility', 'off');   % equator
    xline(ax, 0, '-', 'Color', [0.80 0.80 0.80], 'HandleVisibility', 'off');   % prime meridian
    if exist('coastlines.mat', 'file') == 2
        try
            S = load('coastlines');               % coastlat, coastlon (Mapping TB)
            plot(ax, S.coastlon, S.coastlat, '-', 'Color', [0.70 0.70 0.70], ...
                 'LineWidth', 0.5, 'HandleVisibility', 'off');
        catch
        end
    end
end

function attach_tips(hs, nm, id, h, sE, sN, sU)
% ATTACH_TIPS  Add per-point data-tip rows (R2019a+); silently skip on older.
    if isempty(hs) || ~isvalid(hs) || isempty(get(hs, 'XData')), return; end
    try
        dtr = hs.DataTipTemplate.DataTipRows;
        dtr(1).Label = 'lon';
        dtr(2).Label = 'lat';
        dtr(end+1) = dataTipTextRow('station', cellstr(nm));
        dtr(end+1) = dataTipTextRow('id',      cellstr(id));
        dtr(end+1) = dataTipTextRow('h (m)',   h,        '%.3f');
        dtr(end+1) = dataTipTextRow('sE (mm)', sE*1000,  '%.2f');
        dtr(end+1) = dataTipTextRow('sN (mm)', sN*1000,  '%.2f');
        dtr(end+1) = dataTipTextRow('sU (mm)', sU*1000,  '%.2f');
        hs.DataTipTemplate.DataTipRows = dtr;
    catch
    end
end

function d = dms2deg(hemi, deg, mn, sc)
% DMS2DEG  Hemisphere (N/S/E/W) + deg/min/sec -> signed decimal degrees.
    d = abs(deg) + mn/60 + sc/3600;
    if any(upper(hemi) == 'SW'), d = -d; end
end

function s = note2name(note)
% NOTE2NAME  First token of the NOTE (the 9-char station id), comma-stripped.
    s = '';
    if isempty(note), return; end
    t = regexp(strtrim(note), '^[^\s,]+', 'match', 'once');
    if ~isempty(t), s = t; end
end

function e = note2epoch(note)
% NOTE2EPOCH  Pull "epoch <year>" out of the NOTE, else NaN.
    e = NaN;
    t = regexp(note, 'epoch\s+([0-9]+\.?[0-9]*)', 'tokens', 'once');
    if ~isempty(t), e = str2double(t{1}); end
end

% --- small numeric/string helpers (NaN-safe, empty-safe) ---
function y = minf(v), v = v(isfinite(v)); y = tern(isempty(v), NaN, min(v)); end
function y = maxf(v), v = v(isfinite(v)); y = tern(isempty(v), NaN, max(v)); end
function y = medf(v), v = v(isfinite(v)); y = tern(isempty(v), NaN, median(v)); end

function y = pctl(v, p)
% PCTL  Toolbox-free percentile (linear interpolation on sorted data).
    v = sort(v(isfinite(v)));
    if isempty(v),    y = NaN;  return; end
    if isscalar(v),   y = v;    return; end
    x = linspace(0, 100, numel(v));
    y = interp1(x, v, p, 'linear');
end

function e = log_edges(v, n)
% LOG_EDGES  n logarithmically spaced bin edges spanning the positive data.
    v = v(isfinite(v) & v > 0);
    if isempty(v), e = [0.1 1]; return; end
    e = logspace(log10(min(v)), log10(max(v)), n);
    if e(1) == e(end), e = e(1) * [0.9 1.1]; end
end

function s = shortname(p)
    [~, nm, ext] = fileparts(p); s = [nm ext];
end

function out = tern(cond, a, b)
    if cond, out = a; else, out = b; end
end
