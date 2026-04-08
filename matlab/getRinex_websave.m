function rinex = getRinex_websave( station, rinex, host, options )
%getRinex_websave Download RINEX files from an https server
% Inputs:
%  - target
%      .folder: The target (local) folder where RINEX files are stored
%               after downloading from the ftp server.
%  - rinex
%      .stations: Desired station(s)  [STAY, WDBN]
%      .year:     Desired year(s)     [2020, 2021]
%      .doy:      Desired day of year [1:1:365]
%  - host
%      .domain: The host domain/address
%                   https://noaa-cors-pds.s3.amazonaws.com/
%      .folder: The host folder than contains sub-folders with
%               RINEX files inside.
%                   ./rinex/2022/035/stay/stay0350.22o.gz
% Notes:
%  - Download status stored in log files:
%       STAY_log_19-Sep-2022_17-22-39.txt
%       STAY_log_19-Sep-2022_17-34-02.txt
%  - Created for connection to Amazon Web Service (AWS) S3 server.

%global options;

%% Initial checks
% Does requested station exist on the server?
if ~any( contains( host.stations, rinex.stations ) )
    return;
end

%% Prepare directories
targetFolder = options.optConfig.homeFolder.value + "rinex\";
prepareTargetFolder(targetFolder);
createTargetDirectories(targetFolder,rinex.stations,rinex.year);

%% Static variables
% Required for parallel processing
rinex_stations = rinex.stations;
rinex_year     = rinex.year;
rinex_doyStr   = rinex.doyStr';
nDoy           = numel( rinex_doyStr );
rinex_files    = rinex.files(rinex.stations == station,:);
%rinex_files = strings( numel( rinex.stations ), numel( rinex.doy ) );

%% Download
tStart = tic;

% Non-Parallel the stations
for stationIdx = 1:numel( rinex_stations )
    %     parfor stationIdx = 1:numel( rinex_stations )

    currentStation = rinex_stations( stationIdx );
    currentLocalTime = strrep( string( datetime( 'now' ) ), " ", "_" );
    currentLocalTime = strrep( currentLocalTime, ":", "-" );
    logFileID = fopen( char( string( targetFolder ) + currentStation + "\" + currentStation + "_log_" + currentLocalTime + ".txt" ), 'w' );

    % Each year
    for yearIdx = 1:numel( rinex_year )

        currentYear = rinex_year( yearIdx );

        % Change local directory
        currentLocalFolder = targetFolder + currentStation + "\" + currentYear;

        cd( currentLocalFolder );

        %localFiles = ls('*.gz');
        localFiles = ls;

        % Each day
        for dayIdx = 1:nDoy

            currentDay = rinex_doyStr( dayIdx );

            % The folder that contains the rinex files uses format: yyddd
            currentYear_char = char( string( currentYear ) );

            requestedRnxList = getCurrentRinexFile( host.domain, currentYear_char, currentDay, currentStation );

            for rnxIdx = 1:numel(requestedRnxList)

                requestedRnx = requestedRnxList(rnxIdx);

                currentHostPath = getCurrentHostPath( host.domain, host.folder, host.subFolder, currentYear, currentDay, currentStation, requestedRnx );

                [~,pRinex] = fileparts( rinex_files( stationIdx, dayIdx ) );

                if ~any( contains( string( localFiles ), requestedRnx, 'IgnoreCase', true ) ) &&...
                        ~any( contains( pRinex, currentStation, 'IgnoreCase', true ) & contains( pRinex, string( currentYear ) + currentDay ) )

                    remoteFileIsFound = urlexist(currentHostPath);

                    if remoteFileIsFound

                        % Download the file from the host server.
                        websave( currentLocalFolder + "\" + requestedRnx, currentHostPath );
                        fprintf(logFileID,'Downloaded rinex file: %s\n',requestedRnx);
                        disp("Downloaded rinex file: " + requestedRnx);
                        pause( 0.1 ); % server courtesy

                        try % to unzip
                            unzip_RINEX( currentLocalFolder + "\" + requestedRnx );
                            fileUnzip = removeZipExtension(requestedRnx);
                            rinex_files( stationIdx, dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                        catch
                            fprintf(logFileID,'Failed to unzip rinex file: %s\n',requestedRnx);
                        end

                        %break; % not great.. needed to download .yyo file and skip .yyd file, if both exist

                    end

                else % Zip file exists locally, don't download it again.

                    fprintf(logFileID,'Skip download, file found in local folder: %s\n',requestedRnx);

                    fileUnzip = removeZipExtension(requestedRnx);

                    isDailyV3FileMatch  = contains( string(localFiles), currentStation, 'IgnoreCase', 1 ) & contains( string(localFiles), currentYear + currentDay, 'IgnoreCase', 1 ) & contains( string(localFiles), ".rnx" );

                    if ~any( isDailyV3FileMatch ) && ~any( strcmp( localFiles, fileUnzip ) ) &&...
                            ~any( contains( pRinex, currentStation, 'IgnoreCase', true ) & contains( pRinex, string( currentYear ) + currentDay ) )

                        % zip file exists, unzip does not exist
                        try % to unzip
                            unzip_RINEX( currentLocalFolder + "\" + requestedRnx );
                            fileUnzip = removeZipExtension(requestedRnx);
                            rinex_files( stationIdx, dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                        catch
                            fprintf(logFileID,'Failed to unzip rinex file: %s\n',requestedRnx);
                        end

                    else % zip and unzip exist
                        fprintf(logFileID,'Skip unzip, file found in local folder: %s\n',fileUnzip);
                        if rinex_files( stationIdx, dayIdx ) == ""
                            rinex_files( stationIdx, dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                        end
                    end

                end % check if requested rinex file exists locally

            end

        end % day

    end % year

    fprintf(logFileID,'getRinex complete in %0.3f seconds\n', toc( tStart ));
    fprintf(          '%s: getRinex complete in %0.3f seconds\n', currentStation, toc( tStart ));
    fclose( logFileID );

end % station

rinex.stations = rinex_stations;
rinex.year    = rinex_year;
rinex.files(rinex.stations == station,:)   = rinex_files;

%fclose( logFileID_all );

end


function prepareTargetFolder(target_folder)
if ~isfolder( target_folder )
    fprintf('Create target directory: %s\n', target_folder);
    mkdir( target_folder );
    if ~isfolder( target_folder )
        fprintf('Unable to create target directory: %s\n', target_folder);
        fprintf('Likely a permission issue, try to store files in a different folder\n');
        return;
    end
end
end


function createTargetDirectories(target_folder,rinex_stations,rinex_year)
if any( ~isfolder(  target_folder + rinex_stations + "\" + rinex_year ) )
    for staIdx = 1:numel( rinex_stations )
        currentFolderPath =  target_folder + rinex_stations( staIdx ) + "\" + rinex_year;
        if any( ~isfolder( currentFolderPath ) )
            for fi = 1:numel( currentFolderPath )
                if ~isfolder( currentFolderPath( fi ) )
                    try
                        fprintf( 'Create station folder: %s\n', rinex_stations( staIdx ) );
                        mkdir( currentFolderPath( fi ) );
                    catch
                        fprintf('Unable to create directory: %s\n', currentFolderPath( fi ));
                        fprintf('Likely a permission issue, try to store files in a different folder\n');
                        return;
                    end
                end
            end
        end
    end
end
end


function tf = urlexist(url)

    URL = java.net.URL(url); %create the URL object

    % Get the proxy information using the MATLAB proxy API.
    proxy = com.mathworks.webproxy.WebproxyFactory.findProxyForURL(URL); 

    % Open a connection to the URL.
    if isempty(proxy)
        urlConnection = URL.openConnection;
    else
        urlConnection = URL.openConnection(proxy);
    end

    try % to start the input stream
        inputStream = urlConnection.getInputStream;
        tf = true;
    catch % file not found
        tf = false;
    end

end


function hostPath = getCurrentHostPath( host_domain, host_folder, host_subFolder, cYear, cDay, cStation, rnx )

if contains( host_domain, "noaa-cors" )
    hostPath = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + "/" + lower( cStation ) + "/" + rnx;
elseif contains( host_domain, "geodesy.cwu" )
    hostPath = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + string( host_subFolder ) + rnx;
elseif contains( host_domain, "unavco" )
    hostPath = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + "/" + rnx;
else
    % Not yet implemented
end

end


function [rnxFile] = getCurrentRinexFile( host_domain, cYear_char, cDay, cStation )

if contains( host_domain, "noaa-cors" )

    % RINEX V2: stay0010.22o.gz (uncompressed) or stay0010.22d.gz (compressed)
    %rnxFile = lower( cStation ) + cDay + "0." + cYear_char(end-1:end) + "o" + ".gz";
    rnxFile = lower( cStation ) + cDay + "0." + cYear_char(end-1:end) + "d" + ".gz";

elseif contains( host_domain, "geodesy.cwu" )

    % RINEX V3: ANAT00USA_R_20230010000_01D_01S_MO.crx.bz2
    % RINEX V3: ANAT00USA_R_20230010000_01D_05S_MO.crx.bz2
    rnxFile(1,1) = upper( cStation ) + "00USA_R_" + string(cYear_char) + cDay + "0000_01D_01S_MO.crx.bz2";
    rnxFile(2,1) = upper( cStation ) + "00USA_R_" + string(cYear_char) + cDay + "0000_01D_05S_MO.crx.bz2";
    rnxFile(3,1) = upper( cStation ) + "00USA_R_" + string(cYear_char) + cDay + "0000_01D_15S_MO.crx.bz2";
    rnxFile(4,1) = upper( cStation ) + "00USA_R_" + string(cYear_char) + cDay + "0000_01D_30S_MO.crx.bz2";

elseif contains( host_domain, "unavco" )

    % RINEX V3: P01800USA_R_20210030000_01D_15S_MO.crx.gz
    rnxFile = upper( cStation ) + "00USA_R_" + string(cYear_char) + cDay + "0000_01D_15S_MO.crx.gz";

else
    % Unknown rinex version
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
