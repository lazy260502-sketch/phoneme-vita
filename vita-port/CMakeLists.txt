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
# 01.01 = b152: touch support + input debug logging
# 01.02 = b153: Scheduler.cpp VITA fix - event pump driven by timer tick even
#         when no Java thread is blocked in async native (audio paths sync)
# 01.11 = MEDIA_EVENT_SIGNAL routing enabled: midp_master_mode_events.o was
#         built 08-28 without -DENABLE_JSR_135 (make doesn't track CFLAGS),
#         so END_OF_MEDIA was dropped at the switch default case and the
#         music toggle deadlocked the VM. Rebuilt with USE_JSR_135=true
#         config; jump table slot 36 now routes to StoreMIDPEventInVmThread.
# 01.30 = b170: real network stack restored (vita_net.c) + net_poll moved
#         to JVMSPI_CheckEvents + per-call net logging (net_log.txt) +
#         REAL-DEVICE INSTALL FIX: drop MEMSIZE 128 from vita_create_self
#         - fself -m is a KiB budget where 0x1000-0x12800 is System-mode
#         only and 0 is the only legal value for a normal app; 128 (0x80)
#         produced an invalid SELF control info that Vita3K ignored but
#         real hw rejects with 0x8010113D on install. Also raised the
#         sceNetInit pool 64KB -> 1MB (DNS resolver lives in it; official
#         net_http_bsd sample uses 1MB).
# 01.32 = b171: LCDUI init hang fix chain -
#         (a) pcsl_file_* app0-fallback built from the RAW path produced
#             "app0:ux0:/..." (always ENOENT): skin PNGs in the VPK's
#             data/J2ME00001/lib/ were unreachable, chameleon came up
#             with ~30 missing images and UC WEB hung in init. The
#             fallback now swaps the "ux0:/data/" prefix for "app0:/".
#         (b) per-game appdb was set by the launcher but then silently
#             discarded: runMidlet unconditionally re-ran
#             midpSetAppDir(getApplicationDir()) = MIDP_HOME + "/appdb",
#             so every suite shared the one appdb and the appdb_XXXX
#             dirs stayed empty. runMidlet now keeps a launcher-provided
#             dir (new midpGetAppDir query); storage moved under rms/
#             (rms/appdb + rms/appdb_<TAG>) with a one-time rename
#             migration of the old shared appdb.
#
# v01.33 (2026-09-08): two runtime fixes driven by the first v01.32 log
#         capture:
#         (a) sockets never became non-blocking: fcntl(O_NONBLOCK) is a
#             NO-OP on sceNet descriptors (newlib's fd IS the sceNetSocket
#             id), so connect() blocked in-syscall and froze the whole VM
#             for 27s until ETIMEDOUT (net_log errno=116 + Vita3K
#             sceNetConnect 0x8041013C at +27s). Non-blocking is now set
#             with sceNetSetsockopt(SCE_NET_SO_NBIO) at socket creation
#             (client + server), restoring the EINPROGRESS -> select() ->
#             NETWORK_*_SIGNAL wake chain the port was designed around.
#         (b) vm_output.log rendered one-char-per-line: CLDC's
#             PrintStream.write(int) invokes JVMSPI_PrintRaw PER CHARACTER
#             (length==1) and the debug hook appended '\n' per call.
#             JVMSPI_PrintRaw now writes the bytes verbatim (fwrite).
#         Note: the sceNetInit 0x80410110 (EBUSY) + sceNetResolverCreate
#         stub warnings in Vita3K are benign: newlib's gethostbyname
#         lazily re-inits the net stack on the second launch round; the
#         first init in vita_net_early_init() already succeeded.
#
# v01.34 (2026-09-08): RMS directory enumeration was a NULL stub.
#         RecordStore.listRecordStores() -> getNumberOfStores ->
#         pcsl_file_openfilelist() returned NULL -> OUT_OF_MEM_LEN ->
#         OutOfMemoryError raised from native inside UC's startup
#         state restore (observed as ao.a() NPE one frame up). First
#         launch worked (creates go through pcsl_file_open/exist,
#         both real); every subsequent launch died - deleting the
#         appdb dir "fixed" it because the listing then legitimately
#         returned empty. Implemented openfilelist/getnextentry/
#         closefilelist with sceIoDopen/Dread/Dclose following the
#         reference POSIX contract (root + match-prefix split, full
#         path results). This also un-breaks midp_remove_suite's file
#         cleanup loop and rmsdb_remove_record_stores_for_suite,
#         which iterate the same way.
#
# v01.35 (2026-09-08): silence the misleading "file in use" noise from
#         midp_remove_suite's cleanup loop. Vita3K maps SCE errno onto
#         the wrong Windows errno table when it logs io_error_impl, so a
#         plain ENOENT (0x80010002 = 0x80010000 | 2, "file does not
#         exist") shows up as "Error code: 32 (another program is using
#         this file)". The cleanup loop unlinks every enumerated entry
#         unconditionally, so a target that vanished (or is only a
#         filename prefix of a store) legitimately returns ENOENT.
#         pcsl_file_unlink now treats "already gone" as success (probed
#         with sceIoGetstat, no hardcoded code), so storage_delete_file
#         stops printing the noise and the loop no longer aborts early
#         on a vanished entry.
#
#         (net) vita_net.c: g_handles[] was a BSS global (zero-filled),
#         so na_create() never found an fd==-1 free slot and returned
#         NULL for EVERY non-blocking (EINPROGRESS) connect - the
#         "connect EINPROGRESS -> select -> NETWORK_*_SIGNAL" wake chain
#         silently died and real connects to web servers never completed
#         (net_log: socket_open OK but no connect result for reachable
#         hosts; only unreachable WAP proxy 10.0.0.172 logged the
#         immediate ETIMEDOUT errno=116). Explicitly initialised the
#         table to fd=-1.
#
# v01.36 (2026-09-08): the v01.35 ENOENT-tolerance patch was NOT enough.
#         Verified against the user's exact Vita3K build (v0.2.1
#         496939b6, io.cpp remove_file): the "Error code: 32" line is
#         Windows' ERROR_SHARING_VIOLATION message verbatim - a REAL
#         sharing violation, not an errno mis-mapping. Vita3K's
#         open_file() holds a FILE* (_wfopen, no FILE_SHARE_DELETE) for
#         every open fd, so a remove fails with Error 32 whenever ANY
#         handle in-process still refers to the file; Vita3K then
#         returns 0x80010002 for BOTH "not exist" and "in use", which
#         is what misled v01.35. POSIX allows unlinking open files
#         (phoneME's cleanup loops rely on it), Windows does not. Fix:
#         an open-handle registry in vita_pcsl.c - pcsl_file_open
#         registers, pcsl_file_close unregisters, and pcsl_file_unlink,
#         when the remove failed and the file still exists, evicts all
#         same-path handles (sceIoClose + fd=-1: the POSIX "name gone"
#         state) and retries the remove once.
#
# v01.37 (2026-09-08): REVERTED the v01.36 eviction. Making the remove
#         succeed turned out to be WRONG: midp_remove_suite's cleanup
#         loop targets the appdb entries of the RUNNING temp suite
#         (FFFFFFFF* = UC's live RMS data), and the sharing violation
#         had been accidentally PROTECTING that data across launches.
#         v01.36 evicted fds still held by Java code -> RMS files came
#         back half-written -> second launch broke (NPE, ~1 fps).
#         pcsl_file_unlink now reports the failure (file survives);
#         the registry + evict helper stay in place unused for a
#         future targeted fix (evict only on real uninstall).
#
# v01.38 (2026-09-08): init-slowness fix + noise halving. (a) The
#         per-open debug_log() (open+write+close of debug_log.txt for
#         EVERY pcsl_file_open, two lines on success) multiplied UC's
#         init I/O by ~3x - hundreds of opens during RMS/config/skin
#         restore each costing 3 extra syscalls plus a Vita3K trace
#         line per syscall. Disabled behind VITA_FILE_DEBUG (default
#         0). (b) pcsl_file_unlink no longer falls back to the app0
#         copy when the ux0 file exists but is locked: the app0 remove
#         always failed too, doubling the "file in use" log noise with
#         zero benefit. Net effect: UC data-init is much faster and
#         the remaining sharing-violation lines are one group per
#         protected suite file (expected, harmless).
# v01.39: anilib/vita os_port.cpp Os_WaitForEventOrTimeout condition fix
#   (upstream is "ms != 0", Vita port had "ms == 0" - for any positive
#    timeout the timespec stayed {0,0}=epoch so pthread_cond_timedwait
#    returned ETIMEDOUT immediately; the scheduler's 50ms event-pump
#    wait degenerated into a busy loop: fps=1, heartbeat counter
#    racing (#1..#8193+), UC init crawl. Also rebuilt os_port.o and
#    repacked libcldc_vm_ani.a / libcldc_vm_ani_r.a.)
set(VITA_VERSION  "01.39")

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

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fpermissive -Wall -Wno-unused-parameter")

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
    DEPENDS ${MIDP_OBJ_DIR}/libobj.a
    COMMENT "Preparing libobj_no_main.a (dropping runMidlet_md, lfjport_fb_export, mastermode_export)"
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
    src/vita_pcsl.c
    src/vita_net.c
    src/vm_rom_stubs.c
    src/vita_storage.c
    src/vita_audio_javacall.c
    src/vita_media_notify.c
    src/vita_sms_javacall.c
    src/vita_checkevents.c
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
    SceNet_stub
    SceNetCtl_stub
    SceLibKernel_stub
    SceSysmodule_stub
    SceAppMgr_stub
    SceGxm_stub
    SceAudio_stub
    m
)

#==============================================================================
# VPK packaging (config copies carry the 240x320 virtual screen)
#==============================================================================
# MEMSIZE deliberately NOT set: fself -m <kib> has no legal value for a
# normal app except 0 (see vita-make-fself usage); passing 128 wrote an
# invalid memory budget into the SELF control info -> real-device install
# fails with 0x8010113D (Vita3K never checks it).
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

set(MIDP_SYSTEM_JAR ${MIDP_BUILD}/classes/midp_system.jar)

add_custom_command(
    OUTPUT  ${MIDP_SYSTEM_JAR}
    COMMAND ${Java_JAR_EXECUTABLE} cf ${MIDP_SYSTEM_JAR} -C ${MIDP_BUILD}/classes .
            -C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .
    DEPENDS ${MIDP_BUILD}/classes
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
    FILE     ${PORT_DIR}/config/font.ttf         data/J2ME00001/font.ttf
    FILE     ${PORT_DIR}/config/fontbitmap.bin    data/J2ME00001/fontbitmap.bin
    FILE     ${MIDLET_JAR}                       data/J2ME00001/Hello.jar
    ${VITA_SKIN_FILE_ARGS}
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

