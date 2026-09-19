"""CORV worked example: legacy NAD83(2011)@2010.00  <->  ITRF2020 / NATRF2022 @2020.00
   hand computation -> PROJ -> NCAT, with the SIF, the NGS datasheet and the RINEX Delta H."""
import json, math, h5py, numpy as np
from pyproj import CRS, Transformer
np.set_printoptions(suppress=True)
MAS = math.pi/180/3600/1000
def llh2xyz(la,lo,h,a=6378137.0,fi=298.257222101):
    e2=(2-1/fi)/fi; la,lo=math.radians(la),math.radians(lo); N=a/math.sqrt(1-e2*math.sin(la)**2)
    return np.array([(N+h)*math.cos(la)*math.cos(lo),(N+h)*math.cos(la)*math.sin(lo),(N*(1-e2)+h)*math.sin(la)])
def xyz2llh(x,a=6378137.0,fi=298.257222101):
    e2=(2-1/fi)/fi; lo=math.atan2(x[1],x[0]); p=math.hypot(x[0],x[1]); la=math.atan2(x[2],p*(1-e2))
    for _ in range(8):
        N=a/math.sqrt(1-e2*math.sin(la)**2); h=p/math.cos(la)-N; la=math.atan2(x[2],p*(1-e2*N/(N+h)))
    return la,lo,h
def Renu(la,lo):   # columns = E,N,U unit vectors in ECEF
    return np.array([[-math.sin(lo),-math.sin(la)*math.cos(lo),math.cos(la)*math.cos(lo)],
                     [ math.cos(lo),-math.sin(la)*math.sin(lo),math.cos(la)*math.sin(lo)],
                     [ 0.0,          math.cos(la),             math.sin(la)]])
def helmert_cf(x,T,eps_mas,s_ppb,inverse=False):
    """NGS / EPSG:1056 coordinate-frame form, linearised:  x' = x + T + s x + E x,  E = [[0,ez,-ey],[-ez,0,ex],[ey,-ex,0]]"""
    ex,ey,ez=np.array(eps_mas)*MAS; E=np.array([[0,ez,-ey],[-ez,0,ex],[ey,-ex,0]]); s=s_ppb*1e-9
    if not inverse: return x+T+s*x+E@x, (T, s*x, E@x)
    y=x-T; return y-s*y-E@y, None
def dms(d,m,s): return d+m/60+s/3600
def show(tag,d,R): e=R.T@d*1000; print(f"   {tag:58s} dE {e[0]:8.1f}  dN {e[1]:8.1f}  dU {e[2]:8.1f}   3D {np.linalg.norm(e):7.1f} mm")

# ---------------------------------------------------------------- 0. inputs
sif=[s for s in json.load(open(r"C:\GnssProcessor\data\static\sif\ncn20_SIF.json"))['STATIONS'] if s['ID']=='CORV00USA'][0]
occ=[o for o in sif['OCCUPATION'] if not o['DO_NOT_USE']][-1]
X20=np.array(occ['COORDINATES']); V=np.array(occ['VELOCITY'])/1000.0          # ITRF2020 @ 2020.0, monument (GRP)
dH=sif['ANTENNA'][-1]['ARP_MINUS_GRP_ENU'][2]                                  # = RINEX "ANTENNA: DELTA H/E/N"
DS=llh2xyz(dms(44,35,7.91300),-dms(123,18,16.51681),105.953)                   # datasheet AH2488, NAD83(2011)@2010.00, MONUMENT
WS_ITRF_ARP=np.array([-2498424.007,-3802820.797,4454737.751])                  # workshop slide 19, ITRF2020@2020.0 (ARP)
WS_NAD_ARP =np.array([-2498423.096,-3802822.020,4454737.741])                  # workshop slide 22, "NGS published" NAD83@2010.0 (ARP)
la,lo,h=xyz2llh(X20); R=Renu(la,lo); up=R[:,2]
print("0. INPUTS  (CORV, SIF occupation %s -> open, antenna %s)"%(occ['START'][:10],sif['ANTENNA'][-1]['TYPE']))
print(f"   SIF ITRF2020@2020.0 (monument)  {X20[0]:.4f} {X20[1]:.4f} {X20[2]:.4f}    h={h:.4f} m")
print(f"   SIF velocity ECEF {V*1000} mm/yr   ENU {R.T@V*1000} mm/yr")
print(f"   RINEX Delta H (ARP - monument)  {dH} m")

# ---------------------------------------------------------------- 1. Delta H
print("\n1. DELTA H:  monument + 1.521 m Up = ARP      (datasheet = monument; CORS coordinate file / workshop slides = ARP)")
show("workshop ITRF2020 ARP  - (SIF monument + dH*Up)", WS_ITRF_ARP-(X20+dH*up), R)
show("workshop NAD83 ARP     - (datasheet monument + dH*Up)", WS_NAD_ARP-(DS+dH*up), R)

# ---------------------------------------------------------------- 2. modern -> legacy
print("\n2. MODERN -> LEGACY by hand:  ITRF2020@2020.0 --velocity--> @2010.0 --Helmert(2010.0)--> NAD83(2011)@2010.00")
X10=X20-10.0*V
print(f"   2a  epoch:  X - 10 V                     {X10[0]:.4f} {X10[1]:.4f} {X10[2]:.4f}   (moved {np.linalg.norm(10*V)*1000:.1f} mm)")
T1=(np.array([-1.4,-0.9,1.4])+np.array([0.0,-0.1,0.2])*(2010.0-2015.0))/1000.0   # IGN set run back 5 yr to 2010.0
X14,_=helmert_cf(X10,T1,(0,0,0),-0.42)
print(f"   2b  ITRF2020->ITRF2014 @2010.0  T={T1*1000} mm, s=-0.42 ppb   {X14[0]:.4f} {X14[1]:.4f} {X14[2]:.4f}")
N10,(tT,tS,tR)=helmert_cf(X14,np.array([1.00530,-1.90921,-0.54157]),(26.78138,-0.42027,10.93206),0.36891)
print(f"   2c  ITRF2014->NAD83(2011) @2010.0 (t = t0, no rate step)")
print(f"         translation {tT}  m\n         rotation    {tR}  m\n         scale       {tS}  m")
print(f"       NAD83(2011)@2010.00 by hand            {N10[0]:.4f} {N10[1]:.4f} {N10[2]:.4f}")
proj=Transformer.from_crs(CRS.from_epsg(9988),CRS.from_epsg(6317),always_xy=True)
P10=np.array(proj.transform(*X10,2010.0)[:3])
print(f"       PROJ EPSG:9988->6317 @2010.0           {P10[0]:.4f} {P10[1]:.4f} {P10[2]:.4f}")
print(f"       NGS datasheet AH2488                   {DS[0]:.4f} {DS[1]:.4f} {DS[2]:.4f}")
show("hand - PROJ", N10-P10, R); show("hand - NGS datasheet", N10-DS, R)

# ---------------------------------------------------------------- 3. legacy -> modern
print("\n3. LEGACY -> MODERN:  datasheet NAD83(2011)@2010.00  ->  ITRF2020 = NATRF2022 @2020.00.   Who supplies the 10 years?")
inv=Transformer.from_crs(CRS.from_epsg(6317),CRS.from_epsg(9988),always_xy=True)
I10=np.array(inv.transform(*DS,2010.0)[:3])
import os
f=h5py.File(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..","data","static","ifdm2022","IFDM2022_vel_svel.nc"),'r')
glon=f['lon'][:]; glat=f['lat'][:]; lad,lod=math.degrees(la),math.degrees(lo)+360
i=np.searchsorted(glon,lod)-1; j=np.searchsorted(glat,lad)-1; tx=(lod-glon[i])/(glon[i+1]-glon[i]); ty=(lad-glat[j])/(glat[j+1]-glat[j])
bil=lambda n:(lambda b:(1-tx)*(1-ty)*b[0,0]+tx*(1-ty)*b[0,1]+(1-tx)*ty*b[1,0]+tx*ty*b[1,1])(f[n][j:j+2,i:i+2])
Vifdm=R@np.array([bil('ve'),bil('vn'),bil('vu')])/1000.0
NCAT=np.array([-2498423.410,-3802819.901,4454736.682])                          # beta NCAT, input = datasheet XYZ
rigid=np.array(inv.transform(*DS,2020.0)[:3])                                  # 14-parameter ONLY: assume v_NAD83 = 0
print(f"   truth (SIF ITRF2020@2020.0)                {X20[0]:.4f} {X20[1]:.4f} {X20[2]:.4f}")
show("(i)   station velocity from the SIF              - truth", I10+10*V-X20, R)
show("(ii)  IFDM2022 grid velocity (our interpolation) - truth", I10+10*Vifdm-X20, R)
show("(iii) beta NCAT (NGS's own tool)                 - truth", NCAT-X20, R)
show("(iv)  14-parameter ONLY (rates stand in for v)   - truth", rigid-X20, R)
print(f"   velocities ENU mm/yr:  SIF {R.T@V*1000}   IFDM2022 {R.T@Vifdm*1000}")

# ---------------------------------------------------------------- 4. NATRF2022
print("\n4. WORKING IN NATRF2022:  x_NATRF(t) = x_ITRF2020(t) - (w x x)(t - 2020.0),   beta EPP2022 w = (0.046,-0.704,-0.047) mas/yr")
w=np.array([0.046,-0.704,-0.047])*MAS
vplate=np.cross(w,X20); vnat=V-vplate
Rd=np.array([0.06667,-0.75744,-0.05133])*MAS; Td=np.array([0.00079,-0.00070,-0.00124]); sd=-0.07201e-9
E=lambda r:np.array([[0,r[2],-r[1]],[-r[2],0,r[0]],[r[1],-r[0],0]])
vnad=V+Td+sd*X20+E(Rd)@X20
print(f"   plate motion at CORV (w x x)        ENU {R.T@vplate*1000} mm/yr")
print(f"   CORV velocity in ITRF2020           ENU {R.T@V*1000} mm/yr")
print(f"   CORV velocity in NATRF2022          ENU {R.T@vnat*1000} mm/yr")
print(f"   CORV velocity in NAD83(2011)        ENU {R.T@vnad*1000} mm/yr")
for t in (2020.0,2026.0,2030.0):
    xi=X20+V*(t-2020); xn=xi-vplate*(t-2020); e=R.T@(xn-X20)*1000
    print(f"   t={t:.1f}  ITRF2020 moved {np.linalg.norm(xi-X20)*1000:6.1f} mm | NATRF2022 coordinate moved dE {e[0]:6.1f} dN {e[1]:6.1f} dU {e[2]:5.1f} mm from its 2020.00 value")
