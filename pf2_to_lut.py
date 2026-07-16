import struct, math, os

icc_path   = '/mnt/c/Program Files/Canon/Picture Style Editor/DPP4Lib/icc/FDS.ICC'
srgb_path  = '/mnt/c/Program Files/Canon/Picture Style Editor/DPP4Lib/icc/sRGB Profile.icc'
out_path   = '/mnt/c/Users/Public/Kiro/ML_6D/docs/FineDetail_6D.cube'

# ── helpers ──────────────────────────────────────────────────────────────────

def read_u16be(buf, off): return struct.unpack_from('>H', buf, off)[0]
def read_u8(buf, off):    return buf[off]

def clamp01(v): return max(0.0, min(1.0, v))

# ICC Lab ↔ XYZ (D50 whitepoint)
D50 = (0.9642, 1.0000, 0.8249)

def lab_to_xyz(L, a, b):
    fy = (L + 16) / 116
    fx = a / 500 + fy
    fz = fy - b / 200
    kappa, epsilon = 903.3, 0.008856
    X = fx**3 if fx**3 > epsilon else (116*fx - 16) / kappa
    Y = ((L + 16)/116)**3 if L > kappa*epsilon else L/kappa
    Z = fz**3 if fz**3 > epsilon else (116*fz - 16) / kappa
    return X * D50[0], Y * D50[1], Z * D50[2]

def xyz_to_srgb_linear(X, Y, Z):
    # D50-adapted XYZ → linear sRGB (Bradford chromatic adaptation included)
    r =  3.2404542*X - 1.5371385*Y - 0.4985314*Z
    g = -0.9692660*X + 1.8760108*Y + 0.0415560*Z
    b =  0.0556434*X - 0.2040259*Y + 1.0572252*Z
    return r, g, b

def linear_to_srgb(v):
    v = clamp01(v)
    if v <= 0.0031308:
        return 12.92 * v
    return 1.055 * (v ** (1/2.4)) - 0.055

# ── parse mft2 A2B0 ──────────────────────────────────────────────────────────

with open(icc_path, 'rb') as f:
    icc = f.read()

num_tags = struct.unpack_from('>I', icc, 128)[0]
a2b_off = None
for i in range(num_tags):
    base = 132 + i * 12
    sig = icc[base:base+4]
    if sig == b'A2B0':
        a2b_off = struct.unpack_from('>I', icc, base+4)[0]
        break

assert a2b_off is not None, 'A2B0 tag not found'

nin   = read_u8(icc, a2b_off + 8)
nout  = read_u8(icc, a2b_off + 9)
ngrid = read_u8(icc, a2b_off + 10)
# entry counts are at byte 48 and 50 (AFTER the 36-byte matrix at offset 12)
n_in  = read_u16be(icc, a2b_off + 48)
n_out = read_u16be(icc, a2b_off + 50)

print(f'ICC: {os.path.basename(icc_path)}')
print(f'  Grid {ngrid}^{nin} → {nout} ch  |  in_entries={n_in}  out_entries={n_out}')

# Data offsets within the tag
HDR = 52  # 4 type + 4 reserved + 1+1+1+1 + 36 matrix + 2+2 counts
in_table_bytes  = nin  * n_in  * 2
clut_bytes      = (ngrid**nin) * nout * 2
out_table_bytes = nout * n_out * 2
clut_start = a2b_off + HDR + in_table_bytes

print(f'  CLUT offset in file: 0x{clut_start:08X}  size={clut_bytes} B')

def read_clut_lab(r_idx, g_idx, b_idx):
    """Read Lab from the 33x33x33 CLUT. ICC stores R-fastest."""
    node = r_idx * ngrid * ngrid + g_idx * ngrid + b_idx
    off  = clut_start + node * nout * 2
    L_raw = read_u16be(icc, off)
    a_raw = read_u16be(icc, off + 2)
    b_raw = read_u16be(icc, off + 4)
    # ICC Lab encoding: L = val/65535 * 100, a/b = val/65535 * 255 - 128
    L = L_raw / 65535.0 * 100.0
    a = a_raw / 65535.0 * 255.0 - 128.0
    b = b_raw / 65535.0 * 255.0 - 128.0
    return L, a, b

# ── build 33³ LUT grid (input = sRGB gamma-encoded) ─────────────────────────

LUT_SIZE = 33
print(f'\nBuilding {LUT_SIZE}^3 LUT ...')

lut = []
for bi in range(LUT_SIZE):
    for gi in range(LUT_SIZE):
        for ri in range(LUT_SIZE):
            # Normalised input (0..1, sRGB gamma)
            r_in = ri / (LUT_SIZE - 1)
            g_in = gi / (LUT_SIZE - 1)
            b_in = bi / (LUT_SIZE - 1)

            # Look up in ICC CLUT (33-node grid, bilinear trilinear interp)
            # Fast path: exact grid nodes
            r_f = r_in * (ngrid - 1)
            g_f = g_in * (ngrid - 1)
            b_f = b_in * (ngrid - 1)

            r0, r1 = int(r_f), min(int(r_f) + 1, ngrid - 1)
            g0, g1 = int(g_f), min(int(g_f) + 1, ngrid - 1)
            b0, b1 = int(b_f), min(int(b_f) + 1, ngrid - 1)

            tr = r_f - r0
            tg = g_f - g0
            tb = b_f - b0

            # Trilinear interpolation over 8 corners
            def get(ri2, gi2, bi2):
                return read_clut_lab(ri2, gi2, bi2)

            def lerp3(v000, v001, v010, v011, v100, v101, v110, v111):
                # interpolate 3 channels together
                result = []
                for ch in range(3):
                    c000 = v000[ch]; c001 = v001[ch]; c010 = v010[ch]; c011 = v011[ch]
                    c100 = v100[ch]; c101 = v101[ch]; c110 = v110[ch]; c111 = v111[ch]
                    c = (c000*(1-tr)*(1-tg)*(1-tb) + c100*tr*(1-tg)*(1-tb) +
                         c010*(1-tr)*tg*(1-tb)      + c110*tr*tg*(1-tb) +
                         c001*(1-tr)*(1-tg)*tb       + c101*tr*(1-tg)*tb +
                         c011*(1-tr)*tg*tb           + c111*tr*tg*tb)
                    result.append(c)
                return result

            L, a, b = lerp3(
                get(r0,g0,b0), get(r0,g0,b1),
                get(r0,g1,b0), get(r0,g1,b1),
                get(r1,g0,b0), get(r1,g0,b1),
                get(r1,g1,b0), get(r1,g1,b1)
            )

            # Lab → XYZ → linear sRGB → gamma sRGB
            X, Y, Z = lab_to_xyz(L, a, b)
            rl, gl, bl2 = xyz_to_srgb_linear(X, Y, Z)
            r_out = linear_to_srgb(rl)
            g_out = linear_to_srgb(gl)
            b_out = linear_to_srgb(bl2)

            lut.append((clamp01(r_out), clamp01(g_out), clamp01(b_out)))

    if bi % 4 == 0:
        print(f'  slice {bi+1}/{LUT_SIZE} done')

# ── write .cube ───────────────────────────────────────────────────────────────

os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, 'w') as f:
    f.write('TITLE "Fine Detail for older cameras (Canon 6D / Digic5)"\n')
    f.write('# Generated from FDS.ICC (Canon Picture Style Editor)\n')
    f.write('# Profile: Fine Detail sRGB - scanner input to Lab PCS\n')
    f.write('# Conversion: ICC Lab -> XYZ D50 -> linear sRGB -> gamma sRGB\n')
    f.write(f'LUT_3D_SIZE {LUT_SIZE}\n')
    f.write('DOMAIN_MIN 0.0 0.0 0.0\n')
    f.write('DOMAIN_MAX 1.0 1.0 1.0\n\n')
    for r, g, b in lut:
        f.write(f'{r:.6f} {g:.6f} {b:.6f}\n')

print(f'\nDone → {out_path}')
print(f'Entries: {len(lut)} ({LUT_SIZE}^3 = {LUT_SIZE**3})')

# ── quick sanity check ────────────────────────────────────────────────────────
print('\nSanity check (should be near 0,0,0 / 0.5,0.5,0.5 / 1,1,1):')
for test_r, test_g, test_b in [(0,0,0),(16,16,16),(32,32,32)]:
    idx = test_r * LUT_SIZE * LUT_SIZE + test_g * LUT_SIZE + test_b
    print(f'  input ({test_r},{test_g},{test_b}) -> {lut[idx]}')
