# DiskAtlas is configured from the repository root (parent directory).
#
#   cmake -S .. -B ../build
#
# Out-of-tree builds place diskatlas_core and diskatlas_app artifacts under ../build/
# (not under src/build/).

cmake_minimum_required(VERSION 3.18)
message(FATAL_ERROR
  "Configure DiskAtlas from the repository root, not from src/.\n"
  "Example:\n"
  "  cd \"${CMAKE_CURRENT_LIST_DIR}/..\"\n"
  "  cmake -S . -B build\n"
  "  cmake --build build"
)
