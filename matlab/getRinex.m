function rinex = getRinex( station, rinex, host, options )
%getRinex_unavco Download RINEX files from the UNAVCO server
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

% Check authentication
authentication = checkAuthentication(options);

%% Prepare directories
targetFolder = options.optConfig.homeFolder.value + "rinex\";
prepareTargetFolder(targetFolder);
createTargetDirectories(targetFolder,station,rinex.year);

%% Static variables
% Required for parallel processing
rinex_year     = rinex.year;
rinex_doyStr   = rinex.doyStr';
nDoy           = numel( rinex_doyStr );
rinex_files    = rinex.files(rinex.stations == station,:);
%rinex_files    = rinex.files(:,rinex.stations == station);
%rinex_files = strings( numel( rinex.stations ), numel( rinex.doy ) );

%% Download
tStart = tic;

currentLocalTime = strrep( string( datetime( 'now' ) ), " ", "_" );
currentLocalTime = strrep( currentLocalTime, ":", "-" );
logFileID = fopen( char( string( targetFolder ) + station + "\" + station + "_log_" + currentLocalTime + ".txt" ), 'w' );

% Each year
for yearIdx = 1:numel( rinex_year )

    currentYear = rinex_year( yearIdx );

    % Change local directory
    currentLocalFolder = targetFolder + station + "\" + currentYear;

    cd( currentLocalFolder );

    %localFiles = ls('*.gz');
    localFiles = string(ls);

    % Each day
    for dayIdx = 1:nDoy

        currentDay = rinex_doyStr( dayIdx );

        [hst,rnx] = getHostPath( authentication, host.domain, host.folder, host.subFolder, currentYear, currentDay, station );

        [~,pRinex] = fileparts( rinex_files( dayIdx ) );

        if ~any( contains( localFiles, rnx, 'IgnoreCase', true ) ) &&...
                ~any( contains( pRinex, station, 'IgnoreCase', true ) & contains( pRinex, string( currentYear ) + currentDay ) )

            if contains( host.domain, "gage-data.earthscope" ) || contains( host.domain, "cddis.nasa" )

                if contains( host.domain, "gage-data.earthscope" )
                    curlCommand = char( authentication.curl + " --fail -m 30.0 -c C:\temp\curlCookies.txt" + " --netrc-file " + authentication.netrc +...
                        " -L -O " + hst + " --header " + '"' + "Authorization: Bearer " + authentication.token.access_token + '"' );
                elseif contains( host.domain, "cddis.nasa" )
                    curlCommand = char( authentication.curl + " --fail -m 30.0 -c C:\temp\curlCookies.txt" + ...
                        " -n -L -O " + '"' + "https://" + hst + '"' );
                else
                    fprintf('Error. host.domain type is not implemented using curl.\n');
                end

                [status,cmdOut] = system( [ curlCommand ] );

            elseif contains( host.domain, "noaa-cors" ) || contains( host.domain, "geodesy.org" )
                if urlexist2(hst)
                    try
                        websave( currentLocalFolder + "\" + rnx, hst );
                        status = 0;
                    catch
                        status = -999;
                    end
                else
                    status = -999;
                end
            else
                fprintf('Error. The input host.domain type is not implemented using websave.\n');
            end

            if status ~= 0
                %fprintf( '%s NOT found\n', rnx );
            else

                fprintf(logFileID,'Downloaded rinex file: %s\n',rnx);
                disp("Downloaded rinex file: " + rnx);
                pause( 0.1 ); % server courtesy

                try % to unzip
                    unzip_RINEX( currentLocalFolder + "\" + rnx, options.optFiles.sevenZFilePath.value );
                    fileUnzip = removeZipExtension(rnx);
                    rinex_files( dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                catch
                    fprintf(logFileID,'Failed to unzip rinex file: %s\n',rnx);
                end

            end

        else % Zip file exists locally, don't download it again.

            fprintf(logFileID,'Skip download, file found in local folder: %s\n',rnx);

            fileUnzip = removeZipExtension(rnx);

            [~,~,fExt] = fileparts(localFiles);

            isRnxExisting = contains( localFiles, station, 'IgnoreCase', 1 ) &...
                contains( localFiles, currentYear + currentDay, 'IgnoreCase', 1 ) &...
                contains( fExt, ".rnx" );
            isCrxExisting = contains( localFiles, station, 'IgnoreCase', 1 ) &...
                contains( localFiles, currentYear + currentDay, 'IgnoreCase', 1 ) &...
                contains( fExt, ".crx" );
            isDExisting = contains( localFiles, station, 'IgnoreCase', 1 ) &...
                contains( localFiles, currentDay, 'IgnoreCase', 1 ) &...
                contains( fExt, "." ) & contains( fExt, "d" );
            currentYear_char = char(string(currentYear));
            if numel(currentYear_char) == 4
                yy = currentYear_char(3:4); % 2022 --> 22
            elseif numel(currentYear_char) == 2
                yy = currentYear_char(1:2); % 22 --> 22
            else
                fprintf('Problem determining 2-char year for RINEX file.\n');
                yy = currentYear_char(end-1:end); % 2022 --> 22
            end
            % isD2OExisting = contains( localFiles, station, 'IgnoreCase', 1 ) &...
            %     contains( localFiles, currentDay+"0", 'IgnoreCase', 1 ) &...
            %     contains( fExt, "."+yy ) & contains( fExt, "."+yy+'o','IgnoreCase', true );
            isD2OExisting = contains( localFiles, station + currentDay + "0", 'IgnoreCase', 1 ) &...
                contains( fExt, "."+yy ) & contains( fExt, "."+yy+'o','IgnoreCase', true );

            if ~any( isRnxExisting ) && ~any( isCrxExisting ) && ~any( isDExisting ) && ~any( isD2OExisting )

                % zip file exists, unzip does not exist
                try % to unzip
                    unzip_RINEX( currentLocalFolder + "\" + rnx, options.optFiles.sevenZFilePath.value );
                    fileUnzip = removeZipExtension(rnx);
                    rinex_files( dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                catch ME
                    fprintf(logFileID,'Failed to unzip rinex file: %s\n',rnx);
                end

            else % zip and unzip exist
                fprintf(logFileID,'Skip unzip, file found in local folder: %s\n',fileUnzip);
                if rinex_files( dayIdx ) == ""
                    if any( isD2OExisting )
                        rinex_files( dayIdx ) = currentLocalFolder + "\" + strrep( localFiles( isD2OExisting ), ' ', '' );
                    else
                        rinex_files( dayIdx ) = currentLocalFolder + "\" + fileUnzip;
                    end
                end
            end

        end % check if requested rinex file exists locally

    end % day

end % year

fprintf(logFileID,'getRinex complete in %0.3f seconds\n', toc( tStart ));
fprintf(          '%s: getRinex complete in %0.3f seconds\n', station, toc( tStart ));
fclose( logFileID );

%rinex.stations = rinex_stations;
rinex.year    = rinex_year;
%rinex.files(:,rinex.stations == station)   = rinex_files;
rinex.files(rinex.stations == station,:)   = rinex_files;

%fclose( logFileID_all );

end


function prepareTargetFolder(target_folder)
if ~isfolder( target_folder )
    %fprintf('Create target directory: %s\n', target_folder);
    mkdir( target_folder );
    if ~isfolder( target_folder )
        %fprintf('Unable to create target directory: %s\n', target_folder);
        %fprintf('Likely a permission issue, try to store files in a different folder\n');
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
                        %fprintf( 'Create station folder: %s\n', rinex_stations( staIdx ) );
                        mkdir( currentFolderPath( fi ) );
                    catch
                        %fprintf('Unable to create directory: %s\n', currentFolderPath( fi ));
                        %fprintf('Likely a permission issue, try to store files in a different folder\n');
                        return;
                    end
                end
            end
        end
    end
end
end


function tf = urlexist(url,auth)

% Command to check if URL exists using curl HEAD request
command = [auth.curl,...
    ' --head --silent --fail -m 30.0',...
    ' -c C:\temp\curlCookies.txt',...
    ' --netrc-file ', auth.netrc,...
    ' -L ' , char(url),...
    ' --header ', '"Authorization: Bearer ', auth.token.access_token, '"' ];

% Execute the command
[status, ~] = system(command);

% If status is 0, it means the URL exists
tf = (status == 0);

end


function tf = urlexist2(url)

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


function [hst,rnx] = getHostPath( auth, host_domain, host_folder, host_subFolder, cYear, cDay, cStation )

hst = [];
rnx = [];

cYear_char = char(string(cYear));
rateList = [ "_01S_" "_05S_" "_15S_" "_30S_" "_60S_" "_300S_" ];

if contains( host_domain, "noaa-cors" )
    rnx = lower( cStation ) + cDay + "0." + cYear_char(end-1:end) + "d" + ".gz";
    hst = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + "/" + lower( cStation ) + "/" + rnx;
elseif contains( host_domain, "geodesy.org" ) || contains( host_domain, "gage-data.earthscope" ) || contains( host_domain, "cddis.nasa" )
    if contains( host_domain, "geodesy.org" )
        ext = ".crx.bz2";
    else
        ext = ".crx.gz";
    end
    for ri = 1:numel(rateList)
        if contains( host_domain, "geodesy.org" )
            cStation_char = char(cStation);
            if numel(cStation_char) == 4
                countryList = [ "00USA" "00CAN" ];
                found = false;
                for ci = 1:numel(countryList)
                    rnx = upper( cStation ) + countryList(ci) + "_R_" + string(cYear_char) + cDay + "0000_01D" + rateList(ri) + "MO" + ext;
                    hst = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + host_subFolder + "/" + rnx;
                    if urlexist(hst,auth)
                        found = true;
                        break;
                    end
                end
                if found, break, end
            else
                rnx = upper( cStation ) + "_R_" + string(cYear_char) + cDay + "0000_01D" + rateList(ri) + "MO" + ext;
                hst = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + host_subFolder + "/" + rnx;
                if urlexist(hst,auth)
                    break;
                end
            end
        else
            rnx = upper( cStation ) + "_R_" + string(cYear_char) + cDay + "0000_01D" + rateList(ri) + "MO" + ext;
            hst = string( host_domain ) + string( host_folder ) + string( cYear ) + "/" + cDay + "/" + cYear_char(end-1:end) + "d" + "/" + rnx;
            if urlexist(hst,auth)
                break;
            end
        end
    end
else
    %fprintf('Error. Unknown RINEX path format on host server.\n');
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


function authentication = checkAuthentication(options)

% Required to access UNAVCO (GAGE) Archive
% Details:
%   https://www.unavco.org/data/gps-gnss/file-server/user-profile.html
%   https://www.unavco.org/data/gps-gnss/file-server/file-server-access-examples.html
%   https://gitlab.com/earthscope/public/earthscope-cli

canAuthenticate = true;

%% Options
authentication.netrc = char( options.optFiles.netrcUnavcoFilePath.value );
authentication.curl  = char( options.optFiles.curlFilePath.value );
authentication.tokenPath = char( options.optFiles.earthScopeSsoTokenPath.value );

%% Check .netrc
% Don't need netrc
if isempty( authentication.netrc ) || ~isfile( authentication.netrc )
    if isempty( authentication.netrc )
        fprintf( "Unable to download UNAVCO RINEX data. Authentication to UNAVCO requires .netrc file\n" );
    end
    if ~isfile( authentication.netrc )
        fprintf( "Unable to download UNAVCO RINEX data. Input .netrc file not found: %s\n", authentication.netrc );
    end
    fprintf( "Configure .netrc: %s\n", "https://www.unavco.org/data/gps-gnss/file-server/user-profile.html");
    canAuthenticate = false;
end

%% Check curl.exe
if isempty( authentication.curl ) || ~isfile( authentication.curl )
    if isempty( authentication.curl )
        fprintf( "Unable to download UNAVCO RINEX data. Authentication to UNAVCO requires curl.exe\n" );
    end
    if ~isfile( authentication.curl )
        fprintf( "Unable to download UNAVCO RINEX data. Input curl.exe file not found: %s\n", authentication.curl );
    end
    fprintf( "Configure curl: %s\n", "https://www.unavco.org/data/gps-gnss/file-server/file-server-access-examples.html");
    canAuthenticate = false;
end

%% Check sso_tokens.json
if isempty( authentication.tokenPath ) || ~isfile( authentication.tokenPath )
    if isempty( authentication.tokenPath )
        fprintf( "Unable to download UNAVCO RINEX data. Authentication to UNAVCO requires token in sso_tokens.json\n" );
    end
    if ~isfile( authentication.tokenPath )
        fprintf( "Unable to download UNAVCO RINEX data. Input sso_tokens.json file not found: %s\n", authentication.tokenPath );
    end
    authentication.token = '';
    fprintf( "Configure token: %s\n", "https://gitlab.com/earthscope/public/earthscope-cli");
    canAuthenticate = false;
else
    % Read the token from the file
    fid = fopen(authentication.tokenPath,'r');
    raw = fread(fid,inf);
    str = char(raw');
    fclose(fid);
    authentication.token = jsondecode(str); % token.access_token works to download
end

%% Error
if ~canAuthenticate
    %error('Unable to authenticate UNAVCO RINEX data download');
    fprintf('ERROR. Unable to authenticate UNAVCO RINEX data download\n');
end

end
