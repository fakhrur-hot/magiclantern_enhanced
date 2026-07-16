import struct, math

pf2_path = "/mnt/c/Users/Public/Kiro/ML_6D/Fine Detail for older cameras.PF2"
dpp_path = "/mnt/c/Program Files/Canon/Picture Style Editor/DPP4Lib/EdsCFParse.dll"

with open(pf2_path, "rb") as f:
    pf2 = f.read()
with open(dpp_path, "rb") as f:
    dpp = f.read()

# Find v1 reader function in DPP4Lib
# From context at 0x4C4F: E8 FD 69 02 00 at offset 0x4C54
call_off = 0x4C54
rel32 = struct.unpack_from('<i', dpp, call_off+1)[0]
v1_reader = call_off + 5 + rel32
print(f"v1 reader at 0x{v1_reader:08X}")

print("\n=== v1 Reader (512 bytes) ===")
ctx = dpp[v1_reader:v1_reader+512]
for i in range(0, len(ctx), 16):
    row = ctx[i:i+16]
    h = " ".join(f"{b:02X}" for b in row)
    a = "".join(chr(b) if 32<=b<127 else "." for b in row)
    print(f"  0x{v1_reader+i:08X}: {h:<48}  |{a}|")

print()
# The file version field at bytes [1:3] as LE = 0x2000 -> v1
# Payload starts at offset 12 (after 12-byte file header)
payload = pf2[12:]
print(f"Payload: {len(payload)} bytes")

# Known PF2 v1 plaintext structure (from ML community reverse engineering):
# [0x00]  2 bytes: style name length (num UTF-16LE chars)
# [0x02]  N*2 bytes: style name (UTF-16LE)
# after name: parameter block
#   int32 contrast     (-4..4)
#   uint32 sharpness   (0..7)
#   int32 saturation   (-4..4)
#   int32 color_tone   (-4..4)
# Then: tone curve data (multiple channels, each with control points)

print("\n=== Attempting v1 plaintext decode ===")

# Try: first 2 bytes = name length
name_len_raw = struct.unpack_from('<H', payload, 0)[0]
print(f"Possible name length: {name_len_raw}")
if 1 <= name_len_raw <= 64:
    name_end = 2 + name_len_raw * 2
    try:
        name = payload[2:name_end].decode('utf-16-le')
        print(f"Name: '{name}'")
        # After name: read parameters
        off = name_end
        if off + 16 <= len(payload):
            c, sh, sa, ct = struct.unpack_from('<4i', payload, off)
            print(f"  @0x{off:04X}: Contrast={c} Sharpness={sh} Saturation={sa} ColorTone={ct}")
    except Exception as e:
        print(f"Name decode failed: {e}")

# Try with different offsets for name
for name_off in [0, 2, 4, 6, 8]:
    try:
        nl = struct.unpack_from('<H', payload, name_off)[0]
        if 3 <= nl <= 40:
            name_bytes = payload[name_off+2:name_off+2+nl*2]
            name = name_bytes.decode('utf-16-le')
            if all(32 <= ord(c) <= 126 for c in name):
                print(f"\nName at offset {name_off} (len={nl}): '{name}'")
                # Read parameters after name
                param_off = name_off + 2 + nl * 2
                if param_off + 16 <= len(payload):
                    c, sh, sa, ct = struct.unpack_from('<4i', payload, param_off)
                    print(f"  Params @0x{param_off:04X}: C={c} S={sh} Sa={sa} CT={ct}")
                    # Check if valid
                    if 0 <= sh <= 7 and abs(c) <= 4 and abs(sa) <= 4 and abs(ct) <= 4:
                        print(f"  *** VALID SLIDER VALUES ***")
    except:
        pass

# Try big-endian name length
for name_off in [0, 2, 4]:
    try:
        nl = struct.unpack_from('>H', payload, name_off)[0]
        if 3 <= nl <= 40:
            name_bytes = payload[name_off+2:name_off+2+nl*2]
            # Try both utf-16 endiannesses
            for enc in ['utf-16-le', 'utf-16-be']:
                try:
                    name = name_bytes.decode(enc)
                    if all(32 <= ord(c) <= 126 for c in name) and len(name) > 2:
                        print(f"\nBE name @{name_off} (len={nl}, {enc}): '{name}'")
                except:
                    pass
    except:
        pass

# Brute force: scan for "Fine Detail" string in any encoding
print("\n=== Searching for 'Fine Detail' string ===")
search_terms = [
    b'Fine Detail',
    'Fine Detail'.encode('utf-16-le'),
    'Fine Detail'.encode('utf-16-be'),
    b'FineDetail',
]
for term in search_terms:
    pos = payload.find(term)
    if pos >= 0:
        print(f"  Found '{term}' at offset 0x{pos:04X}")
        print(f"  Context: {payload[max(0,pos-8):pos+len(term)+16].hex()}")

# Try raw float interpretation
print("\n=== Float scan (tone curve values) ===")
float_vals = []
for off in range(0, len(payload)-4, 4):
    f = struct.unpack_from('<f', payload, off)[0]
    if 0.0 <= f <= 1.0:
        float_vals.append((off, f))

print(f"Float values in [0,1] range: {len(float_vals)}")
if float_vals:
    print(f"First 20: {[(hex(o), round(v,3)) for o,v in float_vals[:20]]}")
    # Consecutive floats in [0,1] would indicate tone curve
    consecutive = []
    for i in range(len(float_vals)-1):
        if float_vals[i+1][0] - float_vals[i][0] == 4:
            consecutive.append(float_vals[i])
    print(f"Consecutive floats: {len(consecutive)}")

# Also try: the file body has a Canon-specific binary format
# where each "section" has a tag and length
# Try TLV (type-length-value) decode
print("\n=== TLV/section scan ===")
off = 0
while off < len(payload) - 8:
    tag = struct.unpack_from('<I', payload, off)[0]
    length = struct.unpack_from('<I', payload, off+4)[0]
    if 0 < length < len(payload) - off - 8 and length % 4 == 0:
        print(f"  @0x{off:04X}: tag=0x{tag:08X} len={length}")
        if length <= 32:
            data = payload[off+8:off+8+length]
            print(f"    data: {data.hex()}")
        off += 8 + length
    else:
        off += 1
