cmake_minimum_required(VERSION 3.18)
project(lua-test LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
)
FetchContent_MakeAvailable(lua)

# Lua's tarball has no CMakeLists.txt, so build the library manually from its
# sources, excluding the two standalone CLI files (lua.c = interpreter,
# luac.c = compiler) which each define their own main().
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM LUA_SOURCES
    ${lua_SOURCE_DIR}/src/lua.c
    ${lua_SOURCE_DIR}/src/luac.c
)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR}/src)

if(UNIX AND NOT APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua PUBLIC m dl)
elseif(APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
    target_link_libraries(lua PUBLIC m)
elseif(WIN32)
    # No extra defines needed; Lua builds fine with default settings on Windows.
endif()

add_executable(lua-test src/main.cpp)
target_link_libraries(lua-test PRIVATE lua)