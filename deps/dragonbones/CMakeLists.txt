# Vendored DragonBonesCPP core (MIT) — the renderer-agnostic 2D-skeletal runtime.
# ANIMATED_IMPORTS.md: we embed this core and add a Skia display adapter; the
# core (armature/animation/model/parser) has NO engine or render dependencies —
# only header-only rapidjson (bundled in 3rdParty/). Built isolated at C++17 to
# avoid clashing with the project's C++23 (mirrors deps/timelinefx).
cmake_minimum_required(VERSION 3.22)
project(dragonBones CXX)

file(GLOB_RECURSE DB_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/DragonBones/src/*.cpp")

add_library(dragonBones STATIC ${DB_CORE_SRC})

target_include_directories(dragonBones PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/DragonBones/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty")

set_target_properties(dragonBones PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(dragonBones PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(dragonBones PRIVATE /wd4996 /wd4244 /wd4267 /wd4305)
endif()
