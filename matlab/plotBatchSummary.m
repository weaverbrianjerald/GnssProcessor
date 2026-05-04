function plotBatchSummary(results, opts)
% PLOTBATCHSUMMARY  Cross-cell repeatability — final ENU position per
% (station, day) referenced to each station's cross-day median ECEF.
%
%   PLOTBATCHSUMMARY(RESULTS) opens one figure with one tile per
%   station.  Each tile's x-axis is DOY; y-axis is ENU offset from
%   the per-station median.  The result is a quick visual check of
%   day-to-day position repeatability across the batch.
%
%   PLOTBATCHSUMMARY(RESULTS, 'LastN', K) takes the median of the last
%   K epochs of each cell's state_trace.csv as that cell's "final"
%   position.  Default K = 100.
%
%   Cells where state_trace is empty (run failed before producing
%   epochs) are silently dropped.
%
%   See also: loadBatchResults, plotCellTimeseries, runBatchPlots.

arguments
    results struct
    opts.LastN double {mustBePositive, mustBeInteger} = 100
end

% --- Drop cells without trace data ----------------------------------
ok = arrayfun(@(c) ~isempty(c.state_trace) && height(c.state_trace) > 0, results);
results = results(ok);
if isempty(results)
    warning('plotBatchSummary:Empty', ...
            'No cells with state_trace data; nothing to plot.');
    return;
end

% --- Per-cell "final" ECEF ------------------------------------------
nCells = numel(results);
finalEcef = nan(nCells, 3);
for i = 1:nCells
    T = results(i).state_trace;
    n = min(opts.LastN, height(T));
    tail = (height(T) - n + 1):height(T);
    finalEcef(i, :) = [median(T.x_m(tail)), ...
                       median(T.y_m(tail)), ...
                       median(T.z_m(tail))];
end

stations = string({results.station});
years    = double([results.year]);
doys     = double([results.doy]);

uStations = unique(stations, 'stable');
nStations = numel(uStations);

% --- Figure: one tile per station, one row -------------------------
figure('Name', 'Batch summary: final ENU per cell', 'Color', 'w');
tl = tiledlayout(nStations, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

for k = 1:nStations
    s    = uStations(k);
    mask = (stations == s);
    ce   = finalEcef(mask, :);
    cd   = doys(mask);
    cy   = years(mask);

    % Reference: per-station cross-day median.  This makes each tile
    % self-anchored — readers see deviation from typical, not
    % absolute ECEF (which would be useless to eyeball).
    refEcef = median(ce, 1);
    llh = ecef2lla(refEcef);
    [E, Nrt, U] = ecef2enu(ce(:,1), ce(:,2), ce(:,3), ...
                           llh(1), llh(2), llh(3), ...
                           referenceEllipsoid('wgs84'), 'degrees');

    % Sort by (year, doy) so the line connects in chronological order.
    [~, ord] = sortrows([cy(:), cd(:)]);
    cd  = cd(ord);
    E   = E(ord);
    Nrt = Nrt(ord);
    U   = U(ord);

    nexttile; hold on; grid on;
    plot(cd, E,   'bo-', 'LineWidth', 1.2, 'MarkerSize', 6);
    plot(cd, Nrt, 'ro-', 'LineWidth', 1.2, 'MarkerSize', 6);
    plot(cd, U,   'ko-', 'LineWidth', 1.2, 'MarkerSize', 6);
    ylabel('ENU (m)');
    title(sprintf('%s  (ref = cross-day median ECEF)', s));
    if k == 1
        legend('East','North','Up','Location','best');
    end
    if k == nStations
        xlabel('DOY');
    end
end

title(tl, sprintf('Daily-final ENU repeatability  (last %d epochs per cell)', ...
                  opts.LastN));
end
