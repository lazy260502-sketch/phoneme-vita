# midp-vita - J2ME/MIDP Emulator for PS Vita

J2ME/MIDP (phoneME) emulator running on PlayStation Vita.

## Project Structure

```
midp-vita/
├── CMakeLists.txt          # Main build configuration
├── build_midp_vita.sh      # Unified build script
├── src/                    # Vita-specific main.c
├── build/                  # Build output (generated)
│   └── midp_vita.vpk       # Final VPK
└── ../phoneme-cldc/        # CLDC VM (phoneME)
└── ../phoneme-midp/        # MIDP implementation (phoneME)
```

## Quick Build

```bash
# From this directory
./build_midp_vita.sh           # Build everything
./build_midp_vita.sh clean     # Clean rebuild
```

The script automatically:
1. **Detects source changes** in `Universe.cpp` (CLDC) and `midp_run.c` (MIDP)
2. **Recompiles** only what's needed
3. **Updates static libraries** (`libcldc_vm_g.a`, `libobj.a`)
4. **Builds VPK** and copies to Windows test folder

## Manual Build

If you need finer control:

```bash
# 1. Update CLDC if Universe.cpp was modified
cd /home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm
arm-vita-eabi-ar d dist/lib/libcldc_vm_g.a _MergedSrc004.o
# ... (rebuild MergedSrc004.o) ...
arm-vita-eabi-ar r dist/lib/libcldc_vm_g.a _MergedSrc004.o

# 2. Update MIDP if midp_run.c was modified
cd /home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm
arm-vita-eabi-ar d obj/arm/libobj.a midp_run.o
# ... (rebuild midp_run.c) ...
arm-vita-eabi-ar r obj/arm/libobj.a midp_run.o

# 3. Build VPK
cd /home/zyb/vitasdk/samples/j2me/midp-vita/build
cmake .. && make
```

## Required Patches

The following patches have been applied to upstream phoneME:

### 1. CLDC - Skip HiddenPackage check on Vita
**File**: `phoneme-cldc/src/vm/share/handles/Universe.cpp`
- Skip `jvm_class_must_be_hidden` fatal error on Vita
- The flag isn't set because `ENABLE_ROM_GENERATOR=0`

### 2. CLDC - Fix JVMSPI_PrintRaw signature
**File**: `phoneme-cldc/src/vm/share/runtime/jvmspi.h`
- Vita's `Main_vita.cpp` provides `JVMSPI_PrintRaw(const char*)`
- Header must match for Vita builds

### 3. MIDP - Bypass getClassPathPlus for INTERNAL_SUITE_ID
**File**: `phoneme-midp/src/ams/ams_base_cldc/reference/native/midp_run.c`
- Use `additionalPath` directly when `jarPath` is empty
- Fixes `MIDP_ERROR_OUT_MEM` during startup

## Testing

VPK output: `build/midp_vita.vpk`

Copy to Vita3K's auto-load directory and test. Output goes to `ux0:data/J2ME00001/log.txt`.

## Debug Symbols

Key debug points (use `fprintf(stderr, ...)` with `fflush(stderr)`):

- `phoneme-midp/src/ams/ams_base_cldc/reference/native/midp_run.c` - Midlet execution
- `phoneme-midp/src/porting/vita/lcdui_display_vita.c` - Display rendering
- `phoneme-midp/src/porting/vita/lcdui_input_vita.c` - Input handling
