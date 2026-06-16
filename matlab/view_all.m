function view_all(output_folder)
%% ============================================================
%  Function: view_all
%  Purpose: Single driver for the GNSS trace-visualization suite.  Point
%           it at the OUTPUT FOLDER of a processor run (the logs/ dir
%           holding the *.csv traces), tick the viewers you want in the
%           active.* block below, hit Run.
%
%  Usage:
%     view_all()                  folder + toggles configured below
%     view_all('D:\run\logs')     same toggles, explicit trace folder
%
%  Behaviour:
%    - Each ACTIVE viewer is skipped with a console note when the trace
%      CSV it needs is missing, so one toggle set works unchanged across
%      PPP / PPP-RTK / baseline / replay runs.
%    - A viewer that errors is reported as FAILED and the run continues.
%    - The folder is published session-wide via gnssVizLogDir(), so after
%      (or instead of) a view_all run every viewer script can still be
%      run STANDALONE and will read from the same folder.
%    - Per-viewer styling switches (plotStyle, plotValue, source, ...)
%      stay inside the individual scripts — this driver only decides
%      WHICH viewers run and on WHAT data.
%
%  See also: gnssVizLogDir, gnssTracePath, runBatchPlots.
%  ============================================================

%% --- Config: trace folder ---------------------------------------------
if nargin < 1 || isempty(output_folder)
    output_folder = 'C:\GnssProcessor\logs';
    %output_folder = 'C:\GnssProcessor\build\x64-Release\logs';
    %output_folder = 'C:\GnssProcessor\build\x64-Debug\logs';
    %output_folder = 'D:\GnssProcessor_Testing\sbf_replay\precise\ppp_rtk\static';
end

closeFiguresFirst = false;   % true -> close all open figures before plotting

%% --- Config: active viewers -------------------------------------------
% State / position
active.state_base_parameters     = true;   % ENU, clock, tropo, 3D trajectory
active.wls                       = true;   % WLS seed / clock-jump diagnostics
active.baseline_trace_parameters = true;   % DD baseline rover trace
% Measurement domain
active.measurement_residuals     = true;   % per-(sat,signal) residual lines
active.plotResidualStrip         = false;   % (sat x epoch) residual raster
active.plotSatelliteSignals      = true;   % band / channel usage raster
active.plotDops                  = true;   % sat counts, PDOP / ADOP
active.plotSatelliteStatus       = true;   % NRCAN-style status strip
active.view_satellite_strip      = true;   % status / elevation raster (shared renderer)
% State domain (per satellite)
active.plotAmbiguities           = true;   % float ambiguity lines
active.view_ambiguity_locktime   = true;   % locktime raster + reset taxonomy
active.plotIonosphere            = true;   % slant iono / VTEC lines
active.view_iono_strip           = false;   % iono / sigma raster
active.plotSatBiases             = false;   % satellite code-bias lines
% Cycle-slip diagnostics
active.view_mwwl_trace           = true;   % MW wide-lane detector trace
active.view_turbo_edit_trace     = true;   % TurboEdit MW+GF resolver trace
active.view_slip_gate_strip      = false;   % deviation / threshold raster
active.view_slip_conditions      = true;   % 2x2 MW x GF condition exhibits
active.view_gf_trace             = false;  % GfSlipDetector (STAGED, uncompiled)
active.view_graphic_trace        = false;  % GraphicSlipDetector trace
active.view_if_slip_trace        = false;  % IfPhaseSlipDetector (STAGED, uncompiled)

%% --- Registry: viewer script -> trace file(s) it needs -----------------
% A viewer runs only when active.<script> is true AND every listed CSV
% exists in the trace folder.  view_slip_gate_strip is registered against
% mwwl_trace.csv (its default source='mw'); flip its internal switch to
% 'gf' and it reads turbo_edit_trace.csv instead.
REG = { ...
    'state_base_parameters',     {'state_trace.csv'}; ...
    'wls',                       {'wls_trace.csv'}; ...
    'baseline_trace_parameters', {'baseline_trace.csv'}; ...
    'measurement_residuals',     {'measurement_residuals.csv'}; ...
    'plotResidualStrip',         {'measurement_residuals.csv'}; ...
    'plotSatelliteSignals',      {'measurement_residuals.csv'}; ...
    'plotDops',                  {'dop_trace.csv'}; ...
    'plotSatelliteStatus',       {'satellite_status_trace.csv'}; ...
    'view_satellite_strip',      {'satellite_status_trace.csv'}; ...
    'plotAmbiguities',           {'ambiguity_trace.csv'}; ...
    'view_ambiguity_locktime',   {'ambiguity_trace.csv'}; ...
    'plotIonosphere',            {'ionosphere_trace.csv'}; ...
    'view_iono_strip',           {'ionosphere_trace.csv'}; ...
    'plotSatBiases',             {'sat_bias_trace.csv'}; ...
    'view_mwwl_trace',           {'mwwl_trace.csv'}; ...
    'view_turbo_edit_trace',     {'turbo_edit_trace.csv'}; ...
    'view_slip_gate_strip',      {'mwwl_trace.csv'}; ...
    'view_slip_conditions',      {'mwwl_trace.csv', 'turbo_edit_trace.csv'}; ...
    'view_gf_trace',             {'gf_slip_trace.csv'}; ...
    'view_graphic_trace',        {'graphic_slip_trace.csv'}; ...
    'view_if_slip_trace',        {'if_slip_trace.csv'}; ...
    };

%% --- Dispatch ----------------------------------------------------------
gnssVizLogDir(output_folder);
logDir = gnssVizLogDir();

if closeFiguresFirst, close all; end

fprintf('\n=== view_all: %s ===\n', logDir);

ran = {}; skipped = {}; failed = {};
for i = 1:size(REG, 1)
    script = REG{i, 1};
    csvs   = REG{i, 2};
    if ~isfield(active, script) || ~active.(script)
        continue;
    end
    present = cellfun(@(c) traceHasData(fullfile(logDir, c)), csvs);
    if ~all(present)
        fprintf('  --  %-28s skipped (missing/empty %s)\n', script, ...
                strjoin(csvs(~present), ', '));
        skipped{end+1} = script; %#ok<AGROW>
        continue;
    end
    fprintf('  ->  %-28s ', script);
    t0 = tic;
    try
        runIsolated(script);
        drawnow;
        fprintf('done (%.1f s)\n', toc(t0));
        ran{end+1} = script; %#ok<AGROW>
    catch err
        fprintf('FAILED: %s\n', err.message);
        failed{end+1} = script; %#ok<AGROW>
    end
end

% Typo guard: every toggle must name a registered viewer.
unknown = setdiff(fieldnames(active), REG(:, 1));
if ~isempty(unknown)
    warning('view_all:UnknownToggle', ...
            'active toggles matching no registered viewer: %s', ...
            strjoin(unknown, ', '));
end

fprintf('=== view_all: %d ran, %d skipped, %d failed ===\n', ...
        numel(ran), numel(skipped), numel(failed));
if ~isempty(failed)
    fprintf('    failed: %s\n', strjoin(failed, ', '));
end
fprintf('\n');

end

function tf = traceHasData(p)
% Present AND non-empty.  A zero-byte trace means the logger was enabled
% but never fired (e.g. wls_trace.csv on runs without WLS reseeds);
% readtable just errors on those, so treat them as absent.
d = dir(p);
tf = ~isempty(d) && d(1).bytes > 0;
end

function runIsolated(scriptName)
% Execute a viewer script inside this throwaway workspace so the
% `clear all` at the top of the script cannot touch the dispatch loop
% state one level up.
run(scriptName);
end
