"""
fuji_recipe_to_lut.py
Converts Fujifilm camera recipes to .cube 3D LUTs.

Recipe format (CSV columns):
  Label, FilmSimulation, Grain, CCFx/CCFxB, WhiteBalance,
  WBShiftR, WBShiftB, DynamicRange, HighlightTone, ShadowTone,
  Color, Sharpness, NoiseReduction, ExposureBias

LUT pipeline per node:
  sRGB in → exposure bias → WB shift → film sim base
  → highlight/shadow tone → color saturation → sRGB out
"""

import csv, math, os, sys, io

LUT_SIZE = 33
OUT_DIR  = '/mnt/c/Users/Public/Kiro/ML_6D/docs/luts/fuji_recipes'
os.makedirs(OUT_DIR, exist_ok=True)

# ── math helpers ──────────────────────────────────────────────────────────────

def clamp(v, lo=0.0, hi=1.0): return max(lo, min(hi, v))

def srgb_to_linear(v):
    v = clamp(v)
    return v/12.92 if v <= 0.04045 else ((v+0.055)/1.055)**2.4

def linear_to_srgb(v):
    v = clamp(v)
    return 12.92*v if v <= 0.0031308 else 1.055*v**(1/2.4)-0.055

def lerp(a, b, t): return a + (b-a)*t

def smoothstep(edge0, edge1, x):
    t = clamp((x-edge0)/(edge1-edge0+1e-9))
    return t*t*(3-2*t)

# ── colour helpers ────────────────────────────────────────────────────────────

def rgb_to_hsl(r, g, b):
    mx, mn = max(r,g,b), min(r,g,b)
    l = (mx+mn)/2
    if mx == mn: return 0.0, 0.0, l
    d = mx-mn
    s = d/(2-mx-mn) if l > 0.5 else d/(mx+mn)
    if mx == r:   h = (g-b)/d + (6 if g < b else 0)
    elif mx == g: h = (b-r)/d + 2
    else:         h = (r-g)/d + 4
    return h/6, s, l

def hsl_to_rgb(h, s, l):
    if s == 0: return l, l, l
    def hue2rgb(p, q, t):
        if t < 0: t += 1
        if t > 1: t -= 1
        if t < 1/6: return p+(q-p)*6*t
        if t < 1/2: return q
        if t < 2/3: return p+(q-p)*(2/3-t)*6
        return p
    q = l*(1+s) if l < 0.5 else l+s-l*s
    p = 2*l-q
    return hue2rgb(p,q,h+1/3), hue2rgb(p,q,h), hue2rgb(p,q,h-1/3)

def adjust_saturation(r, g, b, delta):
    """delta in [-4, +4] maps to saturation multiplier."""
    h, s, l = rgb_to_hsl(r, g, b)
    factor = 1.0 + delta * 0.18
    s = clamp(s * factor)
    return hsl_to_rgb(h, s, l)

def kelvin_to_rgb_shift(kelvin):
    """Approximate red/blue multipliers for a given Kelvin temperature (vs. 5500K neutral)."""
    neutral = 5500.0
    ratio   = kelvin / neutral
    # Warmer = more red, less blue; cooler = less red, more blue
    r_mult = clamp(ratio**0.5,  0.7, 1.3)
    b_mult = clamp((1/ratio)**0.5, 0.7, 1.3)
    g_mult = 1.0
    return r_mult, g_mult, b_mult

WB_KELVIN = {
    'auto': 5500, 'daylight': 5500, 'shade': 7500, 'cloudy': 6500,
    'tungsten': 3200, 'incandescent': 3200,
    'fluorescent': 4000, 'flight1': 3800, 'flight2': 4200,
    'uwater': 7200,
}

def parse_wb(wb_str):
    """Return (r_mult, g_mult, b_mult) for white balance string."""
    s = wb_str.strip().lower().replace('-', '').replace(' ', '')
    if s[-1] == 'k' and s[:-1].isdigit():
        k = int(s[:-1])
    else:
        k = WB_KELVIN.get(s, 5500)
    return kelvin_to_rgb_shift(k)

def apply_wb_shift(r, g, b, shift_r, shift_b):
    """WBShiftR/B: -9..+9. Each step ≈ 100K-equivalent tint."""
    r = clamp(r * (1.0 + shift_r * 0.025))
    b = clamp(b * (1.0 + shift_b * 0.025))
    return r, g, b

# ── tone curve ────────────────────────────────────────────────────────────────

def apply_highlight_tone(v, tone):
    """tone: -2 (soft rolloff) to +4 (hard/bright).
    Affects pixels above ~0.55."""
    if abs(tone) < 0.05: return v
    if v < 0.5: return v
    t  = (v - 0.5) / 0.5          # 0..1 in highlight region
    if tone < 0:
        # Soft: compress highlights toward white, preserving detail
        factor = 1.0 + tone * 0.06   # tone=-2 → factor=0.88
        return 0.5 + t * 0.5 * factor
    else:
        # Hard/boost: push highlights brighter
        factor = 1.0 + tone * 0.04
        return clamp(0.5 + t * 0.5 * factor)

def apply_shadow_tone(v, tone):
    """tone: -2 (lifted/faded) to +4 (deep/crushed).
    Affects pixels below ~0.45."""
    if abs(tone) < 0.05: return v
    if v > 0.5: return v
    t = v / 0.5                    # 0..1 in shadow region
    if tone < 0:
        # Lifted: raise shadow floor
        lift = -tone * 0.04
        return lift + v * (1 - lift)
    else:
        # Deep: pull shadows toward black
        factor = 1.0 - tone * 0.04
        return clamp(v * factor)

def apply_exposure_bias(r, g, b, bias_ev):
    """Apply EV shift in linear light."""
    if abs(bias_ev) < 0.01: return r, g, b
    factor = 2.0 ** bias_ev
    rl = srgb_to_linear(r) * factor
    gl = srgb_to_linear(g) * factor
    bl = srgb_to_linear(b) * factor
    return linear_to_srgb(rl), linear_to_srgb(gl), linear_to_srgb(bl)

# ── base film simulation ──────────────────────────────────────────────────────

FILM_SIM_PARAMS = {
    # (contrast_shadow, contrast_highlight, saturation_delta, r_tint, g_tint, b_tint, is_bw, bw_filter)
    # r/g/b_tint: multiplicative on linear light
    'Provia':      (0.0,  0.0,  0.0,  1.00, 1.00, 1.00, False, None),
    'Velvia':      (0.3,  0.1,  1.5,  1.03, 1.00, 0.98, False, None),
    'Astia':       (-0.3, -0.2, -0.7, 1.01, 1.00, 1.00, False, None),
    'Classic':     (0.1,  -0.2, -1.5, 1.00, 0.99, 0.97, False, None),   # Classic Chrome
    'ClassicNEGA': (-0.2, -0.3, -2.0, 1.01, 1.00, 0.98, False, None),   # Classic Neg
    'Eterna':      (-0.5, -0.4, -0.5, 0.99, 1.00, 1.01, False, None),
    'NEGA':        (-0.2, -0.2, -0.5, 1.00, 1.00, 1.00, False, None),   # Pro Neg Std
    'NEGAhi':      (0.0,  -0.1, 0.0,  1.01, 1.00, 1.00, False, None),   # Pro Neg Hi
    'Reala':       (0.0,  0.0,  0.3,  1.01, 1.00, 1.00, False, None),
    'Sepia':       (0.0,  0.0,  0.0,  1.00, 1.00, 1.00, False, 'sepia'),
    # B&W simulations
    'Acros':       (0.1,  0.0,  0.0,  1.00, 1.00, 1.00, True,  None),
    'AcrosG':      (0.1,  0.0,  0.0,  0.87, 1.15, 0.87, True,  None),   # green filter
    'AcrosR':      (0.1,  0.0,  0.0,  1.15, 0.87, 0.87, True,  None),   # red filter
    'AcrosY':      (0.1,  0.0,  0.0,  1.08, 1.04, 0.87, True,  None),   # yellow filter
    'BW':          (0.0,  0.0,  0.0,  1.00, 1.00, 1.00, True,  None),   # Standard mono
    'BG':          (0.0,  0.0,  0.0,  0.87, 1.13, 0.87, True,  None),   # Green filter
    'BR':          (0.0,  0.0,  0.0,  1.13, 0.87, 0.87, True,  None),   # Red filter
    'BYe':         (0.0,  0.0,  0.0,  1.07, 1.04, 0.87, True,  None),   # Yellow filter
}

def apply_film_sim(r, g, b, sim_name):
    params = FILM_SIM_PARAMS.get(sim_name, FILM_SIM_PARAMS['Provia'])
    c_shadow, c_highlight, sat_delta, rt, gt, bt, is_bw, bw_filter = params

    # Apply tint (in linear light)
    rl = srgb_to_linear(r) * rt
    gl = srgb_to_linear(g) * gt
    bl = srgb_to_linear(b) * bt
    r2 = linear_to_srgb(rl)
    g2 = linear_to_srgb(gl)
    b2 = linear_to_srgb(bl)

    # Apply base tone contrast adjustments
    if c_shadow != 0:
        r2 = apply_shadow_tone(r2, c_shadow * 2)
        g2 = apply_shadow_tone(g2, c_shadow * 2)
        b2 = apply_shadow_tone(b2, c_shadow * 2)
    if c_highlight != 0:
        r2 = apply_highlight_tone(r2, c_highlight * 2)
        g2 = apply_highlight_tone(g2, c_highlight * 2)
        b2 = apply_highlight_tone(b2, c_highlight * 2)

    # B&W conversion
    if is_bw:
        # Luminosity with channel filter weights already in linear
        lum = 0.2126*rl + 0.7152*gl + 0.0722*bl
        grey = clamp(linear_to_srgb(lum))
        if bw_filter == 'sepia':
            return (clamp(grey * 1.1), clamp(grey * 0.9), clamp(grey * 0.65))
        return grey, grey, grey

    # Saturation
    if sat_delta != 0:
        r2, g2, b2 = adjust_saturation(r2, g2, b2, sat_delta)

    return r2, g2, b2

# ── exposure bias parser ──────────────────────────────────────────────────────

def parse_exposure_bias(s):
    """Parse strings like '0', 'P0P33', 'P1P00', 'M0M33', 'M1P00' → float EV."""
    s = str(s).strip().upper()
    if s == '0' or s == '': return 0.0
    # Format: P=plus digit P=point digit digit | M=minus digit M=point ...
    sign = 1 if s[0] == 'P' else -1
    # e.g. P0P33 → 0.33,  P1P00 → 1.00,  M0M33 → -0.33
    parts = s.replace('P', '.').replace('M', '.').lstrip('.')
    try:
        # e.g. "0.33" from "P0P33", "1.00" from "P1P00"
        nums = parts.split('.')
        whole = int(nums[0]) if nums[0] else 0
        frac  = int(nums[1])/100.0 if len(nums) > 1 and nums[1] else 0.0
        return sign * (whole + frac)
    except:
        return 0.0

def parse_grain(s):
    """Returns (has_grain, strength) — not applicable to LUT but noted."""
    s = str(s).strip().upper()
    return s not in ('OFF', '', '0')

def parse_ccfx(s):
    """CCFx / CCFxB: 'WEAK/OFF', 'STRONG/WEAK', etc. Returns (ccfx, ccfxb)."""
    parts = str(s).strip().upper().split('/')
    def to_val(p):
        p = p.strip()
        if p in ('OFF', ''): return 0
        if p == 'WEAK':      return 1
        if p == 'STRONG':    return 2
        return 0
    ccfx  = to_val(parts[0]) if parts else 0
    ccfxb = to_val(parts[1]) if len(parts) > 1 else 0
    return ccfx, ccfxb

def apply_color_chrome(r, g, b, ccfx_strength, ccfxb_strength):
    """Color Chrome Effect: enhances richness in saturated colors."""
    if ccfx_strength == 0 and ccfxb_strength == 0:
        return r, g, b
    h, s, l = rgb_to_hsl(r, g, b)
    if s < 0.1:   # no effect on near-neutral
        return r, g, b
    if ccfx_strength > 0:
        # Subtle saturation+depth boost in mids
        depth = ccfx_strength * 0.04
        l2 = l - depth * s * (1 - abs(2*l-1))
        s2 = clamp(s * (1 + ccfx_strength * 0.06))
        r, g, b = hsl_to_rgb(h, s2, clamp(l2))
    if ccfxb_strength > 0:
        # Blue-specific effect: subtle hue/saturation shift toward cooler blues
        blue_range = abs(math.sin(h * 2 * math.pi))  # peaks near hue=0.5 (blue)
        if blue_range > 0.3:
            h2 = h - ccfxb_strength * 0.01 * blue_range
            r, g, b = hsl_to_rgb(h2, s, l)
    return r, g, b

# ── main LUT builder ──────────────────────────────────────────────────────────

def build_recipe_lut(recipe, size=LUT_SIZE):
    sim    = recipe['FilmSimulation'].strip()
    wb_str = recipe['WhiteBalance'].strip()
    wb_r   = int(recipe.get('WBShiftR', 0) or 0)
    wb_b   = int(recipe.get('WBShiftB', 0) or 0)
    hl     = float(recipe.get('HighlightTone', 0) or 0)
    sh     = float(recipe.get('ShadowTone', 0) or 0)
    color  = float(recipe.get('Color', 0) or 0)
    ev     = parse_exposure_bias(recipe.get('ExposureBias', '0'))
    ccfx, ccfxb = parse_ccfx(recipe.get('CCFx/CCFxB', 'OFF'))

    # WB base multipliers (from kelvin)
    wb_mult = parse_wb(wb_str)

    lut = []
    for bi in range(size):
        for gi in range(size):
            for ri in range(size):
                r = ri / (size-1)
                g = gi / (size-1)
                b = bi / (size-1)

                # 1. Exposure bias
                if ev != 0:
                    r, g, b = apply_exposure_bias(r, g, b, ev)

                # 2. White balance (base kelvin)
                rl = srgb_to_linear(r) * wb_mult[0]
                gl = srgb_to_linear(g) * wb_mult[1]
                bl = srgb_to_linear(b) * wb_mult[2]
                r  = linear_to_srgb(rl)
                g  = linear_to_srgb(gl)
                b  = linear_to_srgb(bl)

                # 3. WB shift (red/blue tint)
                r, g, b = apply_wb_shift(r, g, b, wb_r, wb_b)

                # 4. Film simulation base (tone + colour matrix + B&W)
                r, g, b = apply_film_sim(r, g, b, sim)

                # 5. Highlight/shadow tone adjustments
                if hl != 0:
                    r = apply_highlight_tone(r, hl)
                    g = apply_highlight_tone(g, hl)
                    b = apply_highlight_tone(b, hl)
                if sh != 0:
                    r = apply_shadow_tone(r, sh)
                    g = apply_shadow_tone(g, sh)
                    b = apply_shadow_tone(b, sh)

                # 6. Color saturation
                if color != 0:
                    r, g, b = adjust_saturation(r, g, b, color)

                # 7. Color Chrome effects
                if ccfx or ccfxb:
                    r, g, b = apply_color_chrome(r, g, b, ccfx, ccfxb)

                lut.append((clamp(r), clamp(g), clamp(b)))
    return lut

def write_cube(lut, size, path, title, comment=''):
    with open(path, 'w') as f:
        f.write(f'TITLE "{title}"\n')
        if comment: f.write(f'# {comment}\n')
        f.write(f'LUT_3D_SIZE {size}\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n\n')
        for r, g, b in lut:
            f.write(f'{r:.6f} {g:.6f} {b:.6f}\n')

# ── CLI entry point ───────────────────────────────────────────────────────────

def sanitise(s):
    return ''.join(c if c.isalnum() or c in '-_' else '_' for c in s)

def process_csv(csv_path):
    with open(csv_path, newline='', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        recipes = list(reader)
    print(f'Loaded {len(recipes)} recipes from {csv_path}')
    for rec in recipes:
        label   = rec.get('Label', 'Unknown').strip()
        sim     = rec.get('FilmSimulation', 'Provia').strip()
        fname   = sanitise(f'{sim}_{label}') + '.cube'
        outpath = os.path.join(OUT_DIR, fname)
        if os.path.exists(outpath):
            print(f'  SKIP {fname}')
            continue
        print(f'  Building {fname} ...', end='', flush=True)
        lut = build_recipe_lut(rec)
        write_cube(lut, LUT_SIZE, outpath,
                   f'Fujifilm Recipe: {label} ({sim})',
                   f'FilmSim={sim} HL={rec.get("HighlightTone",0)} '
                   f'SH={rec.get("ShadowTone",0)} Col={rec.get("Color",0)} '
                   f'WB={rec.get("WhiteBalance","Auto")} '
                   f'WBR={rec.get("WBShiftR",0)} WBB={rec.get("WBShiftB",0)}')
        print(f' {os.path.getsize(outpath)//1024}KB')

def process_single(recipe_dict):
    label = recipe_dict.get('Label', 'Recipe').strip()
    sim   = recipe_dict.get('FilmSimulation', 'Provia').strip()
    fname = sanitise(f'{sim}_{label}') + '.cube'
    out   = os.path.join(OUT_DIR, fname)
    print(f'Building {fname} ...')
    lut = build_recipe_lut(recipe_dict)
    write_cube(lut, LUT_SIZE, out, f'Fujifilm: {label}',
               f'FilmSim={sim}')
    print(f'Done -> {out}  [{os.path.getsize(out)//1024}KB]')

# ── run ───────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    if len(sys.argv) > 1:
        process_csv(sys.argv[1])
    else:
        # Embedded demo — the full DemoPreset.csv content
        DEMO_CSV = """\
Label,FilmSimulation,Grain,CCFx/CCFxB,WhiteBalance,WBShiftR,WBShiftB,DynamicRange,HighlightTone,ShadowTone,Color,Sharpness,NoisReduction,ExposureBias
RR Classic Chrome,Classic,-1,WEAK/OFF,Auto,1,-1,200,-1,1,2,2,-2,P0P67
RR Velvia,Velvia,WEAK,OFF,Auto,1,-1,200,-1,0,2,2,-2,P0P67
RR Kodak Portra 160,Classic,WEAK,OFF,Daylight,4,-5,AUTO,-2,-2,1,-2,-4,P1P00
RR Eterna,NEGA,WEAK,OFF,Auto,2,2,400,-2,-1,-4,1,-4,P1P00
RR CineStill 800T,NEGA,STRONG,OFF,3200K,0,0,200,3,1,-1,1,-3,P0P33
MT Acros,Acros,OFF,OFF,Auto,0,0,100,-2,0,0,0,0,0
RR Fujicolor Pro 400H,NEGA,WEAK,OFF,Auto,2,1,200,0,3,4,0,-3,P0P67
GB Colour,Velvia,OFF,OFF,Auto,0,0,100,-1,2,2,2,-1,0
VM Classic Chrome,Classic,OFF,OFF,Auto,0,-1,200,1,2,2,-2,-2,0
RR Ektachrome 100SW,Velvia,WEAK,OFF,Auto,3,-4,200,1,2,-1,0,-3,P0P33
"""
        reader = csv.DictReader(io.StringIO(DEMO_CSV))
        recipes = list(reader)
        print(f'Running demo with {len(recipes)} recipes ...')
        for rec in recipes:
            label   = rec.get('Label','').strip()
            sim     = rec.get('FilmSimulation','Provia').strip()
            fname   = sanitise(f'{sim}_{label}') + '.cube'
            outpath = os.path.join(OUT_DIR, fname)
            print(f'  {fname} ...', end='', flush=True)
            lut = build_recipe_lut(rec)
            write_cube(lut, LUT_SIZE, outpath,
                       f'Fujifilm Recipe: {label}',
                       f'FilmSim={sim} HL={rec.get("HighlightTone",0)} SH={rec.get("ShadowTone",0)}')
            print(f' {os.path.getsize(outpath)//1024}KB')
        print(f'\nDone. LUTs in {OUT_DIR}')
        print('Usage: python fuji_recipe_to_lut.py recipes.csv')
