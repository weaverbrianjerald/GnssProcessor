function [rinex] = formatRinex_daily(rinex,options)
%formatRinex_daily_parallel Summary..
%   Uncompress .gz and .crx, decimate to new rate


targetFolder = options.optConfig.homeFolder.value + "rinex\";
gfz_path = options.optFiles.gfzFilePath.value;
crx_path = options.optFiles.crxFilePath.value;

%% Check for gfz executable
if ~isfile(options.optFiles.gfzFilePath.value)
    fprintf('Error. gfzrnx file must be specified to edit RINEX files\n');
    return;
end

%% Process each station
nDoy = numel( rinex.doyStr );
rinex_files = rinex.files;
%parfor stationIdx = 1:numel( rinex.stations )
for stationIdx = 1:numel( rinex.stations )

    currentUnzipFiles = "";
    currentStation = rinex.stations( stationIdx );

    % Each year
    for yearIdx = 1:numel( rinex.year )

        currentYear = rinex.year( yearIdx );
        currentYear_char = char(string(currentYear));

        % Change local directory
        currentDirectory = targetFolder + currentStation + "\" + currentYear;
        cd( currentDirectory );
        localFiles = string( ls );

        % Each day
        for dayIdx = 1:nDoy

            currentDay = rinex.doyStr( dayIdx );

            % Local rinex files that were successfully downloaded
            isV2InLocal = contains( localFiles, currentStation+currentDay, 'IgnoreCase', 1 );
            isZipV2FileMatch    = isV2InLocal &  contains( localFiles, ".gz" );
            isDotDV2FileMatch   = isV2InLocal & ~contains( localFiles, ".gz" ) & contains( localFiles, "." + string(currentYear_char(end-1:end)) + "d" );
            isDotOV2FileMatch   = isV2InLocal & ~contains( localFiles, ".gz" ) & contains( localFiles, "." + string(currentYear_char(end-1:end)) + "o", 'IgnoreCase', true );
            
            isZipV3FileMatch    = contains( localFiles, currentStation, 'IgnoreCase', 1 ) &...
                                  contains( localFiles, currentYear + currentDay, 'IgnoreCase', 1 ) &...
                                  contains( localFiles, ".crx" ) & ( contains( localFiles, ".gz" ) | contains( localFiles, ".bz2" ) );
            isDailyV3FileMatch  = contains( localFiles, currentStation, 'IgnoreCase', 1 ) & contains( localFiles, currentYear + currentDay, 'IgnoreCase', 1 ) & contains( localFiles, ".rnx" );

            if any( isDotDV2FileMatch ) && ~any( isDotOV2FileMatch ) && any( isZipV2FileMatch ) % daily not found, hourly are found

                currentZipFiles = localFiles( isZipV2FileMatch );

                % Unzip
                for zi = 1:numel( currentZipFiles )
                    unzipRinex(currentZipFiles(zi), options);
                end

                % Find format of unzipped files
                %currentUnzipFiles = string( ls(['*',char(currentStation+currentDay),'*','.','*o']) );
                currentUnzipFiles = string( ls( [ '*',char(currentStation+currentDay),'*',char("."+string(currentYear_char(end-1:end))+"d") ] ) );

                if sum( currentUnzipFiles ~= "" ) > 0

                    % GFZ
                    rinex_files( stationIdx, dayIdx ) = edit_rnxV2( currentUnzipFiles, currentDirectory, gfz_path, crx_path, options.optRinex.samplingRate.value, true );

                    % Delete intermediate files
                    localFiles_temp = string( ls );
                    isHourlyFileMatch_temp = contains( localFiles_temp, ".zip", 'IgnoreCase', 1 );
                    isDailyFileMatch_temp  = contains( localFiles_temp, currentStation+rinex.doyStr+"0", 'IgnoreCase', 1 );

                    del = ~isHourlyFileMatch_temp & ~isDailyFileMatch_temp;
                    filesToDelete = localFiles_temp(del);

                    % for zi = 1:sum(del)
                    %     if strrep( filesToDelete(zi), " ", "" ) ~= "." && strrep( filesToDelete(zi), " ", "" ) ~= ".."
                    %         delete(filesToDelete(zi));
                    %     end
                    % end

                else
                    % did not find any .yyo files
                end

            elseif ~any( isDailyV3FileMatch ) && any( isZipV3FileMatch ) % daily not found, zip is found

                currentZipFiles = localFiles( isZipV3FileMatch );
                currentUnzipFiles = removeZipExtension(currentZipFiles);

                if ~isfile(currentDirectory+"\"+currentUnzipFiles)
                    for zi = 1:numel( currentZipFiles )
                        unzipRinex(currentZipFiles(zi), options);
                    end
                end

                rinex_files( stationIdx, dayIdx ) = edit_rnxV3( currentUnzipFiles, currentDirectory, gfz_path, crx_path, options.optRinex.samplingRate.value, true );

            else % only get the output file names
                if any( isDotDV2FileMatch ) || any( isDotOV2FileMatch )
                    currentZipFiles = localFiles( isZipV2FileMatch );
                    currentUnzipFiles_d = string( ls( [ '*',char(currentStation+currentDay),'*',char("."+string(currentYear_char(end-1:end))+"d") ] ) );
                    currentUnzipFiles_o = string( ls( [ '*',char(currentStation+currentDay),'*',char("."+string(currentYear_char(end-1:end))+"o") ] ) );
                    if ~isempty( currentUnzipFiles_o )
                        currentUnzipFiles = currentUnzipFiles_o;
                    elseif ~isempty( currentUnzipFiles_d )
                        currentUnzipFiles = currentUnzipFiles_d;
                    else
                        fprintf('Unable to edit rinex file because .o or .d not found.\n');
                        currentUnzipFiles = "";
                    end
                    rinex_files( stationIdx, dayIdx ) = edit_rnxV2( currentUnzipFiles, currentDirectory, gfz_path, crx_path, options.optRinex.samplingRate.value, false );
                end
                if any( isDailyV3FileMatch )
                    currentZipFiles = localFiles( isZipV3FileMatch );
                    currentUnzipFiles = removeZipExtension(currentZipFiles);
                    rinex_files( stationIdx, dayIdx ) = edit_rnxV3( currentUnzipFiles, currentDirectory, gfz_path, crx_path, options.optRinex.samplingRate.value, false );
                end
            end

        end % day

    end % year

end % station

rinex.files = rinex_files;

end


function unzipRinex(file_zip, options)

zip_exe = options.optFiles.sevenZFilePath.value;
[status7z,cmdout7z] = system( ['"',char(zip_exe),'"',' x ',char(file_zip),' -aoa '] );

end



function fileOut = edit_rnxV2( rnx, currentDirectory, gfz_exe, crx_path, smp, tf )

% GFZ Options
% RINEX version 3.xx: -vo 3; Overwrite enabled: -f; RINEX interval: -smp 60; Remove sparse observation types: -rsot 40; Keep version: -kv
%gfzOptions = " -f" + " -smp 30" + " -kv";
%gfzOptions = " -f" + " -smp 30" + " -vo 3";
if ~isempty( smp ) && ~isnan(smp)
    %gfzOptions = " -f" + " -smp " + string(smp) + " -vo 3";
    gfzOptions = " -f" + " -smp " + string(smp) + " -kv";
else
    %gfzOptions = " -f" + " -vo 3";
    gfzOptions = " -f" + " -kv";
end

% List all files sequentially
rnx_combined = '';
for i = 1:numel(rnx)
    if i == 1
        rnx_combined = [ char( rnx(i) ) ];
    elseif i == numel(rnx)
        rnx_combined = [ rnx_combined, ' ', char(rnx(i)), ' ' ];
    else
        rnx_combined = [ rnx_combined, ' ', char(rnx(i)) ];
    end
end


% Create a new folder to store RINEX v3 files separately from original files
rnx_char = char(rnx(1));
rnx_char( find( rnx_char == '.', 1, "first" ) - 1 ) = '0';
fileOut = currentDirectory + "\" + string( rnx_char );

% Use command prompt
if tf
    try
        % Execute gfz
        [statusGFZ,cmdoutGFZ] = system( [char(gfz_exe),' ','-finp',' ',rnx_combined,' ','-fout',' ',char(fileOut),' ',char(gfzOptions)] );

        % De-compress if needed
        if contains(cmdoutGFZ,"HATANAKA")

            i = regexp(fileOut, regexptranslate('wildcard', '.**d'));
            fileOut_char = char( fileOut );
            fileOut_o = strrep(fileOut, fileOut_char(i+1:end), strrep(fileOut_char(i+1:end),'d','o') );

            i = regexp(rnx, regexptranslate('wildcard', '.**d'));
            rnx_char = char( rnx );
            rnx_o = strrep(rnx, rnx_char(i+1:end), strrep(rnx_char(i+1:end),'d','o') );

            if ~isfile( fileOut_o )

                % De-compress
                [statuscrx,cmdoutcrx] = system( [char(crx_path),' ',char(rnx)] );

                %delete(rnx);
                fileOut = fileOut_o;

                % Execute gfz
                [statusGFZ,cmdoutGFZ] = system( [char(gfz_exe),' ','-finp',' ',char(rnx_o),' ','-fout',' ',char(fileOut_o),' ',char(gfzOptions)] );

            end

        end

        fprintf( "  %s created successfully\n", fileOut );

    catch
        fprintf( "  FAILED to create %s\n", fileOut );
    end
else
    % Only get the output file name
    i = regexp(fileOut, regexptranslate('wildcard', '.**d'));
    if ~isempty(i)
        fileOut_char = char( fileOut );
        fileOut = strrep(fileOut, fileOut_char(i+1:end), strrep(fileOut_char(i+1:end),'d','o') );
    else
        % Use the original file name
    end
end

end



function fileOut = edit_rnxV3( rnxIn, currentDirectory, gfz_exe, crx_path, smp, tf )

rnx_char = char(rnxIn);
rnx_char_out = rnx_char;

% GFZ Options
% RINEX version 3.xx: -vo 3
% Overwrite enabled:  -f
% RINEX interval:     -smp 60
%gfzOptions = "-vo 3 " + "-f " + "-smp 30";
%gfzOptions = "-kv " + "-f " + "-smp 30";
%gfzOptions = " -f" + " -smp 30" + " -vo 3";
if ~isempty( smp ) && ~isnan(smp)
    %gfzOptions = " -f" + " -smp " + string(smp) + " -vo 3";
    gfzOptions = " -f" + " -smp " + string(smp) + " -kv";
    % Measurement rate
    if  contains(rnxIn,"01S")
        rnx_char_out(strfind(rnxIn,"01S"):strfind(rnxIn,"01S")+2) = [char(string(smp)), 'S'];
    elseif  contains(rnxIn,"05S")
        rnx_char_out(strfind(rnxIn,"05S"):strfind(rnxIn,"05S")+2) = [char(string(smp)), 'S'];
    elseif  contains(rnxIn,"10S")
        rnx_char_out(strfind(rnxIn,"10S"):strfind(rnxIn,"10S")+2) = [char(string(smp)), 'S'];
    elseif  contains(rnxIn,"15S")
        rnx_char_out(strfind(rnxIn,"15S"):strfind(rnxIn,"15S")+2) = [char(string(smp)), 'S'];
    else
        % rate could be 30S or 60S
    end
else
    %gfzOptions = " -f" + " -vo 3";
    gfzOptions = " -f" + " -kv";
end

% Check compression
if contains( rnx_char_out, ".crx" )
    fileOut = currentDirectory + "\" + string( strrep( rnx_char_out, ".crx", ".rnx" ) );
    isCrx = true;
else
    fileOut = currentDirectory + "\" + rnxIn;
    isCrx = false;
end


% Decompress
if isCrx && tf
    try
        [statuscrx,cmdoutcrx] = system( [char(crx_path),' ',char(currentDirectory + "\" + rnxIn)] );
    catch
        %fprintf( "  FAILED to decompress %s\n", fileOut );
    end
end

% Use gfz tool
if ~isempty( smp ) && ~isnan(smp) && tf
    try
        [statusGFZ,cmdoutGFZ] = system( [char(gfz_exe),' ','-finp',' ',char(currentDirectory + "\" + strrep( rnxIn, ".crx", ".rnx" )),' ','-fout',' ',char(fileOut),' ',char(gfzOptions)] );
        %fprintf( "  %s created successfully\n", fileOut );
        % %delete(currentDirectory + "\" + strrep( rnxIn, ".crx", ".rnx" ));
        % if isCrx
        %     delete(currentDirectory + "\" + rnxIn);
        % end
    catch
        %fprintf( "  FAILED to create %s\n", fileOut );
    end
end

end


function fileOut = removeZipExtension(fileIn)

fileOut = fileIn;

zipFormats = [ ".gz"; ".bz2"; ".zip" ];

for i = 1:numel( zipFormats )
    if contains( fileIn, zipFormats(i) )
        fileOut = strrep( fileIn, zipFormats(i), "" );
    end
end

end