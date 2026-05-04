function plotCellTimeseries(cell, opts)
% PLOTCELLTIMESERIES  Per-cell convergence and state-vector timeseries.
%
%   PLOTCELLTIMESERIES(CELL) opens a figure with four stacked panels
%   for one cell: ENU position relative to a reference, position
%   1-sigma, clock + tropo, and iono mean (when available).
%
%   PLOTCELLTIMESERIES(CELL, 'View', 'position') opens a single-panel
%   figure with just the ENU trace — useful for batch reviews where
%   the σ / clock / tropo / iono panels would only add visual noise.
%   Default View is 'full' (the 4-panel layout).
%
%   PLOTCELLTIMESERIES(CELL, 'RefEcef', [X Y Z]) uses an explicit ECEF
%   ground-truth reference (m).  Default is the median of the last
%   'RefN' epochs (default 100), which works for both static and
%   slow-kinematic cells.
%
%   See also: loadBatchResults, plotBatchSummary, runBatchPlots.

arguments
    cell  struct
    opts.View (1,:) char {mustBeMember(opts.View, {'full','position'})} = 'full'
    % RefEcef: leave empty to use the median-of-last-RefN epochs as the
    % reference, or pass a length-3 ECEF vector [x y z] in metres.  No
    % validator on RefEcef because mustBeVector rejects [] as a default
    % (empty isn't classed as a vector); shape is checked at runtime.
    opts.RefEcef double = []
    opts.RefN    double {mustBePositive, mustBeInteger} = 100
end

if ~isempty(opts.RefEcef) && numel(opts.RefEcef) ~= 3
    error('plotCellTimeseries:BadRefEcef', ...
          'RefEcef must be a length-3 [x y z] ECEF vector (got %d elements).', ...
          numel(opts.RefEcef));
end

if isempty(cell.state_trace)
    warning('plotCellTimeseries:Empty', ...
            'Cell %s %d-%03d has no state_trace; skipping.', ...
            cell.station, cell.year, cell.doy);
    return;
end

T = cell.state_trace;
ttl = sprintf('%s %d-%03d', cell.station, cell.year, cell.doy);

% --- GPS week + TOW -> UTC datetime  (matches matlab/state_base_parameters.m)
gpsEpoch = datetime(1980, 1, 6, 0, 0, 0, 'TimeZone', 'UTC');
t = gpsEpoch + seconds(T.week * 7 * 86400 + T.tow_s) + seconds(18);

% --- Reference ECEF for ENU
if ~isempty(opts.RefEcef)
    refEcef = opts.RefEcef(:)';
    refLabel = sprintf('user ref [%.3f %.3f %.3f] m', refEcef);
else
    n = min(opts.RefN, height(T));
    tail = (height(T) - n + 1):height(T);
    refEcef = [median(T.x_m(tail)), median(T.y_m(tail)), median(T.z_m(tail))];
    refLabel = sprintf('median of last %d epochs', n);
end

llh = ecef2lla(refEcef);
[E, Nrt, U] = ecef2enu(T.x_m, T.y_m, T.z_m, ...
                       llh(1), llh(2), llh(3), ...
                       referenceEllipsoid('wgs84'), 'degrees');

% ============================================================
%  Figure (layout depends on opts.View)
% ============================================================
switch opts.View
    case 'position'
        figure('Name', sprintf('Cell ENU: %s', ttl), 'Color', 'w');
        hold on; grid on;
        plot(t, E,   'b', 'LineWidth', 1.2);
        plot(t, Nrt, 'r', 'LineWidth', 1.2);
        plot(t, U,   'k', 'LineWidth', 1.2);
        ylabel('ENU (m)');
        xlabel('UTC time');
        legend('East','North','Up','Location','best');
        title(sprintf('%s — ENU relative to %s', ttl, refLabel), ...
              'Interpreter','none');

    case 'full'
        figure('Name', sprintf('Cell timeseries: %s', ttl), 'Color', 'w');

        % --- (1) ENU position
        subplot(4,1,1); hold on; grid on;
        plot(t, E,   'b', 'LineWidth', 1.2);
        plot(t, Nrt, 'r', 'LineWidth', 1.2);
        plot(t, U,   'k', 'LineWidth', 1.2);
        ylabel('ENU (m)');
        legend('East','North','Up','Location','best');
        title(sprintf('%s — ENU relative to %s', ttl, refLabel), ...
              'Interpreter','none');

        % --- (2) Position 1-sigma
        subplot(4,1,2); hold on; grid on;
        plot(t, T.x_std_m, 'b');
        plot(t, T.y_std_m, 'r');
        plot(t, T.z_std_m, 'k');
        ylabel('\sigma_{xyz} (m)');
        set(gca, 'YScale', 'log');
        legend('\sigma_x','\sigma_y','\sigma_z','Location','best');

        % --- (3) Clock + tropo (twin axes)
        subplot(4,1,3); hold on; grid on;
        yyaxis left;
        plot(t, T.clock_m, 'b', 'LineWidth', 1.0);
        ylabel('Clock (m)');
        yyaxis right;
        plot(t, T.tropo_m, 'r', 'LineWidth', 1.0);
        ylabel('Tropo (m)');
        legend('Clock','Tropo','Location','best');

        % --- (4) Iono mean (PPP-RTK only)
        subplot(4,1,4); hold on; grid on;
        if any(strcmp(T.Properties.VariableNames, 'iono_mean_m'))
            plot(t, T.iono_mean_m, 'b', 'LineWidth', 1.0);
            ylabel('Iono mean (m)');
        else
            text(0.5, 0.5, 'No iono states (non-PPP-RTK)', ...
                 'Units','normalized','HorizontalAlignment','center', ...
                 'FontAngle','italic','Color',[0.4 0.4 0.4]);
            set(gca,'YTick',[]);
        end
        xlabel('UTC time');
end

end
