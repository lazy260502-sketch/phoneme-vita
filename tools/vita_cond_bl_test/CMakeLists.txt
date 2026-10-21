cmake_minimum_required(VERSION 3.16)

# Standalone test app (own title id) - does not touch the J2ME player build.
#   export VITASDK=/usr/local/vitasdk
#   cmake -S tools/vita_cond_bl_test -B /tmp/condbl_build && cmake --build /tmp/condbl_build
# Result: /tmp/condbl_build/cond_bl_test.vpk
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(cond_bl_test C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "cond_bl_test")
set(VITA_TITLEID  "CONDBL001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -Wall -marm")

# The SDK's own on-screen text helper (not copied into the repo).
set(DBG_DIR "${VITASDK}/share/gcc-arm-vita-eabi/samples/common")
include_directories(${DBG_DIR})

add_executable(${PROJECT_NAME}
  main.c
  cond_bl.S
  ext_callee.S
  ${DBG_DIR}/debugScreen.c
)

target_link_libraries(${PROJECT_NAME}
  SceDisplay_stub
  SceCtrl_stub
  SceKernelModulemgr_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
