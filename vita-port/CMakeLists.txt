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
set(VITA_VERSION  "01.00")

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
    src/vita_display.c
    src/vita_input.c
    src/vita_pcsl.c
    src/vm_rom_stubs.c
    src/vita_storage.c
    src/vita_audio_javacall.c
    src/vita_media_notify.c
    src/vita_sms_javacall.c
)

# Build-time version header (git build number + hash), always regenerated
set(VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/vita_version.h)
add_custom_target(gen_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${VERSION_HEADER}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -DAPP_VER=${VITA_VERSION}
        -DAPP_NAME=${VITA_APP_NAME}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersion.cmake
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Generating vita_version.h")
add_dependencies(${PROJECT_NAME} gen_version)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_dependencies(${PROJECT_NAME} libobj_target)

target_link_options(${PROJECT_NAME} PRIVATE
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
    z
    SceDisplay_stub
    SceCtrl_stub
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
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} MEMSIZE 128)

set(MIDLET_JAR ${PORT_DIR}/build/midlet/jar/Hello.jar)
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
