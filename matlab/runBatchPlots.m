function results = runBatchPlots(batch_root, opts)
% RUNBATCHPLOTS  Top-level driver: load a batch and plot every cell.
%
%   RESULTS = RUNBATCHPLOTS(BATCH_ROOT) loads every cell under
%   BATCH_ROOT (a run_batch.py output dir) and produces:
%     1. One overview figure with cross-cell ENU repeatability per
%        station (plotBatchSummary).
%     2. One per-cell figure for every cell that has state_trace data
%        (plotCellTimeseries), showing ENU / sigma / clock+tropo /
%        iono.
%
%   The loaded RESULTS struct array is returned so callers can do
%   their own plotting after this driver.
%
%   Optional name-value pairs:
%     'PerCell'   logical, default true.  Set false to skip per-cell
%                 figures and only produce the summary.
%     'View'      'full' (default) or 'position'.  Forwarded to
%                 plotCellTimeseries — pick 'position' for a quick
%                 batch review with just the ENU trace per cell.
%     'LastN'     integer, default 100.  Window size (epochs) for
%                 "final" position median in both plotters.
%
%   Examples:
%       res = runBatchPlots('C:/GnssProcessor/batch_results/week_2025_200');
%       runBatchPlots('...', 'View', 'position');   % ENU-only per cell
%       runBatchPlots('...', 'PerCell', false);     % summary only
%
%   See also: loadBatchResults, plotCellTimeseries, plotBatchSummary.

arguments
    batch_root (1,:) char
    opts.PerCell logical = true
    opts.View (1,:) char {mustBeMember(opts.View, {'full','position'})} = 'full'
    opts.LastN double {mustBePositive, mustBeInteger} = 100
end

results = loadBatchResults(batch_root);

n_total = numel(results);
n_with_trace = sum(arrayfun(@(c) ~isempty(c.state_trace), results));
fprintf('Batch: %d cells, %d with state_trace data\n', n_total, n_with_trace);
if n_with_trace == 0
    warning('runBatchPlots:NoData', ...
            'No state_trace.csv files were found — nothing to plot.');
    return;
end

plotBatchSummary(results, 'LastN', opts.LastN);

if opts.PerCell
    for i = 1:numel(results)
        if ~isempty(results(i).state_trace)
            plotCellTimeseries(results(i), ...
                'View', opts.View, ...
                'RefN', opts.LastN);
        end
    end
end
end
