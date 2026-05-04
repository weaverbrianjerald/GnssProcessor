function results = loadBatchResults(batch_root)
% LOADBATCHRESULTS  Walk a run_batch.py output tree and load every
% cell's state_trace.csv.
%
%   RESULTS = LOADBATCHRESULTS(BATCH_ROOT) returns a struct array with
%   one entry per (station, date) cell.  Cells whose state_trace.csv is
%   missing or empty come back with .state_trace = [] so the caller can
%   filter.  The summary.json under BATCH_ROOT is the canonical index of
%   what the orchestrator attempted; this function joins it with the
%   on-disk trace files.
%
%   Output struct fields:
%       .station      4- or 9-char station ID (uppercase)
%       .year         integer
%       .doy          integer (day of year)
%       .success      logical (from summary.json)
%       .elapsed_s    double  (cell wall-clock seconds, from summary)
%       .run_dir      char    (absolute path to the cell's dir)
%       .state_trace  table   (from readtable) or []
%
%   See also: plotCellTimeseries, plotBatchSummary, runBatchPlots.

batch_root = char(batch_root);
summary_path = fullfile(batch_root, 'summary.json');
if ~isfile(summary_path)
    error('loadBatchResults:NoSummary', ...
          'summary.json not found at %s', summary_path);
end
summary = jsondecode(fileread(summary_path));

% jsondecode returns a struct array when every result has the same
% fields, which is the orchestrator's contract.  Cell-array fallback
% just-in-case (older MATLAB or hand-edited summaries).
if iscell(summary.results)
    raw = summary.results;
else
    raw = num2cell(summary.results);
end
n = numel(raw);

results = repmat(struct( ...
    'station', '', 'year', 0, 'doy', 0, ...
    'success', false, 'elapsed_s', 0, ...
    'run_dir', '', 'state_trace', []), n, 1);

for i = 1:n
    r = raw{i};
    results(i).station   = upper(char(r.station));
    results(i).year      = double(r.year);
    results(i).doy       = double(r.doy);
    results(i).success   = logical(r.success);
    results(i).elapsed_s = double(r.elapsed_s);
    results(i).run_dir   = char(r.run_dir);

    trace_path = fullfile(results(i).run_dir, 'logs', 'state_trace.csv');
    if ~isfile(trace_path); continue; end

    % Empty CSV (header only or zero bytes) means the run didn't reach
    % the first epoch — leave .state_trace = [] so plotters skip it.
    info = dir(trace_path);
    if info.bytes <= 0; continue; end

    try
        T = readtable(trace_path);
    catch ME
        warning('loadBatchResults:ReadFail', ...
                'Could not parse %s: %s', trace_path, ME.message);
        continue;
    end
    if height(T) == 0; continue; end
    results(i).state_trace = T;
end
end
