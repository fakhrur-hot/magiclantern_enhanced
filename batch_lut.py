"""
batch_lut.py
Converts all Canon Picture Style ICC profiles + WW PF2 files to .cube LUTs.
ICC profiles  -> direct parse
WW PF2 files  -> memory-scan PSE.exe at runtime to grab decrypted CLUT
"""

import struct, math, os, sys, subprocess, time, ctypes
from ctypes import windll, wintypes

# ── paths ─────────────────────────────────────────────────────────────────────
PSE_DIR  = r"C:\Program Files\Canon\Picture Style Editor"
ICC_DIR  = os.path.join(PSE_DIR, r"DPP4Lib\icc")
PF2_DIR  = r"C:\Users\Public\Kiro\ML_6D"
OUT_DIR  = r"C:\Users\Public\Kiro\ML_6D\docs\luts"
PSE_EXE  = os.path.join(PSE_DIR, "PSEditor.exe")
os.makedirs(OUT_DIR, exist_ok=True)

LUT_SIZE = 33   # 33^3 = 35937 nodes — standard for grading LUTs

# ── ICC helpers ───────────────────────────────────────────────────────────────
D50 = (0.9642, 1.0000, 0.8249)

def lab_to_xyz(L, a, b):
    kappa, epsilon = 903.3, 0.008856
    fy = (L + 16) / 116
    fx = a / 500 + fy
    fz = fy - b / 200
    X = fx**3 if fx**3 > epsilon else (116*fx-16)/kappa
    Y = ((L+16)/116)**3 if L > kappa*epsilon else L/kappa
    Z = fz**3 if fz**3 > epsilon else (116*fz-16)/kappa
    return X*D50[0], Y*D50[1], Z*D50[2]

def xyz_to_linear_srgb(X, Y, Z):
    r =  3.2404542*X - 1.5371385*Y - 0.4985314*Z
    g = -0.9692660*X + 1.8760108*Y + 0.0415560*Z
    b =  0.0556434*X - 0.2040259*Y + 1.0572252*Z
    return r, g, b

def linear_to_srgb(v):
    v = max(0.0, min(1.0, v))
    return 12.92*v if v <= 0.0031308 else 1.055*(v**(1/2.4))-0.055

def clamp01(v): return max(0.0, min(1.0, v))

def icc_get_a2b(icc_data):
    """Return (ngrid, nin, nout, clut_offset) from mft2 A2B0 tag."""
    n_tags = struct.unpack_from('>I', icc_data, 128)[0]
    for i in range(n_tags):
        base = 132 + i*12
        if icc_data[base:base+4] == b'A2B0':
            off = struct.unpack_from('>I', icc_data, base+4)[0]
            nin   = icc_data[off+8]
            nout  = icc_data[off+9]
            ngrid = icc_data[off+10]
            n_in  = struct.unpack_from('>H', icc_data, off+48)[0]
            HDR   = 52
            clut_start = off + HDR + nin * n_in * 2
            return ngrid, nin, nout, clut_start
    raise ValueError("A2B0 tag not found")

def build_lut_from_icc(icc_data, lut_size=33):
    ngrid, nin, nout, clut_start = icc_get_a2b(icc_data)

    def read_lab(ri, gi, bi):
        node = ri*ngrid*ngrid + gi*ngrid + bi
        off  = clut_start + node*nout*2
        L = struct.unpack_from('>H', icc_data, off  )[0] / 65535.0 * 100.0
        a = struct.unpack_from('>H', icc_data, off+2)[0] / 65535.0 * 255.0 - 128.0
        b = struct.unpack_from('>H', icc_data, off+4)[0] / 65535.0 * 255.0 - 128.0
        return L, a, b

    lut = []
    for bi in range(lut_size):
        for gi in range(lut_size):
            for ri in range(lut_size):
                r_f = ri/(lut_size-1) * (ngrid-1)
                g_f = gi/(lut_size-1) * (ngrid-1)
                b_f = bi/(lut_size-1) * (ngrid-1)
                r0,r1 = int(r_f), min(int(r_f)+1, ngrid-1)
                g0,g1 = int(g_f), min(int(g_f)+1, ngrid-1)
                b0,b1 = int(b_f), min(int(b_f)+1, ngrid-1)
                tr,tg,tb = r_f-r0, g_f-g0, b_f-b0

                def trilinear(fn):
                    c = (fn(r0,g0,b0)*(1-tr)*(1-tg)*(1-tb) +
                         fn(r1,g0,b0)*   tr *(1-tg)*(1-tb) +
                         fn(r0,g1,b0)*(1-tr)*   tg *(1-tb) +
                         fn(r1,g1,b0)*   tr *   tg *(1-tb) +
                         fn(r0,g0,b1)*(1-tr)*(1-tg)*   tb  +
                         fn(r1,g0,b1)*   tr *(1-tg)*   tb  +
                         fn(r0,g1,b1)*(1-tr)*   tg *   tb  +
                         fn(r1,g1,b1)*   tr *   tg *   tb)
                    return c

                def getLch(ri2,gi2,bi2): return read_lab(ri2,gi2,bi2)

                v000=getLch(r0,g0,b0); v100=getLch(r1,g0,b0)
                v010=getLch(r0,g1,b0); v110=getLch(r1,g1,b0)
                v001=getLch(r0,g0,b1); v101=getLch(r1,g0,b1)
                v011=getLch(r0,g1,b1); v111=getLch(r1,g1,b1)

                L = sum([v000[0]*(1-tr)*(1-tg)*(1-tb), v100[0]*tr*(1-tg)*(1-tb),
                         v010[0]*(1-tr)*tg*(1-tb),      v110[0]*tr*tg*(1-tb),
                         v001[0]*(1-tr)*(1-tg)*tb,      v101[0]*tr*(1-tg)*tb,
                         v011[0]*(1-tr)*tg*tb,           v111[0]*tr*tg*tb])
                a = sum([v000[1]*(1-tr)*(1-tg)*(1-tb), v100[1]*tr*(1-tg)*(1-tb),
                         v010[1]*(1-tr)*tg*(1-tb),      v110[1]*tr*tg*(1-tb),
                         v001[1]*(1-tr)*(1-tg)*tb,      v101[1]*tr*(1-tg)*tb,
                         v011[1]*(1-tr)*tg*tb,           v111[1]*tr*tg*tb])
                b = sum([v000[2]*(1-tr)*(1-tg)*(1-tb), v100[2]*tr*(1-tg)*(1-tb),
                         v010[2]*(1-tr)*tg*(1-tb),      v110[2]*tr*tg*(1-tb),
                         v001[2]*(1-tr)*(1-tg)*tb,      v101[2]*tr*(1-tg)*tb,
                         v011[2]*(1-tr)*tg*tb,           v111[2]*tr*tg*tb])

                X,Y,Z = lab_to_xyz(L,a,b)
                rl,gl,bl2 = xyz_to_linear_srgb(X,Y,Z)
                lut.append((clamp01(linear_to_srgb(rl)),
                             clamp01(linear_to_srgb(gl)),
                             clamp01(linear_to_srgb(bl2))))
    return lut

def write_cube(lut, lut_size, path, title, comment=''):
    with open(path, 'w') as f:
        f.write(f'TITLE "{title}"\n')
        if comment:
            f.write(f'# {comment}\n')
        f.write(f'LUT_3D_SIZE {lut_size}\n')
        f.write('DOMAIN_MIN 0.0 0.0 0.0\n')
        f.write('DOMAIN_MAX 1.0 1.0 1.0\n\n')
        for r,g,b in lut:
            f.write(f'{r:.6f} {g:.6f} {b:.6f}\n')

# ── ICC profiles available ────────────────────────────────────────────────────
ICC_PROFILES = [
    ('SS.ICC',  'Standard_sRGB',         'Canon Standard (sRGB)'),
    ('SA.ICC',  'Standard_AdobeRGB',     'Canon Standard (Adobe RGB)'),
    ('PS.ICC',  'Portrait_sRGB',         'Canon Portrait (sRGB)'),
    ('PA.ICC',  'Portrait_AdobeRGB',     'Canon Portrait (Adobe RGB)'),
    ('LS.ICC',  'Landscape_sRGB',        'Canon Landscape (sRGB)'),
    ('LA.ICC',  'Landscape_AdobeRGB',    'Canon Landscape (Adobe RGB)'),
    ('NS.ICC',  'Neutral_sRGB',          'Canon Neutral (sRGB)'),
    ('NA.ICC',  'Neutral_AdobeRGB',      'Canon Neutral (Adobe RGB)'),
    ('FS.ICC',  'Faithful_sRGB',         'Canon Faithful (sRGB)'),
    ('FA.ICC',  'Faithful_AdobeRGB',     'Canon Faithful (Adobe RGB)'),
    ('FDS.ICC', 'FineDetail_sRGB',       'Canon Fine Detail (sRGB)'),
    ('FDA.ICC', 'FineDetail_AdobeRGB',   'Canon Fine Detail (Adobe RGB)'),
]

print("=" * 60)
print("STEP 1: ICC profile → .cube LUT (Canon built-in styles)")
print("=" * 60)
for icc_file, label, title in ICC_PROFILES:
    icc_path = os.path.join(ICC_DIR, icc_file)
    out_path = os.path.join(OUT_DIR, f'{label}.cube')
    if os.path.exists(out_path):
        print(f'  SKIP  {label}.cube (exists)')
        continue
    with open(icc_path,'rb') as f: icc_data = f.read()
    print(f'  Building {label}.cube ...', end='', flush=True)
    lut = build_lut_from_icc(icc_data, LUT_SIZE)
    write_cube(lut, LUT_SIZE, out_path, title,
               f'Source: {icc_file} (Canon PSE {PSE_DIR})')
    print(f' done  [{os.path.getsize(out_path)//1024}KB]')

# ── WW PF2 → LUT via PSE memory scan ─────────────────────────────────────────
print()
print("=" * 60)
print("STEP 2: WW PF2 files → .cube LUT (via PSE runtime memory)")
print("=" * 60)

WW_FILES = [
    ('WW01_NOSTALGIA.pf2',  'WW01_Nostalgia'),
    ('WW02_CLEAR.pf2',      'WW02_Clear'),
    ('WW03_TWILIGHT.pf2',   'WW03_Twilight'),
    ('WW04_EMERALD.pf2',    'WW04_Emerald'),
    ('WW06_P-STUDIO.pf2',   'WW06_P-Studio'),
    ('WW07_P-SNAPSHOT.pf2', 'WW07_P-Snapshot'),
    ('WW08_VIDEO-X.pf2',    'WW08_Video-X'),
]

# Windows memory reading via ctypes
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT   = 0x1000
PAGE_READABLE = (0x02|0x04|0x20|0x40)  # READ|READWRITE|EXECUTE_READ|EXECUTE_READWRITE

k32  = windll.kernel32
psapi = windll.psapi

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress",       ctypes.c_void_p),
        ("AllocationBase",    ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize",        ctypes.c_size_t),
        ("State",             wintypes.DWORD),
        ("Protect",           wintypes.DWORD),
        ("Type",              wintypes.DWORD),
    ]

def read_process_memory(hProcess, addr, size):
    buf = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(hProcess, ctypes.c_void_p(addr),
                               buf, size, ctypes.byref(read))
    if ok and read.value > 0:
        return buf.raw[:read.value]
    return None

def find_icc_clut_in_process(pid):
    """Scan process memory for mft2 A2B0 pattern with 33-grid CLUT."""
    hProc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hProc:
        return None

    addr  = 0
    mbi   = MEMORY_BASIC_INFORMATION()
    found = []

    while k32.VirtualQueryEx(hProc, ctypes.c_void_p(addr),
                              ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if (mbi.State == MEM_COMMIT and
                mbi.Protect & PAGE_READABLE and
                mbi.RegionSize < 256*1024*1024):  # skip >256MB regions
            chunk = read_process_memory(hProc, mbi.BaseAddress, min(mbi.RegionSize, 4*1024*1024))
            if chunk:
                # Search for mft2 signature
                pos = 0
                while True:
                    pos = chunk.find(b'mft2', pos)
                    if pos < 0: break
                    # Check if looks like valid A2B0 tag
                    if pos+52 < len(chunk):
                        nin   = chunk[pos+8]
                        nout  = chunk[pos+9]
                        ngrid = chunk[pos+10]
                        if nin==3 and nout==3 and 17<=ngrid<=65:
                            n_in = struct.unpack_from('>H', chunk, pos+48)[0]
                            clut_sz = (ngrid**3) * nout * 2
                            clut_off = pos + 52 + nin*n_in*2
                            if clut_off + clut_sz <= len(chunk):
                                found.append((mbi.BaseAddress+pos, chunk[pos:pos+52+nin*n_in*2+clut_sz]))
                                print(f'    Found mft2 CLUT: addr=0x{mbi.BaseAddress+pos:016X} grid={ngrid}')
                    pos += 1
        addr = mbi.BaseAddress + mbi.RegionSize
        if addr >= 0x7FFFFFFFFFFF: break

    k32.CloseHandle(hProc)
    return found

def lut_from_memory_clut(clut_bytes, ngrid, lut_size=33):
    """Build LUT from raw CLUT bytes (Lab uint16 BE)."""
    def read_lab(ri, gi, bi):
        node = ri*ngrid*ngrid + gi*ngrid + bi
        off  = node * 6
        L = struct.unpack_from('>H', clut_bytes, off  )[0] / 65535.0 * 100.0
        a = struct.unpack_from('>H', clut_bytes, off+2)[0] / 65535.0 * 255.0 - 128.0
        b = struct.unpack_from('>H', clut_bytes, off+4)[0] / 65535.0 * 255.0 - 128.0
        return L, a, b

    lut = []
    for bi in range(lut_size):
        for gi in range(lut_size):
            for ri in range(lut_size):
                r_f = ri/(lut_size-1)*(ngrid-1)
                g_f = gi/(lut_size-1)*(ngrid-1)
                b_f = bi/(lut_size-1)*(ngrid-1)
                r0,r1=int(r_f),min(int(r_f)+1,ngrid-1)
                g0,g1=int(g_f),min(int(g_f)+1,ngrid-1)
                b0,b1=int(b_f),min(int(b_f)+1,ngrid-1)
                tr,tg,tb=r_f-r0,g_f-g0,b_f-b0
                def ll(ri2,gi2,bi2): return read_lab(ri2,gi2,bi2)
                v=[ll(r0,g0,b0),ll(r1,g0,b0),ll(r0,g1,b0),ll(r1,g1,b0),
                   ll(r0,g0,b1),ll(r1,g0,b1),ll(r0,g1,b1),ll(r1,g1,b1)]
                w=[(1-tr)*(1-tg)*(1-tb),tr*(1-tg)*(1-tb),(1-tr)*tg*(1-tb),tr*tg*(1-tb),
                   (1-tr)*(1-tg)*tb,    tr*(1-tg)*tb,    (1-tr)*tg*tb,    tr*tg*tb]
                L=sum(v[i][0]*w[i] for i in range(8))
                aa=sum(v[i][1]*w[i] for i in range(8))
                b=sum(v[i][2]*w[i] for i in range(8))
                X,Y,Z=lab_to_xyz(L,aa,b)
                rl,gl,bl2=xyz_to_linear_srgb(X,Y,Z)
                lut.append((clamp01(linear_to_srgb(rl)),
                             clamp01(linear_to_srgb(gl)),
                             clamp01(linear_to_srgb(bl2))))
    return lut

# Process WW files one by one
import winreg
try:
    # Check if PSE is accessible
    subprocess.Popen([PSE_EXE], creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(3)  # let PSE splash finish
except Exception as e:
    print(f'  Cannot launch PSE: {e}')
    sys.exit(1)

# Get PSE PID
import psutil
pse_pids = [p.pid for p in psutil.process_iter(['name']) if 'PSEditor' in p.info['name']]
if not pse_pids:
    print('  PSE not running — skipping WW files')
else:
    pid = pse_pids[0]
    print(f'  PSE running: PID {pid}')

    for pf2_name, label in WW_FILES:
        out_path = os.path.join(OUT_DIR, f'{label}.cube')
        if os.path.exists(out_path):
            print(f'  SKIP  {label}.cube (exists)')
            continue

        pf2_path = os.path.join(PF2_DIR, pf2_name)
        print(f'\n  Opening {pf2_name} in PSE ...', flush=True)

        # Open PF2 in PSE via ShellExecute (associated with PSE)
        import subprocess
        subprocess.Popen([PSE_EXE, pf2_path])
        time.sleep(4)  # wait for PSE to load and decrypt

        # Scan memory for ICC CLUT
        print(f'  Scanning PSE memory for CLUT ...')
        results = find_icc_clut_in_process(pid)

        if results:
            # Use the largest CLUT found (most likely the active style)
            addr, raw = max(results, key=lambda x: len(x[1]))
            nin   = raw[8]
            ngrid = raw[10]
            n_in  = struct.unpack_from('>H', raw, 48)[0]
            clut_start_in_raw = 52 + nin * n_in * 2
            clut_bytes = raw[clut_start_in_raw:]

            print(f'  Building LUT from memory CLUT (ngrid={ngrid}) ...')
            lut = lut_from_memory_clut(clut_bytes, ngrid, LUT_SIZE)
            write_cube(lut, LUT_SIZE, out_path, label,
                       f'Source: {pf2_name} (extracted from PSE runtime memory)')
            print(f'  Wrote {label}.cube [{os.path.getsize(out_path)//1024}KB]')
        else:
            print(f'  No CLUT found in memory for {pf2_name}')

    # Terminate PSE
    for p in psutil.process_iter(['name','pid']):
        if 'PSEditor' in p.info['name']:
            p.kill()

print()
print("=" * 60)
print("Summary:")
cubes = [f for f in os.listdir(OUT_DIR) if f.endswith('.cube')]
for c in sorted(cubes):
    sz = os.path.getsize(os.path.join(OUT_DIR, c))
    print(f'  {c:40s}  {sz//1024:4d} KB')
print(f'\nTotal: {len(cubes)} LUTs in {OUT_DIR}')
