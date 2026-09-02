cmake_minimum_required(VERSION 3.16)

#==============================================================================
# midp-vita - J2ME/MIDP Emulator for PS Vita
#==============================================================================

# Vita toolchain
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
set(VITA_APP_NAME "J2ME Emulator")
set(VITA_TITLEID  "J2ME00001")
set(VITA_VERSION  "01.00")

# Java tools (jar is needed to package midp_system.jar with cldc classes)
find_program(Java_JAR_EXECUTABLE
    NAMES jar
    PATHS $ENV{JAVA_HOME}/bin /usr/lib/jvm/*/bin /opt/jdk*/bin
    DOC   "Path to the jar executable"
)
if(NOT Java_JAR_EXECUTABLE)
    message(FATAL_ERROR "jar executable not found. Set JAVA_HOME or add JDK bin to PATH.")
endif()

# Project paths
set(MIDP_VITA_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(J2ME_DIR      ${MIDP_VITA_DIR}/..)
set(CLDC_DIR      ${J2ME_DIR}/phoneme-cldc)
set(MIDP_DIR      ${J2ME_DIR}/phoneme-midp)
set(MIDP_BUILD    ${MIDP_DIR}/build/vita_arm)
set(CLDC_DIST     ${CLDC_DIR}/build/vita_arm/dist)
set(CLDC_TARGET   ${CLDC_DIR}/build/vita_arm/target)

# Build directories
set(MIDP_OBJ_DIR  ${MIDP_BUILD}/obj/arm)
set(PCSL_LIB_DIR  ${MIDP_BUILD}/pcsl/vita_arm/lib)

# Compiler flags
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fpermissive -Wall -Wno-unused-parameter")

#==============================================================================
# Include Directories
#==============================================================================
# MIDP include paths (auto-collected)
set(MIDP_INCLUDE_DIRS
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
    ${MIDP_DIR}/src/configuration/properties_port/include
    ${MIDP_DIR}/src/events/eventsystem/include
    ${MIDP_DIR}/src/events/input_port/include
    ${MIDP_DIR}/src/events/eventqueue_port/include
    ${MIDP_DIR}/src/events/eventqueue/include
    ${MIDP_DIR}/src/events/slavemode_port/include
    ${MIDP_DIR}/src/events/mastermode_port/include
    ${MIDP_DIR}/src/security/crypto/include
    ${MIDP_DIR}/src/security/file_digest/include
    ${MIDP_DIR}/src/security/secure_random_port/include
    ${MIDP_BUILD}/generated
    ${MIDP_BUILD}/pcsl/vita_arm/inc
    ${CLDC_DIST}/include
    ${CLDC_DIR}/src/vm/cpu/arm
    ${CLDC_DIR}/src/vm/share/natives
)

include_directories(
    ${MIDP_VITA_DIR}/src
    ${MIDP_INCLUDE_DIRS}
)

#==============================================================================
# Library Directories
#==============================================================================
link_directories(
    ${MIDP_OBJ_DIR}
    ${PCSL_LIB_DIR}
    ${CLDC_DIST}/lib
)

#==============================================================================
# Create modified libobj.a (without runMidlet_md.o which has main())
# plus the hand-compiled Vita port objects (framebuffer/display/input)
#==============================================================================
set(VITA_PORT_DIR ${MIDP_DIR}/build/vita_arm)
set(VITA_PORT_OBJS
    ${VITA_PORT_DIR}/platform_vita.o
    ${VITA_PORT_DIR}/gxj_graphics_vita.o
    ${VITA_PORT_DIR}/lcdui_display_vita.o
    ${VITA_PORT_DIR}/lcdui_input_vita.o
    ${VITA_PORT_DIR}/midpNativeAppManagerPeer_vita.o
)
set(LIBOBJ_NO_MAIN ${CMAKE_CURRENT_BINARY_DIR}/libobj_no_main.a)

add_custom_command(
    OUTPUT  ${LIBOBJ_NO_MAIN}
    COMMAND ${CMAKE_COMMAND} -E copy ${MIDP_OBJ_DIR}/libobj.a ${LIBOBJ_NO_MAIN}
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} runMidlet_md.o
    COMMAND arm-vita-eabi-ar r ${LIBOBJ_NO_MAIN} ${VITA_PORT_OBJS}
    DEPENDS ${MIDP_OBJ_DIR}/libobj.a ${VITA_PORT_OBJS}
    COMMENT "Creating libobj.a without runMidlet_md.o + Vita port objects"
    VERBATIM
)

add_custom_target(libobj_target DEPENDS ${LIBOBJ_NO_MAIN})

#==============================================================================
# Executable
#==============================================================================
add_executable(${PROJECT_NAME}
    src/main.c
)

add_dependencies(${PROJECT_NAME} libobj_target)

# Without this, relinking does NOT trigger when libobj_no_main.a is updated
# (the archive is only passed via link_options, not as a tracked dependency)
set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_DEPENDS "${LIBOBJ_NO_MAIN}"
)

target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--whole-archive
    ${LIBOBJ_NO_MAIN}
    ${MIDP_OBJ_DIR}/runMidlet.o
    ${MIDP_OBJ_DIR}/vita_stubs.o
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
    pcsl_string        # PCSL libraries
    pcsl_file
    pcsl_memory
    pcsl_network
    pcsl_print
    pcsl_escfilenames
    # Vita SDK stubs
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
# Create VPK
#==============================================================================
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} MEMSIZE 128)

# Default MIDlet JAR
set(MIDLET_JAR ${MIDP_VITA_DIR}/build/midlet/jar/Hello.jar)

# midp_system.jar is built by phoneme-midp in its classes directory
set(MIDP_SYSTEM_JAR ${MIDP_BUILD}/classes/midp_system.jar)

# Create/refresh midp_system.jar from phoneme-midp classes directory.
# The classes directory contains both midp's own classes AND cldc classes
# (uncompressed from cldc_classes.zip by cldc_vm.gmk). We rebuild the jar
# every CMake configure to ensure cldc classes (e.g. DebuggerInvoke.class)
# are included -- otherwise stale jars miss them and the JVM crashes at
# startup with NoClassDefFoundError.
add_custom_command(
    OUTPUT  ${MIDP_SYSTEM_JAR}
    COMMAND ${Java_JAR_EXECUTABLE} cf ${MIDP_SYSTEM_JAR} -C ${MIDP_BUILD}/classes .
    DEPENDS ${MIDP_BUILD}/classes
    COMMENT "Repackaging midp_system.jar with cldc + midp classes"
    VERBATIM
)
add_custom_target(midp_system_jar ALL DEPENDS ${MIDP_SYSTEM_JAR})

if(NOT EXISTS ${MIDLET_JAR})
    # Try to build it
    if(EXISTS ${MIDP_VITA_DIR}/build_jar.sh)
        execute_process(
            COMMAND bash ${MIDP_VITA_DIR}/build_jar.sh
            WORKING_DIRECTORY ${MIDP_VITA_DIR}
        )
    endif()
endif()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION  ${VITA_VERSION}
    NAME     ${VITA_APP_NAME}
    FILE     ${MIDP_BUILD}/generated/lib/internal.config  data/J2ME00001/lib/internal.config
    FILE     ${MIDP_BUILD}/generated/lib/system.config     data/J2ME00001/lib/system.config
    FILE     ${MIDP_SYSTEM_JAR}                             data/J2ME00001/midp_system.jar
    FILE     ${MIDLET_JAR}                                 data/J2ME00001/Hello.jar
)

#==============================================================================
# Install target
#==============================================================================
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
        DESTINATION .
        RENAME ${VITA_TITLEID}.vpk)