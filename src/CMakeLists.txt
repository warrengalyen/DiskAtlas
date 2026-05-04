cmake_minimum_required(VERSION 3.18)

project(diskatlas
  VERSION 0.1.0
  DESCRIPTION "Disk space analyzer — DiskAtlas (GTK 3 GUI + core library)"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DISKATLAS_BUILD_GUI "Build GTK graphical application" ON)
option(DISKATLAS_LEGACY_WIN32_GUI "Build legacy Win32 GUI (requires GTK off or separate config)" OFF)

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

find_package(Threads REQUIRED)

set(DISKATLAS_CORE_SRCS
  lib/diskatlas_scan_shared.c
  lib/diskatlas_api.c
  lib/diskatlas_index.c
)
if(WIN32)
  list(APPEND DISKATLAS_CORE_SRCS lib/diskatlas_scan_win32.c)
else()
  list(APPEND DISKATLAS_CORE_SRCS lib/diskatlas_scan_posix.c)
endif()

add_library(diskatlas_core SHARED ${DISKATLAS_CORE_SRCS})
target_include_directories(diskatlas_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(diskatlas_core PRIVATE DISKATLAS_BUILD_SHARED)
target_link_libraries(diskatlas_core PRIVATE Threads::Threads)
if(NOT WIN32)
  target_compile_options(diskatlas_core PRIVATE -fvisibility=hidden)
endif()

set_target_properties(diskatlas_core PROPERTIES
  OUTPUT_NAME diskatlas
)
if(WIN32)
  set_target_properties(diskatlas_core PROPERTIES PREFIX "")
endif()

diskatlas_set_core_shared_bin_output(diskatlas_core)

# --- GTK GUI (pkg-config gtk+-3.0) ---
set(DISKATLAS_GTK3_FOUND FALSE)
if(DISKATLAS_BUILD_GUI)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(GTK3 gtk+-3.0)
    if(GTK3_FOUND)
      set(DISKATLAS_GTK3_FOUND TRUE)
    endif()
  endif()
endif()

if(DISKATLAS_GTK3_FOUND)
  add_executable(diskatlas
    app/main.c
    app/app.c
    app/ui_window.c
    app/file_tree_model.c
    app/treemap_widget.c
    app/scan_controller.c
    app/format_text.c
    app/volumes.c
  )
  set_target_properties(diskatlas PROPERTIES OUTPUT_NAME diskatlas)
  diskatlas_set_exe_bin_output(diskatlas)
  target_include_directories(diskatlas PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_options(diskatlas PRIVATE ${GTK3_CFLAGS_OTHER})
  target_include_directories(diskatlas PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_directories(diskatlas PRIVATE ${GTK3_LIBRARY_DIRS})
  target_link_libraries(diskatlas PRIVATE diskatlas_core ${GTK3_LIBRARIES})
  if(WIN32 AND MINGW)
    # Omit -mwindows so g_log/GTK errors can print to console when run from shell; remove if you want no console.
  endif()
elseif(WIN32 AND DISKATLAS_LEGACY_WIN32_GUI)
  add_executable(diskatlas_gui WIN32 app/legacy_win32_main.c app/diskatlas.rc)
  set_target_properties(diskatlas_gui PROPERTIES OUTPUT_NAME diskatlas)
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
  if(DISKATLAS_BUILD_GUI)
    message(STATUS "DiskAtlas: GTK+ 3 not found via pkg-config — building core library only (install gtk+-3.0 dev files for the GUI).")
  endif()
endif()
