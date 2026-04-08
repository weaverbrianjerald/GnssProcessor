function inf = downloadMGEX_withLegacy( inf, options )
%downloadMGEX Download MGEX products from server
% Example (old): https://cddis.nasa.gov/archive/gnss/products/mgex/2182/
% Example (new): https://cddis.nasa.gov/archive/gnss/products/2210/
% May need to run windows command to get new token:
% es sso access --token
%productHomeFolder = "C:\temp\gnss_products\"; % TODO: option
productHomeFolder = options.optConfig.satelliteProductFolder.value;
dti = datetime( inf.time.first' );
dtiFmt = [ dti.Year', dti.Month', dti.Day', dti.Hour', dti.Minute', dti.Second' ];
dtiDoy = date2doy( datenum( dtiFmt ) ); % only need 1 day for clk
dtiDoyStr = string(dtiDoy);
dtiDoyStr( dtiDoy < 100 & dtiDoy >= 10 ) = "0" + dtiDoyStr( dtiDoy < 100 & dtiDoy >= 10 );
dtiDoyStr( dtiDoy < 10 ) = "00" + dtiDoyStr( dtiDoy < 10 );
dti1 = dti - days(1); % previous day
dti1 = dateshift( dti1, "start", "day" );
dtf = datetime( inf.time.last' );
dtf1 = dtf + days(1); % following day
dtf1 = dateshift( dtf1, "start", "day" );
dtList = dti1:days(1):dtf1;
dtListFmt = [ dtList.Year', dtList.Month', dtList.Day', dtList.Hour', dtList.Minute', dtList.Second' ];
[gpsWeek,~,~] = date2gps( dtListFmt );
gpsWeek_unique = unique(gpsWeek,'rows','stable');
doyList = date2doy( datenum( dtListFmt ) );
doyListStr = string(doyList);
doyListStr( doyList < 100 & doyList >= 10 ) = "0" + doyListStr( doyList < 100 & doyList >= 10 );
doyListStr( doyList < 10 ) = "00" + doyListStr( doyList < 10 );
% Server format
yyyydoy = string( dtList.Year' ) + doyListStr;
yyyydoy_rnx = string( dti.Year' ) + dtiDoyStr;
if options.optConfig.useSatelliteOrbitData.value
    inf.orb_zip = strings( 1, numel( yyyydoy ) );
    inf.orb = strings( 1, numel( yyyydoy ) );
end
if options.optConfig.useSatelliteClockData.value
    inf.clk_zip = strings( 1, numel( yyyydoy_rnx ) );
    inf.clk = strings( 1, numel( yyyydoy_rnx ) );
end
if options.optConfig.useSatelliteDcbData.value
    inf.dcb_zip = strings( 1, numel( yyyydoy_rnx ) );
    inf.dcb = strings( 1, numel( yyyydoy_rnx ) );
end
if options.optConfig.useEarthOrientationData.value
    inf.erp_zip = strings( 1, numel( yyyydoy_rnx ) );
    inf.erp = strings( 1, numel( yyyydoy_rnx ) );
end
if options.optConfig.useBroadcastEphemeris.value
    inf.brdc_zip = strings( 1, numel( yyyydoy_rnx ) );
    inf.brdc = strings( 1, numel( yyyydoy_rnx ) );
end
prodTypes = ["orbit", "clock", "dcb", "erp", "brdc" ];
prodFormat = ["zip", "unzip"];
for prodTypeIdx = 1:length(prodTypes)
    for prodFmtIdx = 1:length(prodFormat)
        currentFolder = productHomeFolder + prodTypes( prodTypeIdx ) + "\" + prodFormat( prodFmtIdx );
        if ~isfolder( currentFolder )
            mkdir( currentFolder );
        end
    end
end
%% Download orbit
n = 0;
for y = 1:numel( gpsWeek_unique )
    for d = 1:numel( yyyydoy )
        gps_week = gpsWeek_unique(y);
        % Download zip files
        [orb_zip,~,~,~,~,isNew,isExisting] = getMGEX( productHomeFolder, gps_week, yyyydoy(d), options, "orb" );
        if isNew && ~isExisting
            n = n + 1; % found file on server and downloaded locally
            % Store zip file path
            inf.orb_zip( n ) = orb_zip;
            % Unzip
            [orb_unzip] = unzipMGEX( productHomeFolder, orb_zip, "orb", options );
            % Store unzip file path
            inf.orb( n ) = orb_unzip;
        elseif isExisting
            orb_unzip = strrep( orb_zip, ".gz", "" );
            orb_unzip = strrep( orb_unzip, "\zip\", "\unzip\" );
            if ~contains( inf.orb, orb_unzip )
                n = n + 1;
                inf.orb( n ) = orb_unzip;
            end
        end
    end
end
%% Download clock
n = 0;
for y = 1:numel( gpsWeek_unique )
    gps_week = gpsWeek_unique(y);
    % Download zip files
    [~,clk_zip,~,~,~,isNew,isExisting] = getMGEX( productHomeFolder, gps_week, yyyydoy_rnx, options, "clk" );
    if isNew && ~isExisting
        n = n + 1; % found file on server and downloaded locally
        % Store zip file path
        inf.clk_zip( n ) = clk_zip;
        % Unzip
        if contains(clk_zip,".clk_30s",'IgnoreCase',true)
            [clk_unzip] = unzipMGEX( productHomeFolder, clk_zip, "clk_30s", options );
        else
            [clk_unzip] = unzipMGEX( productHomeFolder, clk_zip, "clk", options );
        end
        % Store unzip file path
        inf.clk( n ) = clk_unzip;
    elseif isExisting
        if contains(clk_zip,"_30s.Z",'IgnoreCase',true)
            clk_unzip = strrep( clk_zip, "_30s.Z", "_30s" );
        else
            clk_unzip = strrep( clk_zip, ".gz", "" );
        end
        clk_unzip = strrep( clk_unzip, "\zip\", "\unzip\" );
        if ~contains( inf.clk, clk_unzip )
            n = n + 1;
            inf.clk( n ) = clk_unzip;
        end
    end
end
%% Download DCB
n = 0;
for y = 1:numel( gpsWeek_unique )
    gps_week = gpsWeek_unique(y);
    % Download zip files
    [~,~,dcb_zip,~,~,isNew,isExisting] = getMGEX( productHomeFolder, gps_week, yyyydoy_rnx, options, "dcb" );
    if isNew && ~isExisting
        n = n + 1; % found file on server and downloaded locally
        % Store zip file path
        inf.dcb_zip( n ) = dcb_zip;
        % Unzip
        [dcb_unzip] = unzipMGEX( productHomeFolder, dcb_zip, "dcb", options );
        % Store unzip file path
        inf.dcb( n ) = dcb_unzip;
    elseif isExisting
        dcb_unzip = strrep( dcb_zip, ".gz", "" );
        dcb_unzip = strrep( dcb_unzip, "\zip\", "\unzip\" );
        if ~contains( inf.dcb, dcb_unzip )
            n = n + 1;
            inf.dcb( n ) = dcb_unzip;
        end
    end
end
%% Download ERP
n = 0;
for y = 1:numel( gpsWeek_unique )
    gps_week = gpsWeek_unique(y);
    % Download zip files
    [~,~,~,erp_zip,~,isNew,isExisting] = getMGEX( productHomeFolder, gps_week, yyyydoy_rnx, options, "erp" );
    if isNew && ~isExisting
        n = n + 1; % found file on server and downloaded locally
        % Store zip file path
        inf.erp_zip( n ) = erp_zip;
        % Unzip
        [erp_unzip] = unzipMGEX( productHomeFolder, erp_zip, "erp", options );
        % Store unzip file path
        inf.erp( n ) = erp_unzip;
    elseif isExisting
        erp_unzip = strrep( erp_zip, ".gz", "" );
        erp_unzip = strrep( erp_unzip, "\zip\", "\unzip\" );
        if ~contains( inf.erp, erp_unzip )
            n = n + 1;
            inf.erp( n ) = erp_unzip;
        end
    end
end
%% Download broadcast ephemeris
n = 0;
yyyydoy_rnx_char = char(yyyydoy_rnx); % terrible, but we need the RINEX file's year
% year = string(dtList.Year(d));
% doy = doyListStr(d);
%[~, ~, ~, ~, brdc_zip, isNew, isExisting] = getMGEX(productHomeFolder, year, yyyydoy(d), options, "brdc");
% Use yyyydoy_rnx, as only one file per day is needed
[~, ~, ~, ~, brdc_zip, isNew, isExisting] = getMGEX(productHomeFolder, str2double(yyyydoy_rnx_char(1:4)), yyyydoy_rnx, options, "brdc");
if isNew && ~isExisting
    n = n + 1;
    inf.brdc_zip(n) = brdc_zip;
    [brdc_unzip] = unzipMGEX(productHomeFolder, brdc_zip, "brdc", options);
    inf.brdc(n) = brdc_unzip;
elseif isExisting
    brdc_unzip = strrep(brdc_zip, ".gz", "");
    brdc_unzip = strrep(brdc_unzip, "\zip\", "\unzip\");
    if ~contains(inf.brdc, brdc_unzip)
        n = n + 1;
        inf.brdc(n) = brdc_unzip;
    end
end
end
%% Prepare to get MGEX products
function [orb,clk,dcb,erp,brdc,isDownloaded,isExisting] = getMGEX( productHomeFolder, gps_week, yyyydoy, options, tag )
%% Check curl requirements
% Needed to authenticate login to server
authentication = checkAuthentication(options);
isDownloaded = false;
isExisting = false;
% Set folder based on product type
if tag == "brdc"
    prod.fold = "/archive/gnss/data/daily/" + gps_week + "/brdc/"; % gps_week is actually year here
else
    prod.fold = "/archive/gnss/products/";
end
%% Create the product structure
prod.host = "https://cddis.nasa.gov";
prod.yyyydoy = yyyydoy;
prod.wn = string( gps_week );
prod.prefix = options.optConfig.satelliteProductSource.value;
prod.type = options.optConfig.satelliteProductType.value;
%% Source url
URL_root = string( prod.host ) + string( prod.fold );
%% Download orbit
if options.optConfig.useSatelliteOrbitData.value && tag == "orb"
    % Default uses 5-minute orbit
    if str2double( prod.wn ) <= 2000
        prod.orb = "igs" + prod.wn + "0.sp3.Z";
    else
        if prod.type == "FIN"
            prod.orb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_05M_ORB.SP3.gz";
        elseif prod.type == "RAP"
            prod.orb = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_05M_ORB.SP3.gz";
        elseif prod.type == "ULT"
            prod.orb = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_05M_ORB.SP3.gz";
        else
            prod.orb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_05M_ORB.SP3.gz";
        end
    end
    [orb,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
    if ~isDownloaded && ~isExisting % Failed to download orbit
        % Try to download 15M orbit files instead
        if str2double( prod.wn ) <= 2000
            prod.orb = "igs" + prod.wn + "0.sp3.Z";
        else
            if prod.type == "FIN"
                prod.orb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
            elseif prod.type == "RAP"
                prod.orb = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
            elseif prod.type == "ULT"
                prod.orb = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
            else
                prod.orb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
            end
        end
        [orb,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
    end
else
    orb = [];
end
%% Download clock
if options.optConfig.useSatelliteClockData.value && tag == "clk"
    % Default uses 30-second clock product
    if str2double( prod.wn ) <= 2000
        prod.clk = "igs" + prod.wn + "0.clk_30s.Z";
    else
        if prod.type == "FIN"
            prod.clk = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_30S_CLK.CLK.gz";
        elseif prod.type == "RAP"
            prod.clk = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_30S_CLK.CLK.gz";
        elseif prod.type == "ULT"
            prod.clk = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_30S_CLK.CLK.gz";
        else
            prod.clk = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_30S_CLK.CLK.gz";
        end
    end
    [clk,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
else
    clk = [];
end
%% Download dcb
if options.optConfig.useSatelliteDcbData.value && tag == "dcb"
    % Default uses 30-second clock product
    if prod.type == "FIN"
        prod.dcb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_01D_OSB.BIA.gz";
    elseif prod.type == "RAP"
        prod.dcb = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_01D_OSB.BIA.gz";
    elseif prod.type == "ULT"
        prod.dcb = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_01D_OSB.BIA.gz";
    else
        prod.dcb = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_01D_OSB.BIA.gz";
    end
    [dcb,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
else
    dcb = [];
end
%% Download erp
% TODO: This is terrible. Read from server.
if options.optConfig.useEarthOrientationData.value && tag == "erp"
    % Default uses 12H product
    if prod.type == "FIN"
        prod.erp = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_12H_ERP.ERP.gz";
    elseif prod.type == "RAP"
        prod.erp = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_12H_ERP.ERP.gz";
    elseif prod.type == "ULT"
        prod.erp = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_12H_ERP.ERP.gz";
    else
        prod.erp = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_12H_ERP.ERP.gz";
    end
    [erp,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
    % If 12H not found, try 01D
    if ~isDownloaded && ~isExisting
        if prod.type == "FIN"
            prod.erp = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
        elseif prod.type == "RAP"
            prod.erp = prod.prefix + "0OPSRAP_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
        elseif prod.type == "ULT"
            prod.erp = prod.prefix + "0OPSULT_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
        else
            prod.erp = prod.prefix + "0MGXFIN_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
        end
        [erp,isDownloaded,isExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options);
    end
else
    erp = [];
end
%% Download broadcast ephemeris
if options.optConfig.useBroadcastEphemeris.value && tag == "brdc"
    prod.brdc = "BRDC00IGS_R_" + yyyydoy + "0000_01D_MN.rnx.gz";
    [brdc, isDownloaded, isExisting] = getProduct(productHomeFolder, URL_root, authentication, prod, tag, options);
else
    brdc = [];
end
end
%% Orbit
function [zipFilePath,isFileDownloaded,isFileExisting] = getProduct(productHomeFolder,URL_root,authentication,prod,tag,options)
isFileDownloaded = false;
isFileExisting = false;
if tag == "orb"
    zipFilePath = productHomeFolder + "orbit\zip\" + prod.(string(tag));
elseif tag == "clk"
    zipFilePath = productHomeFolder + "clock\zip\" + prod.(string(tag));
elseif tag == "dcb"
    zipFilePath = productHomeFolder + "dcb\zip\" + prod.(string(tag));
elseif tag == "erp"
    zipFilePath = productHomeFolder + "erp\zip\" + prod.(string(tag));
elseif tag == "brdc"
    zipFilePath = productHomeFolder + "brdc\zip\" + prod.(string(tag));
else
    fprintf("Satellite tag must be: orb, clk, dcb, erp, brdc\n");
end
if ~isfile( zipFilePath )
    try
        if tag == "orb"
            cd( productHomeFolder + "orbit\zip" );
        elseif tag == "clk"
            cd( productHomeFolder + "clock\zip" );
        elseif tag == "dcb"
            cd( productHomeFolder + "dcb\zip" );
        elseif tag == "erp"
            cd( productHomeFolder + "erp\zip" );
        elseif tag == "brdc"
            cd( productHomeFolder + "brdc\zip" );
        else
            fprintf("Satellite tag must be: orb, clk, dcb, erp, brdc\n");
        end
        if tag == "brdc"
            hostFile = ['"' + string( URL_root + "/" + prod.(string(tag)) ) + '"'];
        else
            hostFile = ['"' + string( URL_root + prod.wn + "/" + prod.(string(tag)) ) + '"'];
        end
        curlCommand = char( authentication.curl + " --fail -m 30.0 -c C:\temp\curlCookies.txt" + " --netrc-file " + authentication.netrc + " -L -O " + hostFile );
        [isFileDownloaded,~] = system( [ curlCommand ] );
        if isFileDownloaded ~= 0 % file was not found
            if options.optConfig.isVerbose.value
                fprintf( '%s NOT found in %s\n', prod.(string(tag)), prod.wn );
            end
            isFileDownloaded = false;
        else
            if options.optConfig.isVerbose.value
                fprintf( ' %s downloaded from %s\n', hostFile, prod.wn );
            end
            isFileDownloaded = true;
            hostFile = "";
        end
    catch ME
        fprintf( '%s NOT found in %s, proceed to next week\n', prod.(string(tag)), prod.wn );
        zipFilePath = NaN;
    end
else
    isFileExisting = true;
end
% Return to the matlab folder
cd( options.optConfig.matlabFolder.value );
end
%% Check authentication to CDDIS archive
function authentication = checkAuthentication(options)
% Required to access CDDIS (NASA) Archive
% Details:
% https://cddis.nasa.gov/Data_and_Derived_Products/CreateNetrcFile.html
% https://cddis.nasa.gov/Data_and_Derived_Products/CDDIS_Archive_Access.html
%% Options
authentication.netrc = char( options.optFiles.netrcFilePath.value );
authentication.curl = char( options.optFiles.curlFilePath.value );
%% Check .netrc
if isempty( authentication.netrc ) || ~isfile( authentication.netrc )
    if isempty( authentication.netrc )
        fprintf( "Unable to download satellite products. Authentication to CDDIS requires .netrc file\n" );
    end
    if ~isfile( authentication.netrc )
        fprintf( "Unable to download satellite products. Input .netrc file not found: %s\n", authentication.netrc );
    end
    fprintf( "Configure .netrc: %s\n", "https://cddis.nasa.gov/Data_and_Derived_Products/CreateNetrcFile.html");
end
%% Check curl.exe
if isempty( authentication.curl ) || ~isfile( authentication.curl )
    if isempty( authentication.curl )
        fprintf( "Unable to download satellite products. Authentication to CDDIS requires curl.exe\n" );
    end
    if ~isfile( authentication.curl )
        fprintf( "Unable to download satellite products. Input curl.exe file not found: %s\n", authentication.curl );
    end
    fprintf( "Configure curl: %s\n", "https://cddis.nasa.gov/Data_and_Derived_Products/CDDIS_Archive_Access.html");
end
%% Error
if ( isempty( authentication.netrc ) || ~isfile( authentication.netrc ) ) || ( isempty( authentication.curl ) || ~isfile( authentication.curl ) )
    % Not possible to authenticate download from CDDIS
    error('Unable to authenticate CDDIS satellite product download');
end
end
%% Unzip
function [prod_unzip] = unzipMGEX(productFolder,prod_zip,tag,options)
zip_exe = options.optFiles.sevenZFilePath.value;
if tag == "orb"
    cd( productFolder + "orbit\unzip" );
    existingFiles = string( ls('*.SP3') );
elseif tag == "clk"
    cd( productFolder + "clock\unzip" );
    existingFiles = string( ls('*.CLK') );
elseif tag == "clk_30s" % legacy
    cd( productFolder + "clock\unzip" );
    existingFiles = string( ls('*.CLK') );
elseif tag == "dcb"
    cd( productFolder + "dcb\unzip" );
    existingFiles = string( ls('*.BIA') );
elseif tag == "erp"
    cd( productFolder + "erp\unzip" );
    existingFiles = string( ls('*.ERP') );
elseif tag == "brdc"
    cd( productFolder + "brdc\unzip" );
    existingFiles = string( ls('*.rnx') );
else
    fprintf("Satellite tag must be orb, clk, dcb, erp, brdc\n");
end
prod_unzip = strrep( prod_zip, ".gz", "" );
prod_unzip = strrep( prod_unzip, "\zip\", "\unzip\" );
prod_unzip = strrep( prod_unzip, ".Z", "" ); % Handle old .Z extension
prod_file = extractAfter(prod_unzip,"unzip\");
if ~any( existingFiles == prod_file )
    [status7z,cmdout7z] = system( ['"',char(zip_exe),'"',' x ',char(prod_zip),' -aoa '] );
else
    if options.optConfig.isVerbose.value
        fprintf( 'Found unzipped orbit file: %s\n', prod_file );
    end
end
cd( options.optConfig.matlabFolder.value );
end