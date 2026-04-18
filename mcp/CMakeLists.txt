cmake_minimum_required(VERSION 3.16)
project(learn_mcp_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default: git submodule at mcp/third_party/cpp-mcp (see repo root .gitmodules).
# Override with -DCPP_MCP_ROOT=... or fall back to legacy sibling / FetchContent.
set(CPP_MCP_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/third_party/cpp-mcp")
set(CPP_MCP_ROOT "${CPP_MCP_DEFAULT}" CACHE PATH "Path to hkr04/cpp-mcp source tree")

if(EXISTS "${CPP_MCP_ROOT}/CMakeLists.txt")
  message(STATUS "Using cpp-mcp at ${CPP_MCP_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream/CMakeLists.txt")
  set(CPP_MCP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream")
  message(STATUS "Using legacy cpp-mcp path at ${CPP_MCP_ROOT}")
else()
  include(FetchContent)
  FetchContent_Declare(
    cpp_mcp_upstream
    GIT_REPOSITORY https://github.com/hkr04/cpp-mcp.git
    GIT_TAG main
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cpp_mcp_upstream)
  set(CPP_MCP_ROOT "${cpp_mcp_upstream_SOURCE_DIR}" CACHE PATH "Path to hkr04/cpp-mcp source tree" FORCE)
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
