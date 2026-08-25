# phoneME CLDC-HI JVM for PS Vita - Development Plan

## Project Overview
Porting Sun phoneME CLDC-HI JVM to PlayStation Vita with full J2ME game support.

---

## 1. Current Status (Phase 1: Complete ✅)

### Completed
- [x] phoneme-cldc JVM core ported to PS Vita
- [x] Cross-generator issues resolved with -m32 approach
- [x] All compilation errors fixed
- [x] All linking errors resolved
- [x] VPK package generated successfully
- [x] Basic CLDC 1.1 support implemented

### Current Output
- **Binary**: `build/vita_arm/target/debug/../../dist/bin/cldc_vm_g`
- **VPK**: `build/vita_arm/target/debug/cldc_vm_g.vpk`
- **Status**: Can run basic Java programs, but **cannot run J2ME games yet**

---

## 2. Architecture Understanding

### J2ME Stack

```
┌─────────────────────────────────────────────────────────┐
│                    J2ME Application                        │
│                  (MIDlet, Game, etc.)                      │
├─────────────────────────────────────────────────────────┤
│                 MIDP Layer (phoneme-midp)                │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │   LCUI      │  │    IO       │  │  MIDlet     │    │
│  │  Graphics   │  │ Connections │  │  Lifecycle   │    │
│  │  Canvas     │  │  HTTP       │  │  Management  │    │
│  └─────────────┘  └─────────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────┤
│                 CLDC Layer (phoneme-cldc) ✅              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  java.lang  │  │  java.io    │  │  java.util  │    │
│  │  Object     │  │  InputStream│  │  Vector      │    │
│  │  String     │  │  OutputStream│  │  Hashtable   │    │
│  └─────────────┘  └─────────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────┤
│                 JVM Core (phoneme-cldc) ✅               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  Interpreter │  │ Memory Mgmt │  │  GC         │    │
│  │  ARM Thumb  │  │  Heap        │  │  Mark-Sweep │    │
│  └─────────────┘  └─────────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────┤
│              Platform Specific (Vita) ✅                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  OS Port    │  │  Threads     │  │  File I/O   │    │
│  │  os_port.h  │  │  pthread     │  │  SceIo      │    │
│  └─────────────┘  └─────────────┘  └─────────────┘    │
└─────────────────────────────────────────────────────────┘
```

### Component Relationships

```
phoneme-cldc (Current Focus ✅)
├── JVM Core
│   ├── Interpreter (ARM)
│   ├── Memory Management
│   ├── Garbage Collection
│   └── Class Loading
│
├── CLDC API
│   ├── java.lang.*
│   ├── java.io.*
│   └── java.util.*
│
└── Platform Port
    ├── OS Interface
    ├── Thread Management
    └── File System

phoneme-midp (Next Phase 🎯)
├── MIDP API
│   ├── javax.microedition.lcdui.*
│   ├── javax.microedition.io.*
│   └── javax.microedition.midlet.*
│
├── AMS (Application Management System)
│   └── MIDlet Suite Management
│
├── LCUI Implementation
│   ├── Display Management
│   ├── Graphics Rendering
│   └── Input Handling
│
└── Native Integration
    ├── Graphics (SceGxm)
    ├── Input (SceCtrl)
    └── Audio (SceAudio)

jamvm (Alternative JVM ⚠️)
└── Independent JVM Implementation
    (Can replace phoneme-cldc interpreter)
```

---

## 3. Development Phases

### Phase 1: Foundation (COMPLETED ✅)
**Goal**: Port phoneme-cldc to PS Vita

- [x] Set up Vita SDK environment
- [x] Resolve cross-generator issues (-m32 approach)
- [x] Fix all compilation errors
- [x] Fix all linking errors
- [x] Generate VPK package
- [x] Verify basic JVM functionality

**Deliverables**:
- Working `cldc_vm_g` binary
- VPK package ready for deployment

---

### Phase 2: MIDP Support (NEXT 🎯)
**Goal**: Enable J2ME game execution

#### 2.1 MIDP Core (High Priority)
- [ ] Port phoneme-midp to Vita
- [ ] Implement MIDlet lifecycle management
- [ ] Add MIDlet suite support
- [ ] Implement AMS (Application Management System)

#### 2.2 LCUI Implementation (High Priority)
- [ ] Port Display class
- [ ] Implement Canvas/Graphics rendering
- [ ] Map Vita controls to J2ME input
- [ ] Implement GameCanvas for games

#### 2.3 I/O Support (Medium Priority)
- [ ] Implement HttpConnection
- [ ] Implement SocketConnection
- [ ] Implement FileConnection

#### 2.4 Platform Integration (High Priority)
- [ ] SceGxm graphics backend
- [ ] SceCtrl input mapping
- [ ] SceAudio sound support

**Deliverables**:
- MIDP-enabled JVM
- Basic J2ME game support
- Input and graphics working

---

### Phase 3: Optimization (Future)
**Goal**: Improve performance and compatibility

- [ ] JIT compilation support
- [ ] Memory optimization
- [ ] Performance profiling
- [ ] Game compatibility testing

---

### Phase 4: jamvm Integration (Optional)
**Goal**: Alternative JVM implementation

- [ ] Port jamvm to Vita
- [ ] Compare performance with phoneme
- [ ] Choose optimal JVM for Vita

---

## 4. Key Components to Port

### 4.1 phoneme-midp Components

#### Critical (Must Have for Games)
```
javax.microedition.midlet.*
├── MIDlet.java              # MIDlet base class
├── MIDletStateChangeException.java
└── MIDletSuite.java         # Suite management

javax.microedition.lcdui.*
├── Canvas.java              # Base canvas
├── GameCanvas.java          # Game-specific canvas
├── Graphics.java            # Drawing operations
├── Display.java             # Display management
├── Displayable.java         # Displayable objects
├── Form.java                # UI forms
├── Image.java               # Image handling
├── Font.java                # Font support
└── Command.java             # User commands

javax.microedition.io.*
├── Connection.java
├── HttpConnection.java
├── SocketConnection.java
└── InputConnection.java
```

#### Important (For Full Compatibility)
```
javax.microedition.rms.*       # Record Management System
├── RecordStore.java
├── RecordStoreException.java
└── RecordEnumeration.java

javax.microedition.media.*    # Media support
├── Player.java
├── Manager.java
└── Control.java
```

### 4.2 Native Integration Points

#### Graphics (SceGxm)
```c
// Required for LCUI implementation
#include <psp2/gxm.h>

// Display setup
SceGxmContext* gxmContext;
SceGxmRenderTarget* renderTarget;

// Canvas rendering
void renderCanvas(Canvas* canvas, Graphics* g) {
    // Convert Java Graphics calls to SceGxm
}
```

#### Input (SceCtrl)
```c
// Required for input mapping
#include <psp2/ctrl.h>

// Button mapping to J2ME keys
typedef struct {
    int vitaButton;      // SCE_CTRL_*
    int j2meKeyCode;    // Canvas key codes
} KeyMapping;

KeyMapping keyMap[] = {
    {SCE_CTRL_CROSS,     KEY_NUM0},      // A button
    {SCE_CTRL_CIRCLE,    KEY_NUM1},      // B button
    {SCE_CTRL_SQUARE,    KEY_NUM2},      // X button
    {SCE_CTRL_TRIANGLE,  KEY_NUM3},      // Y button
    {SCE_CTRL_DPAD_UP,   KEY_UP},        // D-pad up
    {SCE_CTRL_DPAD_DOWN, KEY_DOWN},      // D-pad down
    {SCE_CTRL_DPAD_LEFT, KEY_LEFT},      // D-pad left
    {SCE_CTRL_DPAD_RIGHT,KEY_RIGHT},     // D-pad right
};
```

#### Audio (SceAudio)
```c
// Required for sound support
#include <psp2/audioout.h>

// Audio port setup
SceAudioOutPort* audioPort;

// Play sound from MIDP
void playSound(byte[] soundData) {
    // Convert to SceAudio format and play
}
```

---

## 5. Implementation Strategy

### 5.1 MIDP Porting Steps

#### Step 1: Build phoneme-midp
```bash
# Configure for Vita
cd phoneme-midp
./configure --target=arm-vita-eabi --with-cldc=../phoneme-cldc
make
```

#### Step 2: Integrate with phoneme-cldc
```
# Update build configuration
# Add MIDP library to linking
LIBRARIES += -lcldc_vm_midp_g
```

#### Step 3: Platform Integration
```
# Create Vita-specific implementations
src/midp/platform/vita/
├── Display_vita.cpp
├── Graphics_vita.cpp
├── Input_vita.cpp
└── Audio_vita.cpp
```

### 5.2 Graphics Implementation

#### Architecture
```
Java Layer (MIDP)
    ↓
JNI Bridge
    ↓
Native Layer (C++)
    ↓
SceGxm API
    ↓
Vita GPU
```

#### Key Functions to Implement
```c
// Display management
JNIEXPORT void JNICALL Java_javax_microedition_lcdui_Display_initNative
  (JNIEnv* env, jobject obj);

// Graphics rendering
JNIEXPORT void JNICALL Java_javax_microedition_lcdui_Graphics_drawPixel
  (JNIEnv* env, jobject obj, jint x, jint y, jint color);

// Canvas rendering
JNIEXPORT void JNICALL Java_javax_microedition_lcdui_Canvas_paint
  (JNIEnv* env, jobject obj);
```

### 5.3 Input Implementation

#### Button Mapping Strategy
```
J2ME Key Codes → Vita Buttons
├── KEY_NUM0 → CROSS (A)
├── KEY_NUM1 → CIRCLE (B)
├── KEY_NUM2 → SQUARE (X)
├── KEY_NUM3 → TRIANGLE (Y)
├── KEY_UP → DPAD_UP
├── KEY_DOWN → DPAD_DOWN
├── KEY_LEFT → DPAD_LEFT
├── KEY_RIGHT → DPAD_RIGHT
├── KEY_FIRE → CROSS (A)
└── KEY_SOFT1/2 → L/R triggers
```

#### Input Polling
```c
void pollInput() {
    SceCtrlData ctrlData;
    sceCtrlPeekBufferPositive(0, &ctrlData, 1);
    
    for (int i = 0; i < sizeof(keyMap)/sizeof(keyMap[0]); i++) {
        if (ctrlData.buttons & keyMap[i].vitaButton) {
            // Set J2ME key state
            setKeyState(keyMap[i].j2meKeyCode, true);
        }
    }
}
```

---

## 6. Testing Strategy

### 6.1 Test Applications

#### Basic Tests
1. **Hello World MIDlet** - Basic MIDlet lifecycle
2. **Canvas Test** - Simple drawing
3. **Input Test** - Button detection
4. **Graphics Test** - Shapes, colors, fonts

#### Game Tests
1. **Simple Game** - Basic game loop
2. **Sprite Test** - Sprite rendering
3. **Collision Test** - Collision detection
4. **Scrolling Test** - Background scrolling

### 6.2 Test Cases

```java
// Test 1: Basic MIDlet
public class HelloMIDlet extends MIDlet {
    public void startApp() {
        Display.getDisplay(this).setCurrent(new Form("Hello"));
    }
    
    public void pauseApp() {}
    public void destroyApp(boolean unconditional) {}
}

// Test 2: Canvas Test
public class CanvasTest extends MIDlet {
    public void startApp() {
        Display.getDisplay(this).setCurrent(new TestCanvas(this));
    }
    
    class TestCanvas extends Canvas {
        public void paint(Graphics g) {
            g.setColor(0xFF0000); // Red
            g.fillRect(0, 0, getWidth(), getHeight());
        }
    }
}

// Test 3: Input Test
public class InputTest extends MIDlet {
    public void startApp() {
        Display.getDisplay(this).setCurrent(new InputCanvas(this));
    }
    
    class InputCanvas extends Canvas {
        public void paint(Graphics g) {
            g.drawString("Press buttons", 10, 10, Graphics.TOP | Graphics.LEFT);
        }
        
        public void keyPressed(int keyCode) {
            System.out.println("Key pressed: " + keyCode);
        }
    }
}
```

---

## 7. File Structure

### Current Structure
```
phoneme-cldc/
├── build/
│   └── vita_arm/
│       ├── target/
│       │   └── debug/
│       │       ├── bin/cldc_vm_g          # Final binary
│       │       ├── cldc_vm_g.velf         # VELF format
│       │       ├── cldc_vm_g.fself        # FSELF format
│       │       └── cldc_vm_g.vpk          # VPK package
│       └── loopgen/
│       └── romgen/
│
├── src/
│   ├── javaapi/cldc1.1/                 # CLDC API
│   ├── vm/                             # JVM Core
│   │   ├── share/                      # Shared code
│   │   ├── os/                         # OS-specific
│   │   │   └── vita/                   # Vita port
│   │   │       ├── BuildFlags_vita.hpp
│   │   │       ├── Main_vita.cpp
│   │   │       ├── OS_vita.hpp
│   │   │       ├── OsFile_vita.cpp
│   │   │       ├── OsMemory_vita.cpp
│   │   │       ├── OsMisc_vita.cpp
│   │   │       ├── OsSocket_vita.cpp
│   │   │       └── os_port.h
│   │   └── ...
│   └── anilib/                          # ANILib
│       ├── share/
│       │   ├── ani.h
│       │   ├── ani.cpp
│       │   ├── ani_bsd_socket.cpp
│       │   └── ...
│       └── vita/                       # Vita ANILib port
│           └── os_port.cpp
└── build/
```

### Target Structure (After MIDP Port)
```
phoneme-midp/
├── build/
│   └── vita_arm/
│       └── target/
│           └── debug/
│               └── bin/cldc_vm_midp_g    # MIDP library
│
├── src/
│   ├── javaapi/midp/                  # MIDP API
│   │   ├── javax/microedition/
│   │   │   ├── lcdui/
│   │   │   ├── io/
│   │   │   └── midlet/
│   │
│   └── midp/                           # MIDP Core
│       ├── ams/
│       ├── event/
│       ├── lcdui/
│       └── platform/
│           └── vita/                   # Vita MIDP port
│               ├── Display_vita.cpp
│               ├── Graphics_vita.cpp
│               ├── Input_vita.cpp
│               └── Audio_vita.cpp
└── build/
```

---

## 8. Dependencies

### Vita SDK Libraries
```
libSceGxm_stub.a        # Graphics
libSceCtrl_stub.a      # Input
libSceAudio_stub.a     # Audio
libSceDisplay_stub.a   # Display
libSceSysmodule_stub.a # System
libSceKernel_stub.a    # Kernel
libc.a                 # C Library
libstdc++.a           # C++ Library
libm.a                 # Math Library
libpthread.a           # Threads
```

### Build Dependencies
```
arm-vita-eabi-gcc      # Cross compiler
arm-vita-eabi-g++      # Cross compiler (C++)
arm-vita-eabi-as       # Assembler
arm-vita-eabi-ld       # Linker
vita-elf-create        # VELF creator
vita-make-fself        # FSELF creator
vita-pack-vpk          # VPK packager
```

---

## 9. Risk Assessment

### High Risk Items
1. **Graphics Performance** - SceGxm is complex, may have performance issues
2. **Memory Constraints** - Vita has limited RAM, need careful management
3. **Input Latency** - Input polling may introduce latency
4. **Audio Sync** - Audio timing may be challenging

### Medium Risk Items
1. **MIDP Compatibility** - Some MIDP features may not be fully supported
2. **Game Compatibility** - Different games may have different requirements
3. **Performance** - Interpretation may be slow for complex games

### Low Risk Items
1. **Basic MIDlet Support** - Should be straightforward
2. **Simple Graphics** - Basic drawing operations should work
3. **Basic Input** - Button mapping should be simple

---

## 10. Timeline Estimation

### Phase 2: MIDP Support (4-6 weeks)
- Week 1-2: Port phoneme-midp core
- Week 3: Implement basic LCUI
- Week 4: Implement input system
- Week 5: Implement graphics backend
- Week 6: Testing and bug fixing

### Phase 3: Optimization (2-4 weeks)
- Week 1-2: Performance profiling
- Week 3-4: Optimization and testing

### Phase 4: jamvm Integration (Optional, 2-3 weeks)
- Week 1: Port jamvm
- Week 2: Integration and testing
- Week 3: Performance comparison

---

## 11. Success Criteria

### Phase 2 (MIDP Support)
- [ ] Basic MIDlet can start and run
- [ ] Canvas rendering works
- [ ] Input detection works
- [ ] Simple game can run
- [ ] Graphics performance acceptable

### Phase 3 (Optimization)
- [ ] Performance meets expectations
- [ ] Memory usage optimized
- [ ] Game compatibility > 80%

### Phase 4 (jamvm)
- [ ] jamvm compiles and runs
- [ ] Performance comparison complete
- [ ] Optimal JVM selected

---

## 12. Next Steps

### Immediate (This Week)
1. [ ] Review phoneme-midp source code
2. [ ] Identify Vita-specific requirements
3. [ ] Set up phoneme-midp build for Vita
4. [ ] Create basic platform integration files

### Short Term (Next 2 Weeks)
1. [ ] Port phoneme-midp core
2. [ ] Implement basic MIDlet lifecycle
3. [ ] Create test MIDlets
4. [ ] Begin LCUI implementation

### Medium Term (Next Month)
1. [ ] Complete LCUI implementation
2. [ ] Implement input system
3. [ ] Implement graphics backend
4. [ ] Begin testing with real games

---

## 13. References

### Documentation
- [PS Vita SDK Documentation](https://vitasdk.org)
- [phoneME Project Documentation](https://phoneme.dev.java.net)
- [J2ME CLDC Specification](https://jcp.org/en/jsr/detail?id=30)
- [J2ME MIDP Specification](https://jcp.org/en/jsr/detail?id=37)

### Source Code
- phoneme-cldc: `/home/zyb/vitasdk/samples/j2me/phoneme-cldc`
- phoneme-midp: `/home/zyb/vitasdk/samples/j2me/phoneme-midp`
- jamvm: `/home/zyb/vitasdk/samples/j2me/jamvm`

### Tools
- Vita SDK: `/home/zyb/.local/vitasdk`
- Build scripts: `build/vita_arm/vita_arm.cfg`
- Makefiles: `build/share/jvm.make`

---

## 14. Appendix: Useful Commands

### Build Commands
```bash
# Clean build
cd build/vita_arm/target/debug
make clean
make

# Build specific component
make cldc_vm_g

# Build with verbose output
make V=1
```

### Vita Tools
```bash
# Create VELF
vita-elf-create input.elf output.velf

# Create FSELF
vita-make-fself input.velf output.fself

# Create VPK
vita-pack-vpk -s param.sfo -b eboot.bin output.vpk
```

### Debug Commands
```bash
# Check ELF info
arm-vita-eabi-readelf -h binary.elf

# Check symbols
arm-vita-eabi-nm binary.elf

# Check size
arm-vita-eabi-size binary.elf
```

---

**Document Version**: 1.0  
**Last Updated**: 2026-08-25  
**Author**: DeepSeek AI Assistant  
**Status**: Phase 1 Complete, Phase 2 Planning