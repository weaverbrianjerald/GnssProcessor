function unzip_RINEX( fid, sevenZPath )
%unzip_RINEX Summary
%   Unzip RINEX data files

[dir7z,~,~] = fileparts( fid );
dir7z = ['"' char(dir7z) '"'];
fid = ['"' char(fid) '"'];

[status7z,cmdout7z] = system( char(["""" + sevenZPath + """" + " x -o" + dir7z + " " + fid]) );

fprintf('unzipped file %s \n', fid );

end

