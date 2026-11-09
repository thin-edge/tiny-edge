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
  set(TEDGE_CORE_SRC ${TEDGE_ROOT}/core/src)
  file(STRINGS ${TEDGE_ROOT}/VERSION TEDGE_VERSION LIMIT_COUNT 1)

  zephyr_library_named(tedge)
  zephyr_library_sources(
    ${TEDGE_CORE_SRC}/tedge_version.c
    ${TEDGE_CORE_SRC}/tedge_core.c
    ${TEDGE_CORE_SRC}/tedge_device.c
    src/tedge_platform.c
    src/tedge_port.c
    src/tedge_port_os.c
    src/tedge_port_kv.c
    src/tedge_port_net.c
    src/tedge_time.c
    ${TEDGE_CORE_SRC}/tedge_backoff.c
    ${TEDGE_CORE_SRC}/tedge_json.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_TELEMETRY ${TEDGE_CORE_SRC}/tedge_telemetry.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_HEALTH ${TEDGE_CORE_SRC}/tedge_health.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_TRANSPORT_C8Y
    ${TEDGE_CORE_SRC}/tedge_c8y.c
    ${TEDGE_CORE_SRC}/tedge_codec.c
    ${TEDGE_CORE_SRC}/tedge_mqtt.c
    ${TEDGE_CORE_SRC}/tedge_smartrest.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_C8Y_OPERATION_JSON
    ${TEDGE_CORE_SRC}/tedge_op_json.c
  )
  # The HTTP client belongs to every file-transfer feature, not to the first
  # one that needed it.
  zephyr_library_sources_ifdef(CONFIG_TEDGE_HTTP_CLIENT ${TEDGE_CORE_SRC}/tedge_http.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_HTTP
    ${TEDGE_CORE_SRC}/tedge_http_download.c
    ${TEDGE_CORE_SRC}/tedge_http_upload.c
    ${TEDGE_CORE_SRC}/tedge_url.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_FIRMWARE_UPDATE
    ${TEDGE_CORE_SRC}/tedge_firmware.c
    ${TEDGE_CORE_SRC}/tedge_fw_decision.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_SHELL_COMMAND
    src/tedge_shell_cmd.c
    ${TEDGE_CORE_SRC}/tedge_shell_allow_list.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_LOG_UPLOAD
    ${TEDGE_CORE_SRC}/tedge_log_upload.c
    ${TEDGE_CORE_SRC}/tedge_log_ring.c
    src/tedge_log_backend.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_PARAMETERS ${TEDGE_CORE_SRC}/tedge_parameters.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_PARAMETERS_SELF
    ${TEDGE_CORE_SRC}/tedge_self_params.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_COREDUMP src/tedge_coredump.c)
  zephyr_library_sources_ifdef(CONFIG_SHELL src/tedge_shell.c)
  zephyr_library_sources_ifdef(CONFIG_TEDGE_CERT_RENEWAL
    ${TEDGE_CORE_SRC}/tedge_cert_renew.c
    ${TEDGE_CORE_SRC}/tedge_cert_decision.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_REMOTE_ACCESS
    ${TEDGE_CORE_SRC}/tedge_remote_access.c
    ${TEDGE_CORE_SRC}/tedge_ws.c
    ${TEDGE_CORE_SRC}/tedge_ra_allow_list.c
    ${TEDGE_CORE_SRC}/tedge_ra_target.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_AUTH_C8Y_CA
    ${TEDGE_CORE_SRC}/tedge_enroll.c
    src/tedge_port_crypto.c
    ${TEDGE_CORE_SRC}/tedge_otp.c
    ${TEDGE_CORE_SRC}/tedge_pkcs7.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_AUTH_BOOTSTRAP ${TEDGE_CORE_SRC}/tedge_bootstrap.c)
  zephyr_library_compile_definitions(TEDGE_VERSION_STRING="${TEDGE_VERSION}")
  # gmtime_r() is POSIX, not C17. picolibc declares it anyway; a host libc
  # (native_sim) only does when asked, and an undeclared gmtime_r() returns
  # a truncated pointer on a 64-bit host.
  zephyr_library_compile_definitions(_POSIX_C_SOURCE=200809L)

  # Public API (include/tedge/tedge.h) and the Zephyr-only public headers
  # (include/tedge/mbedtls_user_config.h, which mbedTLS itself includes).
  zephyr_include_directories(${TEDGE_ROOT}/include include)
  zephyr_library_include_directories(${TEDGE_CORE_SRC})

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
