function out = gnssVizLogDir(newDir)
%% ============================================================
%  Function: gnssVizLogDir
%  Purpose: Get or set the trace folder shared by every viewer script in
%           this suite.  Replaces the per-script hardcoded fname blocks:
%           set the folder ONCE per session (directly, or via view_all.m)
%           and every viewer resolves its CSV through gnssTracePath.
%
%  Usage:
%     d = gnssVizLogDir()      current folder.  If none has been set this
%                              session, falls back to the first EXISTING
%                              default: C:\GnssProcessor\logs, then the
%                              Release / Debug build log dirs.
%     gnssVizLogDir(newDir)    set the folder for the whole session.
%     gnssVizLogDir('')        clear the override (back to the defaults).
%
%  The value is stored as appdata on groot, NOT as a variable or a
%  persistent — so it survives the `clear all` at the top of the viewer
%  scripts.  It is sticky for the MATLAB session: after a view_all run,
%  any viewer run standalone reads from the same folder.
%
%  See also: gnssTracePath, view_all.
%  ============================================================

KEY = 'GnssVizLogDir';

if nargin == 1
    if isempty(newDir)
        if isappdata(groot, KEY), rmappdata(groot, KEY); end
    else
        newDir = char(newDir);
        if ~isfolder(newDir)
            warning('gnssVizLogDir:NoSuchFolder', ...
                    'Trace folder does not exist (yet): %s', newDir);
        end
        setappdata(groot, KEY, newDir);
    end
end

if nargout > 0 || nargin == 0
    if isappdata(groot, KEY)
        out = getappdata(groot, KEY);
    else
        defaults = {'C:\GnssProcessor\logs', ...
                    'C:\GnssProcessor\build\x64-Release\logs', ...
                    'C:\GnssProcessor\build\x64-Debug\logs'};
        hit = defaults(cellfun(@isfolder, defaults));
        if isempty(hit)
            out = defaults{1};
        else
            out = hit{1};
        end
    end
end

end
