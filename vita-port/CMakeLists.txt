cmake_minimum_required(VERSION 3.16)

#==============================================================================
# vita-port - J2ME/MIDP on PS Vita (clean rebuild of the platform layer)
#
# Differences from midp-vita/CMakeLists.txt:
#   * sources are this directory's src/*.c (display/input/main/pcsl)
#   * libobj_no_main.a additionally drops:
#       - lfjport_fb_export.o  (empty lfjport_refresh -> src/vita_display.c)
#       - mastermode_export.o  (empty checkForSystemSignal -> src/vita_input.c)
#   * old vita_stubs.o is NOT linked (replaced by src/vita_pcsl.c)
#   * packaged internal.config/system.config come from ./config (240x320)
#==============================================================================

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(midp_vita C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

#==============================================================================
# Configuration
#==============================================================================
set(VITA_APP_NAME "J2ME Player")
set(VITA_TITLEID  "J2ME00001")
# 02.70 (2026-10-21): boot white-screen - read the jar once into RAM.
#   log113 (v02.67), round 0: round_begin -> first_content_flip = 10.0 s, of
#   which the boot preload (load_all_in_classpath) is 5.59 s and 4.49 s of that
#   is io_read: n=3892 at 1.15 ms each, ~8 reads per class actually loaded.
#   BufferedFile::BUFFERSIZE is 256 B and find_entry (central directory at the
#   end of the jar) alternates with open_entry/read (local header + data) on
#   one FILE, so nearly every refill is a real sceIoRead.
#   FIX phoneme-cldc OsFile_vita.cpp: OsFile_open(<x>.jar, "rb") reads the
#   whole file with a few large reads into malloc'd RAM and serves
#   OsFile_read/seek/length/eof/close from memory. Caps: 4 MB per jar, 8 MB
#   total, 8 images; over a cap / malloc or read failure => the original
#   fopen path. Images are refcounted per path (JarFileParser reopens).
#   Other files, and any non-"rb" open, are untouched. A/B switch: put '0' in
#   ux0:/data/J2ME00001/jar_mem.cfg to disable.
#   NOT verified on hardware. Expected in vm_boot.log: "[jarmem] enabled=1",
#   "[jarmem] loaded <jar> size=.. us=.." for midp_system.jar and the app jar,
#   and in "[preload] sys(VM thread, delta)": io_read n collapsing from ~3900
#   to a handful, total_us from ~5.6 s towards the resolve-only ~1.6-2 s.

# ============================================================================
# 04.25 (2026-11-03): log183 - 极品飞车 still freezes on the loading screen, and
#   for the first time the stack points at code we wrote rather than at a
#   constant. Two defects in Object3D.duplicate() plus one in find().
#   PART 1 - v04.22 CONFIRMED. "java.lang.Error:  95" (== IllegalAccessError
#   off Appearance.setLayer) is gone. The title is now 49 bytes further along.
#   PART 2 - THE NEW FAILURE:
#       [TIMEREXC] task v@e16b5918 cancelled: java.lang.NullPointerException:   0
#        - ab.a(), bci=135
#        - x.q(), bci=196
#        - x.a(), bci=1608
#        - ab.a(), bci=688
#   ("  0" is empty_message - this build prints the ErrorMsgTag enum index, see
#   the 04.22 block; NPE is the exception itself, not the tag.)
#   Decompiled the title:
#       x.q() bci=196 -> ab.a(int, Node x8, boolean)
#       ab.a() bci=75  : node14 = ac.a(3)          // resource 3 == "/d"
#       ab.a() bci=104 : node14.find(12)           -> g16
#       ab.a() bci=116 : node14.find(13)           -> g15
#       ab.a() bci=135 : g16.addChild(...)         <- NPE, so g16 was null
#   ac.a(3) is `((Object3D) Loader.load(o.a[3])[0]).duplicate()` - measured from
#   ac.class, o.a[3] == "/d" (the title's own string table).
#   DEFECT A: duplicate() dropped the user ID. The "/d" object is Group userID=12,
#   so find(12) was searching for a CHILD with ID 12, found nothing, returned
#   null, and addChild(null) threw. duplicate() now carries the ID over in ONE
#   place (the return of duplicate()) so no per-type branch can forget it. The
#   title corroborates the intent: ab.b(Node) explicitly does duplicate() then
#   setUserID(getUserID()).
#   DEFECT B: duplicate() of a Group returned a CHILDLESS Group. A loaded model
#   duplicated for instancing was an empty shell - nothing under it drew, and
#   find() on the copy could see nothing. Now the children are duplicated too
#   (recursively, which is safe: addChild rejects a node that already has a
#   parent and an M3G file is a tree). Vertex/index data is still SHARED, which
#   is what the spec asks for and keeps duplicate() cheap.
#   DEFECT C: find() could not leave its own sub-tree. Measured on "/d": it is
#   ONE tree of 9 Groups, and the two anchors the title wants (userID 12 and 13)
#   are nodes IN it - the 12 node is a leaf, the root is a third Group.
#   Loader.load("/d")[0] therefore hands the title the 12 node, not the root, so
#   a strict sub-tree search can never reach 13. bci=855 does exactly the same
#   addChild with the find(13) result, so fixing only bci=135 would have moved
#   the crash 720 bytes. find() now falls back to (1) the root of the tree the
#   object belongs to and (2) the sibling top-level roots of the same load.
#   The fallback is stored on the object itself (Object3D.sceneRoots), so a
#   duplicated copy keeps it, and it cannot cross files because user IDs are
#   unique within one .m3g.
#   WHY NOT JUST PUT THE ROOT AT INDEX 0 - deliberately NOT done. v04.08 made
#   index 0 the first Node in FILE order because 极品飞车 addresses objects by
#   POSITION in this array. Measured: /aq's first Node is index 0 but its first
#   ROOT is index 3, and /af, /bl are the same shape. Re-sorting would re-target
#   every position-addressed lookup and would bring back the log177
#   ClassCastException / log179 wrong-texture class of bug. The ordering is
#   untouched; only find() got smarter.
#   VERIFIED ON HOST (no device needed, all 28 .m3g in debug/3d_game):
#     - every file parses, readAll()s and duplicates every Node: 28 ok, 0 fail
#     - "/d" end-to-end simulation of the title's sequence:
#         model=objs[0].duplicate() -> id=12
#         find(12) -> Group id=12      (was NULL before A)
#         find(13) -> Group id=13      (was NULL before B+C)
#         addChild on both -> OK       (both bci 135 and bci 855 clear)
#     - 生化危机's files still load (they have a World, so index 0 is unchanged)
#   NOT verified on device. What is claimed: the two NPE sites in this title's
#   loading path are gone, and no .m3g in the sample set regressed.
#   STILL MISSING, found while measuring and not fixed here: duplicate() of a
#   World goes through the Group branch and returns a plain Group (type and the
#   activeCamera/background are lost). No title in the sample set duplicates a
#   World, so it is recorded rather than changed blind.

# (superseded by the 04.26 set() below)
# 04.26 (2026-11-03): log184 - two defects, both mine, both fixed here.
#   (1) SECOND-ROUND CRASH (regression v04.18 -> v04.21). log182 (v04.20) ran
#       several rounds fine; log183 (v04.23) and log184 (v04.25) die on EVERY
#       round after the first with
#           [heap] chunk alloc FAILED heap_chunk=33562624
#           free=33577072  -> "Could not allocate VM heap"
#       exit_probe.log: round 0 status=0, rounds 1+ status=-1.
#       Cause: v04.18 routed BOTH chunks through vita_try_vm_chunk, so each
#       round also allocated and freed a 2 MiB VM block for the bit-vector.
#       sceKernelOpenVMDomain() is irreversible and the block charges the
#       process budget, so after the first round a 32 MiB malloc no longer
#       fits - log184's arena has only ~14 KB of slack against the request.
#       Raw [heapmem] line counts agree: log182 had 10, log184 has 47.
#       Fix: only slot 0 (the heap) may use a VM-domain block. The bit-vector
#       holds no code, so executable memory for it was never useful.
#   (2) jitprobe.cfg WAS A SECOND, CONTRADICTORY SWITCH. The probe already runs
#       only when the user asks for jit!=0, so requiring a file as well meant
#           [JITPROBE] skipped: ux0:/data/J2ME00001/jitprobe.cfg absent
#           jit: executable-memory probe FAILED
#           jit: cfg=2 -> policy 0 (interpreter); no executable memory
#       i.e. turning the JIT on produced a permanent "no executable memory"
#       verdict naming a file the user never created. That is also why log182
#       probed and log184 did not. The gate and the "JIT 探针" settings row are
#       both gone; the probe now runs whenever the policy needs it.
#   NOT addressed here: a 32 MiB heap still cannot be executable (VM blocks cap
#       at 16 MiB), so the JIT still stays off until "Java 堆" is lowered.

# ---- version marker ---------------------------------------------------
# Every release used to append its own essay + set(VITA_VERSION) above this
# point: 180 of them, and only the last one had any effect (CMake is
# last-set-wins). Those essays are preserved in PROJECT_MEMORY.md, findable
# by version number; keeping them here buried the real configuration.
# Keep this as the LAST set(VITA_VERSION) in the file.
set(VITA_VERSION  "04.54")
# 04.29 (2026-11-04): log186 - the Java heap finally became executable, and the
#   JIT was STILL off. Two independent ordering faults, both in runMidlet.c,
#   both fixed here.
#
#   What log186 proved (one boot, three rounds):
#       round 1: heap_mb=15 -> Java heap 15728640 bytes
#                [heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY: 0x85c00000,
#                15728640 bytes => THE JAVA HEAP IS EXECUTABLE; JIT may run
#       every round: JIT requested but the Java heap is not in executable
#                memory -> -int (interpreter)
#   So the memory work from v04.17/v04.18 was CORRECT all along; the decision
#   that consumes it was asking the question at a moment when the answer could
#   not exist yet.
#
#   (1) THE HEAP IS NOT ALLOCATED WHERE v04.24 ASSUMED.
#       v04.24 moved the JIT decision to "right after JVM_Initialize()", on the
#       belief that JVM_Initialize() allocates the heap. It does not - it only
#       sets up tty and Argument parsing (utilities/Arguments.cpp:505). The
#       heap is created by Universe::initialize() -> ObjectHeap::create()
#       (handles/Universe.cpp:549), which runs inside JVM::start(), i.e. from
#       midpRunVm - AFTER the last point where "-int" can still be injected.
#       Every vita_heap_executable() query ever made from runMidlet.c was
#       therefore answered by the PREVIOUS round's state, and round 0 always
#       got 0. Querying cannot be fixed by moving it; it has to go.
#       Fix: PREDICT from the requested size instead. The arithmetic is exact
#       and lives in OsMemory.cpp:
#           heap_chunk = HeapCapacity + 8192     (MimimumMarkingStackSize)
#           a VM block is a 1 MiB multiple, <= 16 MiB
#           15 MiB -> 15736832 -> 16777216  FITS (exactly)
#           16 MiB -> 16785408 -> 17825792  does NOT fit
#       so "heap_mb <= 15" is the precondition, and the VM still logs the real
#       outcome ("[heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY" vs "... outside
#       the VM block range -> malloc") so a wrong prediction is visible.
#
#   (2) setHeapParameters() SILENTLY UNDID THE OVERRIDE.
#       The heap_mb value was applied before setHeapParameters(), which does
#           JVM_SetConfig(JVM_CONFIG_HEAP_CAPACITY, getHeapRequirement());
#       unconditionally (ams_common/native/heap.c:135). That is why log186
#       printed "heap_mb=15" and still booted a 32 MiB heap.
#       Fix: split into vita_read_heap_mb() (called early, feeds the JIT
#       decision) and vita_apply_heap_mb() (called right after
#       setHeapParameters(), so nothing overwrites it).
#
#   Also in this build: build.sh Step 0.5, which recompiles the orphaned
#   obj/arm/runMidlet.o. Without it none of the above reaches the VPK at all -
#   v04.19 through v04.28 were dead code (see the v04.28 note).

# 04.29 (2026-11-04): log185 - and the big news is that this is NOT a freeze.
#   PART 1 - v04.27 CONFIRMED, DECISIVELY. For the first time in this title the
#   game is REACHED AND RUNNING:
#       - no more [TIMEREXC] in the log at all
#       - frames=320, render calls=685, 1,098,444 vertices drawn
#       - gameplay meshes drawn: "gmesh#7 n=675", "rootstate#71 mesh=185ab735"
#       - RMS written ('nfs10set' ver=12, then '0' store created)
#       - exit_probe: 4 launches, all status=0 at the menu
#   The last m3g line is "rootstate#71", not an exception - the scene simply
#   stops being interesting at the sample point.
#   PART 2 - THE ONE REAL BUG FOUND IN log185:
#       [APPEXC] java.lang.IllegalArgumentException: parametric camera
#        - javax.microedition.m3g.Camera.getProjection(), bci=31
#        - ab.a(), bci=2        <- ab.a(Camera, Transform, Transform)
#        - x.a(), bci=83 / z.a(), bci=855 / h.b(), bci=30 / x.a(), bci=46
#        - ab.paint(), bci=156
#   It fired ONCE, at ab.paint() - i.e. inside a frame, and the frame died.
#   THE CAUSE WAS OUR OWN INVENTION. Camera.getProjection(Transform) threw
#   whenever the camera was PERSPECTIVE/PARALLEL and the caller passed a real
#   Transform. JSR-184 says the opposite in so many words:
#       "This method is available REGARDLESS OF THE TYPE OF PROJECTION, since
#        parallel and perspective projections can always be returned in the 4x4
#        matrix form."
#     and the only documented throw is ArithmeticException for near == far.
#   极品飞车 builds a PERSPECTIVE camera and calls getProjection(t) with a real
#   Transform because it wants the matrix to pre-multiply into its own camera
#   space (ab.a bci=2..21 then postMultiply). We threw; paint() died.
#   This is the SIXTH instance of "a legal API call throws" (FUNC_BLEND v03.29,
#   WINDING v04.17, SPOT v04.17, setLayer v04.22, Loader node v04.27, this one) -
#   and the first one this port invented rather than inherited.
#   FIX: the matrix is built for all three types. The construction was moved out
#   of Renderer.projection() into Renderer.buildProjection() so there is exactly
#   ONE definition of it and the camera-reported matrix can never disagree with
#   the one we render with. Behaviour kept identical for the render path
#   (buildProjection(type, fovy, viewportAspect, near, far, null) is what
#   Renderer.projection() now calls). near == far still throws ArithmeticException
#   as documented - and is unreachable from Java anyway because setPerspective /
#   setParallel reject it.
#   GUARD ADDED: debug/tools/m3gprobe/ApiSweep.java. It calls every JSR-184 entry
#   point this title's constant pool references (55 calls, extracted with
#   `javap -v | grep '// javax/microedition/m3g/'`, not from the spec) with legal
#   arguments and fails on any throw: "55 calls, 0 threw -> ALL PASS".
#   debug/tools/m3gprobe/CamChk.java covers Camera specifically (all three
#   projection types, the numeric values of the resulting matrix, GENERIC
#   round-trip, and that the setters reject near == far).
#   PART 3 - WHY IT STILL *LOOKS* STUCK, and it is not a bug in this area:
#       [PERF] f=300 fps=5.3 period=187.64(33.36..3553.60)
#             flip=21.16(12.37..39.13) prepare=12.33 arm=333MHz
#       [PERF] VERDICT: arm stuck at 333 MHz (asked 444, all strategies failed)
#   The device refuses the 444 MHz bump, so everything runs at 333 MHz with the
#   GPU at 111 MHz. The per-frame Java work is only ~74-100 ms (ptime total=74,
#   of which draw=3..5, present=19..21, skin=3..6); the killer is the 46-127 ms
#   "gap" plus occasional 3553 ms outliers, i.e. the title is waiting, and at
#   5.3 fps a loading screen that should take 3 s takes 25 s and reads as a hang.
#   NOT caused by m3g: shade/draw/walk are all single-digit ms. Recorded, not
#   "fixed" - the honest next lever is the ARM clock (clock.cfg / AC / Wi-Fi off,
#   as the log itself suggests) and the 3553 ms outlier, which needs its own
#   measurement round.
#   NOT verified on device.
# 04.17 note (2026-11-02): the JIT memory question is ANSWERED - see the block
#   above v04.17's set() in this file for the log181 evidence. What follows is
#   the immediate consequence: the crash in log181 was the probe PASSING and the
#   policy therefore being left at 2 while ObjectHeap still generates into the
#   managed heap.
#   (1) The probe result no longer decides the policy on its own.
#       vita_main.c now requires vita_jit_code_area_ready() as well, and that
#       returns 0 until the VM actually uses the VM-domain block. So "jit=2"
#       prints "executable memory WORKS (probe passed) but the compiler area is
#       still inside the Java heap" and stays on the interpreter instead of
#       killing the process in the first frame. This is the difference between a
#       useful log line and a coredump.
#   (2) vita_jitmem.c gains the code area itself:
#         vita_jit_code_area_acquire()  4 MiB VM block, OpenVMDomain FIRST then
#                                       alloc (the order log181 proved), domain
#                                       left OPEN (a JIT patches constantly)
#         vita_jit_code_area_ready/base/size
#         vita_jit_flush_icache()       -> sceKernelSyncVMDomain
#       Acquire proves the block is usable before anyone trusts it: stores at
#       the head AND at block-16, reads them back, then EXECUTES the head. The
#       kernel still reports access=0x4 for this memory, so the access field is
#       not used as a gate - log181 showed the writes and the call succeed while
#       it claimed R only, which is what misled v03.77 for seven versions.
#   (3) Nothing on the default path changes: jit=0 never calls any of it.
#   STILL TO DO, and it is the real work: point ObjectHeap at this area. The
#   compiler area is [compiler_area_end() - size, compiler_area_end()) inside the
#   heap, and compact_and_move_compiler_area() can relocate it during GC. Moving
#   it out means (a) compiler_area_end() returning the VM block end under VITA,
#   (b) skipping the GC relocation, (c) relaxing verify_layout()'s heap-order
#   guarantees for the compiler area. Doing that in one step without a way to
#   A/B it is how the last 12 rounds went; this version first makes the backing
#   store provable on its own, so the ObjectHeap change has something known-good
#   to point at.
# 03.33 (2026-10-27): log135 - the "steady sizzle" on the xiyangyang ring tones,
# polyphase FIR upsampler (see vita_wav_resample.h).
#   (its description is in the block above 03.34's set(); kept as history)
# 03.28 (2026-10-27): log133 diagnostics only - NO behaviour change. Neither game's failure could be located:
#   (1) 极品飞车 never touched Graphics3D (no [m3g] line at all) and the screen stayed on the pre-init fill colour
#       (ab.paint draws ad.a(g,54) while ab.c==false). Its main loop is a 15 ms java.util.Timer task; TimerThread
#       cancelled any task that threw, with NO trace, which would look exactly like this (heartbeat alive, nothing
#       drawn, no [APPEXC] because paint() itself does not throw). Timer.java now prints "[TIMEREXC] ..." with the
#       stack for a cancelled task, an Error, and a dying timer thread (System.out -> vm_output.log).
#   (2) 生化危机 printed "render calls=1 frames=0" with every reject counter 0, then never again: the old report
#       needed frames to advance by 30, which can never happen when frames stays 0. Renderer now reports every 120
#       releaseTarget() calls and counts the two previously silent exits (noCamWorld = renderWorld without a camera,
#       nullNode = render(null, ...) -> NPE swallowed by BioCanvas.printException, which is an empty method).
#   Not verified on hardware.
#   (version line removed in 03.29 - this block is history; see 03.29 above)

# 03.23 (2026-10-26): log129 + user: "crackle ALWAYS, clicks and hiss; J2ME Loader on a phone sounds better; dump the audio".
#   Honest state: every audio change since v03.12 was reasoned from a WAV rendered on the HOST. Nothing measured what the
#   DEVICE sends to the DAC, and my "no underrun" evidence (ran - cur within a few ms) was wrong: when the audio thread is late
#   the port queue is empty, sceAudioOutOutput returns at once and the thread catches up, so wall time and submitted audio
#   stay equal - underruns are invisible to that number. A late chunk is exactly a click.
#   This version changes NO sound; it measures (vita_pcm_probe.h, host-tested by debug/tools/wavq/test_pcm_probe.c):
#     1. audio_debug.log "[PCM] wav submissions=N intervals(ms): <9.0=.. 9-10=.. 10-10.5=.. 10.5-10.9=.. 10.9-11.5=..
#        11.5-13=.. 13-16=.. 16-25=.. 25-50=.. >=50=.. max=.. us, quick-returns(<2ms)=.."  one line per tune. A healthy stream
#        has everything in 10.5-10.9 (a 512-frame chunk is 10.667 ms). Anything in >=13 or any quick-return is an underrun.
#     2. ux0:/data/J2ME00001/pcm_dump.raw = the first 6.4 s (600 chunks) EXACTLY as handed to sceAudioOutOutput
#        (raw s16le stereo 48000 Hz). Written once per process, after the tune stops, so it cannot disturb the timing.
#        Analyse with debug/tools/wavq/analyze_dump.py (clipping, jumps, L/R mismatch, 512-frame seams, zero runs, diff
#        against the host reference).
#   Also settled from log129: it is v03.21 and its audio code is byte-identical to v03.20 (RingTone.java, Sound.java,
#   vita_audio_javacall.c, vita_wav_resample.h untouched since 02:44 / 15:43).
# 03.20 (2026-10-26): user: "obviously the decoding of this audio type is wrong". Decoding re-checked from scratch:
#   an independent OTA bit-stream decoder written from Sonivox eas_ota.c (not from RingTone.java) was run on the
#   three real blobs. The BIT STREAM is right (header, 1 pattern, instrCount 34 / 255 / 255, notes 32 / 181 / 253,
#   tempo/scale/style handled, all bits consumed up to zero padding; the melodies are plausible: D1 is a bass line
#   C C G C C A# C A ..., D0 a C-major tune). The PITCH is wrong: RingTone took the frequency from Sound.FREQ_TABLE,
#   Nokia's table for the Sound(freq, duration) constructor, which sits +50..+54 cents (HALF A SEMITONE) above
#   equal temperament at every note (midi 60 -> 270 Hz, not 261.6; midi 69 -> 453, not 440). An OTA note is a MIDI
#   note number (nibble + scale*12 + 59 in eas_ota.c) and Sonivox - what J2ME-Loader / Android use - plays it at
#   A4 = 440. Measured on the rendered WAVs: every note was -46..-50 cents off its note name, now -1.6..+0.8.
#   That is a different, constant mistuning against any reference recording, and a tune played half a semitone
#   sharp against nothing sounds "off" even with a clean waveform. RTTTL notes go through the same code, so they
#   are fixed too. Sound(freq, duration) still uses Nokia's table (its input is a frequency from that table).
#   Not found wrong: pattern repeat (all three have instrCount > 0, no repeat headers), loops (0), unknown
#   instructions (none), note durations. Unknown/unchecked: whether the reference decoders apply the +50 cents
#   themselves (Sonivox does not: it feeds the MIDI note straight to its synth).
#   Needs `build.sh midp` (RingTone is ROMized). Host test: RingToneTest now checks 11 pitches (<4 cents) and
#   the Nyquist / negative cases.
# 03.19 (2026-10-26): log127 (v03.17): "pause/resume no longer stalls, but the crackle is still there -
#   a rasping tail". Asked which moment: the user answered "all the way through the tune, the tail of a
#   note is just more obvious". Measured on the REAL tune (host JVM runs the real RingTone.toWav on the
#   three blobs from the game; Hann-windowed DFT; power that is NOT on the note's own harmonic lines):
#   the plain 8 kHz square wave leaves -15.5 / -13.9 / -12.8 / -13.8 dB (270 / 404 / 539 / 800 Hz)
#   relative to the note - its odd harmonics above 4 kHz fold back into the audible band at frequencies
#   that are not multiples of the note (2.7 - 5.0 % of the power, on EVERY note). That is a rough,
#   metallic buzz, and it is the only measured defect left that is present throughout a tune.
#   Things measured and ruled out: note data (no jumps larger than the swing, fades are clean, 16 distinct
#   levels per 16-sample fade), the end of the tune (renderer pads real zeros), port volume, underruns,
#   the resampler (images at 8k+-f are -57..-68 dB), the pause fade-out (soft thump < 200 Hz, not a buzz).
#   Fix: integer PolyBLEP band-limited square in RingTone.synth (blepSquare, no float, no sin()):
#   -29.9 / -30.1 / -31.0 / -31.7 dB with the same peak (44/127) and the same pitch; a pure-Java run on
#   the real tunes: note counts unchanged (22 / 181 / 127), lengths unchanged, only the 2 samples around
#   each edge differ (48279 of 48307 changed samples in the 34 s tune are within 2 samples of an edge).
#   This is a 14-18 dB improvement, NOT silence: the residual left is the 8-bit rounding and the 8 kHz
#   rate itself. Whether it removes what the user hears is not established. RingToneTest's synthetic
#   OTA case had been failing since v02.89 (it used the v02.88 bit layout, before AND after every later
#   change); fixed, all 13 checks pass. Needs `build.sh midp` (RingTone is ROMized).
#   Pause -> next start is still ~2.5 s (log127 median 2525 ms): 2.4 s of it is BEFORE Sound.init, i.e.
#   the game's own loading between stopMusic and new Sound(); our Sound() takes ~0.33 s now.
# 03.16 (2026-10-25): log126 audio follow-ups (user: "sound still not great, some crackle - slightly better;
#   pause -> resume stalls ~3 s"). Another session took 03.15 for the CAPCOM title in the same minutes (see
#   below), so this is 03.16 and contains both.
#   Stall: reproducible now that the log has timestamps - pause -> next start is 3041, 2689, 2778, 2984,
#   2967, 2840, 3187, 3324 ms (median 2984); log125 (v03.13) has the SAME gap (median 2835 ms; I had not
#   computed it then), so it is not a v03.14 regression. NOT the cause (each measured): render thread
#   (stops 1..10 ms after the request), sceAudioOutOutput (never blocked), the toWav synth (0..2 ms, cache
#   hit; first calls 530 / 2214 ms), synchronous audio_debug writes (17 VM-thread lines per player, ~0.1-0.4 s
#   at the measured 14.7-21.9 ms each), the watchdog (nothing stalled in the window). The remaining ~2.4 s is
#   inside Java createPlayer / MediaDownload / prefetch and is NOT located yet, so this version only measures:
#   "[NS] t=.. create / first packet / buffered / acquire_device / prefetch / start" (audio_debug.log) and
#   "[SOUND] t=.. init / ringtone->wav / wav player created createPlayer=N ms / play steps stop=..
#   prefetch=.. setMediaTime=.. start=.." (vm_output.log), all on one wall clock.
#   Crackle: log126 has no quality signal. Measured instead: the WAV data is click-free (0 jumps larger than the
#   square swing, tunes start from 0), the port volume is 0 dB, no underrun (ran-cur -9..+3 ms over 21 pauses).
#   What IS there: every pause cut the square wave mid-note = a step of 34 % of full scale to 0 in one sample,
#   ~91 % of cuts land on full-level sound. wav_render_player now plays one 512-frame linear fade-out chunk
#   after a stop/superseded exit (vwr_fadeout, host-tested: first step 22 instead of 11264). Whether that is
#   the crackle the user hears is NOT established; the other candidate is the bare square-wave timbre.
# 03.15 (2026-10-25): log126 fixes - the CAPCOM title no longer black-screens.
#   (1) log126: "IllegalArgumentException: viewport outside the target" at
#       Graphics3D.setViewport bci=70 from BioCtrl.D_Game_JSR184(), then EVERY
#       later frame threw "already bound" because the escaped exception left
#       bound=true. setViewport now CLAMPS to the target (a viewport sized for
#       another handset is normal) and returns silently when it lands entirely
#       outside; bindTarget now treats a re-bind as an implicit release.
#   (2) The clip-space result is now mapped into the viewport rectangle
#       (scale + offset matrix applied after the projection in Renderer.frame).
#       Before this, a sub-viewport was drawn full-screen and then clipped,
#       leaving the rectangle blank; present()/drawRGB blit only the viewport.
#   (3) Projection aspect comes from the viewport, not the whole target, and
#       setScissor keeps the software rasteriser inside the same rectangle.
#   Diagnostics: one "bind target WxH clip=x,y" line per target and a
#   "viewport clamp ..." line whenever a title asks for more than it got.
# 03.14 (2026-10-25): log125 (v03.13): sound plays, no stutter, no stall - but "poor quality, some of
#   it cannot be made out". log125 itself has no quality signal (pause latency 0..10 ms, log write avg
#   4.7..5.9 ms, no sceAudioOutOutput stall), so this was analysed on the real tunes with the host JVM
#   running the real RingTone.toWav on the three blobs from the game (D0/D1/D2). Two measured defects:
#   (1) RingTone: style defaulted to 1 (continuous, no gap). The reference parser (Sonivox eas_ota.c:895)
#       starts at 0 (last 1/16 of each note is silent), and none of the three tunes sends a style
#       instruction. Result: in the 34 s tune 180 of 181 note boundaries had a 0 ms gap, 46 of them
#       between two notes of the SAME pitch -> one long tone, rhythm lost. Now median gap 11 ms.
#       (nokia_src is ROMized: needs `build.sh midp`.)
#   (2) wav_render_player: output length = frames * (48000 / rate) with an INTEGER ratio, nearest sample.
#       11025/22050/44100 Hz WAVs played 8.8 % fast (+147 cents), 32000 Hz 50 % fast, > 48000 Hz not at
#       all; and nearest-sample upsampling keeps every image tone (SNR on a 1 kHz sine: 8.2 dB for an
#       8 kHz source, 21.7 dB for 44.1 kHz, vs 25.1 / 54.6 dB with linear interpolation). New header
#       vita_wav_resample.h (exact 32.32 position, linear interpolation), tested on the host with the
#       real header: debug/tools/wavq/test_resample.c (found and fixed a uint64 overflow in the first
#       draft: (n*rate)<<32 overflows once n*rate >= 2^32, a 34 s tune at 8 kHz already does).
#   NOT changed, on purpose: the timbre (8 kHz / 8-bit square wave). A softer 16 kHz/16-bit tone is
#   measurably cleaner on paper (non-harmonic energy -49 dB vs -15 dB at 340 Hz) but needs twice the
#   samples in the interpreter, and the first-call cost of RingTone.toWav on the device is not
#   measured - it could bring the load stall back. Next step is a timing probe, not a guess.
# 03.13 (2026-10-25): listen to the audio pause/resume path (user: "pause then resume stalls a few
#   seconds, only with sound on"). My v03.12 analysis said "no pause/resume event" - wrong: the log has
#   7 "[IN ] pause" lines, one per player; the Java side's Player.stop() is DirectPlayer.doStop() ->
#   nPause -> javacall_media_pause(). What was missing is TIMING: audio_debug.log has no timestamps, so
#   a pause could not be put on the heartbeat timeline. Now, all with the same wall clock as the
#   HEARTBEAT ms= field in input_debug.log (gettimeofday):
#     [PR] t=.. pause#N ran=..ms playing=.. cur=..ms   pause()/stop() reached the port
#     [PR] t=.. render stopped, Nms after the request  the render thread really left its loop
#     [PR] t=.. sceAudioOutOutput blocked N us         one 10.7 ms chunk took > 50 ms
#     [PR] t=.. resume (restarts from 0)               javacall_media_resume() was called
#     [T]  t=.. close took N us / 64 log writes: avg   VM-thread cost of close() / of a log write
#   Not a behaviour change except for the logging.
# 03.12 (2026-10-25): log124 follow-ups. v03.11's FP fix is CONFIRMED on the device:
#   "[ALERT-LAYER] fp i2d=240 dmul*2=480 dadd=241 dmul.95=228 d2l=228 f=228
#   cmp.95<w=true int-math=228" (every step correct) and the UC Alert box draws.
#   Xiyangyang sound "not continuous, stuttering" - root cause found in code and
#   reproduced numerically: wav_render_player computed the source frame as
#   rendered * frames / out_frames_total in 32-bit long arithmetic. On the Vita
#   long is 32 bits, so the product passes 2^31 after 0.17 s (270 KB tune) or
#   0.62 s (72 KB tune) of output; from then on the read position wraps to a small,
#   half-negative frame number and the player replays a 0.3-1.2 s window of the
#   tune (reading memory BEFORE the PCM buffer half of the time). Now 64-bit.
#   Stall on pause/resume: not reproduced as an event - the log has no pause/resume
#   record (javacall_media_resume was never called, the port handles no PS-button
#   suspend). The game re-creates its Sound on every scene change; each create ran
#   ~60-170 synchronous sceIoWrite audio_debug lines on the VM thread
#   (get_buffer_address + do_buffering once per 4 KB packet, 68 per 270 KB tune).
#   They are HOT-sampled (1 in 64) now, and a timing probe records what a log
#   write and close() really cost ("[T] ..." lines in audio_debug.log).
#   Needs ./rebuild_vm.sh? No: port layer only -> build.sh.
# 03.11 (2026-10-25): the FP fix moves into the CLDC layer, and the C++ side of
#   the same bug is fixed. Nothing for it lives in this file any more.
#   Layout: phoneme-cldc/src/vm/os/vita/vita_fp_overlay.S (adapters) +
#   patch_interp.sh, run by rebuild_vm.sh pack_lib, so libcldc_vm.a already
#   contains the fixed Interpreter_arm.o; no objcopy rule, no extra source
#   here, no ASM language, no link-order trick (03.10 had all four).
#   New in 03.11: jvm_d2i/jvm_f2i were integer-protocol for EVERY caller. The
#   C++ sin/cos/tan kernels call them hard-float (double in d0), so
#   ieee754_rem_pio2 got n = 0 and Math.sin/cos/tan were wrong for |x| > pi/4
#   (model on the real ELF: sin(10) = 532.9, cos(100) = -1.1e17, tan(10) =
#   19.4). Small arguments, floor, ceil were right. Now jvm_d2i/jvm_f2i are
#   hard-float entries that tail-call the interpreter's own bodies, renamed
#   jvm_d2i_isoft/jvm_f2i_isoft.
#   Needs ./rebuild_vm.sh (the library has to be repacked), then build.sh.
#   NOT verified on hardware.
# 03.10 (2026-10-25): the Java VM's floating point was broken (log123 follow-up).
#   Symptom: AlertLayer printed (int)(.95 * 240) == 0 while the javac-folded
#   constant (int)(.95 * 240) printed 228, i.e. the same Java expression gave
#   different answers depending on who computed it.
#   Cause (measured, not guessed): the interpreter (Interpreter_arm.o) calls
#   every FP helper - jvm_dadd/dmul/i2d/f2d/d2f/dcmp*/fadd.../sin.. - with the
#   operands in INTEGER registers (r0:r1 = double high:low) and takes the result
#   from r0/r1. The C helpers behind those names live in libcldc_vm.a and are
#   compiled -mfloat-abi=hard, so they read d0/d1 and return in d0. Only
#   jvm_d2i/jvm_f2i (hand-written, defined inside Interpreter_arm.o) matched.
#   I ran the REAL bc_impl_* handlers from the ELF through an ARM/VFP model
#   (calibrated against GCC-compiled soft-float references, 0 mismatches):
#   6/40 FP bytecode cases correct before, 40/40 after.
#   Fix (moved out of this file in 03.11, see there): 28 jvm_X_isoft adapters
#   that move r0-r3 into VFP registers, call the real helper and move the
#   result back, wired in by renaming the interpreter's references.
#   Affects every game that does float/double math (UI layout, animation,
#   physics, Math.sqrt/sin/cos). NOT verified on hardware.

# 02.49 (2026-10-18): restore non-blocking connect. The v02.45 "epoll never
#   reports EPOLLOUT" attribution was wrong: log94 ALSO had the
#   wait-descriptor bug (vita_net_poll signalled the bare fd while MIDP
#   waits on the NetHandle pointer), fixed in v02.45, so the 158 s hang is
#   fully explained by the descriptor mismatch and EPOLLOUT was likely
#   reported all along. The blocking connect v02.45 introduced then caused
#   log96: UC connecting to the unreachable safe.ucweb.com froze the single
#   VM thread (and the whole UI) for the full connect timeout. Revert to
#   non-blocking connect (NBIO before connect, EINPROGRESS => WOULDBLOCK =>
#   event-pump completes it via vita_net_poll), which never blocks the VM.
#   Added select()/epoll probes (vita_net_poll_calls/ready/writable/lastn,
#   printed as "[WD] net poll" at a HANG) to settle it on the next run:
#   poll_writable>0 while a connect stalls => descriptor bug (log94), else
#   EPOLLOUT really is not reported and connect completion needs
#   sceNetGetSockInfo. Removed the now-dead SO_SNDTIMEO connect-bound code.
# 02.47 (2026-10-17): watchdog + boot-timing probes, one bug fix.
#   FIX vita_watchdog.c: the "if (!moved)" opened at KICK0 was closed AFTER
#   the "KICK RECOVERED / re-arm / continue" block, so a pump that recovered
#   during KICK0's own 4 polls skipped recovery and fell into the coredump
#   abort (log93: CE kept advancing after the HANG line, yet the process was
#   killed by its own watchdog). The brace now closes before that block.
#   PROBES:
#   - watchdog early stall sampling: one in-memory sample per frozen poll
#     (VM thread state/CPU clock, interpreter counters, vita_phase_id,
#     VM-thread syscall activity). Written out as "[WD-STALL]" lines when
#     the pump recovers after >= 1 s, or at the start of a HANG report.
#   - ux0:/data/J2ME00001/wd_early_dump.cfg (opt-in, KILLS the process):
#     coredump ~2 s into a stall, so PC/LR describe the stall.
#   - ux0:/data/J2ME00001/pad_nodivert.cfg (opt-in): loop detector off, the
#     real trampoline always runs - reproduces the FileManager exit loop.
#   - preload: sym/verify/iter timers + VM-thread syscall delta
#     ("[preload] phases2" and "[preload] sys(...)" in vm_boot.log).
#   - VitaPhaseScope markers (jar find/open, read, resolve, verify, lazy
#     class load, GC, resource open, clinit setup).
# 02.46 (2026-10-17): FIX the wake-up descriptor in vita_net_poll(). MIDP
#   parks a Java thread with midp_thread_wait(NETWORK_*_SIGNAL,
#   (int)pcslHandle, ...) where pcslHandle is the NetHandle pointer our
#   pcsl_socket_*_start() returned, and midp_thread_signal_list() matches
#   descriptors by equality. vita_net_poll() signalled the bare newlib fd
#   (15, 16, ...), so no WOULDBLOCK waiter was ever woken - the real reason
#   behind log94 "stuck in the network test" (select/epoll was never shown
#   to be at fault: nothing logged its result). 02.45's blocking connect
#   only moves the stall: the first recv() -> EWOULDBLOCK parks the thread
#   on the same never-signalled handle. vita_net_poll() now signals
#   (int)&g_handles[i]; the 02.45 connect change is kept as is.
#   Also in 02.46: nothing else changed vs 02.44/02.45 (timing probes stay).
# 02.44 (2026-10-17): root-cause FIX - NBIO was never enabled. log91 (real
#   hw, three networked launches) printed "[NET] set_nbio fd=15/16
#   rc=0x80410109" = SCE_NET_ERROR_EBADF on every socket: vita_set_nbio()
#   passed the newlib user-space fd straight to sceNetSetsockopt(), but
#   newlib's socket() returns a small fd while the raw sceNetSocket id is
#   kept in __vita_fdmap[fd] (verified in libc.a socket.o/setsockopt.o
#   disasm). So SCE_NET_SO_NBIO never took effect, sockets stayed blocking,
#   and recv(MSG_PEEK) (pcsl_socket_available) blocked in-kernel - watchdog
#   log91 "avail enter=6 exit=5 inside=1" + "VM stuck in a kernel wait".
#   Fix: call newlib setsockopt() which translates fd->id internally. This
#   is the network-path hang only; the shared_entry switch race (FileManager
#   landing-pad self-loop) is a SEPARATE bug, still open.
#   02.44 also carries two untested additions (same version on purpose -
#   nothing of 02.44 has been run on hardware yet):
#   - vita_set_nbio records errno (newlib setsockopt returns -1 and puts the
#     reason in errno): "[NET] set_nbio fd= rc= errno=" and the watchdog
#     "[WD] net nbio ... lastrc= errno=" line.
#   - landing-pad loop detector (InterpreterRuntime_arm.cpp): the 3rd
#     consecutive pad entry with an unchanged (current thread, r6) diverts to
#     the shadow trampoline immediately instead of after land>=1e6. DEBUG
#     EXPERIMENT: if the shadow breaks the loop on its first lap, land stays
#     tiny and exit_probe.log shows "v243 trip_rep>0 trip_land~7"; if the
#     loop persists on the shadow, trip_rep/land grow and the earlier
#     "rescued after 1e6 laps" was not a code difference. The old land
#     threshold remains as a fallback.
#   - launch-timing instrumentation (probes only) for the "every app shows
#     a white screen for seconds" report, suspected to be the v01.91 eager
#     preload (load_all_in_classpath: every .class of midp_system.jar +
#     the app jar is read, inflated and resolved before the first byte
#     runs, each round). One clock, sceKernelGetProcessTimeWide() us:
#       boot_timing.log  round_begin / runMidlet_enter / first_check_events /
#                        first_flip / first_content_flip(first non-white
#                        frame) / runMidlet_exit        (vita_main.c,
#                        vita_display.c, vita_checkevents.c)
#       vm_boot.log      every line prefixed "[+N us]"; "[preload] jar[i]
#                        entries= defined= fast= us=" per jar (i=0 is
#                        midp_system.jar) and "[preload] done ... open_us
#                        read_us resolve_us" (Universe.cpp, JVM.cpp).
# 02.43 (2026-10-17): probes only, NO behaviour change. Answers the two
#   questions log87..log89 left open:
#   (a) exit probe (vita_exit_probe, OS_vita.cpp, called from vita_main.c
#       after runMidlet returns): writes land / v242 st / thread counters
#       to exit_probe.log on a NORMAL exit. log88 round 0 (FileManager)
#       already shows rec[8] (land=1000000) on the same loop thread as
#       rec[5..7] (land=6..8): the loop forms in FileManager and the shadow
#       trampoline breaks it after ~1e6 laps. Prediction: every normal
#       FileManager exit shows land >= 1000000 and shadow_in > 0. If land
#       stays in the tens, the loop never forms and the "shadow rescues
#       it" reading is wrong (timing/layout luck instead).
#   (b) network probe (vita_net.c + vita_watchdog.c): log88/log89#2 froze
#       the VM thread in recv(MSG_PEEK) <- pcsl_socket_available while all
#       four connect() calls returned "immediate OK" (never EINPROGRESS).
#       vita_set_nbio now records sceNetSetsockopt(NBIO)'s return value
#       and the watchdog prints "[WD] net nbio ok/fail/lastrc | avail
#       enter/exit/inside/fd/lastn/lasterr".

# 02.37 (2026-10-02): v02.36 was NEVER BUILT - a make dependency gap:
#   _MergedSrc002.cpp is a 76-byte shell #including __MergedSrc002.cpp.incl
#   which #includes Scheduler.cpp by ABSOLUTE PATH; make only watches the
#   shell file's timestamp, so both v02.35 and v02.36 Scheduler.cpp edits
#   compiled to NOTHING (confirmed: no vita_last_started_id symbol in
#   v02.36's .sym, _MergedSrc002.o dated Oct 1 11:33 predating both edits).
#   log59/60 verdicts are therefore VOID: log60's "birth_delay=1 still
#   froze" tested a binary with no gate at all. Fix: rm the stale .o (the
#   documented rm-before-rebuild trap, now in its MergedSrc variant),
#   rebuild, and VERIFY AT SYMBOL LEVEL (nm must show vita_last_started_id
#   in the .a and the .sym). Code fix riding along: sceKernelDelayThread
#   needs a file-scope extern "C" declaration (merged-src units have no
#   direct system includes; function-local extern "C" is illegal C++).
# 02.36 (2026-10-02): birth-delay round 2 - the v02.35 gate was DEAD.
#   THREAD_JUST_BORN is only set under _debugger_active, so gating on it
#   never fired: log56 (control) froze with birth_delay=0, log57/58
#   (birth_delay=1) froze IDENTICALLY with swjb_seen=0 - the delay never
#   ran once. Round 2 anchors on Scheduler::start(): it records the last
#   started thread's id (plain int, GC-immune); the gate sits right
#   before the actual context switch in switch_thread() and delays when
#   the thread being switched TO is that id (its FIRST dispatch).
#   log57/58 did buy two real verdicts though: (a) ticker_cpu=2 LIVE on
#   cpu2, VM still froze on cpu1 -> same-core tick-injection hypothesis
#   DEAD; (b) same-binary dead pc walked stub+0x18 vs stub+0 and
#   set_current totals 126/100/104 vs 97 - a deterministic deadlock
#   would not walk, timing-race fingerprint reinforced.
# 02.15 (2026-10-28): log39 verdict (ticker pin INEFFECTIVE) + the
#   VM-side liveness experiment.
#   log39 ran v02.14's pin. The pin SUCCEEDED - thread audit shows
#   'cldc_ticker' aff=0x20000(init 0x20000) cpu=32767 last=1, i.e. the
#   ticker is now on the VM's core exactly as designed - yet the hang
#   signature is IDENTICAL to log38 item by item (vm runClocks d=0,
#   cpu1 idleN/switchN d=0, vm 'RUNNING wait=none cpu=1', KICK no
#   effect, dead pc = start_lightweight_thread_asm+0x8). The cross-core
#   ticker race is therefore EXCLUDED as root cause. Side effect worth
#   recording: pinning moved the ONLY independent heartbeat onto cpu1,
#   so log39 shows rt_ticks=0 and vita_tick_count frozen at 1118 where
#   log38 (ticker on cpu0) showed rt_ticks=1400 - the pin cost us the
#   one probe that could still see a live cpu1 from outside.
#   Two probe defects found while re-reading the evidence (both
#   invalidate earlier conclusions, and both are fixed in this build):
#   - vita_site_dump() read vita_site_seq twice with no delay, so
#     'frozen=yes' was a tautology in log35..log39: every frozen-ring
#     inference from those logs is void. Now: a real 200 ms liveness
#     test on the (alive) watchdog thread.
#   - The 'q5 CYCLE(3)' queue print is normal - Scheduler.cpp's dump
#     walks the circular priority list and stops on head re-encounter,
#     printing element count; log36's identical output is the same
#     healthy state, not a corrupt ring.
#   v02.15 change (launcher only, no VM source): wd_dump_vm_progress()
#   dual-samples the VM's OWN counters 500 ms apart at hang time and
#   prints a verdict, splitting the last four hypotheses:
#     switch/fast/site/jsmp moving -> VM thread executing Java
#     tick (cldc_ticker) moving    -> cpu1 still dispatching
#   Both -> VM/Java livelock. VM only -> ticker starved on a live core.
#   tick only -> VM stuck in a kernel wait. Neither -> cpu1 scheduler
#   dead (then direction B: kubridge kernel forensics).
# 02.14 (2026-10-28): direction-A audit + ticker pin to the VM core.
#   log38 verdict (change-core pin A/B) proved the stalled core FOLLOWS
#   the VM host core (vm runClocks d=0 + cpu1 switchN d=0 => kernel-level
#   single-core stall, not a userland livelock). Direction-A audit of the
#   green-thread SP switch + kernel-interaction windows concluded:
#   - green-thread SP model is architecturally sound: jsp=r6 (heap Java
#     stack) / sp=r13 (OS/primordial stack) are separate registers, every
#     syscall runs on the OS stack via get_primordial_sp(sp) - not a bug.
#   - event-pump chain (JVMSPI_CheckEvents -> ANI -> pthread_cond_timedwait)
#     runs on the OS stack and log38's ani_mark "OUT" proves the last pump
#     exited normally (not stuck inside pthread).
#   - the ONE remaining Vita-port-specific defect is the cross-core
#     ticker race: upstream linux delivers ticks via setitimer+SIGVTALRM
#     (handler on the VM's own thread/core); the vita port's standalone
#     'cldc_ticker' thread was created with affinity=0 and landed on cpu0
#     (log38 last=0) while the VM is pinned to cpu1, so real_time_tick()
#     does non-atomic cross-core writes to _rt_timer_ticks /
#     _real_time_has_ticked / _compiler_stack_limit / _timer_has_ticked.
#   v02.14 change:
#   - OS_vita.cpp: pin 'cldc_ticker' to the VM thread's core
#     (sceKernelGetThreadCpuAffinityMask of the creating thread) to
#     restore the upstream same-core tick semantics and remove the
#     cross-core data race. Fallback = SCE_KERNEL_CPU_MASK_USER_ALL.
# 02.13 (2026-10-28): log37 verdict + change-core pin A/B.
#   log37 ran v02.12's CPU0 pin: pin SUCCEEDED (rc=0x70000 is the OLD
#   affinity mask returned by sceKernelChangeThreadCpuAffinityMask, not
#   an error; watchdog KICK1/KICK2 return old masks too) - confirmed
#   aff=0x10000(init 0x70000) cpu=0 chgcpu=5 (vs log36 2839). Yet the
#   VM still froze (runClocks d=0, KICK no effect, ce 412->412) AND one
#   WHOLE CORE stalled (sysinfo 100ms window: cpu0 idle/idleN/switchN all
#   d=0; log36 stalled cpu1 instead). Dead point same green-thread switch
#   stub cluster (pc=start_lightweight_thread_asm+0x14, a plain userland
#   str of NULL into pending_entries) -> cross-core migration hypothesis
#   RETRACTED; fault upgraded to "kernel-level single-core stall".
#   v02.13 change: pin to CPU1 (0x20000) instead of CPU0. If the stalled
#   core FOLLOWS the VM -> the VM's execution (or its kernel interaction)
#   poisons its host core; if the stalled core is fixed/random and
#   unrelated to the VM -> an independent kernel bug and the VM is just a
#   starved victim (then go kernel-side: kubridge).
# 02.12 (2026-10-28): log36 verdict + single-core pin experiment.
#   v02.11's tick-stage probe (ttk_stage=7 hits 1..7=309x7) proved the
#   tick body ran to completion 309/309 times -> JIT-on-tick hypothesis
#   RETRACTED (log36's JIT was off anyway: jit cfg=0 -> policy=0).
#   log36 froze the VM main thread INSIDE a green-thread switch stub
#   (pc=invoke_pending_entries+0x10, lr=start_lightweight_thread_asm,
#   pending==NULL => one-instruction return) with status=RUNNING,
#   runClocks d=0, cpu1 fully idle, cpsr=User-mode: the thread is NOT
#   spinning, NOT in a syscall, NOT in a userland livelock (gnrt=0/0,
#   lanes slow=0, sf=0/0/0) - the real-HW kernel simply never re-
#   dispatches it. Its aff=0x70000 (all three cores) is the only
#   difference from the healthy tone/wd/ticker threads (aff=0x0); the
#   KICK battery (signal/aff/prio/suspend) all failed. Vita3K never
#   reproduces it because its single host scheduler masks cross-core
#   migration bugs.
#   v02.12 change:
#   - vita_main.c: pin the VM main thread to CPU0 (aff=0x10000) BEFORE
#     the VM starts, gated by VITA_VM_CPU_AFFINITY_MASK (0=off A/B).
#     If the hang disappears on real HW, the cross-core runqueue bug is
#     proven and the pin ships as the workaround. Pin rc logged into
#     crumb.log + boot_log.txt.
# 02.11 (2026-10-28): log35 verdict (CORRECTED) + the probe it needs.
#   The Phase-1 verdict ("unbounded park: Scheduler::yield's slow lane
#   with sleep_time=-1 because Os::suspend_ticks() is a Vita no-op")
#   is WRONG and is retracted here. log35's own numbers refute it:
#   slow=0 (that lane was never taken), master mode is proven by the
#   build artifacts (midp_master_mode_events.o present, no vita
#   slavemode_port), so wait_for_event_or_timer()'s sleep_time=-1
#   branch is unreachable; rt_ticks=1341 >> ttk=357 proves the v02.00
#   sleep_forever idle loop ran; vita_checkevents.c already clamps the
#   ANI wait to <=50 ms; pump==CE==394 and the input heartbeat ran
#   right up to the freeze. The real clue is ttk==jsmp==357: the tick
#   BODY was entered 357 times and the 357th never returned =>
#   the VM thread is inside Thread::timer_tick(), past the java-frame
#   sample and short of Scheduler::yield(); gnrt=0/0 says the runnable
#   queue never emptied; ani_mark.log=="CE #394 ANI OUT" is the
#   log19/20/22/23 fingerprint (long-standing, not a v02.10
#   regression). ENABLE_COMPILER IS on in this build
#   (JVMCompiler::on_timer_tick(int) is in midp_vita.velf, crumb.log
#   says UseCompiler=1) and Compiler::on_timer_tick() compiles
#   SYNCHRONOUSLY on the tick - that is the prime suspect.
#   v02.11 changes:
#   - Thread.cpp: tick-stage probe (stages 1..7 + per-stage hit counts)
#     so the next hang names the wedged step instead of a symbol hunt;
#     the watchdog prints "ttk_stage=N hits 1..7=...".
#   - Compiler::on_timer_tick: VITA-only switch
#     (vita_tick_compile_enabled(), file
#     ux0:/data/J2ME00001/tick_compile.cfg, absent = enabled = b332
#     behaviour). One build can A/B the JIT hypothesis: write "0" into
#     that file and tick-driven compilation stops; if the hang goes
#     away, the diagnosis is confirmed and the fix is a
#     safepoint-driven compile policy.
#   - Scheduler.cpp: priority queues are CIRCULAR lists, so the
#     fixed-count q walk printed log35's q5 as ~32 id=4/id=3 pairs
#     (a buffer-full artifact that also silently dropped every later
#     queue). The walk now stops at the head and prints CYCLE(n) /
#     (buffer full) / (max) - residual Risk 3 closed.
#   - OS_vita.cpp: site_ring.log also prints mark=0x%08x
#     (&vita_site_mark) so ring slots are addr2line-able without slide
#     guessing. log35's hot slot decoded inside an unnamed
#     Scheduler-side static whose callee is Os::check_compiler_timer
#     +0x44 - a JIT compile-time-budget call site, consistent with the
#     compiler hypothesis.
# 02.10 (2026-10-27): log34 verdict + final kick battery.
#   log34 ran v02.09 on real HW: runClocks d=0 re-confirmed (the vm
#   thread NEVER executes while the ledger says RUNNING), and the
#   "safe" 1 KiB static runstatus buffer STILL killed the watchdog -
#   pc sat on the instruction AFTER the GetThreadRunStatus blx with
#   the arguments untouched in r0/r1 and the buffer unmodified: the
#   syscall itself data-aborts in the kernel while dereferencing the
#   ghost thread's corrupt control block. Probe RETIRED (2 kills).
#   The site ring finally completed: VM frozen native stack =
#   Scheduler::wait_for+0x54 <- pump_events_vita+0x8 (the green
#   scheduler's idle wait), rt_ticks 573 vs timer_tick 350 (ticker
#   ran 223 ticks the frozen VM never consumed).
#   v02.10 changes:
#   - runstatus probe DELETED; replaced by sceKernelGetSystemInfo
#     per-core ledger (idleClock/comesOutOfIdleCount/threadSwitch
#     Count, two samples 200 ms apart) - a GLOBAL instrument that
#     never touches a per-thread TCB. switchN frozen on a core our
#     own polling thread runs on => per-core scheduler itself stuck;
#     switchN moving => only the vm thread ghosts.
#   - KICK battery MOVED UP (right after the audit table, before the
#     ring/snapshot captures): the kick is now the main experiment.
#   - new KICK5: SuspendThreadForVM/ResumeThreadForVM (exported by
#     the stub, absent from SDK headers) - the only forced stop/
#     restart primitive for the thread's dispatch state. If it trips
#     the same corrupt-TCB abort, the psp2core dump still fires.
# 02.09 (2026-10-26): log33 verdict + safe runstatus + kick0/kick4.
# 02.08 (2026-09-25): log32 verdict + runClocks/run-status instruments.
#   log32 ran the three-pronged kick on real HW: ALL THREE prongs were
#   ACCEPTED and took effect (KICK1 aff->0x30000 returned the old mask
#   0x70000, KICK2 aff->0x70000 returned 0x30000 = KICK1's own write,
#   KICK3 prio rc=0) yet ce never moved. A live-but-migratable thread
#   that ignores two forced runqueue migrations is not a scheduler
#   bookkeeping ghost -> run-bit loss via "lost runqueue entry" is
#   DEAD. What remains: the thread IS executing but its green-thread
#   machinery never advances ce (userland livelock whose pc residue
#   lands in tick/trampoline stubs), or kernel-mode spin invisible to
#   the pc snapshot. Coredump (slide=0x12000, DRES-calibrated) shows
#   vm pc inside shared_monitor_exit+0x7c, lr=timer_tick+0x10, cpsr
#   Z=1 - the SAME Z=1+branch-pending fingerprint as log30/31.
#   v02.08 instruments the decisive split:
#   - wd_dump_thread now samples runClocks (accumulated CPU cycles)
#     twice 500 ms apart inside the hang window: d>0 => really on a
#     core (kernel-mode spin / userland livelock), d==0 => ledger
#     ghost (never actually dispatched).
#   - sceKernelGetThreadRunStatus dumps which tid each core is running
#     right now: if no core ever names the vm tid, RUNNING is fiction.
#   Both run before the kick so a kick-recovery cannot contaminate them.
# 02.07 (2026-10-15): log31 verdict + three-pronged KICK.
#   log31 ran the full v02.06 apparatus on real HW:
#   - Verdict-table row 1 MISSED: vm thread aff=0x70000 (healthy
#     three-core mask), cpu=2==lastcpu=2, preempt=0. The "affinity
#     corruption" simple form is DEAD.
#   - Row 2 INCONCLUSIVE: the kick was a dud - mask 3 rejected with
#     rc=0x80028025 SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK. Vita's
#     affinity encoding is NOT 1<<cpu; it is bits 16..18 (0x70000 =
#     all three user cores; 0x0 = MASK_DEFAULT).
#   - coredump symbolization (slide=0x11000, calibrated against the
#     DRES self-abort pc at instruction level): VM pc frozen at
#     interpreter_timer_tick+0x28 (bleq, cpsr Z=1, pending=0 => the
#     branch returns in one instruction), SAME family as log30's
#     start_lightweight_thread_asm+0x18. Kernel says RUNNING, both
#     clocks advance, yet ce/gc/clinit never move: the thread is NOT
#     executing, but the kernel believes it is. => KERNEL RUN-BIT LOSS
#     is the last hypothesis standing.
#   v02.07: replace the dud kick with a forced-migration kick:
#   KICK1 aff->0x30000 (cores 0+1: real runqueue migration OFF cpu2),
#   KICK2 aff->0x70000 (restore), KICK3 prio->0x99 then back (last
#   resort requeue). Any prong recovering ce => hypothesis PROVEN and
#   the kick ships as the workaround.
# 02.06 (2026-10-15): log30 verdict + therapeutic apparatus.
#   log30 closed the book on all three v02.05 hypotheses ((a) downgrade
#   survival, (b)/(c) falsified via properly-anchored symbolization,
#   slide=0x32000): the VM thread died in a plain thread switch, pc at
#   a harmless new-green-thread trampoline that returns in one
#   instruction. New leading hypothesis: KERNEL SCHEDULER RUN-BIT LOSS -
#   real HW dual-core per-core runqueues vs Vita3K's host scheduler;
#   the VM thread ends up READY but never picked, all userland frozen,
#   both clocks alive.
#   v02.06 apparatus (to prove/disprove + potentially SHIP the fix):
#   - wd_dump_thread now prints attr/currentCpuAffinityMask(+init)/
#     currentCpuId/lastExecutedCpuId/preempt per thread. Verdict table:
#     aff==0 or single-core AND cpu!=lastcpu => run-bit loss confirmed.
#   - same fields added to the thread-audit table dump.
#   - THERAPEUTIC KICK: after all read-only forensics, the watchdog
#     calls sceKernelChangeThreadCpuAffinityMask(vm_tid, 3) and watches
#     the ce counter for ~3 s. RECOVERED => hypothesis PROVEN and the
#     kick itself is the shipping workaround (re-arms, up to 3 times,
#     then takes a coredump on the next hang). No effect => data abort
#     + coredump as before.
#   - links SceKernelThreadMgr_stub (ChangeThreadCpuAffinityMask is not
#     re-exported by SceLibKernel).
#   vita-port: vita_watchdog.c + vita_thread_audit.c + CMakeLists.txt.
# 02.05 (2026-10-14): log29 verdict + forensic upgrade.
#   log29 RE-ANALYZED the log27/28 coredumps with a properly anchored
#   symbolization (4 independent anchors incl. the wd "DRES" write):
#   the old slide was off by 0x37040, so every "pc in .rodata / ring
#   pointing at libpng/stdio" claim in the log27/28 analyses was an
#   artifact. Real scene: VM healthy-looking at
#   interpreter_timer_tick+0x14, but ALL liveness frozen (fast=372
#   slow=0 pump=381==CE switch=188996, clinit=UI count=35) - the
#   log23 "pure Java compute starves the event pump" fingerprint
#   again. Also found+fixed: v02.01's wd_snapshot double-dereference
#   bug (Thread* is a handle cell; method calls already dereference
#   through it - the explicit *(OopDesc**)ct read garbage fields).
#   v02.05 forensics (to split the 3 surviving hypotheses: (a)
#   back-branch checkpoints dead, (b) timer_tick entered but never
#   completes, (c) completes but yield bails early):
#   - Thread::timer_tick entry counter (_vita_ttk_count, accessor
#     vita_sched_ttk_count) + _rt_timer_ticks snapshot (accessor
#     vita_sched_rt_ticks): rt_ticks climbing with ttk frozen = (a);
#     both frozen together = (b)/(c).
#   - vita_site_java_frame: samples the current Java class.method at
#     the top of Thread::timer_tick (length-bounded Symbol copy,
#     PRODUCT-safe) - names the Java loop; sample count climbing with
#     lanes frozen kills (a) outright.
#   - wd_snapshot prints rt_ticks/ttk/jsmp; watchdog dumps
#     vita_site_dump_java into site_ring.log.
#   CLDC: OS_vita.cpp + Thread.cpp + Scheduler.cpp + Globals_vita.hpp,
#   all inside #if defined(VITA); vita-port: vita_watchdog.c.
# 02.01 (2026-09-22): log26 verdict - v02.00 sleep_forever theory FALSIFIED.
#   Fresh dump with the fixed queue dump shows sf=0/0/0 (sleep_forever
#   never entered) because the priority queue is NEVER empty: ghost
#   members (id1 parked with wait_obj, id4 parked with wakeup_time)
#   stay linked in the runnable ring. The OS thread froze dispatching
#   id4 at start_lightweight_thread_asm+0x14 (pc in dump, r0=id4,
#   TLS +0x0c = id4) with ring frozen on the last switch_thread mark.
#   Forensics upgrade only: cur_thread cell dereference (was printing
#   the BSS cell address, stable 0x81401d10 across runs), per-thread
#   wait_obj/wakeup_time (landed in v02.00), gnrt=hits/nulls dispatch
#   counters to separate "one-shot freeze" from "ghost ring keeps
#   dispatching" on the next hang.
# 02.00 (2026-09-21): THE REAL FIX - Scheduler::sleep_forever idle loop.
#   log24 (v01.99) falsified the "spinning loop" theory: the v01.99 pump
#   DID run (ring freshest slot = pump_events_vita+0x48 after slide
#   correction) and then ring seq / pump / CE / heartbeat ALL froze
#   simultaneously. Nothing spins - the VM is ASLEEP. Decoding all 17
#   kernel threads in the dump's THREAD_INFO showed every thread idle,
#   priority queues empty, all 5 Java threads ACTIVE-but-blocked (a
#   game waiting for a keypress). Root cause: this port runs upstream
#   master mode with NO host event loop, so when the LAST runnable
#   thread blocks, switch_thread_master_mode(NULL) falls into
#   sleep_forever() whose upstream loop is while(queue empty)
#   Os::sleep(0x7fffffff) - it NEVER pumps events, NEVER wakes timed
#   sleepers, so the queue can never refill: permanent deadlock. On
#   real phones a host slave loop owns the pump; here nobody does.
#   Fix (#if defined(VITA) in sleep_forever): bounded idle loop - wake
#   due sleepers + the same 50ms-throttled pump switch_thread uses +
#   50ms nap - then set_current(woken) mirroring the debugger branch.
#   Key/touch/timer events now re-fill the queue and the VM wakes.
#   CLDC: Scheduler.cpp +27 lines, single function, VITA-only.
# 01.99 (2026-09-23): THE FIX for the multi-round "hangs after a few
#   keypresses" saga. log23 (first v01.98 forensics capture) closed the
#   causal chain: at hang the site ring holds ONLY switch_thread marks,
#   lanes freeze (fast=377 slow=0) while switch=512169 keeps climbing,
#   pump stops at 370 == CE count, priority queues empty, all 5 threads
#   ACTIVE but unschedulable. The spin loop is pure Java compute that
#   re-enters switch_thread every quantum but never passes yield() or
#   start0 - so every pump site from v01.83/v01.93 could never fire and
#   the pump starved -> blocked waiter never woken -> deadlock. Fix:
#   one-line call to Scheduler::pump_events_vita() inside the existing
#   VITA block of switch_thread() - the single checkpoint every quantum
#   must pass. It reuses the existing 50 ms wall-clock throttle, so a
#   healthy VM pays ~nothing; in the spin it drains input events, wakes
#   the waiter, and ANI-mode CheckEvents adds back-pressure that also
#   throttles the loop. CLDC: Scheduler.cpp +25 lines (comment + call),
#   all inside #if defined(VITA).
# 01.98 (2026-09-22): log22 forensics instrumentation - quantify the
#   fast-lane ping-pong. Four counters (yield fast/slow lane, pump
#   executions, switch_thread passes) exported through the v01.95
#   extern "C" bridge into vita_wd_snapshot, plus a thread inventory
#   dump (global thread list + priority queues, id/pri/status per
#   thread) to answer "who are the active_count=5 threads". CLDC:
#   Scheduler.cpp/.hpp + OS_vita.cpp, all inside #if defined(VITA).
#   Watchdog output: site_ring.log gains "lanes fast=... pump=..." and
#   "threads(active-list)/threads(priority-queues)" sections.
# 01.97 (2026-09-21): WAV buffer made dynamic - fixes the >32 KB truncation
#   left open by 01.96. Java's download loop advances its read offset by
#   the length do_buffering reports, so clamping (dropping bytes) would
#   desync the stream permanently; growth is the only safe answer.
#   - do_buffering WAV branch: wav_ensure_cap() doubles the block (32 MB
#     ceiling ~ 3 min of 44.1k stereo 16-bit) so every packet fits whole;
#     need_more_data stays TRUE until Java's EOF end-marker (whole_size
#     is only a hint, not all sources know it); next_chunk_size stays
#     4096 - a 0 return makes KNI treat the next nBuffering as the end
#     marker, so 0 must never be returned while more data is wanted.
#   - close() waits (bounded 50 x 4 ms) for a playing render to notice
#     stop_flag before freeing jts_data (use-after-free guard).
#   - parse failure after buffering shrinks the block back to JTS_MAX_SIZE.
#   JTS path untouched: sequences always fit the initial block, so
#   jts_len/whole_size logic is unchanged.
# 01.96 (2026-09-21): WAV PCM playback (audio/x-wav) - the biggest MMAPI
#   gap after tone. Three pieces, all in the port layer:
#   1. vita_wav.c (new): RIFF parser (PCM 8/16-bit, 1-2 ch, 4-96 kHz) AND
#      the javacall_media_download_handled_by_device override. The stub in
#      libpcsl_multimedia.a returned NOT_IMPLEMENTED without touching the
#      OUT param, KNI's nIsHandledByDevice pre-sets TRUE, so Java believed
#      the device pulls data itself and never created its MediaDownload -
#      nBuffering never fired, no byte ever reached us. Answering FALSE
#      turns on Java's chunked download for every non-device player.
#   2. vita_audio_javacall.c: caps entry {MS_PCM, audio/x-wav, memory|file}
#      (terminator kept - see the v-wild-pointer comment); realize() marks
#      players whose MIME is audio/x-wav; do_buffering() ACCUMULATES the
#      sliding window KNI writes to jts_data start (GetRawArrayRegion does
#      not append) and parses RIFF at end-of-buffering; start() hands the
#      player to the shared thread via g_wav_req/g_wav_seq (same handshake
#      as JTS, VM thread never blocks); wav_render_player() streams
#      frames -> 48k stereo chunks, nearest-neighbour resample, stop/preempt
#      checks per chunk, END_OF_MEDIA on natural end (seconds, as Java
#      multiplies by 1000).
#   3. get_java_buffer_size: first chunk 4096 for WAV (covers RIFF header
#      in one packet), JTS unchanged at 1024.
#   Untouched: tone/JTS paths, scheduler, watchdog (hang hunt not disturbed;
#   the only shared file gains are the wav fields + new branches).
# 01.95 (2026-09-21): log21 forensics landed - the freeze is INSIDE the
#   green-thread stub cluster. v01.94's ring worked: 16/16 slots =
#   pump_events_vita+0x3c (last healthy pumps) and coredump PC=
#   invoke_pending_entries entry / LR=start_lightweight_thread_asm
#   entry / fp=0 / r1(pending)=NULL, C stack down to ONE frame
#   (JVM::start+0x178, right after primordial_to_current_thread). The
#   cycle touches NO C++ site at all - not even Thread::finish /
#   force_terminated / lightweight_thread_exit. Also learned: the
#   coredump structurally cannot see .data/.bss head nor the Java heap
#   (Vita3K dumps only dirty anonymous pages), so Thread fields /
#   VerifyOnly / scheduler counters must be read from the live process.
#   v01.95 adds: (1) vita_wd_snapshot() in OS_vita.cpp - watchdog reads
#   _current_thread + status/pending/jsp, VerifyOnly, active/async
#   count, timer_ticked, ev_readiness at HANG time (no slide math);
#   (2) switch_thread mark - frozen ring WITH switch marks = pure stub
#   ping-pong, WITHOUT = dispatch freeze before first switch;
#   (3) Thread::allocate mark - counts spawns during the freeze
#   (log21 clinit ring ended at java/util/Timer <clinit>, spawn storm
#   suspected). CLDC: Scheduler.cpp/.hpp, Thread.cpp, OS_vita.cpp.
# 01.79 (2026-09-18): real-hw STARTUP crash fix - omit the frame pointer.
#   See the long comment at the CMAKE_C_FLAGS block below: the 9-17
#   coredump pinned the fault at vita_menu_run+0x134 (`str [r7,#812]`)
#   with r7=0x81000000 = the henkaku user-syscall number base. taiHEN
#   plugin stubs (oclockvita/Framecounter were in the process) load the
#   syscall id into r7 for `svc 0` and never restore it; at -O0 every
#   function in the port kept r7 as its frame pointer, so the first
#   hooked call inside the menu startup chain corrupted the FP and the
#   next local write hit read-only .text. -fomit-frame-pointer removes
#   the dependency entirely.
# 01.80 (2026-09-18): 已安装 tab had a DEAD dpad (user report: "在已安装
#   游戏那里，没有上下移动切换游戏，只能在设置标签中上下切换" - and
#   设置 was the ONLY tab that still moved).
#   Root cause is v01.78f, i.e. the commit that fixed the same class of
#   bug on the settings tab. The cursor dispatcher was written as
#       if (tab == 0 && set_list_grid == TAB_GRIDMODE) {...}
#       else if (tab == 1) ... else if (tab == 2) ...
#       else if (tab == 3) ... else { has_cursor = 0; }
#   where that trailing `else` was only ever meant to catch tab 4 (退出).
#   Because the FIRST test also carried the grid-mode condition, the
#   common case - tab 0 in LIST mode (the default view) - matched no
#   branch at all, fell into the tab-4 else, and got has_cursor = 0.
#   UP/DOWN then did nothing on 已安装, while 设置 kept working simply
#   because it owns an explicit branch. Grid mode was never affected
#   (its own `if` hit), which is why SQUARE-toggling the view looked
#   like a workaround.
#   Fix: the branch is `if (tab == 0)` and the list/grid choice moved
#   INSIDE it (grid swaps psel/ptop to gsel/gtop; list keeps sel/top) -
#   one tab, one branch, so no future tab can inherit tab 4's else.
#   The 1 s heartbeat also logs tab/focus/view now, so this whole class
#   ("which tab owns the dpad") is visible in crumb.log without a
#   rebuild.
# 01.78h (2026-09-17): quit STILL hung on v01.78g. New forensics on the
#   unstripped binary (nm + objdump) nailed the crash site: PC was
#   vita_menu_run's EPILOGUE `ldmia.w sp!, {r4,r5,r7,r8,r9,sl,fp,pc}`
#   (0x8100682a in b255), SP=0x80000328 = entrySP-32 = the saved-register
#   block itself, and EVERY popped register read 0 while crumb.log showed
#   30000+ healthy heartbeats and the exit breadcrumbs written fine. So
#   the block was zeroed sometime DURING the session and only gets read
#   by that final pop - patching the exit path can never help. Also
#   disproven: stack overflow (main thread stack is 0x18000 = 96 KB in
#   the module params; usage ~3 KB) and the v01.78g "PNG rowbytes +
#   batch free" theory. Fix: the menu now leaves via
#   longjmp(vita_menu_escape, 1); main arms it with setjmp before the
#   call (jmp_buf in .bss, out of the walker's reach) and reads the
#   answer from vita_menu_result - no epilogue pop executes at all.
#   Forensics kept: a volatile sentinel array in the frame checked by
#   the 1 s heartbeat prints "menu sen BAD" the moment any similar
#   zero-writer strikes again, bracketing it to one heartbeat window.
# 01.78f (2026-09-17): fixed the 设置 tab stealing the games cursor
#   (user report: "on 设置 the UP/DOWN keys do nothing, they actually
#   move the installed-games list").
#   The dpad cursor dispatcher had no tab==3 branch, so psel fell back
#   to &sel/&top (the installed-games cursor) and nent stayed
#   game_count. Now tab 3 gets its own cursor (setsel/settop, nent =
#   SET_N) and tab 4 (退出, no list) is marked has_cursor=0 so the
#   dpad moves nothing there. The scroll clamp only runs when the
#   current tab actually has a list.
# 01.78e (2026-09-17): fixed the empty 文件 tab (user report: "the Vita
#   root and partitions show nothing"). Root cause: fe_scan called
#   sceIoDopen("/") and returned EARLY when it failed, so the partition
#   probe that was supposed to be the fallback never ran. Now the root
#   list is built by probing fe_devices[] (ux0:/ur0:/uma0:/imc0:/
#   grw0:/xmc0:/xmc1:) with dir_exists first, then merging whatever
#   sceIoDopen("/") does enumerate (deduped via fe_has) - the tab can
#   no longer end up empty. Root rows also show "<free> / <total> MB"
#   per partition via sceAppMgrGetDevInfo.
# 01.78d (2026-09-17): user request round 3.
#   - 文件 tab now browses from the Vita pseudo-root ("/" lists
#     ux0:, ur0:, uma0:, ...) instead of the data dir sandbox; ".."
#     stops at "/". fe_scan appends the right separator so entries
#     read "/ux0:" style paths. Installing a jar from ANOTHER
#     partition falls back from sceIoRename (EXDEV) to a copy - the
#     source is kept (a jar on uma0: is the user's archive, not an
#     inbox drop). Tab-2 tap row math fixed to the y=80 content
#     origin (was 64 since the cwd line was added).
#   - 设置 tab is a real option list now: rows with a cursor
#     (UP/DOWN + setsel), X or tap cycles the value. Options:
#     游戏视图 (列表/网格) and JIT 编译 (关闭/仅首轮/每轮). Both
#     persist - menu.cfg moved to a "view=/jit=" line format (the
#     old single-char file still reads), and the JIT choice is
#     written into launch.cfg line 4 so the main loop's
#     read_jit_policy picks it up next round. Info block below the
#     options keeps ux0 capacity, counts, the v01.45 JIT warning and
#     the data dir.
# 01.59 (2026-09-14): JSR 75 groundwork - the PCSL directory layer.
#   pcsl_directory.h declares 8 entry points this port never implemented
#   (grep pcsl_directory_ in vita_pcsl.c was empty), so anything calling
#   is_directory/mkdir/rmdir/getfreesize/gettotalsize/get_attribute/
#   set_attribute/get_time would have failed at link time. They are real
#   now: sceIoGetstat/sceIoMkdir/sceIoRmdir with the same app0: fallback
#   pcsl_file_exist uses, sceAppMgrGetDevInfo for the volume sizes (the
#   "ux0:"-style prefix is extracted from the path so a per-volume query
#   works), and a libc-free days_from_civil conversion for st_mtime -
#   SceIoStat keeps a SceDateTime struct, not epoch seconds, so the first
#   attempt did not even compile. READ/WRITE attributes answer 1 (Vita
#   has no per-file permission bits), EXECUTE/HIDDEN answer 0, and
#   set_attribute accepts READ/WRITE as a no-op. Nothing calls them yet -
#   javax.microedition.io.file is the next step - so this build behaves
#   exactly like 01.58.
# 01.58 (2026-09-14): JIT policy knob. The compiler was always compiled into
#   the VPK (Globals.hpp:231 product(bool, UseCompiler, true); 29 Compiler
#   symbols in the binary) but runMidlet.c unconditionally passed "-int",
#   so the JIT never ran after v01.46. Now launch.cfg line 4 accepts
#   "jit=0|1|2" (default 0 = interpreter every round = v01.46 behaviour):
#   1 = JIT on round 1 only, 2 = JIT on every round. vita_main.c owns the
#   value in the global vita_jit_policy; runMidlet.c reads it through the
#   weak symbol of the same name and injects "-int" only when the policy is
#   0, so non-Vita builds and the standalone libmidp.so link keep the old
#   behaviour. "-int" is sticky (UseCompiler is a process global,
#   Arguments.cpp:129) which is why jit=1 yields interpreter from round 2 on.
#   jit=2 is the minimal reproducer for the still-unfixed round-2 crash.
# 01.57 (2026-09-14): Real-device coredump (psp2core-...-eboot.bin.psp2dmp)
#   forensics: faulting PC 0x810bb25a = strge r3,[r5] inside
#   pte_osSemaphoreCreate with r5 (the pHandle out-param) == NULL. That is
#   a NULL store AFTER a successful sceKernelCreateSema - i.e. a caller
#   passed a NULL out-param, the classic newlib lazy FILE-lock init
#   signature, hit from a VM worker thread's first stdio write. The
#   JDWP/exception path was ruled out (_debugger_active is only set by
#   connect_java_debugger, and USE_ON_DEVICE_DEBUG=0). Mitigations: every
#   hot print path is now stdio-free (vita_crumb.c = sceIo append channel;
#   JVMSPI_PrintRaw and pcsl_print_chars both route through crumb_append),
#   and the linker --wrap=pte_osSemaphoreCreate logs EVERY create with its
#   caller address and refuses the NULL-pHandle call, so the next device
#   run names the culprit in ux0:/data/J2ME00001/crumb.log.
# 01.46 (2026-09-10, phoneme-midp 6525f4a): Second-launch UC crash (JIT)
#   isolated. PC=0x85f05fd8 was inside the heap compiler_area (JIT code
#   cache); the faulting ldr r4,[r5,#0x404] is the standard JIT prologue
#   GP read of _compiler_stack_limit, but the fault address (0x4E5060004)
#   did not match r5+0x404 -> execution had already jumped into bad code.
#   runMidlet now passes the standard "-int" VM option on every round
#   (UseCompiler=false, process-global) so the whole JIT path is off
#   while the in-process-restart JIT issue is investigated. Perf trade,
#   root cause still open.

find_program(Java_JAR_EXECUTABLE
    NAMES jar
    PATHS $ENV{JAVA_HOME}/bin /usr/lib/jvm/*/bin /opt/jdk*/bin
    DOC   "Path to the jar executable"
)
if(NOT Java_JAR_EXECUTABLE)
    message(FATAL_ERROR "jar executable not found. Set JAVA_HOME or add JDK bin to PATH.")
endif()

# Project paths
set(PORT_DIR      ${CMAKE_CURRENT_SOURCE_DIR})
set(J2ME_DIR      ${PORT_DIR}/..)
set(CLDC_DIR      ${J2ME_DIR}/phoneme-cldc)
set(MIDP_DIR      ${J2ME_DIR}/phoneme-midp)
set(MIDP_BUILD    ${MIDP_DIR}/build/vita_arm)
set(CLDC_DIST     ${CLDC_DIR}/build/vita_arm/dist)

set(MIDP_OBJ_DIR  ${MIDP_BUILD}/obj/arm)
set(PCSL_LIB_DIR  ${MIDP_BUILD}/pcsl/vita_arm/lib)

link_directories(
    ${MIDP_OBJ_DIR}
    ${PCSL_LIB_DIR}
    ${CLDC_DIST}/lib
)

# v01.79 REAL-HW STARTUP CRASH FIX (taiHEN plugin stubs clobber r7):
#   The 9-17 coredump faulted at `str.w r3, [r7, #812]` in
#   vita_menu_run with r7=0x81000000 = the henkaku user-syscall base;
#   taiHEN plugin stubs (oclockvita/Framecounter were in the process)
#   load the syscall id into r7 for `svc 0` and never restore it. At
#   -O0 (empty CMAKE_BUILD_TYPE) every function kept r7 as its frame
#   pointer, so one hooked call in the menu startup chain smashed the
#   FP and the next local write hit read-only .text. See the v01.79
#   version note above for the full forensics.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wno-unused-parameter -fomit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fpermissive -Wall -Wno-unused-parameter -fomit-frame-pointer")

#==============================================================================
# Include Directories
#==============================================================================
set(MIDP_INCLUDE_DIRS
    ${MIDP_BUILD}
    ${MIDP_DIR}/src/ams/example/jams/include
    ${MIDP_DIR}/src/ams/ams_base/include
    ${MIDP_DIR}/src/ams/example/ams_common/include
    ${MIDP_DIR}/src/ams/nams/include
    ${MIDP_DIR}/src/ams/appmanager_ui_resources/include
    ${MIDP_DIR}/src/ams/suitestore/common_api/include
    ${MIDP_DIR}/src/ams/suitestore/recordstore_api/include
    ${MIDP_DIR}/src/ams/suitestore/internal_api/include
    ${MIDP_DIR}/src/ams/suitestore/installer_api/include
    ${MIDP_DIR}/src/ams/suitestore/secure_api/include
    ${MIDP_DIR}/src/ams/suitestore/task_manager_api/include
    ${MIDP_DIR}/src/ams/suitestore_javacall/include
    ${MIDP_DIR}/src/core/log_base/include
    ${MIDP_DIR}/src/core/string/include
    ${MIDP_DIR}/src/core/global_status/include
    ${MIDP_DIR}/src/core/storage/include
    ${MIDP_DIR}/src/core/vm_services/include
    ${MIDP_DIR}/src/configuration/properties_port/include
    ${MIDP_DIR}/src/events/eventsystem/include
    ${MIDP_DIR}/src/events/input_port/include
    ${MIDP_DIR}/src/events/eventqueue_port/include
    ${MIDP_DIR}/src/events/eventqueue/include
    ${MIDP_DIR}/src/events/slavemode_port/include
    ${MIDP_DIR}/src/events/mastermode_port/include
    ${MIDP_DIR}/src/highlevelui/keymap/include
    ${MIDP_DIR}/src/highlevelui/lcdlf/include
    ${MIDP_DIR}/src/highlevelui/lfjport/include
    ${MIDP_DIR}/src/lowlevelui/graphics/gx_putpixel/include
    ${MIDP_DIR}/src/lowlevelui/graphics/gx_putpixel/native
    ${MIDP_DIR}/src/lowlevelui/graphics/include
    ${MIDP_DIR}/src/lowlevelui/image_api/include
    ${MIDP_DIR}/src/lowlevelui/graphics_api/include
    ${MIDP_DIR}/src/core/kni_util/include
    ${MIDP_DIR}/src/security/crypto/include
    ${MIDP_DIR}/src/security/file_digest/include
    ${MIDP_DIR}/src/security/secure_random_port/include
    ${MIDP_DIR}/src/lowlevelui/putpixel_port/include
    ${MIDP_BUILD}/generated
    ${MIDP_BUILD}/pcsl/vita_arm/inc
    ${CLDC_DIST}/include
    ${CLDC_DIR}/src/vm/cpu/arm
    ${CLDC_DIR}/src/vm/share/natives
)

include_directories(
    ${PORT_DIR}/src
    ${MIDP_INCLUDE_DIRS}
)

#==============================================================================
# Prepare libobj_no_main.a: drop members superseded by this port layer
#==============================================================================
set(LIBOBJ_NO_MAIN ${CMAKE_CURRENT_BINARY_DIR}/libobj_no_main.a)

add_custom_command(
    OUTPUT  ${LIBOBJ_NO_MAIN}
    COMMAND ${CMAKE_COMMAND} -E copy ${MIDP_OBJ_DIR}/libobj.a ${LIBOBJ_NO_MAIN}
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} runMidlet_md.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} lfjport_fb_export.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} mastermode_export.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} gxjport_text.o
    # v02.87: the annunciator stub always answered "no vibrate support".
    # src/vita_vibrate.c provides anc_start_vibrate/anc_stop_vibrate.
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} anc_st_vibrate.o
    # v02.90: upstream's hard-coded permission policy prompts (and so
    # blocks the MIDlet thread) for an unsigned jar's first network/file
    # call. src/vita_permissions.c provides the same eight symbols with a
    # relaxed default and restores the upstream tables in strict mode.
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} generic_policy_load.o
    DEPENDS ${MIDP_OBJ_DIR}/libobj.a
    COMMENT "Preparing libobj_no_main.a (dropping runMidlet_md, lfjport_fb_export, mastermode_export, generic_policy_load)"
    VERBATIM
)

add_custom_target(libobj_target DEPENDS ${LIBOBJ_NO_MAIN})

#==============================================================================
# Executable: our port layer first, then the MIDP archive
#==============================================================================
add_executable(${PROJECT_NAME}
    src/vita_main.c
    src/vita_menu.c
    src/vita_font.c
    src/vita_icon.c
    src/vita_display.c
    src/vita_input.c
    # v02.85: on-screen keypad drawn over the letterbox bars
    src/vita_vkey.c
    # v02.92: screenshot capture (SELECT+START / on-screen camera button)
    src/vita_shot.c
    src/vita_pcsl.c
    src/vita_net.c
    src/vm_rom_stubs.c
    src/vita_storage.c
    src/vita_audio_javacall.c
    src/vita_wav.c
    src/vita_midi.c
    # v02.88: shared compressed-audio decoder layer (vita_pcm_dec) +
    # MP3 (minimp3), AMR-NB (vendored 3GPP reference codec) and AAC
    # (sceAudiodec).
    src/vita_pcm_dec.c
    src/vita_mp3.c
    src/vita_amr.c
    src/amr3gpp/interf_dec.c
    src/amr3gpp/sp_dec.c
    src/vita_aac.c
    # v04.45: Ogg Vorbis (stb_vorbis). stb_vorbis.c itself is NOT listed:
    # vita_ogg.c #includes it, and that file is implementation + declarations
    # in one, so a second translation unit would duplicate every symbol.
    # NOTE: this line was lost once when the file was reverted to its 04.31
    # state (the v04.47->v04.48 window); the build then failed with
    # "undefined reference to vita_dec_ogg" while the packaged VPK looked fine.
    # If that link error ever reappears, check this line first.
    src/vita_ogg.c
    src/vita_vibrate.c
    # v02.90: MIDP permission policy (replaces generic_policy_load.o)
    src/vita_permissions.c
    # v04.32/v04.38: suspend-resume listeners, the six-axis publisher and the
    # locale timezone. These three lines were lost together with vita_ogg.c
    # when the file was reverted to its 04.31 state; the linker then reported
    # "undefined reference to vita_lifecycle_init / vita_motion_start".
    src/vita_lifecycle.c
    src/vita_motion.c
    src/vita_timezone.c
    # v02.91: HTTPS for MIDP. vita_https.c drives SceHttp (the console's TLS
    # stack) and vita_https_kni.c is the bridge the com.sun.midp.ssl tunnel
    # on the Java side calls. SEE ALSO: the nativeFunctionTable entries are
    # generated from VitaHttpNative.java by the MIDP build.
    src/vita_https.c
    src/vita_https_kni.c
    # v02.94: per-round JIT policy restore + JIT probe
    src/vita_jit.c
    # v02.97: 444 MHz clock request + per-frame timing (P1-3). Reads
    # ux0:/data/J2ME00001/clock.cfg, writes ux0:/data/J2ME00001/perf.log.
    src/vita_perf.c
    # v02.99: native loading screen for the round_begin -> first frame
    # window (P1-2). Draws as an overlay inside flip_to_display(), so it
    # reuses the single sceDisplaySetFrameBuf path.
    src/vita_splash.c
    # v03.45: framebuffer write-bandwidth probe (measurement only). Explains
    # the prepare=17.1 ms that perf.log reports every frame: the blit writes
    # 884 KB per frame into UNCACHED CDRAM, which is ~52 MB/s - so the
    # question is "how wide are the stores", not "is the ALU fast enough".
    src/vita_membw.c
    # v03.70: on-screen diagnostics overlay (P1-8): fps / period / prepare /
    # arm clock on one line, plus the measured blit and bandwidth on a second
    # when diag.cfg asks for detail. Pure sampling - no behaviour change.
    src/vita_diag.c
    # v03.71: VM-domain executable-memory probe (JIT groundwork). Opt-in via
    # jitprobe.cfg; it allocates 4 KB, writes ARM instructions, opens the VM
    # domain and calls them - the decisive test for whether a JIT can run here.
    src/vita_jitmem.c
    src/vita_media_notify.c
    src/vita_ime.c
    src/vita_gxm.c
    src/vita_gl3d.c
    src/vita_sw3d.c
    src/vita_3d.c
    src/vita_sms_javacall.c
    src/vita_checkevents.c
    src/vita_watchdog.c
    src/vita_crumb.c
    src/vita_thread_audit.c
    # v02.18: kubridge-backed kernel-side stall forensics (HANG path)
    src/vita_kernel_forensics.c
    # v02.27: zero-I/O syscall deadpoint tracer (see vita_trace.c)
    src/vita_trace.c
)

# Size of the bundled jar, baked into the binary for the runtime
# copy-integrity check (must be defined BEFORE the gen_version target)
set(MIDLET_JAR ${PORT_DIR}/build/midlet/jar/Hello.jar)
file(SIZE ${MIDLET_JAR} HELLO_JAR_SIZE_N)

# Build-time version header (git build number + hash), always regenerated
set(VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/vita_version.h)
add_custom_target(gen_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${VERSION_HEADER}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -DAPP_VER=${VITA_VERSION}
        -DAPP_NAME=${VITA_APP_NAME}
        -DHELLO_JAR_SIZE=${HELLO_JAR_SIZE_N}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersion.cmake
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Generating vita_version.h")
add_dependencies(${PROJECT_NAME} gen_version)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# v02.13: VM main thread single-core pin (log37 change-core A/B).
# 0x20000=CPU1 (v02.12 used 0x10000=CPU0, both pinned the hang core
# in log37..log43). v02.19: DEFAULT IS NOW 0 = unpinned three-core -
# the 02.18 "unpinned default" only edited the #ifndef fallback in
# vita_main.c while this definition kept 0x20000 alive, so log43 ran
# pinned despite no vm_cpu file on the card. To re-pin for an A/B
# round, either set this back to 0x20000/0x40000 or drop a vm_cpu
# file on the card ('1'/'2') - the runtime switch needs no rebuild.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VITA_VM_CPU_AFFINITY_MASK=0)

add_dependencies(${PROJECT_NAME} libobj_target)

# Link order matters: the port-layer objects MUST be seen by the linker
# BEFORE ${PCSL_LIB_DIR}/libpcsl_multimedia.a. That archive is a
# gen_stubs.py-generated all-stub file (javacall_media_* return
# NOT_IMPLEMENTED); with --allow-multiple-definition the FIRST
# definition wins, so an archive-first order silently overrides this
# port's real audio implementation (v01.12-v01.14 "Failed to play
# tone" root cause - the KNI layer saw the stub's NOT_IMPLEMENTED and
# the real code in vita_audio_javacall.c never executed, which is also
# why audio_debug.log never appeared).
# target_link_options() always lands BEFORE the object files, so the
# whole-archive block + library group must live in target_link_libraries,
# which CMake orders after its own objects.
target_link_libraries(${PROJECT_NAME}
    -Wl,--whole-archive
    ${LIBOBJ_NO_MAIN}
    ${MIDP_OBJ_DIR}/runMidlet.o
    -Wl,--no-whole-archive
    -Wl,--allow-multiple-definition
    # v02.85: 64 KiB of headroom before the RW segment.
    #   vitasdk's linker script does
    #     . = DATA_SEGMENT_ALIGN(0x10000, 0x10000);
    #     . += DEFINED(__sce_headroom) ? __sce_headroom : 0;
    #     . = ALIGN(0x10000);
    #   so the RW segment normally starts at the next 64 KiB boundary after
    #   the RO content. vita-elf-create then has to fit its SCE metadata at
    #   the END of segment 0 - and v02.85's new code grew .text/.rodata past
    #   that boundary by ~3.3 KB, which failed as
    #     "Cannot allocate 4040 bytes for SCE data at end of segment 0;
    #      segment 1 overlaps"  (followed by a vita-elf-create segfault).
    #   Defining the symbol moves .data/.bss one 64 KiB page later, giving
    #   the metadata room and leaving ~60 KB of future growth. Relocations
    #   cover the change (the loader applies them), and the only absolute
    #   addresses affected are inside our own image.
    -Wl,--defsym=__sce_headroom=0x10000
    # v01.67 REAL-HW BOOT CRASH FIX: on a real console the loader slides
    # the code segment (observed +0x72000 on fw3.65, plugins present) and
    # the RW segment independently (+0x120000), and it applies the ELF's
    # relocation tables - EXCEPT that ld's default long-range ARM->Thumb
    # interworking veneers (____X_from_arm, "ldr pc,[pc,#-4]; .word abs")
    # carry NO relocation entry for their literal, verified on the v01.66
    # ELF: 38 veneers, 0 relocs. The 4th .init_array ctor (the C++
    # _GLOBAL__sub_I for Frame::_in_gc_state) ends in "b ____aeabi_atexit_
    # from_arm", so at boot the jump lands on the stale link-time absolute
    # address -> wild code -> data abort BEFORE main() (hence no boot_log/
    # crumb/net_log/vm_output files ever appear; Vita3K loads at a fixed
    # 0x81000000 so the emulator never reproduces it). --pic-veneer makes
    # ld emit position-independent veneers ("adr/ldr pc-relative" or
    # movw/movt pairs that DO get relocation records).
    -Wl,--pic-veneer
    # v01.57 crash-hunt: intercept the pthread-emulation semaphore init
    # (the v01.56 coredump faulting PC). __wrap_pte_osSemaphoreCreate in
    # vita_crumb.c logs every create and refuses the NULL-pHandle call;
    # references inside libpthread.a resolve to __real_*. NOTE: --wrap
    # only affects references from objects linked into this executable -
    # exactly the callers we care about (newlib FILE-lock init, VM/ANi
    # pthread usage). Wrapper must come before -lpthread.
    -Wl,--wrap=pte_osSemaphoreCreate
    # v02.02 thread forensics: register EVERY thread created in this
    # process (direct callers + libpthread's pte_osThreadCreate inside
    # vita_osal.o, whose priority mapping 0x140-pte_prio gives ANI pool
    # threads real priorities 0x81..0xC0 - numerically LOWER and thus
    # HIGHER priority than the VM thread on the lower-number-wins Vita
    # scheduler; Vita3K does not model this, real HW does).
    # vita_thread_audit.c keeps the registry; the watchdog dumps it on
    # HANG (status/wait/prio/cpu + per-core run state) to prove or
    # disprove VM starvation in one snapshot.
    -Wl,--wrap=sceKernelCreateThread
    -Wl,--wrap=sceKernelStartThread
    # v02.03 (log27): the coredump caught the VM thread RUNNING inside
    # png_do_rgb_to_gray - unreachable through any legal caller (MIDP's
    # imgdcd decoder never calls libpng; the only png_read_image caller
    # is vita_icon_decode_png, done before the menu exits). Wrapping
    # png_read_image counts entries from vita_icon.c so a repeat run
    # can cross-check "libpng really was executing" vs "stale/garbage
    # register context".
    -Wl,--wrap=png_read_image
    # v02.27: syscall deadpoint tracer (vita_trace.c). Every wrapper is
    # enter-mark -> __real_* -> exit-mark, no semantic change. At the
    # next HANG the watchdog dumps trace_ring.log: a per-thread slot
    # stuck at ODD seq names the call that never returned. The VM
    # thread's slot is the deadpoint answer 13 rounds of coredump
    # analysis could not produce (kernel half encrypted, register pc
    # a dump artifact). The IO wraps also cover the watchdog's own
    # dump calls - harmless: its slot ends EVEN on every success.
    -Wl,--wrap=sceIoOpen
    -Wl,--wrap=sceIoClose
    -Wl,--wrap=sceIoRead
    -Wl,--wrap=sceIoWrite
    -Wl,--wrap=sceIoPwrite
    -Wl,--wrap=sceIoLseek
    -Wl,--wrap=sceIoSync
    -Wl,--wrap=sceIoRemove
    -Wl,--wrap=sceIoMkdir
    -Wl,--wrap=sceIoDopen
    -Wl,--wrap=sceIoDread
    -Wl,--wrap=sceIoDclose
    -Wl,--wrap=sceIoGetstat
    -Wl,--wrap=sceKernelDelayThread
    -Wl,--wrap=sceDisplaySetFrameBuf
    -Wl,--wrap=sceDisplayWaitVblankStart
    -Wl,--wrap=sceCtrlPeekBufferPositive
    -Wl,--wrap=sceTouchPeek
    # v02.28: sync family (census-driven; see vita_trace.h). These are
    # the ONLY blocking calls left in the binary after the IO/display
    # wraps above - pthread's own locking goes through LwMutex/Mutex/
    # Sema/MsgPipe thunks and --wrap intercepts them exactly like it
    # did libc's sceIoOpen calls. WaitEventFlag/WaitCond/WaitSignal
    # deliberately NOT wrapped: never called in this ELF and no
    # plain-name stub exists (sceIoChdir lesson).
    -Wl,--wrap=sceKernelLockMutex
    -Wl,--wrap=sceKernelUnlockMutex
    -Wl,--wrap=sceKernelTryLockMutex
    -Wl,--wrap=sceKernelLockLwMutex
    -Wl,--wrap=sceKernelUnlockLwMutex
    -Wl,--wrap=sceKernelTryLockLwMutex
    -Wl,--wrap=sceKernelWaitSema
    -Wl,--wrap=sceKernelPollSema
    -Wl,--wrap=sceKernelSignalSema
    -Wl,--wrap=sceKernelPollEventFlag
    -Wl,--wrap=sceKernelSetEventFlag
    -Wl,--wrap=sceKernelClearEventFlag
    -Wl,--wrap=sceKernelSignalCond
    -Wl,--wrap=sceKernelSendSignal
    -Wl,--wrap=sceKernelWaitThreadEndCB
    -Wl,--wrap=sceKernelSendMsgPipe
    -Wl,--wrap=sceKernelReceiveMsgPipe
    -Wl,--wrap=sceKernelDelayThreadCB
    # v02.29: close the EVEN-slot holes (log51 verdict). The VM slot
    # ended io_pwrite RETURNED, so the deadpoint is either inside a
    # syscall the v02.27/28 set never traced (head suspect: newlib's
    # fsync -> sceIoSyncByFd, the natural next call after a write) or
    # beyond every wrapper. Census of the v02.28 ELF: SyncByFd 1,
    # Lseek32 1, Rename 5, Rmdir 5, Chstat 2, ChstatByFd 2,
    # GetstatByFd 3, Devctl 1, AudioOutOutput 9, NetEpollWait 1,
    # ExitDeleteThread 1, ExitProcess 1 direct blx sites each (all
    # bind __real_ through the fstubs table, same proven route as
    # v02.27/28). sceKernelExitThread excluded: zero direct sites -
    # every call goes via the __sceKernelExitThread_from_thumb local
    # veneer, which --wrap cannot redirect. Exit-family slots stay
    # ODD on success by design: a live process cannot have returned.
    -Wl,--wrap=sceIoSyncByFd
    -Wl,--wrap=sceIoLseek32
    -Wl,--wrap=sceIoRename
    -Wl,--wrap=sceIoRmdir
    -Wl,--wrap=sceIoChstat
    -Wl,--wrap=sceIoChstatByFd
    -Wl,--wrap=sceIoGetstatByFd
    -Wl,--wrap=sceIoDevctl
    -Wl,--wrap=sceAudioOutOutput
    -Wl,--wrap=sceNetEpollWait
    -Wl,--wrap=sceKernelExitDeleteThread
    -Wl,--wrap=sceKernelExitProcess
    -Wl,--start-group
    ${PCSL_LIB_DIR}/libpcsl_multimedia.a
    -lcldc_vm
    -lcldc_vm_ani
    -lcldc_vm_anix
    -lcldc_vmtest
    -lpthread
    -lstdc++
    -Wl,--end-group
)

# v02.94: the VM is linked as "-lcldc_vm" etc., which CMake cannot see as
# file dependencies. Rebuilding the VM (./rebuild_vm.sh) therefore did NOT
# relink midp_vita: build.sh printed "Build successful" with the OLD VM
# inside (found while adding the JIT probe: the library had the probe, the
# ELF did not, ELF mtime 03:53:50 < library 03:54:07). LINK_DEPENDS makes
# a newer library force the relink.
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    ${CLDC_DIST}/lib/libcldc_vm.a
    ${CLDC_DIST}/lib/libcldc_vm_ani.a
    ${CLDC_DIST}/lib/libcldc_vm_anix.a
    ${CLDC_DIST}/lib/libcldc_vmtest.a
    ${PCSL_LIB_DIR}/libpcsl_multimedia.a
)

target_link_libraries(${PROJECT_NAME}
    pcsl_string
    pcsl_file
    pcsl_memory
    pcsl_network
    pcsl_print
    pcsl_escfilenames
    png
    z
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    # v04.49: vita_motion.c (JSR-256 Condition monitoring for the accelerometer)
    # calls sceMotionGetSensorState/sceMotionStartSampling but the stub was
    # never added here, so every build after that file landed died with
    #     undefined reference to `sceMotionGetSensorState'
    SceMotion_stub
    SceNet_stub
    SceNetCtl_stub
    SceLibKernel_stub
    SceSysmodule_stub
    SceAppMgr_stub
    SceGxm_stub
    SceAudio_stub
    # v02.91: sceHttpInit/CreateTemplate/CreateConnection/SendRequest/
    # ReadData/GetAllResponseHeaders. This is the only TLS the console
    # exposes to user code (SceSsl has no record layer in vitaSDK).
    SceHttp_stub
    # v02.88: sceAudiodecInitLibrary/CreateDecoder/Decode for AAC in
    # vita_aac.c. Probed at runtime: if the module is unavailable (Vita3K
    # without SceAudiodec) AAC simply stays unsupported instead of
    # crashing. MP3 does NOT use this path - see vita_mp3.c.
    SceAudiodec_stub
    # v02.97: scePowerSet{Arm,Bus,Gpu}ClockFrequency + the matching getters
    # in vita_perf.c. The getters are what make the log honest - a refused
    # request is reported with the clock that stayed in effect.
    ScePower_stub
    # v02.51: sceImeDialogInit/GetStatus/GetResult/Term for the system
    # keyboard (vita_ime.c). These live in the CommonDialog stub, not the
    # SceIme stub (which only has the low-level sceImeOpen/Close/SetText).
    SceCommonDialog_stub
    # v02.69: sceAppUtilInit (CommonDialog reads app info; samples/ime calls
    # it before sceCommonDialogSetConfigParam) and sceSysmoduleLoadModule
    # (SCE_SYSMODULE_IME) - the two omissions that plausibly explain log113's
    # permanent 0x8002047f (UNEXPECTED_FATAL) after the first dialog.
    SceAppUtil_stub
    # v01.67: sceKernelAllocMemBlock/GetMemBlockBase for the CDRAM
    # framebuffers (vita_fbmem.h)
    SceSysmem_stub
    # v04.32: sceRtcGetCurrentClockLocalTime/sceRtcGetCurrentClock in
    # src/vita_timezone.c - the local-timezone report. v04.38: SceMotion for
    # src/vita_motion.c (sceMotionStartSampling/sceMotionGetSensorState, the
    # six-axis feed). Both lines were lost with the 04.31 revert; the linker
    # said "undefined reference to sceRtcGetCurrentClock" / "sceMotion*".
    SceRtc_stub
    SceMotion_stub
    # v03.83: pss_code_mem_alloc / pss_code_mem_flush_icache / pss_code_mem_free
    # (psp2/pss.h) - the JIT code memory and the USERLAND instruction-cache
    # flush that every other route failed to provide. These live in the
    # SceLibMonoBridge stub, i.e. the PSS/Mono managed-runtime bridge: a JVM
    # has the same problem a .NET runtime has, and Sony solved it there.
    # WEAK rather than strong on purpose: a firmware without SceLibMonoBridge
    # must not fail to start the app. vita_jitmem.c treats a NULL return from
    # pss_code_mem_alloc as "no JIT here" and downgrades the policy, which is
    # exactly what the weak import then allows.
    SceLibMonoBridge_stub_weak
    # v02.06: sceKernelChangeThreadCpuAffinityMask (therapeutic kick in
    # the hang watchdog) lives only in the ThreadMgr stub; SceLibKernel
    # re-exports most threadmgr symbols but not this one. Duplicate
    # symbols across the two stubs are fine: --allow-multiple-definition
    # is already set above (v01.67).
    SceKernelThreadMgr_stub
    # v02.18: sceKernelGetModuleList/GetModuleInfo - the kernel-forensics
    # probe enumerates KERNEL modules (type 0x00) to find kubridge and
    # SceKernelThreadmgr before any ku* call is issued.
    SceKernelModulemgr_stub
    # v02.18: kuKernelCpuUnrestrictedMemcpy (kernel-side stall
    # forensics at HANG time, vita_kernel_forensics.c). The call is
    # only issued AFTER sceKernelGetModuleList(0x00,...) verified the
    # kubridge plugin is actually loaded - on a console without it the
    # probe degrades to a log line instead of a fatal syscall trap.
    kubridge_stub
    # v02.26: taihenModuleUtils_stub (module_get_by_name_nid /
    # module_get_offset) REMOVED - log49 proved those syscalls never
    # return on this console: the main thread died on the SVC veneer
    # at boot, game crashed at startup. Do not re-link this stub.
    # 3D backend (vita_gl3d.c): vitaGL and its deps. Order matters (static);
    # stdc++ after vitaGL because it contains C++ objects.
    vitaGL
    vitashark
    SceShaccCgExt
    mathneon
    taihen_stub
    SceShaccCg_stub
    SceKernelDmacMgr_stub
    SceGxm_stub
    SceDisplay_stub
    SceAppMgr_stub
    SceSysmem_stub
    stdc++
    m
)

#==============================================================================
# VPK packaging (config copies carry the 240x320 virtual screen)
#==============================================================================
# MEMSIZE deliberately NOT set: fself -m <kib> has no legal value for a
# normal app except 0 (see vita-make-fself usage); passing 128 wrote an
# invalid memory budget into the SELF control info -> real-device install
# fails with 0x8010113D (Vita3K never checks it).
# v03.04: UNSAFE (no -s). Needed so vitaGL can see ur0:data/libshacccg.suprx;
# the safe sandbox hides ur0:data entirely (log122).
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

# NOTE (v01.52): the jar output MUST live OUTSIDE the classes/ directory.
# It used to be ${MIDP_BUILD}/classes/midp_system.jar while the package
# command is "jar cf <out> -C classes ." - i.e. the output sat INSIDE the
# tree being packaged. The old directory-mtime DEPENDS almost never
# re-ran the rule, which hid the bug; the v01.51 glob-every-.class fix
# made it re-run on every build, and each run swallowed the previous
# jar into itself (exponential growth: 1.2 MB -> 138 GB observed, and
# classes.zip then packaged that monster too - 30 GB more).
set(MIDP_SYSTEM_JAR ${MIDP_BUILD}/midp_system.jar)

# NOTE (v01.51): depending on the classes *directory* is not enough. Its
# mtime only changes when an entry is added or removed, so recompiling an
# existing .class file (javac overwrites in place) left a stale
# midp_system.jar behind - which is how a v01.51 change managed to be
# absent from the VPK while every step reported success. Depend on the
# class files themselves instead.
file(GLOB_RECURSE MIDP_SYSTEM_JAR_CLASSES ${MIDP_BUILD}/classes/*.class)
file(GLOB_RECURSE MIDP_SYSTEM_JAR_RESOURCES
    ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource/*)

add_custom_command(
    OUTPUT  ${MIDP_SYSTEM_JAR}
    COMMAND ${Java_JAR_EXECUTABLE} cf ${MIDP_SYSTEM_JAR} -C ${MIDP_BUILD}/classes .
            -C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .
    DEPENDS ${MIDP_SYSTEM_JAR_CLASSES} ${MIDP_SYSTEM_JAR_RESOURCES}
            ${MIDP_BUILD}/classes
    COMMENT "Repackaging midp_system.jar with cldc + midp classes + chameleon skin resources"
    VERBATIM
)
add_custom_target(midp_system_jar ALL DEPENDS ${MIDP_SYSTEM_JAR})

if(NOT EXISTS ${MIDLET_JAR})
    message(STATUS "Hello.jar missing at ${MIDLET_JAR}, will try build_jar.sh")
    execute_process(
        COMMAND bash ${PORT_DIR}/build_jar.sh
        WORKING_DIRECTORY ${PORT_DIR}
        RESULT_VARIABLE JAR_BUILD_RES
    )
    if(NOT EXISTS ${MIDLET_JAR})
        message(FATAL_ERROR "Hello.jar could not be built (see build_jar.sh output).")
    endif()
endif()

# Flatten chameleon skin PNGs into data/J2ME00001/lib/ for the
# filesystem fallback path: ResourceHandler.getSystemResource reads
# getConfigRoot + "<imageIdentifier>.png" (e.g. lib/stringitem.image_lnk.png)
file(GLOB VITA_SKIN_PNGS
    ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource/skin/*.png)

set(VITA_SKIN_FILE_ARGS "")
foreach(_png ${VITA_SKIN_PNGS})
    get_filename_component(_name ${_png} NAME)
    list(APPEND VITA_SKIN_FILE_ARGS FILE ${_png} data/J2ME00001/lib/${_name})
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION  ${VITA_VERSION}
    NAME     ${VITA_APP_NAME}
    FILE     ${PORT_DIR}/assets/sce_sys/icon0.png                         sce_sys/icon0.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/bg.png          sce_sys/livearea/contents/bg.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/startup.png     sce_sys/livearea/contents/startup.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/template.xml    sce_sys/livearea/contents/template.xml
    FILE     ${PORT_DIR}/config/internal.config          data/J2ME00001/lib/internal.config
    FILE     ${PORT_DIR}/config/internal.landscape.config data/J2ME00001/lib/internal.landscape.config
    FILE     ${PORT_DIR}/config/system.config            data/J2ME00001/lib/system.config
    FILE     ${MIDP_SYSTEM_JAR}                  data/J2ME00001/midp_system.jar
    FILE     ${PORT_DIR}/config/fontbitmap.bin    data/J2ME00001/fontbitmap.bin
    FILE     ${MIDLET_JAR}                       data/J2ME00001/Hello.jar
    # v02.60: General-MIDI sound bank for the software MIDI synthesizer
    # (vita_midi.c). Read-only; loaded straight from app0: (never copied
    # to ux0:). 5.7 MB. GPL-2, see assets/TimGM6mb.COPYING.txt.
    FILE     ${PORT_DIR}/assets/TimGM6mb.sf2     data/J2ME00001/TimGM6mb.sf2
    # v02.18: kubridge plugin bundled for one-stop install - copy to
    # ux0:tai/kubridge.skprx and add "kubridge.skprx" under *KERNEL in
    # ux0:tai/config.txt (see ab/README.md). Bundling it here does NOT
    # load it; the app only ever CALLS ku* after verifying the module
    # is present (vita_kernel_forensics.c).
    FILE     ${PORT_DIR}/kernel/kubridge.skprx   kubridge.skprx
    ${VITA_SKIN_FILE_ARGS}
)

#==============================================================================
# v01.87: unified VPK output directory
#   Historically the VPKs landed in two places: the build dir root (in-source
#   cmake .) AND out/vpk/ (whatever the last manual copy left there). Keep ONE
#   canonical location: out/vpk/, with the version-tagged name
#   midp_vita_v<version>.vpk (plus the plain midp_vita.vpk copy). The post-
#   build step runs after every make, so both files always reflect the last
#   build; stale version-tagged files from older builds stay (history).
#==============================================================================
# POST_BUILD on the executable fires BEFORE the VPK custom target packs
# the archive (first build copied the PREVIOUS vpk - caught by md5), so
# the copy must be its own target depending on the VPK target instead.
add_custom_target(vpk_out_copy ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PORT_DIR}/out/vpk
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
            ${PORT_DIR}/out/vpk/midp_vita.vpk
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
            ${PORT_DIR}/out/vpk/midp_vita_v${VITA_VERSION}.vpk
    # v01.95: 符号归档——psp2dmp 解析（debug/tools/parse_psp2dmp.py）需要
    # 与 dump 同版本的未 strip 链接产物；build/ 下的会被下次构建覆盖。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}
            ${PORT_DIR}/out/vpk/midp_vita_v${VITA_VERSION}.sym
    DEPENDS ${PROJECT_NAME}.vpk-vpk
    COMMENT "Copying VPK to out/vpk/ (midp_vita.vpk + midp_vita_v${VITA_VERSION}.vpk + .sym)"
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
        DESTINATION .
        RENAME ${VITA_TITLEID}.vpk)

# DO NOT patch e_entry in the velf. In the SCE layout e_entry is NOT the
# program entry - it is the sce_module_info locator: (segment<<30)|offset.
# vita-elf-create sets it correctly (e.g. 0x364740 -> .sceModuleInfo.rodata
# at 0x81364740). The kernel derives the real entry from module_info's
# module_start field; patching e_entry to _start makes Vita3K parse
# .text bytes as sce_module_info_raw in load_imports and crash with a
# read violation right after "Linking SELF" (b137->b146 startup crash).
# (version assigned above, next to the 03.31 comment block)

