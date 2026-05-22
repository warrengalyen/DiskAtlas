cmake_minimum_required(VERSION 3.18)

project(diskatlas
  VERSION 1.0.0
  DESCRIPTION "Disk space analyzer — DiskAtlas (GTK 3 GUI + core library)"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DISKATLAS_BUILD_GUI "Build GTK graphical application" ON)
# When ON: GUI exe uses the Windows subsystem (no extra console). MinGW: -mwindows; MSVC: /SUBSYSTEM:WINDOWS.
option(DISKATLAS_WIN32_WINDOWS_SUBSYSTEM "Windows: link GUI exe without a console (subsystem windows)" OFF)
set(DISKATLAS_UPDATE_MANIFEST_URL "https://www.mechanikadesign.com/software/diskatlas/updates" CACHE STRING
    "HTTPS URL to the update manifest JSON document (e.g. .../update.json). If the URL has no ?query and does not end with .json, CMake appends /update.json (directory-style base). URLs containing ? are left unchanged. Leave empty to disable Help > Check for Updates.")

# URL compiled into the app (libupdate fetches this string as-is).
set(DA_EMBED_UPDATE_MANIFEST_URL "${DISKATLAS_UPDATE_MANIFEST_URL}")
if(NOT DA_EMBED_UPDATE_MANIFEST_URL STREQUAL "")
  string(FIND "${DA_EMBED_UPDATE_MANIFEST_URL}" "?" _da_manifest_query)
  if(_da_manifest_query EQUAL -1)
    string(TOLOWER "${DA_EMBED_UPDATE_MANIFEST_URL}" _da_manifest_low)
    if(NOT _da_manifest_low MATCHES "\\.json$")
      string(REGEX REPLACE "/+$" "" DA_EMBED_UPDATE_MANIFEST_URL "${DA_EMBED_UPDATE_MANIFEST_URL}")
      set(DA_EMBED_UPDATE_MANIFEST_URL "${DA_EMBED_UPDATE_MANIFEST_URL}/update.json")
    endif()
  endif()
endif()

set(DISKATLAS_SRC "${CMAKE_SOURCE_DIR}/src")
set(DISKATLAS_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(DISKATLAS_RES_DIR "${CMAKE_SOURCE_DIR}/resources")
file(MAKE_DIRECTORY "${DISKATLAS_BIN_DIR}")

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/da_update_config.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/da_update_config.h"
  @ONLY
)

option(DISKATLAS_BUILD_LIBUPDATE "Build bundled libupdate (update shared library + updater) into bin/" ON)
option(DISKATLAS_BUILD_TESTS "Build Unity unit/integration tests" ON)

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
  "${DISKATLAS_SRC}/lib/diskatlas_wildcard.c"
  "${DISKATLAS_SRC}/lib/diskatlas_csv_export.c"
  "${DISKATLAS_SRC}/lib/filesystem/filesystem.c"
  "${DISKATLAS_SRC}/lib/platform/platform_path.c"
)
if(WIN32)
  list(APPEND DISKATLAS_CORE_SRCS "${DISKATLAS_SRC}/lib/diskatlas_ntfs_mft_parse.c")
endif()
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

if(DISKATLAS_BUILD_LIBUPDATE AND EXISTS "${CMAKE_SOURCE_DIR}/lib/libupdate/libupdate/third_party/miniz/miniz.h")
  include("${CMAKE_SOURCE_DIR}/cmake/libupdate_in_tree.cmake")
endif()

# --- GTK GUI (pkg-config gtk+-3.0) ---
set(DISKATLAS_GTK3_FOUND FALSE)
if(DISKATLAS_BUILD_GUI)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(GTK3 gtk+-3.0>=3.24)
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
      ${DISKATLAS_RES_DIR}/ui/about_dialog.glade
      ${DISKATLAS_RES_DIR}/ui/treemap_export_dialog.glade
      ${DISKATLAS_RES_DIR}/icons/about-icon.png
      ${DISKATLAS_RES_DIR}/app-icon.ico
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
    "${DISKATLAS_SRC}/app/dm_treemap_colors.c"
    "${DISKATLAS_SRC}/app/scan_controller.c"
    "${DISKATLAS_SRC}/app/file_ops.c"
    "${DISKATLAS_SRC}/app/shell_icon.c"
    "${DISKATLAS_SRC}/app/format_text.c"
    "${DISKATLAS_SRC}/app/scan_source_combo.c"
    "${DISKATLAS_SRC}/app/csv_export.c"
    "${DISKATLAS_SRC}/app/da_message_dialog.c"
    "${DISKATLAS_SRC}/app/volumes.c"
    "${DISKATLAS_SRC}/app/diskatlas_ini.c"
    "${DISKATLAS_SRC}/app/da_default_mime_categories.c"
    "${DISKATLAS_SRC}/app/settings_mime_tab.c"
    "${DISKATLAS_SRC}/app/da_color_picker_dialog.c"
    "${DISKATLAS_SRC}/app/dm_mime_db.c"
    "${DISKATLAS_SRC}/app/file_type_view.c"
    "${DISKATLAS_SRC}/app/win32_shell_context_menu.c"
    "${DISKATLAS_SRC}/app/da_fs_monitor.c"
    "${DISKATLAS_SRC}/app/da_drag_drop.c"
    "${DISKATLAS_SRC}/app/da_app_update.c"
    "${CMAKE_SOURCE_DIR}/shared/gtk_libupdate/gtk_libupdate.c"
    ${DISKATLAS_GRESOURCE_C}
  )
  set_target_properties(diskatlas_app PROPERTIES OUTPUT_NAME diskatlas)
  diskatlas_set_exe_bin_output(diskatlas_app)
  target_include_directories(diskatlas_app PRIVATE "${DISKATLAS_SRC}/include" "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/shared/gtk_libupdate")
  target_compile_options(diskatlas_app PRIVATE ${GTK3_CFLAGS_OTHER})
  target_include_directories(diskatlas_app PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_directories(diskatlas_app PRIVATE ${GTK3_LIBRARY_DIRS})
  target_link_libraries(diskatlas_app PRIVATE diskatlas_core ${GTK3_LIBRARIES})
  if(NOT WIN32)
    target_link_libraries(diskatlas_app PRIVATE m)
  endif()
  target_compile_definitions(diskatlas_app PRIVATE
    "GTK_LIBUPDATE_HAVE_LIBUPDATE=$<BOOL:$<TARGET_EXISTS:update>>")
  if(TARGET update)
    target_link_libraries(diskatlas_app PRIVATE update)
  endif()
  if(APPLE)
    target_sources(diskatlas_app PRIVATE "${DISKATLAS_SRC}/app/shell_icon_apple.m")
    target_link_libraries(diskatlas_app PRIVATE "-framework" "AppKit")
  endif()
  if(WIN32)
    enable_language(RC)
    set(DISKATLAS_WIN_ICON_PATH_SLASH "${DISKATLAS_RES_DIR}/app-icon.ico")
    string(REPLACE "\\" "/" DISKATLAS_WIN_ICON_PATH_SLASH "${DISKATLAS_WIN_ICON_PATH_SLASH}")
    set(DISKATLAS_WIN_APP_RC_GEN "${CMAKE_CURRENT_BINARY_DIR}/diskatlas_win_app.rc")
    configure_file(
      "${DISKATLAS_RES_DIR}/diskatlas_win_app.rc.in"
      "${DISKATLAS_WIN_APP_RC_GEN}"
      @ONLY
    )
    target_sources(diskatlas_app PRIVATE "${DISKATLAS_WIN_APP_RC_GEN}")
    set_source_files_properties("${DISKATLAS_WIN_APP_RC_GEN}" PROPERTIES
      OBJECT_DEPENDS "${DISKATLAS_RES_DIR}/app-icon.ico"
    )
    target_link_libraries(diskatlas_app PRIVATE shell32 advapi32 ole32)
    set_target_properties(diskatlas_app PROPERTIES ENABLE_EXPORTS OFF)
    if(DISKATLAS_WIN32_WINDOWS_SUBSYSTEM)
      if(MINGW OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_options(diskatlas_app PRIVATE "-mwindows")
      elseif(MSVC)
        target_link_options(diskatlas_app PRIVATE
          "/SUBSYSTEM:WINDOWS"
          "/ENTRY:mainCRTStartup"
        )
      endif()
    endif()
  endif()
else()
  if(DISKATLAS_BUILD_GUI)
    message(STATUS "DiskAtlas: GTK+ 3 not found via pkg-config — building core library only (install gtk+-3.0 dev files for the GUI).")
  endif()
endif()

if(DISKATLAS_BUILD_TESTS)
  enable_testing()
  set(UNITY_SUPPORT_INT64 ON CACHE BOOL "Unity 64-bit integers" FORCE)
  add_subdirectory(lib/unity)
  add_subdirectory(tests)
endif()
