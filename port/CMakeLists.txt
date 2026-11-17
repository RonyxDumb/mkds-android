cmake_minimum_required(VERSION 3.22)
project(mkds_port C)

# The game assumes 32-bit pointers (structures and files loaded in place).
# - 32-bit targets (armeabi-v7a, the i386 Linux test build): one image, the game linked with the platform layer.
# - 64-bit ARM (arm64-v8a, aarch64 Linux): the game and the DS platform layer form a separate "guest" image built
#   with GCC for AArch64 with 32-bit pointers (tools/build_guest.py), loaded below 4 GiB by the host library.
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "64-bit builds are for AArch64 only")
    endif()
    set(GUEST_SPLIT ON)
endif()

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DECOMP ${REPO}/decomp)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(DECOMP_VERSION eur)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ------------------------------------------------------------------ decomp sources, prepared for clang
file(GLOB_RECURSE DECOMP_INPUTS CONFIGURE_DEPENDS ${DECOMP}/src/*.c ${DECOMP}/libs/*.c ${DECOMP}/include/types.h
     ${DECOMP}/config/${DECOMP_VERSION}/arm9/overlays/*/delinks.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${REPO}/tools/portify.py ${CMAKE_CURRENT_SOURCE_DIR}/exclude.txt)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${REPO}/tools/portify.py ${DECOMP} ${GEN} ${CMAKE_CURRENT_SOURCE_DIR}/exclude.txt ${DECOMP_VERSION}
    RESULT_VARIABLE PORTIFY_RESULT)
if (NOT PORTIFY_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/portify.py failed")
endif()
file(STRINGS ${GEN}/sources.txt GAME_SOURCES)

file(GLOB_RECURSE DECOMP_INCLUDE_DIRS LIST_DIRECTORIES true ${DECOMP}/libs/*)
list(FILTER DECOMP_INCLUDE_DIRS INCLUDE REGEX "/include$")
list(SORT DECOMP_INCLUDE_DIRS)

if (NOT GUEST_SPLIT)
add_library(game STATIC ${GAME_SOURCES})
target_include_directories(game PRIVATE
    ${GEN}/include
    ${GEN}/msl_include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    )
target_compile_definitions(game PRIVATE version_EUR)
target_compile_options(game PRIVATE
    -std=gnu11
    -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prefix.h
    -O2
    -fno-strict-aliasing   # the game type-puns freely
    -fwrapv                # and relies on wrapping arithmetic
    -fcommon               # tentative definitions shared between files, as CodeWarrior allows
    -fshort-wchar          # wchar_t is 16 bits on the DS
    -Dconst=               # constant data is RAM on the DS and the game patches some of it: keep it writable
    -ffunction-sections
    -w)
# Plain char: unsigned in game code, signed in the libraries (tools/project.py).
foreach(SRC ${GAME_SOURCES})
    if (SRC MATCHES "/src/libs/")
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -fsigned-char)
    else()
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -funsigned-char)
    endif()
endforeach()
endif()

# ------------------------------------------------------------------ platform layer
if (GUEST_SPLIT)
    # The host library: the renderers, the host side of the system services and the loader of the guest image.
    set(PLATFORM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/gpu2d.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/tex.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/soft3d.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hostside/memory.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hostside/hostapi_impl.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hostside/bridge.c
        ${CMAKE_CURRENT_BINARY_DIR}/guest_image.S)
    set(GUEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/guest)
    set(GUEST_CROSS "aarch64-linux-gnu-" CACHE STRING "Prefix of the GCC cross toolchain for the guest image")
    add_custom_target(guest_image ALL
        COMMAND ${Python3_EXECUTABLE} ${REPO}/tools/build_guest.py ${GEN} ${GUEST_DIR} ${GUEST_CROSS}
        BYPRODUCTS ${GUEST_DIR}/guest.elf
        COMMENT "Building the guest image (game, 32-bit pointers)")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/guest_image.S
        ".section .rodata\n.balign 64\n.globl guest_image_start\nguest_image_start:\n"
        ".incbin \"${GUEST_DIR}/guest.elf\"\n.globl guest_image_end\nguest_image_end:\n"
        ".section .note.GNU-stack,\"\",%progbits\n")
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/guest_image.S PROPERTIES
        OBJECT_DEPENDS ${GUEST_DIR}/guest.elf)
    enable_language(ASM)
else()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/hostside/*.c)
    list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/hostside/bridge.c)
endif()
option(HOST_GL "Host test build: render with OpenGL ES (EGL, Mesa)" OFF)
file(GLOB GL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/gl/*.c)
if (ANDROID)
    file(GLOB ANDROID_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/android/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/platform/android/*.cpp)
    list(APPEND PLATFORM_SOURCES ${ANDROID_SOURCES} ${GL_SOURCES})
elseif (HOST_GL)
    list(APPEND PLATFORM_SOURCES ${GL_SOURCES})
endif()

if (ANDROID)
    add_library(mkds SHARED ${PLATFORM_SOURCES})
else()
    # Headless test build for Linux (i386, or AArch64 with the guest image).
    file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/*.c)
    add_executable(mkds ${PLATFORM_SOURCES} ${HOST_SOURCES})
    target_link_libraries(mkds PRIVATE pthread m)
    target_link_options(mkds PRIVATE -Wl,-z,now)
    if (HOST_GL)
        target_compile_definitions(mkds PRIVATE HOST_GL)
        target_link_libraries(mkds PRIVATE EGL GLESv2)
    endif()
endif()
target_include_directories(mkds PRIVATE
    ${GEN}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/gl
    )
target_compile_definitions(mkds PRIVATE version_EUR)
target_compile_options(mkds PRIVATE -O2 -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    )
if (GUEST_SPLIT)
    add_dependencies(mkds guest_image)
    target_include_directories(mkds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/guest)
else()
    target_link_libraries(mkds PRIVATE -Wl,--whole-archive game -Wl,--no-whole-archive)
endif()
if (ANDROID)
    target_link_libraries(mkds PRIVATE android log EGL GLESv3 aaudio m)
endif()
# The game writes to some of its constant data (pointers patched into tables): no read-only relocations.
target_link_options(mkds PRIVATE -Wl,--no-undefined -Wl,-z,norelro)
if (ANDROID)
    target_link_options(mkds PRIVATE -Wl,-z,max-page-size=16384)
endif()
