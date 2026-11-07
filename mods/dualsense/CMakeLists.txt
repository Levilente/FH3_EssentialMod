cmake_minimum_required(VERSION 3.20)
project(fh3_dualsense VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The FH3 Mod Platform checkout (only include/fh3mod.h is needed to build a mod); this folder lives in it.
set(FH3_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "FH3 Mod Platform checkout")
if(NOT EXISTS "${FH3_PLATFORM_DIR}/include/fh3mod.h")
    message(FATAL_ERROR "fh3mod.h not found in ${FH3_PLATFORM_DIR}/include")
endif()
file(STRINGS "${FH3_PLATFORM_DIR}/CMakeLists.txt" _pv REGEX "^project\\(fh3_mod_platform VERSION")
string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" FH3_PLATFORM_VERSION "${_pv}")

add_library(dualsense SHARED dualsense.cpp)
target_include_directories(dualsense PRIVATE "${FH3_PLATFORM_DIR}/include")
target_compile_definitions(dualsense PRIVATE
    DUALSENSE_VERSION="${PROJECT_VERSION}"
    FH3MOD_PLATFORM_VERSION="${FH3_PLATFORM_VERSION}"
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dualsense PRIVATE setupapi hid ole32 winmm)
if(NOT MSVC)
    # -fms-extensions: __try/__except around reads of game memory.
    target_compile_options(dualsense PRIVATE -Wall -Wextra -fms-extensions "$<$<CONFIG:Release>:-O2>")
    target_link_options(dualsense PRIVATE -static "$<$<CONFIG:Release>:-s>")
endif()
set_target_properties(dualsense PROPERTIES OUTPUT_NAME DualSense PREFIX "")
