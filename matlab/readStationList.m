function [rtn_stations] = readStationList(options)
%readControlFile Read files containing lists of stations to process
%   Individual options use the following format:
%       key, variable name, type, value
%       # is the comment flag

%global options;

%% Files to read
if isfile(options.optRinex.stationListFilePath.value)
    rtn_list_file  = options.optRinex.stationListFilePath.value;
else
    rtn_list_file = [];
end

%% Configurations
commentStyle = '#';
%delimStyle = "=";
%validTypes = ["string", "bool", "double", "int", "vector"];

%% Read stations
if ~isempty( rtn_list_file )
    
    % Open the file
    fileID = fopen( rtn_list_file );
    numRTN = 0;
    rtn_stations = "";
    
    while ~feof( fileID )
        textLine = fgetl( fileID );
            if ( ~isempty( textLine ) ) && ( textLine( 1 ) ~= commentStyle )
                numRTN = numRTN + 1;
                rtn_stations( numRTN ) = textLine;
                % TODO: separate line contents using delimiter to store published coordinates
            end
    end
    fclose( fileID );
else
    rtn_stations = [];
end

end

