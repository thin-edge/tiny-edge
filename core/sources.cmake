# SPDX-License-Identifier: Apache-2.0
#
# The core's source files, by feature: the one list every port builds from.
#
#   include(<tiny-edge>/core/sources.cmake)
#   tedge_core_sources(srcs "")        # options are TEDGE_* variables
#   tedge_core_sources(srcs <prefix>)  # options are <prefix>TEDGE_*
#
# Each feature is read from the CMake variable <prefix><option>, which is
# true when the feature is on. Zephyr passes the prefix Kconfig gives its
# symbols in CMake. A port adds its own sources beside these.

function(tedge_core_sources out prefix)
  set(src
    tedge_version.c
    tedge_core.c
    tedge_absent.c
    tedge_device.c
    tedge_backoff.c
    tedge_json.c
  )
  macro(_on option)
    if(${prefix}${option})
      list(APPEND src ${ARGN})
    endif()
  endmacro()

  _on(TEDGE_TELEMETRY tedge_telemetry.c)
  _on(TEDGE_HEALTH tedge_health.c)
  _on(TEDGE_TRANSPORT_C8Y
    tedge_c8y.c
    tedge_codec.c
    tedge_mqtt.c
    tedge_smartrest.c
  )
  _on(TEDGE_C8Y_OPERATION_JSON tedge_op_json.c)
  # The HTTP client belongs to every file-transfer feature, not to the first
  # one that needed it.
  _on(TEDGE_HTTP_CLIENT tedge_http.c)
  _on(TEDGE_HTTP
    tedge_http_download.c
    tedge_http_upload.c
    tedge_url.c
  )
  _on(TEDGE_FIRMWARE_UPDATE tedge_firmware.c tedge_fw_decision.c)
  _on(TEDGE_SHELL_COMMAND tedge_shell_allow_list.c)
  _on(TEDGE_LOG_UPLOAD tedge_log_upload.c tedge_log_ring.c)
  _on(TEDGE_PARAMETERS tedge_parameters.c)
  _on(TEDGE_PARAMETERS_SELF tedge_self_params.c)
  _on(TEDGE_CERT_RENEWAL tedge_cert_renew.c tedge_cert_decision.c)
  _on(TEDGE_REMOTE_ACCESS
    tedge_remote_access.c
    tedge_ws.c
    tedge_ra_allow_list.c
    tedge_ra_target.c
  )
  _on(TEDGE_AUTH_C8Y_CA tedge_enroll.c tedge_otp.c tedge_pkcs7.c)
  _on(TEDGE_AUTH_BOOTSTRAP tedge_bootstrap.c)

  list(TRANSFORM src PREPEND ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/)
  set(${out} ${src} PARENT_SCOPE)
endfunction()
