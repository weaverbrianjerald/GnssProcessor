function [rinex] = downloadRINEX(stationList,options)
%downloadRINEX Download RINEX files
%%% Overview
%   1. Read text file containing stations names
%       a. File: rinexList.txt
%       b. Enable/disable stations using comment at the start of a line (#this is a comment)
%   2. Download RINEX files from server
%       a. Skip download if requested file is found locally
%   3. Output filepaths to each RINEX file that was found or downloaded successfully
%%% Notes
%   1. Stations are from a variety of providers: ODOT, LCSO, WSDOT, UNAVCO, NOAA


%% Initialize
% The structure that contains all rinex properties
rinex = initializeRinex( stationList, options );

%% Select host
[host_wsr] = getRinexConfig('WSRN');
[host_una] = getRinexConfig('UNAVCO');
[host_aws] = getRinexConfig('AWS');
[host_igs] = getRinexConfig('IGS');

hostList = strings(numel(stationList),1);
for i = 1:numel(stationList)
    if options.optRinex.prioritizeNoaaCors.value
        % Priority given here, to NOAA CORS Network stations
        if ismember(stationList(i), host_aws.stations )
            hostList(i) = "AWS";
        elseif ismember(stationList(i),     host_igs.stations )
            hostList(i) = "IGS";
        elseif ismember(stationList(i), host_wsr.stations )
            hostList(i) = "WSRN";
        elseif ismember(stationList(i), host_una.stations )
            hostList(i) = "UNAVCO";
        else
            fprintf('Station not found in host list: %s\n',stationList(i));
            stationList(i) = [];
        end
    else
        % Priority given here, to hopefully get RINEX v3 files
        if ismember(stationList(i),     host_igs.stations )
            hostList(i) = "IGS";
        elseif ismember(stationList(i), host_wsr.stations )
            hostList(i) = "WSRN";
        elseif ismember(stationList(i), host_una.stations )
            hostList(i) = "UNAVCO";
        elseif ismember(stationList(i), host_aws.stations )
            hostList(i) = "AWS";
        else
            fprintf('Station not found in host list: %s\n',stationList(i));
            stationList(i) = [];
        end
    end
end

if ~isempty( stationList )

    if options.optConfig.useParallel.value

        if isempty( gcp('nocreate') )
            pool = parpool('local', feature('numCores'));
        else
            pool = [];
        end

        rinex_files = rinex.files;
        %results = cell(1, numel(stationList));
        results = cell(1, numel(stationList));

        parfor i = 1:numel(stationList) % each station
            temp_files = [];
            rinex_par = rinex;
            station = stationList(i);
            t = tic;
            try
                if hostList(i) == "WSRN" || hostList(i) == "AWS"
                    if hostList(i) == "WSRN"
                        temp_files = getRinex( station, rinex_par, host_wsr, options );
                    else
                        temp_files = getRinex( station, rinex_par, host_aws, options );
                    end
                elseif hostList(i) == "UNAVCO"
                    temp_files = getRinex( station, rinex_par, host_una, options );
                elseif hostList(i) == "IGS"
                    temp_files = getRinex( station, rinex_par, host_igs, options );
                else
                    %rinex = [];
                end
                results{i} = temp_files.files(rinex.stations == station,:);
                %[ data ] = processRnx( rnxPath(i), rnxName(i), options );
                %fprintf( 'Finished: %s (%6.3g sec) [%s]\n', rnxName(i), toc(t), datetime('now') );
            catch ME
                fprintf( 'Failed:   %s (%6.3g sec) [%s]\n', station, toc(t), datetime('now') );
                results{i} = [];
                fprintf('Error message: %s\n', ME.message);
                fprintf('Error identifier: %s\n', ME.identifier);
                for k = 1:length(ME.stack)
                    fprintf('In %s at line %d\n', ME.stack(k).file, ME.stack(k).line);
                end
            end
        end

        %delete(pool); don't delete to keep parallel pool active

        % Aggregate results back into rinex_files
        for i = 1:numel(stationList)
            station = stationList(i);
            if ~isempty(results{i})
                rinex_files(rinex.stations == station, :) = results{i};
            end
        end

        % Assign the modified rinex_files back to rinex.files
        rinex.files = rinex_files;

    else % non-parallel (sequential)

        for i = 1:numel(stationList) % each station
            station = stationList(i);
            t = tic;
            try
                if hostList(i) == "WSRN" || hostList(i) == "AWS"
                    if hostList(i) == "WSRN"
                        rinex = getRinex( station, rinex, host_wsr, options );
                    else
                        rinex = getRinex( station, rinex, host_aws, options );
                    end
                elseif hostList(i) == "UNAVCO"
                    rinex = getRinex( station, rinex, host_una, options );
                elseif hostList(i) == "IGS"
                    rinex = getRinex( station, rinex, host_igs, options );
                else
                    rinex = [];
                end
                %[ data ] = processRnx( rnxPath(i), rnxName(i), options );
                %fprintf( 'Finished: %s (%6.3g sec) [%s]\n', rnxName(i), toc(t), datetime('now') );
            catch ME
                fprintf('Error message: %s\n', ME.message);
                fprintf('Error identifier: %s\n', ME.identifier);
                for k = 1:length(ME.stack)
                    fprintf('In %s at line %d\n', ME.stack(k).file, ME.stack(k).line);
                end
            end
        end

    end


    %% Formatting
    % Unzip, crx2rnx, gfz
    if options.optConfig.useParallel.value
        rinex = formatRinex_daily_parallel(rinex,options);
    else
        rinex = formatRinex_daily(rinex,options);
    end

    del = all(rinex.files == "",2);
    rinex.files(del,:) = [];
    rinex.stations(del) = [];

else
    rinex = [];
end

end



function rinex = initializeRinex( stationList,options )

% Stations, year(s) and doy(s)
rinex.stations = stationList;
rinex.year = options.optRinex.yearInterval.value(1):1:options.optRinex.yearInterval.value(2);
rinex.doy  = options.optRinex.doyInterval.value(1):1:options.optRinex.doyInterval.value(2);

% Format the DOY, as string and with leading zeros
rinex.doyStr = string( [ rinex.doy ] );

% Create indices for DOYs that require either one or two leading zeros
doubleZeroIdx = rinex.doy < 10;
singleZeroIdx = rinex.doy >= 10 & rinex.doy < 100;

% Add leading zeros to DOY
rinex.doyStr( doubleZeroIdx ) = "00" + rinex.doyStr( doubleZeroIdx );
rinex.doyStr( singleZeroIdx ) =  "0" + rinex.doyStr( singleZeroIdx );

% Files are stored in a matrix, where rows are stations and columns are doy
%rinex.files = strings( numel( rinex.doy ), numel( rinex.stations ) ); % row = doy, col = station
rinex.files = strings( numel( rinex.stations ), numel( rinex.doy ) ); % row = station, col = doy

end

