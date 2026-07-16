# Canon Picture Style — Magic Lantern 6D Reference

**Camera:** Canon EOS 6D (firmware 1.1.6)  
**Processor:** Digic 5  
**ML Build:** 2026-05-19.6D.116  
**Source:** ML source `src/property.h`, `src/picstyle.c`, forum topic 16299

---

## Active Style Selector

**Property:** `PROP_PICTURE_STYLE`  
**Hex ID:** `0x80000028`  
**Signed int32 (Lua):** `-2147483608`  
**Size:** 4 bytes  
**Canon internal name:** `PROP_FLAVOR_MODE`

### Style Values

| Style | Hex | Decimal |
|-------|-----|---------|
| Standard | `0x81` | 129 |
| Portrait | `0x82` | 130 |
| Landscape | `0x83` | 131 |
| Neutral | `0x84` | 132 |
| Faithful | `0x85` | 133 |
| Monochrome | `0x86` | 134 |
| Auto | `0x87` | 135 |
| User Def 1 | `0x21` | 33 |
| User Def 2 | `0x22` | 34 |
| User Def 3 | `0x23` | 35 |

### Switch Style via Lua

```lua
-- 0x80000028 overflows int32; use signed equivalent
local PROP_PICTURE_STYLE = -2147483608

-- Switch to Neutral
property[PROP_PICTURE_STYLE]:request_change(0x84, 4)

-- Switch to User Def 1
property[PROP_PICTURE_STYLE]:request_change(0x21, 4)
```

> **Requires** `LUA_PROP_REQUEST_CHANGE` enabled in `lua_property.c` (enabled in this build).

---

## Per-Style Parameter Properties

Each style has its own property ID holding a 24-byte parameter struct.  
**Scheme:** Digic 4/5 (`CONFIG_DIGIC_45` / `CONFIG_DIGIC_V`)

| Style | Property ID | Signed int32 (Lua) |
|-------|-------------|-------------------|
| Standard | `0x02060001` | `33947649` |
| Portrait | `0x02060002` | `33947650` |
| Landscape | `0x02060003` | `33947651` |
| Neutral | `0x02060004` | `33947652` |
| Faithful | `0x02060005` | `33947653` |
| Monochrome | `0x02060006` | `33947654` |
| User Def 1 | `0x02060007` | `33947655` |
| User Def 2 | `0x02060008` | `33947656` |
| User Def 3 | `0x02060009` | `33947657` |

> These IDs are below `0x7FFFFFFF` so they do **not** overflow int32 — no signed conversion needed in Lua.

---

## Parameter Struct Layout

**Size:** 24 bytes (0x18)  
**Sliders:** 4 (Digic 5 — no fineness/threshold)  
**Source:** `src/picstyle.c` line 86, `NUM_PICSTYLE_SLIDERS == 4`

```
Offset  Field               Type     Range
------  ------------------  -------  ----------
0x00    contrast            int32    -4 .. +4
0x04    sharpness           uint32    0 .. 7
0x08    saturation          int32    -4 .. +4
0x0C    color_tone          int32    -4 .. +4
0x10    (unused)            uint32   0xDEADBEEF
0x14    (unused)            uint32   0xDEADBEEF
```

### Notes
- Monochrome style replaces `saturation` with **filter effect** and `color_tone` with **toning effect**
- The struct is always written as a complete 24-byte block — partial writes are not supported
- Canon validates the struct size; mismatched length is silently ignored

---

## Reading Parameters via ML C API

ML exposes a global array populated by property handlers:

```c
// Defined in picstyle.c
struct picstyle_settings_prop_struct picstyle_settings[NUM_PICSTYLES];

// Access helpers (from picstyle.h)
int get_sharpness(int pic_style_index);
int get_contrast(int pic_style_index);
int get_saturation(int pic_style_index);
int get_color_tone(int pic_style_index);

void set_sharpness(int pic_style_index, int value);
void set_contrast(int pic_style_index, int value);
void set_saturation(int pic_style_index, int value);
void set_color_tone(int pic_style_index, int value);
```

---

## Lua Limitations

The Lua `property[id]:request_change(value, len)` API takes a **single integer value**.  
Writing the full 24-byte picstyle struct from Lua is not directly possible with this mechanism.

| Operation | Via Lua | Method |
|-----------|---------|--------|
| Switch active style | ✅ | `property[PROP_PICTURE_STYLE]:request_change(0x84, 4)` |
| Read current style | ✅ | `property[PROP_PICTURE_STYLE].handler = function(v) ... end` |
| Write individual parameter | ❌ | Struct write required — not supported by Lua API |
| Write full parameter struct | ❌ | Would need TCC C scripting or native ML module |

### Practical Lua Alternative — Use ML Menu

```lua
-- Switch style via ML menu (safe, no property bus)
menu.set("Shoot", "Picture Style", "Neutral")
menu.set("Shoot", "Picture Style", "User 1")
```

> Menu item name and tab may vary — verify on camera by navigating ML menu first.

---

## User-Defined Slots and Extended Styles

User Def slots 1–3 (`0x21–0x23`) support loading Canon `.PF2` picture style files  
created with **Canon Picture Style Editor** software.

`.PF2` files contain data beyond the 4 camera-side sliders:
- Full tone curves (per-channel RGB + luminance)
- Hue rotation tables
- Saturation matrices

This gives curves-level JPEG rendering control while appearing as a normal User Def style on the camera. The 4-slider property struct shown above is only the **summary representation** that Canon writes back after loading a `.PF2`.

**Forum source:** [Reverse Engineering Picture Styles — topic 16299](https://www.magiclantern.fm/forum/index.php?topic=16299.0) (15 pages, community reverse engineering of the `.PF2` binary format)

---

## Property ID Quick Reference (6D Digic 5)

| Property | Hex ID | Signed int32 | Size |
|----------|--------|--------------|------|
| `PROP_PICTURE_STYLE` (active) | `0x80000028` | `-2147483608` | 4 B |
| `PROP_PICSTYLE_SETTINGS_STANDARD` | `0x02060001` | `33947649` | 24 B |
| `PROP_PICSTYLE_SETTINGS_PORTRAIT` | `0x02060002` | `33947650` | 24 B |
| `PROP_PICSTYLE_SETTINGS_LANDSCAPE` | `0x02060003` | `33947651` | 24 B |
| `PROP_PICSTYLE_SETTINGS_NEUTRAL` | `0x02060004` | `33947652` | 24 B |
| `PROP_PICSTYLE_SETTINGS_FAITHFUL` | `0x02060005` | `33947653` | 24 B |
| `PROP_PICSTYLE_SETTINGS_MONOCHROME` | `0x02060006` | `33947654` | 24 B |
| `PROP_PICSTYLE_SETTINGS_USERDEF1` | `0x02060007` | `33947655` | 24 B |
| `PROP_PICSTYLE_SETTINGS_USERDEF2` | `0x02060008` | `33947656` | 24 B |
| `PROP_PICSTYLE_SETTINGS_USERDEF3` | `0x02060009` | `33947657` | 24 B |
| `PROP_PICSTYLE_OF_USERDEF1` (base) | `0x0206000a` | `33947658` | 4 B |
| `PROP_PICSTYLE_OF_USERDEF2` (base) | `0x0206000b` | `33947659` | 4 B |
| `PROP_PICSTYLE_OF_USERDEF3` (base) | `0x0206000c` | `33947660` | 4 B |

---

## HTP Interaction

When `PROP_HTP = 0x8000004a` is enabled (`1`), Canon forces:
- Minimum ISO to **200** (ISO 100 unavailable)
- Tone curve head room extended — highlights rendered with more detail
- ALO is automatically disabled if HTP is active in some modes

This interacts with picture style tone rendering. Neutral or Faithful styles  
combined with HTP ON gives the flattest in-camera JPEG suitable for post-grading.

---

*Generated from ML source analysis — `src/property.h`, `src/picstyle.c`, `src/picstyle.h`*  
*Forum reference: [topic 16299](https://www.magiclantern.fm/forum/index.php?topic=16299.0)*
