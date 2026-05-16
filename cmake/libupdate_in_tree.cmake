# Build libupdate from sources under lib/libupdate without modifying that tree's CMake files.
# Paths mirror lib/libupdate/libupdate/CMakeLists.txt and lib/libupdate/updater/CMakeLists.txt.

set(LU_ROOT "${CMAKE_SOURCE_DIR}/lib/libupdate")
set(LU_LIB "${LU_ROOT}/libupdate")
set(LU_UPD "${LU_ROOT}/updater")

if(NOT EXISTS "${LU_LIB}/third_party/miniz/miniz.h")
  message(FATAL_ERROR
    "libupdate miniz missing. From repo root run:\n"
    "  git -C lib/libupdate submodule update --init --recursive")
endif()

add_library(update SHARED
  "${LU_LIB}/src/update.c"
  "${LU_LIB}/src/update_path.c"
  "${LU_LIB}/src/update_remote_check.c"
  "${LU_LIB}/src/update_ops.c"
  "${LU_LIB}/src/update_extract.c"
  "${LU_LIB}/src/json_mini.c"
  "${LU_LIB}/src/sha256.c"
  "${LU_LIB}/src/platform_process.c"
  "${LU_LIB}/src/platform_fs.c"
  "${LU_LIB}/third_party/miniz/miniz.c"
  "${LU_LIB}/third_party/miniz/miniz_tinfl.c"
  "${LU_LIB}/third_party/miniz/miniz_tdef.c"
  "${LU_LIB}/third_party/miniz/miniz_zip.c"
)

if(WIN32 AND NOT CYGWIN)
  target_sources(update PRIVATE "${LU_LIB}/src/http_transport_winhttp.c")
  target_link_libraries(update PRIVATE winhttp)
else()
  target_sources(update PRIVATE "${LU_LIB}/src/http_transport_socket.c")
  target_compile_definitions(update PRIVATE _DEFAULT_SOURCE)
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_compile_definitions(update PRIVATE LIBUPDATE_HAVE_OPENSSL)
    target_link_libraries(update PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  else()
    message(WARNING "OpenSSL not found: libupdate HTTPS may be unavailable")
  endif()
endif()

if(WIN32 AND NOT CYGWIN)
  # Native Windows uses SRWLOCK, not pthread.
else()
  target_link_libraries(update PRIVATE Threads::Threads)
endif()

target_include_directories(update
  PUBLIC
    $<BUILD_INTERFACE:${LU_LIB}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    "${LU_LIB}/platform"
    "${LU_LIB}/src"
    "${LU_LIB}/third_party/miniz"
)

target_compile_definitions(update PRIVATE MINIZ_NO_ARCHIVE_WRITING_APIS)
target_compile_definitions(update PRIVATE "UPDATE_APP_VERSION_STRING=\"${PROJECT_VERSION}\"")

if(WIN32)
  target_compile_definitions(update PRIVATE UPDATE_EXPORTS)
else()
  target_compile_options(update PRIVATE -fvisibility=hidden)
endif()

set_target_properties(update PROPERTIES
  OUTPUT_NAME update
  DEBUG_POSTFIX ""
)

if(WIN32 AND (MINGW OR MSYS))
  set_target_properties(update PROPERTIES PREFIX "")
endif()

diskatlas_set_core_shared_bin_output(update)

# --- updater executable ---
add_executable(updater
  "${LU_UPD}/main.c"
  "${LU_UPD}/update_state.c"
  "${LU_LIB}/src/json_mini.c"
)

target_include_directories(updater PRIVATE
  "${LU_UPD}"
  "${LU_LIB}/src"
)

target_link_libraries(updater PRIVATE update)

diskatlas_set_exe_bin_output(updater)
