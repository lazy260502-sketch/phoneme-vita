cmake_minimum_required(VERSION 3.10)

# Set up Vita toolchain
set(CMAKE_TOOLCHAIN_FILE /usr/local/vitasdk/share/vita.toolchain.cmake)

# Project name
project(cldc-vita C CXX ASM)

# Source directories
set(CLDC_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../phoneme-cldc/src)
set(VITA_OS_DIR ${CLDC_SRC_DIR}/vm/os/vita)

# Include directories
include_directories(
    ${CLDC_SRC_DIR}/vm/share
    ${CLDC_SRC_DIR}/vm/share/natives
    ${CLDC_SRC_DIR}/vm/share/utilities
    ${CLDC_SRC_DIR}/vm/cpu/arm
    ${CLDC_SRC_DIR}/vm/includeDB
    ${VITA_OS_DIR}
    /usr/local/vitasdk/include
    /usr/local/vitasdk/include/vita2d
)

# Source files
set(VM_SOURCES
    # Core VM
    ${CLDC_SRC_DIR}/vm/share/Interpreter.cpp
    ${CLDC_SRC_DIR}/vm/share/StackWalker.cpp
    ${CLDC_SRC_DIR}/vm/share/Frame.cpp
    ${CLDC_SRC_DIR}/vm/share/Method.cpp
    ${CLDC_SRC_DIR}/vm/share/ObjectHeap.cpp
    ${CLDC_SRC_DIR}/vm/share/RootSet.cpp
    ${CLDC_SRC_DIR}/vm/share/StackMapTable.cpp
    ${CLDC_SRC_DIR}/vm/share/StaticFields.cpp
    ${CLDC_SRC_DIR}/vm/share/Type.cpp
    ${CLDC_SRC_DIR}/vm/share/UTF8.cpp
    ${CLDC_SRC_DIR}/vm/share/Verifier.cpp
    ${CLDC_SRC_DIR}/vm/share/VM.cpp
    ${CLDC_SRC_DIR}/vm/share/VMThread.cpp
    ${CLDC_SRC_DIR}/vm/share/Zip.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipFile.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipInflator.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipEntry.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipFileCache.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipFileEntry.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipFileInputStream.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipFileOutputStream.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipOutputStream.cpp
    ${CLDC_SRC_DIR}/vm/share/ZipUtils.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/BSDSocket.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Class.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Double.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Float.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Integer.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Long.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Math.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Object.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Random.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Short.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/String.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/System.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Thread.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/Throwable.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMAccessController.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMClass.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMClassLoader.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMClassRegistry.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMObject.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMRuntime.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMString.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMSystem.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMThread.cpp
    ${CLDC_SRC_DIR}/vm/share/natives/VMThrowable.cpp
    ${CLDC_SRC_DIR}/vm/share/utilities/GlobalDefinitions_gcc.cpp
    ${CLDC_SRC_DIR}/vm/share/utilities/Hashtable.cpp
    ${CLDC_SRC_DIR}/vm/share/utilities/Vector.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Addressing_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Assembler_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CodeGenerator_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CodeOptimizer_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CompiledMethod_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CompilerStubs_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Disassembler_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Frame_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/GlobalDefinitions_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Instructions_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/InterpreterGenerator_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/InterpreterRuntime_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/InterpreterStubs_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/NativeGenerator_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/ObjectHeap_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/RegisterAllocator_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/SharedStubs_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/SourceAssembler_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/SourceMacros_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/TemplateTable_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/VirtualStackFrame_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/FloatSupport_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/FPURegisterMap_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/GPTableGenerator_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/BinaryAssembler_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Debug_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Disassembler_wmmx.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Assembler_wmmx.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/Assembler_armthumb.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CodeOptimizer_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CompilationQueue_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/CompiledMethodDesc_arm.cpp
    ${CLDC_SRC_DIR}/vm/cpu/arm/SourceMacros_arm.cpp
    # Vita OS layer
    ${VITA_OS_DIR}/OS_vita.cpp
    ${VITA_OS_DIR}/OsMemory_vita.cpp
    ${VITA_OS_DIR}/OsMisc_vita.cpp
    ${VITA_OS_DIR}/OsFile_vita.cpp
    ${VITA_OS_DIR}/OsSocket_vita.cpp
    ${VITA_OS_DIR}/JVM_vita.cpp
    ${VITA_OS_DIR}/Main_vita.cpp
)

# Compiler flags
add_compile_options(
    -march=armv7-a
    -mtune=cortex-a9
    -mfloat-abi=hard
    -mfpu=vfpv3
    -DARM
    -DVITA
    -D__PSP2__
    -DSUPPORTS_MEMORY_MAPPED_FILES=0
    -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0
    -DSUPPORTS_TIMER_THREAD=0
    -DUSE_VM_EXCEPTIONS=0
    -DALWAYS_LSW_FIRST_FOR_DOUBLE=1
    -fno-exceptions
    -fno-rtti
    -fpermissive
    -Wno-overflow
    -Wno-write-strings
)

# Linker flags
add_link_options(
    -Wl,--gc-sections
    -Wl,-z,max-page-size=0x1000
    -lvita2d
    -lSceDisplay_stub
    -lSceCtrl_stub
    -lSceGxm_stub
    -lSceAudioOut_stub
    -lSceKernel_stub
    -lSceSysmodule_stub
    -lSceCommonDialog_stub
    -lSceAppMgr_stub
    -lSceIo_stub
    -lScePgf_stub
    -lSceJpeg_stub
    -lScePng_stub
    -lSceNet_stub
    -lSceNetCtl_stub
    -lSceHttp_stub
    -lSceSsl_stub
    -lpthread
    -lm
)

# Create executable
add_executable(cldc_vita ${VM_SOURCES})

target_compile_options(cldc_vita PRIVATE -DMAIN_FUNCTION_NAME=main)
