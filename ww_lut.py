"""
ww_lut.py  — Windows only
Opens each WW PF2 in PSE, scans process memory for the decrypted ICC CLUT,
builds a 33^3 .cube LUT.  Run from Windows Python (not WSL).
"""
import struct, os, sys, time, ctypes, subprocess
from ctypes import windll, wintypes

PSE_EXE = r"C:\Program Files\Canon\Picture Style Editor\PSEditor.exe"
PF2_DIR = r"C:\Users\Public\Kiro\ML_6D"
OUT_DIR = r"C:\Users\Public\Kiro\ML_6D\docs\luts"
LUT_SIZE = 33
os.makedirs(OUT_DIR, exist_ok=True)

WW_FILES = [
    ('WW01_NOSTALGIA.pf2',  'WW01_Nostalgia',   'Nostalgia'),
    ('WW02_CLEAR.pf2',      'WW02_Clear',        'Clear'),
    ('WW03_TWILIGHT.pf2',   'WW03_Twilight',     'Twilight'),
    ('WW04_EMERALD.pf2',    'WW04_Emerald',      'Emerald'),
    ('WW06_P-STUDIO.pf2',   'WW06_P-Studio',     'P-Studio'),
    ('WW07_P-SNAPSHOT.pf2', 'WW07_P-Snapshot',   'P-Snapshot'),
    ('WW08_VIDEO-X.pf2',    'WW08_Video-X',      'Video-X'),
]

D50 = (0.9642, 1.0000, 0.8249)

def lab_to_xyz(L, a, b):
    kappa, eps = 903.3, 0.008856
    fy=(L+16)/116; fx=a/500+fy; fz=fy-b/200
    X=fx**3 if fx**3>eps else (116*fx-16)/kappa
    Y=((L+16)/116)**3 if L>kappa*eps else L/kappa
    Z=fz**3 if fz**3>eps else (116*fz-16)/kappa
    return X*D50[0], Y*D50[1], Z*D50[2]

def xyz_to_srgb(X,Y,Z):
    r= 3.2404542*X-1.5371385*Y-0.4985314*Z
    g=-0.9692660*X+1.8760108*Y+0.0415560*Z
    b= 0.0556434*X-0.2040259*Y+1.0572252*Z
    def gam(v):
        v=max(0.0,min(1.0,v))
        return 12.92*v if v<=0.0031308 else 1.055*v**(1/2.4)-0.055
    return gam(r),gam(g),gam(b)

def build_lut_from_clut(clut, ngrid, size=33):
    def node(ri,gi,bi):
        o=(ri*ngrid*ngrid+gi*ngrid+bi)*6
        L=struct.unpack_from('>H',clut,o  )[0]/65535.0*100.0
        a=struct.unpack_from('>H',clut,o+2)[0]/65535.0*255.0-128.0
        b=struct.unpack_from('>H',clut,o+4)[0]/65535.0*255.0-128.0
        return L,a,b
    lut=[]
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
    return lut

def write_cube(lut, size, path, title, src=''):
    with open(path,'w') as f:
        f.write(f'TITLE "{title}"\n')
        if src: f.write(f'# Source: {src}\n')
        f.write(f'LUT_3D_SIZE {size}\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n\n')
        for r,g,b in lut:
            f.write(f'{max(0.0,min(1.0,r)):.6f} {max(0.0,min(1.0,g)):.6f} {max(0.0,min(1.0,b)):.6f}\n')

# ── Windows memory scan ───────────────────────────────────────────────────────
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000

class MBI(ctypes.Structure):
    _fields_=[("BaseAddress",ctypes.c_void_p),("AllocationBase",ctypes.c_void_p),
              ("AllocationProtect",wintypes.DWORD),("RegionSize",ctypes.c_size_t),
              ("State",wintypes.DWORD),("Protect",wintypes.DWORD),("Type",wintypes.DWORD)]

k32 = windll.kernel32

def scan_pid(pid):
    hp = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hp: return []
    found=[]
    addr=0
    mbi=MBI()
    while k32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        readable = mbi.Protect & 0x66  # READ|READWRITE|EX_READ|EX_READWRITE
        if mbi.State==MEM_COMMIT and readable and 0 < mbi.RegionSize < 32*1024*1024:
            buf=ctypes.create_string_buffer(mbi.RegionSize)
            rd=ctypes.c_size_t(0)
            ok=k32.ReadProcessMemory(hp, ctypes.c_void_p(mbi.BaseAddress),
                                     buf, mbi.RegionSize, ctypes.byref(rd))
            if ok and rd.value:
                chunk=buf.raw[:rd.value]
                pos=0
                while True:
                    pos=chunk.find(b'mft2',pos)
                    if pos<0: break
                    if pos+52<len(chunk):
                        nin=chunk[pos+8]; nout=chunk[pos+9]; ngrid=chunk[pos+10]
                        if nin==3 and nout==3 and 17<=ngrid<=65:
                            n_in=struct.unpack_from('>H',chunk,pos+48)[0]
                            clut_sz=ngrid**3*nout*2
                            co=pos+52+nin*n_in*2
                            if co+clut_sz<=len(chunk):
                                found.append((ngrid, chunk[co:co+clut_sz]))
                                print(f'    mft2 CLUT found: grid={ngrid} @region+0x{pos:X}')
                    pos+=1
        addr=(mbi.BaseAddress or 0)+mbi.RegionSize
        if addr>=0x7FFFFFFFFFFF: break
    k32.CloseHandle(hp)
    return found

def get_pse_pid():
    import ctypes.wintypes as wt
    TH32CS_SNAPPROCESS=0x2
    class PE(ctypes.Structure):
        _fields_=[("dwSize",wt.DWORD),("cntUsage",wt.DWORD),("th32ProcessID",wt.DWORD),
                  ("th32DefaultHeapID",ctypes.POINTER(ctypes.c_ulong)),
                  ("th32ModuleID",wt.DWORD),("cntThreads",wt.DWORD),
                  ("th32ParentProcessID",wt.DWORD),("pcPriClassBase",ctypes.c_long),
                  ("dwFlags",wt.DWORD),("szExeFile",ctypes.c_char*260)]
    snap=k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)
    pe=PE(); pe.dwSize=ctypes.sizeof(PE)
    pids=[]
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if b'PSEditor' in pe.szExeFile:
                pids.append(pe.th32ProcessID)
            if not k32.Process32Next(snap, ctypes.byref(pe)): break
    k32.CloseHandle(snap)
    return pids

# ── main ──────────────────────────────────────────────────────────────────────
print("Starting PSE ...")
proc = subprocess.Popen([PSE_EXE])
time.sleep(5)

pids = get_pse_pid()
if not pids:
    print("PSE not found in process list — aborting")
    sys.exit(1)
pid = pids[0]
print(f"PSE PID: {pid}")

import win32api, win32con
HWND_PSE = None

for pf2_name, label, style_name in WW_FILES:
    out_path = os.path.join(OUT_DIR, f'{label}.cube')
    if os.path.exists(out_path):
        print(f'SKIP {label}.cube'); continue

    pf2_path = os.path.join(PF2_DIR, pf2_name)
    print(f'\nOpening {pf2_name} ...', flush=True)

    # Open file via shell association → PSE opens it
    subprocess.Popen([PSE_EXE, pf2_path])
    time.sleep(5)  # let PSE load + decrypt

    # Re-get PID (may have new window)
    pids2 = get_pse_pid()
    if pids2: pid = pids2[-1]

    print(f'  Scanning memory (PID {pid}) ...', flush=True)
    results = scan_pid(pid)

    if results:
        # Prefer largest CLUT (most grid nodes)
        ngrid, clut = max(results, key=lambda x: x[0])
        print(f'  Building LUT (grid={ngrid}) ...')
        lut = build_lut_from_clut(clut, ngrid, LUT_SIZE)
        write_cube(lut, LUT_SIZE, out_path,
                   f'Canon Picture Style - {style_name}',
                   f'{pf2_name} (PSE runtime memory extraction)')
        print(f'  -> {label}.cube  [{os.path.getsize(out_path)//1024}KB]')
    else:
        print(f'  No CLUT found for {pf2_name}')

# Close PSE
import signal
proc.terminate()

# Summary
print('\nDone.')
cubes = sorted(f for f in os.listdir(OUT_DIR) if f.endswith('.cube'))
for c in cubes:
    print(f'  {c:45s} {os.path.getsize(os.path.join(OUT_DIR,c))//1024:4d} KB')
