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

set(DISKATLAS_SRC "${CMAKE_SOURCE_DIR}/src")
set(DISKATLAS_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(DISKATLAS_RES_DIR "${CMAKE_SOURCE_DIR}/resources")
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
  "${DISKATLAS_SRC}/lib/diskatlas_scan_shared.c"
  "${DISKATLAS_SRC}/lib/diskatlas_api.c"
  "${DISKATLAS_SRC}/lib/diskatlas_csv.c"
  "${DISKATLAS_SRC}/lib/diskatlas_index.c"
)
if(WIN32)
  list(APPEND DISKATLAS_CORE_SRCS "${DISKATLAS_SRC}/lib/diskatlas_scan_win32.c")
  list(APPEND DISKATLAS_CORE_SRCS "${DISKATLAS_SRC}/lib/diskatlas_ntfs_raw.c")
  list(APPEND DISKATLAS_CORE_SRCS "${DISKATLAS_SRC}/lib/diskatlas_ntfs_mft.c")
else()
  list(APPEND DISKATLAS_CORE_SRCS "${DISKATLAS_SRC}/lib/diskatlas_scan_posix.c")
endif()

# --- Storage abstraction layer (platform + filesystem) ---
list(APPEND DISKATLAS_CORE_SRCS
  "${DISKATLAS_SRC}/lib/filesystem/filesystem.c"
  "${DISKATLAS_SRC}/lib/platform/platform_path.c"
)
if(WIN32)
  list(APPEND DISKATLAS_CORE_SRCS
    "${DISKATLAS_SRC}/lib/platform/platform_win32.c"
  )
else()
  list(APPEND DISKATLAS_CORE_SRCS
    "${DISKATLAS_SRC}/lib/platform/platform_posix.c"
  )
endif()

add_library(diskatlas_core SHARED ${DISKATLAS_CORE_SRCS})
target_include_directories(diskatlas_core
  PUBLIC
    $<BUILD_INTERFACE:${DISKATLAS_SRC}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    "${DISKATLAS_SRC}/lib/platform"
    "${DISKATLAS_SRC}/lib/filesystem"
)
target_compile_definitions(diskatlas_core PRIVATE DISKATLAS_BUILD_SHARED)
target_link_libraries(diskatlas_core PRIVATE Threads::Threads)
if(WIN32)
  # advapi32: registry / security APIs used by existing scan code.
  # ole32 + oleaut32: CoInitializeEx / CoCreateInstance for WPD/MTP enumeration.
  # portabledeviceapi is a COM in-proc server loaded at runtime — no static
  # import library is needed; ole32 is sufficient for CoCreateInstance.
  target_link_libraries(diskatlas_core PRIVATE advapi32 ole32 oleaut32)
endif()
if(NOT WIN32)
  target_compile_options(diskatlas_core PRIVATE -fvisibility=hidden)
endif()

# Distinct from diskatlas.exe OUTPUT_NAME "diskatlas": MinGW would reuse libdiskatlas.dll.a and
# -Wl,--out-implib on the exe overwrites the DLL import lib → undefined __imp_* refs.
set_target_properties(diskatlas_core PROPERTIES
  OUTPUT_NAME diskatlas_core
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
  if(APPLE)
    enable_language(OBJC)
  endif()
  find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

  set(DISKATLAS_GRESOURCE_XML "${DISKATLAS_RES_DIR}/gresources.xml")
  set(DISKATLAS_GRESOURCE_C "${CMAKE_CURRENT_BINARY_DIR}/diskatlas-resources.c")
  add_custom_command(
    OUTPUT ${DISKATLAS_GRESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES}
    ARGS
      --sourcedir=${DISKATLAS_RES_DIR}
      --generate-source
      --target=${DISKATLAS_GRESOURCE_C}
      --c-name=diskatlas
      ${DISKATLAS_GRESOURCE_XML}
    DEPENDS
      ${DISKATLAS_GRESOURCE_XML}
      ${DISKATLAS_RES_DIR}/ui/diskatlas_window.ui
      ${DISKATLAS_RES_DIR}/ui/settings_dialog.glade
      ${DISKATLAS_RES_DIR}/app.css
    COMMENT "glib-compile-resources diskatlas"
    VERBATIM
  )

  add_executable(diskatlas_app
    "${DISKATLAS_SRC}/app/main.c"
    "${DISKATLAS_SRC}/app/app.c"
    "${DISKATLAS_SRC}/app/ui_window.c"
    "${DISKATLAS_SRC}/app/da_cell_renderer_progress.c"
    "${DISKATLAS_SRC}/app/file_tree_sort.c"
    "${DISKATLAS_SRC}/app/file_tree_model.c"
    "${DISKATLAS_SRC}/app/flat_list_model.c"
    "${DISKATLAS_SRC}/app/tree_view_model.c"
    "${DISKATLAS_SRC}/app/treemap_widget.c"
    "${DISKATLAS_SRC}/app/scan_controller.c"
    "${DISKATLAS_SRC}/app/shell_icon.c"
    "${DISKATLAS_SRC}/app/format_text.c"
    "${DISKATLAS_SRC}/app/scan_source_combo.c"
    "${DISKATLAS_SRC}/app/csv_export.c"
    "${DISKATLAS_SRC}/app/volumes.c"
    "${DISKATLAS_SRC}/app/diskatlas_ini.c"
    "${DISKATLAS_SRC}/app/da_default_mime_categories.c"
    "${DISKATLAS_SRC}/app/settings_mime_tab.c"
    "${DISKATLAS_SRC}/app/dm_mime_db.c"
    ${DISKATLAS_GRESOURCE_C}
  )
  set_target_properties(diskatlas_app PROPERTIES OUTPUT_NAME diskatlas)
  diskatlas_set_exe_bin_output(diskatlas_app)
  target_include_directories(diskatlas_app PRIVATE "${DISKATLAS_SRC}/include")
  target_compile_options(diskatlas_app PRIVATE ${GTK3_CFLAGS_OTHER})
  target_include_directories(diskatlas_app PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_directories(diskatlas_app PRIVATE ${GTK3_LIBRARY_DIRS})
  target_link_libraries(diskatlas_app PRIVATE diskatlas_core ${GTK3_LIBRARIES})
  if(APPLE)
    target_sources(diskatlas_app PRIVATE "${DISKATLAS_SRC}/app/shell_icon_apple.m")
    target_link_libraries(diskatlas_app PRIVATE "-framework" "AppKit")
  endif()
  if(WIN32)
    target_link_libraries(diskatlas_app PRIVATE shell32 advapi32)
    set_target_properties(diskatlas_app PROPERTIES ENABLE_EXPORTS OFF)
  endif()
  if(WIN32 AND MINGW)
    # Omit -mwindows so g_log/GTK errors can print to console when run from shell; remove if you want no console.
  endif()
else()
  if(DISKATLAS_BUILD_GUI)
    message(STATUS "DiskAtlas: GTK+ 3 not found via pkg-config — building core library only (install gtk+-3.0 dev files for the GUI).")
  endif()
endif()
