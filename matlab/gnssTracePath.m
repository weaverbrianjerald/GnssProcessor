function p = gnssTracePath(csvName)
%% ============================================================
%  Function: gnssTracePath
%  Purpose: Full path of a trace CSV inside the active log folder.  The
%           single path-resolution point for every viewer script — no
%           more per-script hardcoded fname blocks.
%
%  Usage:
%     p = gnssTracePath('mwwl_trace.csv')
%
%  The folder comes from gnssVizLogDir(): set it once per session
%  (directly or via view_all.m); when unset it falls back to
%  C:\GnssProcessor\logs, then the Release / Debug build log dirs.
%
%  See also: gnssVizLogDir, view_all.
%  ============================================================

p = fullfile(gnssVizLogDir(), char(csvName));

end
