function cmap = stripColormap(name, n)
%% ============================================================
%  Function: stripColormap
%  Purpose: Central colormap library for the strip-chart viewers.  Returns
%           an n-by-3 RGB map for a named colormap so every viewer shares
%           one set of perceptually-uniform / colorblind-aware maps instead
%           of hand-rolled rainbow ramps.
%
%  Sequential (for locktime, elevation, sigma, counts):
%     'parula'  'turbo'  'viridis'  'cividis'  'magma'  'inferno'
%     ...and their reversed variants with a '_r' suffix (e.g. 'turbo_r').
%  Diverging (for signed quantities -- iono, gate ratio, residuals):
%     'rdbu'  (blue=neg, white=0, red=pos; ColorBrewer)
%     'vik'   (Crameri, perceptually-uniform diverging)
%     'bwr'   (the original linear blue-white-red; kept for comparison)
%
%  An Nx3 matrix passed as NAME is returned unchanged (escape hatch).
%  ============================================================
    if nargin < 2 || isempty(n), n = 256; end
    if isnumeric(name) && size(name,2)==3, cmap = name; return; end

    nm = lower(char(name));
    rev = endsWith(nm, '_r');
    if rev, nm = nm(1:end-2); end

    switch nm
        case 'parula',  base = parula(n);
        case 'turbo',   base = turbo(n);
        case 'viridis', base = interpAnchors(viridisAnchors(), n);
        case 'cividis', base = interpAnchors(cividisAnchors(), n);
        case 'magma',   base = interpAnchors(magmaAnchors(),   n);
        case 'inferno', base = interpAnchors(infernoAnchors(), n);
        case 'rdbu',    base = interpAnchors(rdbuAnchors(),    n);
        case 'vik',     base = interpAnchors(vikAnchors(),     n);
        % Legacy aliases from the first-generation viewers -> chosen maps.
        case {'freshness','elevation','sequential'}
                        base = interpAnchors(viridisAnchors(), n);
        case 'diverging', base = interpAnchors(vikAnchors(),   n);
        case 'bwr'
            half = floor(n/2);
            blue = [linspace(0.10,1,half)' linspace(0.30,1,half)' linspace(0.75,1,half)'];
            red  = [linspace(1,0.75,n-half)' linspace(1,0.10,n-half)' linspace(1,0.10,n-half)'];
            base = [blue; red];
        otherwise,      base = parula(n);
    end
    if rev, base = flipud(base); end
    cmap = base;
end

%% ---- interpolation ----
function cmap = interpAnchors(A, n)
    xi = linspace(0,1,size(A,1));
    xq = linspace(0,1,n);
    cmap = [interp1(xi,A(:,1),xq)', interp1(xi,A(:,2),xq)', interp1(xi,A(:,3),xq)'];
    cmap = min(max(cmap,0),1);
end

%% ---- perceptually-uniform anchor tables (sampled 0:0.1:1) ----
function A = viridisAnchors()
    A = [0.267004 0.004874 0.329415; 0.282623 0.140926 0.457517; ...
         0.253935 0.265254 0.529983; 0.206756 0.371758 0.553117; ...
         0.163625 0.471133 0.558148; 0.127568 0.566949 0.550556; ...
         0.134692 0.658636 0.517649; 0.266941 0.748751 0.440573; ...
         0.477504 0.821444 0.318055; 0.741388 0.873449 0.149561; ...
         0.993248 0.906157 0.143936];
end

function A = cividisAnchors()
    A = [0.000000 0.135112 0.304751; 0.000000 0.183846 0.402529; ...
         0.249425 0.299309 0.412259; 0.353330 0.371851 0.433811; ...
         0.444070 0.439060 0.452470; 0.491484 0.479875 0.471140; ...
         0.575160 0.546157 0.446631; 0.671162 0.616818 0.420708; ...
         0.770914 0.690087 0.383493; 0.881443 0.769230 0.326856; ...
         0.995737 0.909344 0.217772];
end

function A = magmaAnchors()
    A = [0.001462 0.000466 0.013866; 0.078815 0.054184 0.211667; ...
         0.232077 0.059889 0.437695; 0.390384 0.100379 0.501864; ...
         0.550287 0.161158 0.505719; 0.716387 0.214982 0.475290; ...
         0.868793 0.287728 0.409303; 0.967671 0.439703 0.359630; ...
         0.994738 0.624350 0.427397; 0.996580 0.795190 0.543864; ...
         0.987053 0.991438 0.749504];
end

function A = infernoAnchors()
    A = [0.001462 0.000466 0.013866; 0.087411 0.044556 0.224813; ...
         0.258234 0.038571 0.406485; 0.416331 0.090203 0.432943; ...
         0.578304 0.148039 0.404411; 0.735683 0.215906 0.330245; ...
         0.865006 0.316822 0.226055; 0.954506 0.468744 0.099874; ...
         0.987622 0.652180 0.211624; 0.964394 0.843848 0.273391; ...
         0.988362 0.998364 0.644924];
end

%% ---- diverging anchor tables (low -> high) ----
function A = rdbuAnchors()
    % ColorBrewer RdBu (11-class), reversed to blue(neg)->white->red(pos).
    A = [5 48 97; 33 102 172; 67 147 195; 146 197 222; 209 229 240; ...
         247 247 247; 253 219 199; 244 165 130; 214 96 77; 178 24 43; ...
         103 0 31] / 255;
end

function A = vikAnchors()
    % Crameri 'vik' (approximate sample), blue -> desaturated -> red.
    A = [0.001 0.098 0.310; 0.020 0.231 0.463; 0.137 0.373 0.580; ...
         0.337 0.518 0.660; 0.580 0.682 0.749; 0.859 0.859 0.847; ...
         0.780 0.616 0.541; 0.722 0.420 0.310; 0.624 0.247 0.149; ...
         0.510 0.149 0.075; 0.384 0.090 0.043];
end
