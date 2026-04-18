cmake_minimum_required(VERSION 3.16)
project(learn_mcp_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 单配置生成器（Make/Ninja）下默认 Release，避免未指定时 Debug 体积与性能意外
if(NOT CMAKE_CONFIGURATION_TYPES)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "learn_mcp_server / cpp-mcp build type" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# IDE/clangd：生成 compile_commands.json（多配置生成器上由 CMake 按配置生成）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default: git submodule at mcp/third_party/cpp-mcp (see repo root .gitmodules).
# Override with -DCPP_MCP_ROOT=... or fall back to legacy sibling / FetchContent.
set(CPP_MCP_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/third_party/cpp-mcp")
set(CPP_MCP_ROOT "${CPP_MCP_DEFAULT}" CACHE PATH "Path to cpp-mcp source tree (submodule: Andersonhere/cpp-mcp)")

option(
  GQ_MCP_FETCH_CPP_MCP_IF_MISSING
  "If cpp-mcp is not on disk, clone it via FetchContent (needs network). Otherwise CMake fails with a hint."
  ON
)

if(EXISTS "${CPP_MCP_ROOT}/CMakeLists.txt")
  message(STATUS "Using cpp-mcp at: ${CPP_MCP_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream/CMakeLists.txt")
  set(CPP_MCP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream")
  message(STATUS "Using legacy cpp-mcp path at: ${CPP_MCP_ROOT}")
elseif(GQ_MCP_FETCH_CPP_MCP_IF_MISSING)
  message(STATUS "cpp-mcp not found under ${CPP_MCP_DEFAULT}; cloning via FetchContent (GIT_TAG=main)")
  include(FetchContent)
  FetchContent_Declare(
    cpp_mcp_upstream
    GIT_REPOSITORY https://github.com/Andersonhere/cpp-mcp.git
    GIT_TAG main
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cpp_mcp_upstream)
  set(CPP_MCP_ROOT "${cpp_mcp_upstream_SOURCE_DIR}" CACHE PATH "Path to cpp-mcp source tree" FORCE)
  message(STATUS "FetchContent cpp-mcp available at: ${CPP_MCP_ROOT}")
else()
  message(
    FATAL_ERROR
    "cpp-mcp not found.\n"
    "  Expected: ${CPP_MCP_DEFAULT}/CMakeLists.txt\n"
    "  Fix: from repo root run  git submodule update --init --recursive\n"
    "  Or re-configure with  -DGQ_MCP_FETCH_CPP_MCP_IF_MISSING=ON"
  )
endif()

set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CPP_MCP_ROOT}" "${CMAKE_BINARY_DIR}/_cpp_mcp")

add_executable(learn_mcp_server src/learn_mcp_server.cpp)
target_link_libraries(learn_mcp_server PRIVATE mcp)
target_include_directories(
  learn_mcp_server
  PRIVATE
  "${CPP_MCP_ROOT}/include"
  "${CPP_MCP_ROOT}/common"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(learn_mcp_server PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
install(TARGETS learn_mcp_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
