# Phase 2: Simplified MIDP Porting Plan for PS Vita

## Strategy: Minimal Native Port + x86 Cross-Compilation

### Overview
Instead of porting all MIDP native code to ARM Vita, we'll use a hybrid approach:
1. Compile MIDP C/C++ code on x86 host using `-m32` flag (same approach as phoneme-cldc romgen)
2. Create minimal Vita native layer for display, input, and graphics
3. Link everything into a single Vita ELF binary

### Key Insight
The phoneme-cldc build already uses `-m32 /usr/bin/g++-11` to compile romgen and other host tools. This makes `sizeof(void*)==4`, matching the 32-bit ARM target. We can apply the same approach to MIDP native code.

### Minimal Vita Native Layer

#### 1. Display Management (`lcdui_display.c`)
**Purpose**: Handle Display class operations
**Vita Implementation**: Simple framebuffer management

**Files to create/modify**:
- `phoneme-midp/src/porting/vita/lcdui_display_vita.c` - Vita display implementation
- Integration with phoneme-cldc's existing Vita OS layer

#### 2. Input Handling (`lcdui_input.c`)
**Purpose**: Map Vita controller to J2ME key codes
**Vita Implementation**: SceCtrl → J2ME key mapping

**Files to create/modify**:
- `phoneme-midp/src/porting/vita/lcdui_input_vita.c` - Vita input implementation

#### 3. Basic Graphics (`gxj_graphics.c`)
**Purpose**: Pixel-level drawing operations
**Vita Implementation**: Software rendering to framebuffer

**Files to create/modify**:
- `phoneme-midp/src/porting/vita/gxj_graphics_vita.c` - Vita graphics implementation

### Build Strategy

#### Step 1: Compile MIDP Native Code on x86 Host
```bash
# Use -m32 flag to generate 32-bit code matching ARM target
cd phoneme-midp
export GCC_DIR=/usr/bin
g++-11 -m32 -c src/lowlevelui/graphics/gx_putpixel/native/gxj_graphics.c \
    -Iinclude \
    -o gxj_graphics.o \
    -DLINUX=1 \
    -DVITA=1 \
    -marm -march=armv7-a
```

#### Step 2: Create Vita Native Layer
Create minimal C files that interface with phoneme-cldc's existing Vita port:
- Display: Read from Vita framebuffer
- Input: Poll SceCtrl, map to J2ME codes
- Graphics: Draw to Vita framebuffer using existing libvita2d

#### Step 3: Integrate into phoneme-cldc Build
Modify phoneme-cldc's `vita_arm.mk` to include MIDP object files:
```makefile
# Add MIDP native objects
MIDP_NATIVE_SRCS += $(WorkSpace)/phoneme-midp/src/porting/vita/*.c

# Add to compilation
CFLAGS += -DUSE_MIDP=1
```

#### Step 4: Build and Package
```bash
# Build the complete Vita JVM + MIDP
make -C phoneme-cldc/build/vita_arm IsTarget=true

# Generate VPK
vita-elf-create -s input.elf output.velf
vita-make-fself input.velf output.fself
vita-pack-vpk -s param.sfo -b eboot.bin output.vpk
```

### Critical Native Files (Prioritized)

Based on the analysis, these are the MOST critical MIDP native files to port:

| Priority | Subsystem | File | Reason |
|----------|-----------|------|--------|
| 1 | AMS | `midpNativeAppManagerPeer.c` | MIDlet lifecycle management |
| 2 | LCUI | `lcdui_display.c` | Display refresh |
| 3 | LCUI | `lcdui_input.c` | Key input handling |
| 4 | LowLevelUI | `gxj_graphics.c` | Basic pixel drawing |
| 5 | LowLevelUI | `gxj_putpixel.c` | Single pixel operations |
| 6 | Core | `midpMalloc.c` | Memory management |
| 7 | Core | `midp_string.c` | String operations |

### Implementation Plan

#### Phase 2A: Build Infrastructure (1 week)
1. Create `phoneme-midp/src/porting/vita/` directory
2. Create minimal native layer files (display, input, graphics)
3. Modify phoneme-cldc build to include MIDP objects
4. Test compilation with `-m32`

#### Phase 2B: AMS Port (2 weeks)
1. Port `midpNativeAppManagerPeer.c` for Vita
2. Implement MIDlet lifecycle management
3. Test HelloWorld MIDlet

#### Phase 2C: LCUI Port (3 weeks)
1. Implement display refresh
2. Map Vita controller to J2ME keys
3. Basic graphics operations

#### Phase 2D: Graphics Backend (2 weeks)
1. Implement gxj_graphics operations
2. Software rendering to framebuffer
3. Optimize for Vita performance

### Success Criteria

| Metric | Target |
|--------|--------|
| Build time | < 30 minutes |
| Binary size | < 2MB |
| HelloWorld MIDlet | Runs on Vita |
| Canvas drawing | Basic shapes display |
| Input response | Controller buttons work |
| Memory usage | < 10MB runtime |

### Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Too many native files to port | Use -m32 compilation on x86, only port critical files |
| Performance too slow | Start with software rendering, optimize later |
| Memory constraints | Use -m32 to keep sizeof(void*)==4, optimize memory |
| JNI overhead | Minimize JNI calls, integrate closely with phoneme-cldc |

### Next Immediate Actions

1. ✅ Create `phoneme-midp/src/porting/vita/` directory structure
2. ✅ Create `lcdui_display_vita.c` - minimal display implementation
3. ✅ Create `lcdui_input_vita.c` - minimal input implementation  
4. ✅ Create `gxj_graphics_vita.c` - minimal graphics implementation
5. ✅ Modify phoneme-cldc `vita_arm.mk` to include MIDP objects
6. ✅ Test compilation with `-m32 g++-11`

---
*This plan prioritizes getting a working MIDP port quickly over complete feature coverage.*