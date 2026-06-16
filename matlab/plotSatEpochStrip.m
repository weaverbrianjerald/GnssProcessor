function plotSatEpochStrip(D, opts)
%% ============================================================
%  Function: plotSatEpochStrip
%  Purpose: Shared (satellite x epoch) strip-chart renderer for the GNSS
%           trace viewers.  Factors the imagesc grid that
%           plotResidualStrip.m / view_ambiguity_locktime.m each
%           re-implemented: one figure per constellation, one tile per
%           "facet" (signal, signal-pair, or none), (satellite x epoch)
%           cells colored by a scalar (or a category), satellites not
%           tracked at an epoch left transparent, optional event markers,
%           a shared colorbar/legend, and linked x-axes.
%
%  Driver scripts (view_*_strip.m) load their CSV, melt it into the
%  long-form table D below, choose render options, and call this once.
%
%  ---- D : long-form table, one row per (satellite, facet, epoch) ----
%     system     string   "GPS" / "Galileo" / ...
%     satellite  string   "G03", "E21", ...
%     facet      string   per-tile partition key (e.g. "L2W" or
%                         "L1C/L2W"); "" => a single tile per constellation
%     utc        datetime epoch on the GPS time scale
%     value      double   the quantity to paint (for categorical mode this
%                         is the category INDEX 1..K, mapped by the driver)
%
%  ---- opts : struct of render options (all optional unless noted) ----
%     .figTitle     char    base title; per figure becomes "<t> -- <sys>"
%     .valueLabel   char    colorbar / legend label (REQUIRED in practice)
%     .subtitle     char    small caption under the figure title
%     .figTag       char    short token used in the figure window Name
%     .sysOrder     cellstr constellation render order (default canonical)
%
%     .mode         'continuous' (default) | 'categorical'
%   continuous:
%     .colormap     name | Nx3   name in {freshness, freshness_r,
%                                diverging, elevation, sequential,
%                                turbo_r, parula_r, parula}
%     .diverging    logical default false; if true and caxis is auto, the
%                                limits are made symmetric about 0 and the
%                                default map becomes 'diverging'
%     .caxis        [lo hi] | 'auto' | 'autosym'   default 'auto'
%     .refTicks     numeric vector of colorbar ticks to force (e.g.
%                                [-1 0 1] for a detector gate)
%     .annotate     logical default true -> per-tile "mu/sigma/N" box
%   categorical:
%     .cat.labels   string/cellstr 1xK  legend labels (low->high index)
%     .cat.colors   Kx3 RGB             one row per category
%
%     .markers      table(system,satellite,facet,utc,kind) of events to
%                                overlay as glyphs (optional)
%     .markerStyles struct: field per kind -> struct('marker',ch,
%                                'color',1x3,'size',n).  Missing kinds get
%                                a sensible default.
%  ============================================================

    arguments
        D table
        opts struct
    end

    % ---- option defaults --------------------------------------------
    g = @(f,d) (isfield(opts,f) && ~isempty(opts.(f))) * 1;   % present?
    if ~g('figTitle',1),   opts.figTitle   = '';            end
    if ~g('valueLabel',1), opts.valueLabel = '';            end
    if ~g('subtitle',1),   opts.subtitle   = '';            end
    if ~g('figTag',1),     opts.figTag     = 'strip';       end
    if ~g('sysOrder',1),   opts.sysOrder   = {'GPS','Galileo','GLONASS','BeiDou','QZSS','SBAS'}; end
    if ~g('mode',1),       opts.mode       = 'continuous';  end
    if ~g('colormap',1),   opts.colormap   = 'parula';      end
    if ~g('diverging',1),  opts.diverging  = false;         end
    if ~g('caxis',1),      opts.caxis      = 'auto';        end
    if ~g('annotate',1),   opts.annotate   = true;          end
    if ~isfield(opts,'refTicks'),     opts.refTicks    = []; end
    if ~isfield(opts,'markers'),      opts.markers     = []; end
    if ~isfield(opts,'markerStyles'), opts.markerStyles = struct(); end

    isCat = strcmpi(opts.mode,'categorical');
    if isCat
        assert(isfield(opts,'cat') && isfield(opts.cat,'colors'), ...
            'categorical mode requires opts.cat.colors / opts.cat.labels');
    end

    if isempty(D)
        warning('plotSatEpochStrip: empty data, nothing to draw'); return;
    end
    D.system    = string(D.system);
    D.satellite = string(D.satellite);
    D.facet     = string(D.facet);
    D.facet(ismissing(D.facet)) = "";

    epochs  = unique(D.utc);
    nEpochs = numel(epochs);

    % Continuous colormap + caxis resolved once (uniform across figures).
    if isCat
        K     = size(opts.cat.colors,1);
        cmap  = opts.cat.colors;
        cLim  = [0.5, K+0.5];
    else
        if opts.diverging && ischar(opts.colormap) && strcmpi(opts.colormap,'parula')
            opts.colormap = 'diverging';
        end
        cmap = resolveColormap(opts.colormap, 256);
        cLim = resolveCaxis(opts.caxis, D.value, opts.diverging);
    end

    sysPresent = opts.sysOrder(cellfun(@(s) any(D.system==string(s)), opts.sysOrder));
    if isempty(sysPresent)
        warning('plotSatEpochStrip: no recognized constellations'); return;
    end

    figIdx = 0;
    for s = 1:numel(sysPresent)
        sysName = sysPresent{s};
        Dsys = D(D.system==string(sysName), :);
        if isempty(Dsys), continue; end

        facets = sort(unique(Dsys.facet));
        facets = facets(~ismissing(facets));
        if isempty(facets), facets = ""; end
        nF = numel(facets);

        figHeight = min(950, 175*nF + 150);
        figIdx = figIdx + 1;
        fig = figure('Name', sprintf('%s -- %s', opts.figTag, sysName), ...
                     'Color','w', ...
                     'Position', [60+(figIdx-1)*25 60+(figIdx-1)*25 1400 figHeight]);
        tl = tiledlayout(fig, nF, 1, 'TileSpacing','tight','Padding','compact');
        if ~isempty(opts.figTitle)
            title(tl, sprintf('%s -- %s', opts.figTitle, sysName), 'Interpreter','tex');
        end
        if ~isempty(opts.subtitle)
            subtitle(tl, opts.subtitle, 'Interpreter','tex', 'FontSize', 8);
        end

        columnAxes = gobjects(0);
        for r = 1:nF
            fac = facets(r);
            sub = Dsys(Dsys.facet==fac, :);
            if isempty(sub), continue; end

            sats = prnSort(unique(sub.satellite));
            nSats = numel(sats);

            % (sat x epoch) grid; NaN -> transparent.
            G = nan(nSats, nEpochs);
            [~, si] = ismember(sub.satellite, sats);
            [~, ei] = ismember(sub.utc, epochs);
            keep = si>0 & ei>0;
            G(sub2ind([nSats nEpochs], si(keep), ei(keep))) = double(sub.value(keep));

            ax = nexttile(tl); hold(ax,'on');
            columnAxes(end+1) = ax; %#ok<AGROW>

            imH = imagesc(ax, [epochs(1) epochs(end)], [1 nSats], G);
            set(imH, 'AlphaData', ~isnan(G));
            colormap(ax, cmap);
            clim(ax, cLim);
            ax.YDir = 'reverse';
            ax.YTick = 1:nSats;
            ax.YTickLabel = cellstr(sats);
            ax.YLim = [0.5 nSats+0.5];
            ax.XLim = [epochs(1) epochs(end)];
            ax.TickLabelInterpreter = 'none';
            grid(ax,'on'); box(ax,'on');

            drawMarkers(ax, opts, sysName, fac, sats);

            if r == nF, xlabel(ax,'GPS Time'); else, ax.XTickLabel = []; end
            if fac == ""
                if isfield(opts,'facetLabelFallback') && ~isempty(opts.facetLabelFallback)
                    ylabel(ax, opts.facetLabelFallback);
                end
            else
                ylabel(ax, fac);
            end

            if ~isCat && opts.annotate
                v = double(sub.value); v = v(isfinite(v));
                if ~isempty(v)
                    text(ax, 0.995, 0.94, ...
                        sprintf('\\mu=%.3g  \\sigma=%.3g  N=%d', mean(v), std(v), numel(v)), ...
                        'Units','normalized','HorizontalAlignment','right', ...
                        'VerticalAlignment','top','FontSize',8, 'BackgroundColor','w', ...
                        'EdgeColor',[0.7 0.7 0.7],'Margin',1);
                end
            end
            hold(ax,'off');
        end

        if ~isempty(columnAxes)
            cb = colorbar(columnAxes(end), 'Location','eastoutside');
            cb.Label.String = opts.valueLabel;
            cb.Label.Interpreter = 'tex';
            if isCat
                cb.Ticks = 1:size(opts.cat.colors,1);
                if isfield(opts.cat,'labels') && ~isempty(opts.cat.labels)
                    cb.TickLabels = cellstr(opts.cat.labels);
                end
                cb.TickLabelInterpreter = 'none';
            elseif ~isempty(opts.refTicks)
                cb.Ticks = opts.refTicks;
            end
        end
        if numel(columnAxes) > 1, linkaxes(columnAxes, 'x'); end
    end
end

%% ---- local helpers ------------------------------------------------
function drawMarkers(ax, opts, sysName, fac, sats)
% Overlay event glyphs from opts.markers that fall in this (system,facet)
% tile, styled by opts.markerStyles per kind.
    M = opts.markers;
    if isempty(M) || ~istable(M), return; end
    m = string(M.system)==string(sysName) & string(M.facet)==fac;
    if ~any(m), return; end
    M = M(m, :);
    for e = 1:height(M)
        row = find(sats==string(M.satellite(e)), 1);
        if isempty(row), continue; end
        kind = char(M.kind(e));
        [mk, col, sz] = markerStyle(opts.markerStyles, kind);
        plot(ax, M.utc(e), row, mk, 'MarkerSize', sz, ...
             'MarkerEdgeColor', col, 'LineWidth', 1.2);
    end
end

function [mk, col, sz] = markerStyle(styles, kind)
% Resolve a glyph for a marker kind; fall back to a neutral default.
    if isfield(styles, kind)
        st = styles.(kind);
        mk  = st.marker; col = st.color; sz = st.size;
    else
        mk = 'o'; col = [0 0 0]; sz = 6;
    end
end

function sats = prnSort(sats)
% Sort satellite IDs by numeric PRN (chars 2-3), stable on parse failure.
    sats = unique(string(sats));
    prn = nan(numel(sats),1);
    for k = 1:numel(sats)
        ch = char(sats(k));
        if numel(ch) >= 3, prn(k) = str2double(ch(2:3)); end
    end
    [~, ord] = sortrows([prn (1:numel(sats))'], [1 2]);
    sats = sats(ord);
end

function c = resolveCaxis(spec, vals, diverging)
% Numeric passthrough, or robust auto limits (2nd/98th pct) avoiding
% outlier saturation.  'autosym' (or diverging+'auto') -> symmetric.
    if isnumeric(spec) && numel(spec)==2
        c = spec; return;
    end
    v = double(vals); v = v(isfinite(v));
    if isempty(v), c = [0 1]; return; end
    sym = strcmpi(spec,'autosym') || (diverging && strcmpi(spec,'auto'));
    if sym
        m = pctl(abs(v), 98);
        if ~(m>0), m = max(abs(v)); end
        if ~(m>0), m = 1; end
        c = [-m m];
    else
        lo = pctl(v, 2); hi = pctl(v, 98);
        if ~(hi>lo), hi = lo + max(eps, abs(lo)); end
        c = [lo hi];
    end
end

function p = pctl(v, q)
% Toolbox-free percentile (nearest-rank) so we don't depend on Stats.
    v = sort(v(isfinite(v)));
    n = numel(v);
    if n==0, p = NaN; return; end
    idx = min(max(round(q/100*(n-1))+1, 1), n);
    p = v(idx);
end

function cmap = resolveColormap(name, n)
% Delegate to the shared stripColormap library: named maps, '_r' reversed
% variants, legacy aliases (freshness/elevation/diverging/sequential), and
% Nx3 passthrough are all handled there.
    cmap = stripColormap(name, n);
end
