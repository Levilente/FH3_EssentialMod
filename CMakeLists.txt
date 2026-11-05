cmake_minimum_required(VERSION 3.20)
# Bump VERSION before tagging a release (the tag build checks they match).
project(fh3_mod_platform VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT WIN32)
    message(FATAL_ERROR "Targets Windows. On Linux use -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake")
endif()

# When this repo is added to a mod with add_subdirectory(), the mod gets:
#   fh3mod_platform        the d3dcompiler_47.dll to ship next to azimuth.exe
#   fh3mod_api             interface target with include/fh3mod.h
#   FH3MOD_PLATFORM_VERSION_STRING   "2.0.0", to pass in FH3ModInfo
set(FH3MOD_PLATFORM_VERSION ${PROJECT_VERSION})
set(FH3MOD_PLATFORM_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(FH3MOD_PLATFORM_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(FH3MOD_PLATFORM_VERSION_PATCH ${PROJECT_VERSION_PATCH})
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FH3MOD_PLATFORM_VERSION_STRING ${PROJECT_VERSION} PARENT_SCOPE)
endif()
configure_file(cmake/platform_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/platform_version.h)

set(_warn)
if(MSVC)
    set(_warn /W4 /permissive- /utf-8 /EHsc)
else()
    # -fms-extensions: __try/__except around each mod's fh3mod_init.
    set(_warn -Wall -Wextra -Wno-missing-field-initializers -fms-extensions)
endif()

add_library(fh3mod_api INTERFACE)
target_include_directories(fh3mod_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(fh3mod_api INTERFACE FH3MOD_PLATFORM_VERSION="${PROJECT_VERSION}")

# MinHook (BSD 2-Clause, third_party/minhook/LICENSE.txt), built into the
# platform so every mod's hooks go through one copy of it.
set(_minhook third_party/minhook/src)
set(_minhook_src ${_minhook}/hook.c ${_minhook}/buffer.c ${_minhook}/trampoline.c ${_minhook}/hde/hde64.c)
set_source_files_properties(${_minhook_src} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<BOOL:${MSVC}>>:-w>")

# SQLite (public domain, third_party/sqlite/VERSION.txt), built in for the game
# database changes. Windows has its own (winsqlite3.dll), but Wine and Proton
# don't, so relying on it switched database changes off for Linux players.
set(_sqlite_src third_party/sqlite/sqlite3.c)
set_source_files_properties(${_sqlite_src} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/W0;/O1,-w;-Os>"
    COMPILE_DEFINITIONS "SQLITE_OMIT_LOAD_EXTENSION;SQLITE_DEFAULT_MEMSTATUS=0")

add_library(fh3mod_platform SHARED src/platform.cpp src/strings.cpp src/updates.cpp src/files.cpp
            src/database.cpp src/media_overrides.cpp src/ui_patches.cpp src/mods_page.cpp src/intro_skip.cpp src/titles.cpp src/session.cpp src/hotkeys.cpp src/commands.cpp src/crash.cpp src/hooks.cpp src/upgrades.cpp src/settings.cpp src/version.rc ${_minhook_src} ${_sqlite_src})
target_include_directories(fh3mod_platform PRIVATE third_party/minhook/include ${_minhook} third_party/sqlite)
if(NOT MSVC)
    # InterlockedCompareExchange128, for rewriting call sites atomically.
    set_source_files_properties(src/upgrades.cpp PROPERTIES COMPILE_OPTIONS -fms-extensions)
    set_source_files_properties(src/hooks.cpp PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
if(NOT MSVC)
    target_sources(fh3mod_platform PRIVATE src/d3dcompiler_47.def)
    target_link_options(fh3mod_platform PRIVATE "$<$<CONFIG:Release>:-Wl,--gc-sections;-s>" -static)
endif()
target_compile_options(fh3mod_platform PRIVATE ${_warn} "$<$<AND:$<NOT:$<BOOL:${MSVC}>>,$<CONFIG:Release>>:-O2;-ffunction-sections;-fdata-sections>")
target_compile_definitions(fh3mod_platform PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                           FH3MOD_PLATFORM_VERSION="${PROJECT_VERSION}")
target_include_directories(fh3mod_platform PRIVATE include src ${CMAKE_CURRENT_BINARY_DIR}/generated)
# Only kernel32/user32 (and the C runtime) at load time: azimuth.exe loads
# this DLL very early, and an app-folder version.dll or winhttp.dll proxy
# from some other mod must not get pulled in by us. WinHTTP is loaded from
# System32 when the first update check runs.
target_link_libraries(fh3mod_platform PRIVATE user32)
set_target_properties(fh3mod_platform PROPERTIES OUTPUT_NAME d3dcompiler_47 PREFIX "")
# Azimuth 1.0.3 no longer imports d3dcompiler_47.dll, while 1.0.1 and later import DWrite.dll.
# The DLL forwards both, so the build leaves a copy of it under that name too.
add_custom_command(TARGET fh3mod_platform POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fh3mod_platform>
            $<TARGET_FILE_DIR:fh3mod_platform>/DWrite.dll)

# Only when this is the top-level project: the example mod and the tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_library(hello_mod SHARED examples/hello_mod/hello_mod.cpp)
    target_link_libraries(hello_mod PRIVATE fh3mod_api)
    target_compile_options(hello_mod PRIVATE ${_warn})
    if(NOT MSVC)
        target_link_options(hello_mod PRIVATE -static)
    endif()
    set_target_properties(hello_mod PROPERTIES OUTPUT_NAME HelloMod PREFIX "")

    # fh3mod.exe: runs mods' commands in the running game (docs/api.md, Commands).
    add_executable(fh3mod_cli tools/fh3mod.cpp)
    target_compile_definitions(fh3mod_cli PRIVATE UNICODE _UNICODE)
    target_compile_options(fh3mod_cli PRIVATE ${_warn})
    if(NOT MSVC)
        target_link_options(fh3mod_cli PRIVATE -municode -static)
    endif()
    set_target_properties(fh3mod_cli PROPERTIES OUTPUT_NAME fh3mod)
endif()
