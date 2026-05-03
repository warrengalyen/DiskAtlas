cmake_minimum_required(VERSION 3.18)

project(diskatlas
  VERSION 0.1.0
  DESCRIPTION "Disk space analyzer — DiskAtlas (Win32 GUI + core DLL)"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Binary outputs live in repo root ../bin (CMAKE_SOURCE_DIR = .../DiskAtlas/src).
set(DISKATLAS_BIN_DIR "${CMAKE_SOURCE_DIR}/../bin")
file(MAKE_DIRECTORY "${DISKATLAS_BIN_DIR}")

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

add_library(diskatlas_core SHARED
  lib/diskatlas.c
  lib/diskatlas_index.c
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

if(WIN32)
  add_executable(diskatlas_gui WIN32 app/main.c app/diskatlas.rc)
  set_target_properties(diskatlas_gui PROPERTIES
    OUTPUT_NAME diskatlas
  )

  diskatlas_set_core_shared_bin_output(diskatlas_core)
  diskatlas_set_exe_bin_output(diskatlas_gui)

  target_include_directories(diskatlas_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_definitions(diskatlas_gui PRIVATE UNICODE _UNICODE)
  target_link_libraries(diskatlas_gui PRIVATE
    diskatlas_core
    comctl32
    shell32
    user32
    gdi32
    ole32
    uuid
  )

  if(MINGW)
    target_link_options(diskatlas_gui PRIVATE -mwindows)
  endif()
else()
  diskatlas_set_core_shared_bin_output(diskatlas_core)
endif()
