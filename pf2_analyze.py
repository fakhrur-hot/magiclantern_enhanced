import struct

# From disassembly at 0x4BAF:
# 0F B7 16 = MOVZX EDX, WORD PTR [RSI]   ; read version word from file
# 0F B7 CA = MOVZX ECX, DX               ; ECX = version word
# 66 C1 E9 08 = SHR CX, 8               ; shift right 8 -> get HIGH byte
# 88 4C 24 50 = MOV [rsp+0x50], CL       ; store high byte
# 88 54 24 51 = MOV [rsp+0x51], DL       ; store low byte
# 0F B7 4C 24 50 = MOVZX ECX, [rsp+0x50] ; ECX = high byte
# 66 89 4F 12 = MOV WORD PTR [RDI+0x12], CX ; store it
#
# Then later: 66 8B C1 = MOV AX, CX  (CX still = high byte of version)
# AND AX, 0xFF00 -> ?
#
# Wait: CX = high byte of version word, then MOVZX ECX -> ECX = 0x00XX
# 66 8B C1 = MOV AX, CX -> AX = 0x00XX (8-bit value in 16-bit reg)
# AND AX (as EAX), 0xFF00 -> result is 0 if CX < 0x100 (always true for single byte)
#
# WAIT - let me re-read:
# 0F B7 16 = MOVZX EDX, WORD PTR [RSI] -- reads 16-bit word, zero-extends to 32-bit
# For our file: [RSI] = bytes 0x02-0x03 = 20 01 (LE) = EDX = 0x0120
# 0F B7 CA = MOVZX ECX, DX -- ECX = 0x0120 (DX = low 16 bits of EDX)
# 66 C1 E9 08 = SHR CX, 8  -- CX = 0x0001 (shift 0x0120 right by 8 = 0x01)
# 88 4C 24 50 = MOV [rsp+50], CL -- stores 0x01 (low byte of CX = 0x01)
# 88 54 24 51 = MOV [rsp+51], DL -- stores 0x20 (low byte of DX = 0x20)
# 0F B7 4C 24 50 = MOVZX ECX, [rsp+50] -- ECX = 0x0001
# 66 89 4F 12 = MOV [RDI+12], CX -- stores 0x0001
#
# Then at 0x4C2F:
# 66 8B C1 = MOV AX, CX -- AX = 0x0001
# 25 00 FF 00 00 = AND EAX, 0xFF00 -- EAX & 0xFF00 = 0x0001 & 0xFF00 = 0x0000 (!)
#
# 0x0000 != 0x2000 -> v1 comparison fails!
#
# BUT WAIT: what if RSI points to a DIFFERENT offset in the file?
# The code reads [RSI] as the version. Where does RSI point?
#
# The PSP check: the file has "PSP" at offset 4
# If the parser finds PSP and then RSI = file_ptr + 2 (after PSP):
# file[6:8] = 00 00 (after PSP\0)
# If RSI = file_ptr + 4 (PSP itself is at offset 4): file[4:6] = 50 53 = "PS"
# If RSI = file_ptr: file[0:2] = 00 00 -> version word = 0x0000
#
# Let me reconsider: maybe the version check reads from a DIFFERENT offset
# In EU3 code (x64): MOVZX EAX, WORD PTR [EBX+0x0A]
# EBX = file_ptr - 8 (since [EBX+0xC] = PSP)
# [EBX+0x0A] = file_ptr[0x0A - 0x08] = file_ptr[0x02]
# file[0x02:0x04] = 20 01 -> LE = 0x0120
#
# In DPP4Lib code (x64 too based on H.r):
# 0F B7 16 = MOVZX EDX, [RSI] -- what is RSI here?
# We need to trace RSI back to understand what it points to

print("=== File structure re-read ===")
pf2_path = "/mnt/c/Users/Public/Kiro/ML_6D/Fine Detail for older cameras.PF2"
with open(pf2_path, "rb") as f:
    pf2 = f.read()

print(f"Full file hex dump (first 32 bytes):")
for i in range(0, 32, 4):
    print(f"  [{i:02X}]: {pf2[i]:02X} {pf2[i+1]:02X} {pf2[i+2]:02X} {pf2[i+3]:02X}")

# If RSI = file_ptr + 2 (pointing to bytes [02:04] = "20 01"):
rsi_offset = 2
ver_word = struct.unpack_from("<H", pf2, rsi_offset)[0]
print(f"\n[RSI] = file[{rsi_offset}:{rsi_offset+2}] = {pf2[rsi_offset]:02X} {pf2[rsi_offset+1]:02X} = LE: 0x{ver_word:04X}")
# SHR CX, 8: CX = 0x0120 >> 8 = 0x01
cx_after_shift = (ver_word >> 8) & 0xFF
print(f"After SHR 8: CX = 0x{cx_after_shift:02X}")
# MOVZX ECX: ECX = 0x0001
# 66 8B C1: AX = 0x0001
# AND EAX, 0xFF00: 0x0001 & 0xFF00 = 0x0000
print(f"After AND 0xFF00: EAX = 0x{(cx_after_shift & 0xFF00):04X}")

# ALTERNATIVE: what if the version field is at RSI = file_ptr+0
# file[0:2] = 00 00 -> version = 0x0000 -> AND 0xFF00 = 0x0000 -> JE to first handler
print()
print("If RSI = file[0:2] = 00 00:")
print(f"  version = 0x{struct.unpack_from('<H',pf2,0)[0]:04X}")
print(f"  after SHR 8: {pf2[0]>>8:#04x}")
print(f"  after AND 0xFF00: {0x0000 & 0xFF00:#06x}")

# The PSP structure in the file:
# [00-01]: 00 00 -- this could be version field that gives CX = 0x00
# [02-03]: 20 01 -- sub-version or format
# [04-07]: PSP\0 -- magic
# If version at [00:02] = 00 00 -> SHR 8 = 0x00 -> AND 0xFF00 = 0x0000 -> JE +0x3E (first JE)
# The JE at AND result = 0 takes us to... the handler that falls through after the version checks

# Actually wait. If AND result = 0x0000, then:
# AND EAX, 0xFF00 -> EAX = 0
# CMP EAX, 0x2000 -> not equal, no jump
# CMP EAX, 0x3000 -> not equal, no jump
# CMP EAX, 0x4000 -> not equal -> JNZ +0x5A -> ERROR!

# UNLESS the JE at 74 3C takes us somewhere when 0x2000 matched
# Let me trace again. For version 0x0000:
# AND 0xFF00 = 0x0000
# CMP 0x2000 -> not equal -> JE not taken
# CMP 0x3000 -> JE not taken
# CMP 0x4000 -> JNZ taken -> ERROR

# So version 0x0000 also fails!

# Let me look at this completely differently.
# What if the PSP is not at offset 4?
# What if the ENTIRE block [00-0B] is the "PSP header" with:
# [00-03]: 00 00 20 01 = some fields
# [04-07]: PSP\0 = magic confirmed at offset 4
# [08-0B]: 00 00 05 3A = size fields

# And the version that the code reads is at OFFSET 2 RELATIVE TO PSP:
# PSP is at byte 4, so PSP+2 = byte 6 = 00 (null terminator byte of PSP\0)
# OR the version is at an offset relative to start of the STRUCTURE PASSED TO THE FUNCTION
# which might not start at file offset 0

print()
print("=== What offset does the reader use? ===")
print("Testing all 16 possible offsets for version word:")
for off in range(16):
    if off+2 <= len(pf2):
        vw = struct.unpack_from("<H", pf2, off)[0]
        vw_be = struct.unpack_from(">H", pf2, off)[0]
        cx_le = (vw >> 8) & 0xFF
        cx_be = (vw_be >> 8) & 0xFF
        ax_le = cx_le & 0xFF00 # & 0xFF00
        ax_be = cx_be & 0xFF00
        matches = []
        # Direct LE comparison
        if (vw & 0xFF00) == 0x2000: matches.append("v1-direct-LE")
        if (vw & 0xFF00) == 0x3000: matches.append("v2-direct-LE")
        if (vw & 0xFF00) == 0x4000: matches.append("v3-direct-LE")
        # Shifted version
        if ax_le == 0x2000: matches.append("v1-shifted-LE")
        if ax_be == 0x2000: matches.append("v1-shifted-BE")
        if matches:
            print(f"  Offset {off:2d}: LE={vw:#06x} BE={vw_be:#06x} -> {', '.join(matches)}")
        else:
            if vw or vw_be:
                print(f"  Offset {off:2d}: LE={vw:#06x} BE={vw_be:#06x}")
