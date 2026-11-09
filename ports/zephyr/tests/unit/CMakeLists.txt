# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the module's pure helpers, run on native_sim:
#   west build -b native_sim/native/64 ports/zephyr/tests/unit -t run
#
# The helpers have no state, no I/O and no Kconfig of their own, so the test
# compiles them directly instead of building the whole client.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tedge_unit_tests C)

target_sources(app PRIVATE
  src/main.c
  ../../../../core/src/tedge_smartrest.c
  ../../../../core/src/tedge_backoff.c
  ../../../../core/src/tedge_pkcs7.c
  ../../../../core/src/tedge_ra_allow_list.c
  ../../../../core/src/tedge_ra_target.c
  ../../../../core/src/tedge_url.c
  ../../../../core/src/tedge_cert_decision.c
  ../../../../core/src/tedge_telemetry.c
  ../../../../core/src/tedge_json.c
  ../../../../core/src/tedge_shell_allow_list.c
  ../../../../core/src/tedge_log_ring.c
  ../../src/tedge_port.c
  ../../src/tedge_port_os.c
  ../../../../core/src/tedge_op_json.c
  ../../../../core/src/tedge_parameters.c
  ../../../../core/src/tedge_otp.c
  ../../../../core/src/tedge_fw_decision.c
  ../../../../core/src/tedge_http.c
  ../../../../core/src/tedge_ws.c
  ../../../../core/src/tedge_mqtt.c
)

# The telemetry API is compiled in for the tests, with a small buffer so the
# "it filled up" cases are quick to reach.
target_compile_definitions(app PRIVATE
  # As the module itself: gmtime_r() is POSIX (see ports/zephyr/CMakeLists.txt).
  _POSIX_C_SOURCE=200809L
  CONFIG_TEDGE_TELEMETRY=1
  # The port's kernel services need the client's thread priority and heap.
  CONFIG_TEDGE_THREAD_PRIORITY=7
  CONFIG_TEDGE_HEAP_SIZE=1024
  CONFIG_TEDGE_TELEMETRY_BUFFER_BYTES=512
  # The module's log level comes from its Kconfig, which is not part of a
  # test build.
  CONFIG_TEDGE_LOG_LEVEL=3
  # A small ring, so "it wrapped" is one test away.
  CONFIG_TEDGE_LOG_RING_BYTES=256
  CONFIG_TEDGE_C8Y_OPERATION_JSON=1
  # Parameters: room for the mixed set the tests declare, and the
  # schema printer, which is what the tenant registration needs.
  CONFIG_TEDGE_PARAMETERS=1
  CONFIG_TEDGE_PARAMETERS_MAX=8
  CONFIG_TEDGE_PARAMETERS_SCHEMA=1
)
target_include_directories(app PRIVATE ../../../../include ../../include ../../../../core/src)

# The core reads the CONFIG_TEDGE_* values above as TEDGE_*, through the
# header the module generates (see ports/zephyr/cmake/tedge_config.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/tedge_config.cmake)
tedge_generate_config(${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
