# 从本仓库根目录构建（不必先 cd mcp）：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target learn_mcp_server -j
# 若尚未拉子模块，请先：git submodule update --init --recursive
cmake_minimum_required(VERSION 3.16)
project(gq_mcp LANGUAGES CXX)

add_subdirectory(mcp)
