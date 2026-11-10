# SPDX-License-Identifier: Apache-2.0
#
# Zephyr port of tiny-edge: the thin-edge.io device-management client as a
# Zephyr module. Nothing is built unless the application enables CONFIG_TEDGE,
# so an image without it carries no code, RAM or threads from this module.
#
# The portable client lives in core/ and the public API in include/, both at
# the repository root; this directory adds the Zephyr build glue and the
# sources that only make sense on Zephyr (src/).

if(CONFIG_TEDGE)
  get_filename_component(TEDGE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
  file(STRINGS ${TEDGE_ROOT}/VERSION TEDGE_VERSION LIMIT_COUNT 1)

  zephyr_library_named(tedge)
  # The core's files for the features this build has (core/sources.cmake),
  # then the Zephyr side of the port interface.
  include(${TEDGE_ROOT}/core/sources.cmake)
  include(${TEDGE_ROOT}/provisioning/sources.cmake)
  tedge_core_sources(_core_sources CONFIG_)
  tedge_provisioning_sources(_provisioning_sources CONFIG_)
  zephyr_library_sources(
    ${_core_sources}
    ${_provisioning_sources}
    src/tedge_platform.c
    src/tedge_port.c
    src/tedge_port_os.c
    src/tedge_port_kv.c
    src/tedge_port_net.c
    src/tedge_time.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_FIRMWARE_UPDATE src/tedge_port_fw.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_SHELL_COMMAND src/tedge_shell_cmd.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_LOG_UPLOAD src/tedge_log_backend.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_COREDUMP src/tedge_coredump.c)
  zephyr_library_sources_ifdef(CONFIG_SHELL src/tedge_shell.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_AUTH_C8Y_CA src/tedge_port_crypto.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_PROVISIONING_SERIAL
    src/tedge_provisioning_console.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_PROVISIONING
    src/tedge_provisioning_zephyr.c)
  if(CONFIG_TEDGE_PROVISIONING AND CONFIG_WIFI)
    zephyr_library_sources(src/tedge_provisioning_wifi.c)
  endif()
  zephyr_library_compile_definitions(TEDGE_VERSION_STRING="${TEDGE_VERSION}")
  # gmtime_r() is POSIX, not C17. picolibc declares it anyway; a host libc
  # (native_sim) only does when asked, and an undeclared gmtime_r() returns
  # a truncated pointer on a 64-bit host.
  zephyr_library_compile_definitions(_POSIX_C_SOURCE=200809L)

  # Public API (include/tedge/tedge.h) and the Zephyr-only public headers
  # (include/tedge/mbedtls_user_config.h, which mbedTLS itself includes).
  zephyr_include_directories(${TEDGE_ROOT}/include include)
  zephyr_library_include_directories(${TEDGE_ROOT}/core/src)

  # The core reads its configuration as TEDGE_* from <tedge_config.h>,
  # generated from this port's Kconfig (cmake/tedge_config.cmake).
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/tedge_config.cmake)
  tedge_generate_config(${CMAKE_CURRENT_BINARY_DIR}/generated)
  zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

  # Trust anchors: every PEM in CONFIG_TEDGE_C8Y_CA_FILES is concatenated
  # into one bundle and embedded as tedge_ca_bundle.inc.
  if(CONFIG_TEDGE_TRANSPORT_C8Y)
    separate_arguments(_ca_files UNIX_COMMAND "${CONFIG_TEDGE_C8Y_CA_FILES}")
    set(_bundle "")
    foreach(_pem ${_ca_files})
      if(NOT IS_ABSOLUTE ${_pem})
        set(_pem ${TEDGE_ROOT}/${_pem})
      endif()
      if(NOT EXISTS ${_pem})
        message(FATAL_ERROR "CONFIG_TEDGE_C8Y_CA_FILES: no such file: ${_pem}")
      endif()
      file(READ ${_pem} _content)
      string(APPEND _bundle "${_content}")
    endforeach()
    set(_bundle_pem ${ZEPHYR_BINARY_DIR}/misc/generated/tedge_ca_bundle.pem)
    file(WRITE ${_bundle_pem} "${_bundle}")
    generate_inc_file_for_target(tedge ${_bundle_pem}
      ${ZEPHYR_BINARY_DIR}/include/generated/tedge_ca_bundle.inc)
  endif()
endif()
