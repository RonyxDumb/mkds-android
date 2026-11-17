cmake_minimum_required(VERSION 3.22)
project(mkds_port C CXX)

# The game assumes 32-bit pointers (structures and files loaded in place): only 32-bit ARM is supported.
if (ANDROID AND NOT ANDROID_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "The port builds for armeabi-v7a only (got ${ANDROID_ABI})")
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

# ------------------------------------------------------------------ platform layer
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.cpp)
if (ANDROID)
    file(GLOB ANDROID_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/android/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/platform/android/*.cpp)
    list(APPEND PLATFORM_SOURCES ${ANDROID_SOURCES})
endif()

if (ANDROID)
    add_library(mkds SHARED ${PLATFORM_SOURCES})
else()
    # Headless test build for a 32-bit Linux host (tools/run_host.sh).
    file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/*.c)
    add_executable(mkds ${PLATFORM_SOURCES} ${HOST_SOURCES})
    target_link_libraries(mkds PRIVATE pthread m)
endif()
target_include_directories(mkds PRIVATE
    ${GEN}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    )
target_compile_definitions(mkds PRIVATE version_EUR)
target_compile_options(mkds PRIVATE -O2 -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prefix.h>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17 -fno-exceptions -fno-rtti>)
target_link_libraries(mkds PRIVATE -Wl,--whole-archive game -Wl,--no-whole-archive)
if (ANDROID)
    target_link_libraries(mkds PRIVATE android log EGL GLESv3 aaudio m)
endif()
target_link_options(mkds PRIVATE -Wl,--no-undefined -Wl,--error-limit=0)
if (ANDROID)
    target_link_options(mkds PRIVATE -Wl,-z,max-page-size=16384)
endif()
