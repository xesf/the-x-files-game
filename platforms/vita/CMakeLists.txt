# SPDX-License-Identifier: GPL-3.0-or-later
# PlayStation Vita build (VitaSDK):
#
#   cmake -S platforms/vita -B build/vita && cmake --build build/vita
#     -> build/vita/xfiles.vpk (title XFIL00001, game files in ux0:data/xfiles)
#
# Needs VitaSDK with $VITASDK set, or the vitasdk/vitasdk Docker image
# (make vita). Compiles ../../src directly; the platform layer is
# src/platform/vita/plat_vita.c.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "VITASDK is not set: install VitaSDK and export VITASDK")
    endif()
endif()

project(xfiles C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# APP_VER is "XX.YY": a version tag v0.MINOR.PATCH becomes MINOR.PATCH.
set(GAME_VERSION "" CACHE STRING "Release version, for example v0.1.0")
set(VITA_VERSION "00.01")
if(GAME_VERSION MATCHES "^v?0\\.([0-9]+)\\.([0-9]+)")
    set(ver_minor ${CMAKE_MATCH_1})
    set(ver_patch ${CMAKE_MATCH_2})
    if(ver_minor LESS 10)
        set(ver_minor "0${ver_minor}")
    endif()
    if(ver_patch LESS 10)
        set(ver_patch "0${ver_patch}")
    endif()
    set(VITA_VERSION "${ver_minor}.${ver_patch}")
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
file(GLOB ENGINE_SRCS
    ${SRC}/core/*.c ${SRC}/media/*.c ${SRC}/data/*.c ${SRC}/audio/*.c
    ${SRC}/gfx/*.c ${SRC}/vc/*.c ${SRC}/game/*.c ${SRC}/main.c)

add_executable(xfiles ${ENGINE_SRCS} ${SRC}/platform/vita/plat_vita.c ${SRC}/platform/vita/vita_gpu.c)
target_include_directories(xfiles PRIVATE ${SRC} ${SRC}/core)
set_property(TARGET xfiles PROPERTY C_STANDARD 99)
# char is unsigned on ARM; the engine is written for signed char.
target_compile_options(xfiles PRIVATE -O2 -Wall -fsigned-char -Wno-unused-parameter)
target_link_libraries(xfiles SceGxm_stub SceCommonDialog_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub m)

vita_create_self(eboot.bin xfiles UNSAFE)
vita_create_vpk(xfiles.vpk XFIL00001 eboot.bin
    VERSION ${VITA_VERSION}
    NAME "X-Files engine"
    FILE sce_sys/icon0.png sce_sys/icon0.png
         sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
         sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
         sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
