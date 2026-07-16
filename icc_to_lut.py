"""icc_to_lut.py — batch convert Canon PSE ICC profiles to .cube LUTs (cross-platform)"""
import struct, math, os

ICC_DIR = '/mnt/c/Program Files/Canon/Picture Style Editor/DPP4Lib/icc'
OUT_DIR = '/mnt/c/Users/Public/Kiro/ML_6D/docs/luts'
LUT_SIZE = 33

os.makedirs(OUT_DIR, exist_ok=True)

D50 = (0.9642, 1.0000, 0.8249)

def lab_to_xyz(L, a, b):
    kappa, eps = 903.3, 0.008856
    fy = (L+16)/116; fx = a/500+fy; fz = fy-b/200
    X = fx**3 if fx**3>eps else (116*fx-16)/kappa
    Y = ((L+16)/116)**3 if L>kappa*eps else L/kappa
    Z = fz**3 if fz**3>eps else (116*fz-16)/kappa
    return X*D50[0], Y*D50[1], Z*D50[2]

def xyz_to_srgb(X, Y, Z):
    r= 3.2404542*X-1.5371385*Y-0.4985314*Z
    g=-0.9692660*X+1.8760108*Y+0.0415560*Z
    b= 0.0556434*X-0.2040259*Y+1.0572252*Z
    def gamma(v):
        v=max(0.0,min(1.0,v))
        return 12.92*v if v<=0.0031308 else 1.055*v**(1/2.4)-0.055
    return gamma(r), gamma(g), gamma(b)

def build_lut(icc_data, size=33):
    n = struct.unpack_from('>I', icc_data, 128)[0]
    for i in range(n):
        base = 132+i*12
        if icc_data[base:base+4] == b'A2B0':
            off   = struct.unpack_from('>I', icc_data, base+4)[0]
            nin   = icc_data[off+8]
            nout  = icc_data[off+9]
            ngrid = icc_data[off+10]
            n_in  = struct.unpack_from('>H', icc_data, off+48)[0]
            cs    = off + 52 + nin*n_in*2   # CLUT start

            def node(ri,gi,bi):
                idx = ri*ngrid*ngrid + gi*ngrid + bi
                o   = cs + idx*nout*2
                L = struct.unpack_from('>H',icc_data,o  )[0]/65535.0*100.0
                a = struct.unpack_from('>H',icc_data,o+2)[0]/65535.0*255.0-128.0
                b = struct.unpack_from('>H',icc_data,o+4)[0]/65535.0*255.0-128.0
                return L,a,b

            lut = []
            for bi in range(size):
                for gi in range(size):
                    for ri in range(size):
                        rf=ri/(size-1)*(ngrid-1); gf=gi/(size-1)*(ngrid-1); bf=bi/(size-1)*(ngrid-1)
                        r0,r1=int(rf),min(int(rf)+1,ngrid-1)
                        g0,g1=int(gf),min(int(gf)+1,ngrid-1)
                        b0,b1=int(bf),min(int(bf)+1,ngrid-1)
                        tr,tg,tb=rf-r0,gf-g0,bf-b0
                        v=[node(r0,g0,b0),node(r1,g0,b0),node(r0,g1,b0),node(r1,g1,b0),
                           node(r0,g0,b1),node(r1,g0,b1),node(r0,g1,b1),node(r1,g1,b1)]
                        w=[(1-tr)*(1-tg)*(1-tb),tr*(1-tg)*(1-tb),(1-tr)*tg*(1-tb),tr*tg*(1-tb),
                           (1-tr)*(1-tg)*tb,    tr*(1-tg)*tb,    (1-tr)*tg*tb,    tr*tg*tb]
                        L=sum(v[i][0]*w[i] for i in range(8))
                        aa=sum(v[i][1]*w[i] for i in range(8))
                        b=sum(v[i][2]*w[i] for i in range(8))
                        lut.append(xyz_to_srgb(*lab_to_xyz(L,aa,b)))
            return lut, ngrid
    raise ValueError("A2B0 not found")

def write_cube(lut, size, path, title, src=''):
    with open(path,'w') as f:
        f.write(f'TITLE "{title}"\n')
        if src: f.write(f'# Source: {src}\n')
        f.write(f'LUT_3D_SIZE {size}\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n\n')
        for r,g,b in lut:
            f.write(f'{max(0.0,min(1.0,r)):.6f} {max(0.0,min(1.0,g)):.6f} {max(0.0,min(1.0,b)):.6f}\n')

PROFILES = [
    ('SS.ICC',  'Canon_Standard_sRGB',        'Canon Standard (sRGB)'),
    ('SA.ICC',  'Canon_Standard_AdobeRGB',    'Canon Standard (Adobe RGB)'),
    ('PS.ICC',  'Canon_Portrait_sRGB',        'Canon Portrait (sRGB)'),
    ('PA.ICC',  'Canon_Portrait_AdobeRGB',    'Canon Portrait (Adobe RGB)'),
    ('LS.ICC',  'Canon_Landscape_sRGB',       'Canon Landscape (sRGB)'),
    ('LA.ICC',  'Canon_Landscape_AdobeRGB',   'Canon Landscape (Adobe RGB)'),
    ('NS.ICC',  'Canon_Neutral_sRGB',         'Canon Neutral (sRGB)'),
    ('NA.ICC',  'Canon_Neutral_AdobeRGB',     'Canon Neutral (Adobe RGB)'),
    ('FS.ICC',  'Canon_Faithful_sRGB',        'Canon Faithful (sRGB)'),
    ('FA.ICC',  'Canon_Faithful_AdobeRGB',    'Canon Faithful (Adobe RGB)'),
    ('FDS.ICC', 'Canon_FineDetail_sRGB',      'Canon Fine Detail (sRGB)'),
    ('FDA.ICC', 'Canon_FineDetail_AdobeRGB',  'Canon Fine Detail (Adobe RGB)'),
]

for icc_name, label, title in PROFILES:
    out = os.path.join(OUT_DIR, f'{label}.cube')
    if os.path.exists(out):
        print(f'SKIP {label}.cube')
        continue
    print(f'Building {label}.cube ...', end='', flush=True)
    with open(os.path.join(ICC_DIR, icc_name),'rb') as f: d = f.read()
    lut, ngrid = build_lut(d, LUT_SIZE)
    write_cube(lut, LUT_SIZE, out, title, icc_name)
    print(f' done ({os.path.getsize(out)//1024}KB)')

print(f'\nDone. LUTs in {OUT_DIR}')
