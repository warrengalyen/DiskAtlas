cmake_minimum_required(VERSION 3.18)

project(diskatlas
  VERSION 0.1.0
  DESCRIPTION "Disk space analyzer - DiskAtlas"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Binary outputs live in repo root ../bin (CMAKE_SOURCE_DIR = .../DiskAtlas/src).
set(DISKATLAS_BIN_DIR "${CMAKE_SOURCE_DIR}/../bin")
file(MAKE_DIRECTORY "${DISKATLAS_BIN_DIR}")

# Executable: runtime only (.exe).
function(diskatlas_set_exe_bin_output target)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DISKATLAS_BIN_DIR}"
  )
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
      set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${DISKATLAS_BIN_DIR}"
      )
    endforeach()
  endif()
endfunction()

# Core SHARED: only ship the loadable module in bin (.dll / .so). Do not mirror the
# link-time import artifact (Windows: *.dll.a / MSVC *.lib stays under build/).
function(diskatlas_set_core_shared_bin_output target)
  if(WIN32)
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${DISKATLAS_BIN_DIR}"
    )
    if(CMAKE_CONFIGURATION_TYPES)
      foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        set_target_properties(${target} PROPERTIES
          RUNTIME_OUTPUT_DIRECTORY_${cfg} "${DISKATLAS_BIN_DIR}"
        )
      endforeach()
    endif()
  else()
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${DISKATLAS_BIN_DIR}"
    )
    if(CMAKE_CONFIGURATION_TYPES)
      foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        set_target_properties(${target} PROPERTIES
          LIBRARY_OUTPUT_DIRECTORY_${cfg} "${DISKATLAS_BIN_DIR}"
        )
      endforeach()
    endif()
  endif()
endfunction()

# Prefer GTK3 and transitive deps from .pc as static libs (no GTK DLLs bundled with releases).
set(PKG_CONFIG_USE_STATIC_LIBS ON)

find_package(PkgConfig REQUIRED)
# With PKG_CONFIG_USE_STATIC_LIBS=ON, pkg-config resolves static deps (CMake 4.x: no separate STATIC token here).
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_library(diskatlas_core SHARED
  lib/diskatlas.c
)
target_include_directories(diskatlas_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
  target_compile_definitions(diskatlas_core PRIVATE DISKATLAS_BUILD_SHARED)
endif()

set_target_properties(diskatlas_core PROPERTIES
  OUTPUT_NAME diskatlas
)
if(WIN32)
  set_target_properties(diskatlas_core PROPERTIES PREFIX "")
endif()

add_executable(diskatlas_gui WIN32 app/main.c)
set_target_properties(diskatlas_gui PROPERTIES
  OUTPUT_NAME diskatlas
)

diskatlas_set_core_shared_bin_output(diskatlas_core)
diskatlas_set_exe_bin_output(diskatlas_gui)

target_include_directories(diskatlas_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# MinGW: static archives pull circular refs; MSVC link.exe ignores GNU link-group flags.
if(WIN32 AND NOT MSVC)
  target_link_libraries(diskatlas_gui PRIVATE
    -Wl,--start-group
    diskatlas_core
    PkgConfig::GTK3
    -Wl,--end-group
  )
else()
  target_link_libraries(diskatlas_gui PRIVATE diskatlas_core PkgConfig::GTK3)
endif()

if(MINGW)
  target_link_options(diskatlas_gui PRIVATE -mwindows)
endif()
