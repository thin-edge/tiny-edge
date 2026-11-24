# SPDX-License-Identifier: Apache-2.0

add_executable(tedge-port-test test_port.c)
set_target_properties(tedge-port-test PROPERTIES C_STANDARD 11)
target_compile_definitions(tedge-port-test PRIVATE _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_link_libraries(tedge-port-test PRIVATE tedge::tedge)
add_test(NAME port COMMAND tedge-port-test)

# Stopping the client while it waits to enroll: certificate builds only
# (bootstrap builds have no enrollment, and no one-time password to set).
if(TEDGE_AUTH_C8Y_CA)
  add_executable(tedge-lifecycle-test test_lifecycle.c)
  set_target_properties(tedge-lifecycle-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-lifecycle-test PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_link_libraries(tedge-lifecycle-test PRIVATE tedge::tedge)
  add_test(NAME lifecycle COMMAND tedge-lifecycle-test)
endif()

if(TEDGE_PROVISIONING)
  add_executable(tedge-provisioning-test test_provisioning.c)
  set_target_properties(tedge-provisioning-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-provisioning-test PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_link_libraries(tedge-provisioning-test PRIVATE tedge::tedge)
  add_test(NAME provisioning COMMAND tedge-provisioning-test)
endif()

# The shell command needs the core's internal header, as the port does.
if(TEDGE_SHELL_COMMAND)
  add_executable(tedge-shell-test test_shell.c)
  set_target_properties(tedge-shell-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-shell-test PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_include_directories(tedge-shell-test PRIVATE
    $<TARGET_PROPERTY:tedge,INCLUDE_DIRECTORIES>)
  target_link_libraries(tedge-shell-test PRIVATE tedge::tedge)
  add_test(NAME shell COMMAND tedge-shell-test)
endif()

# One HTTP transfer at a time: a log job reports itself busy while it runs.
if(TEDGE_LOG_UPLOAD)
  add_executable(tedge-log-busy-test test_log_busy.c)
  set_target_properties(tedge-log-busy-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-log-busy-test PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_include_directories(tedge-log-busy-test PRIVATE
    $<TARGET_PROPERTY:tedge,INCLUDE_DIRECTORIES>)
  target_link_libraries(tedge-log-busy-test PRIVATE tedge::tedge)
  add_test(NAME log_busy COMMAND tedge-log-busy-test)
endif()

# Which roots each kind of TLS connection trusts, against a loopback server.
add_executable(tedge-tls-test test_tls.c)
set_target_properties(tedge-tls-test PROPERTIES C_STANDARD 11)
target_compile_definitions(tedge-tls-test PRIVATE _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_include_directories(tedge-tls-test PRIVATE
  ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(tedge-tls-test PRIVATE tedge::tedge)
add_test(NAME tls COMMAND tedge-tls-test)

# The stored roots: what a set accepts and refuses, and that they reach the
# port. Needs the core's internal header, as the shell test does.
if(TEDGE_TRUSTED_ROOTS)
  add_executable(tedge-trust-test test_trust.c)
  set_target_properties(tedge-trust-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-trust-test PRIVATE _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_include_directories(tedge-trust-test PRIVATE
    $<TARGET_PROPERTY:tedge,INCLUDE_DIRECTORIES>)
  target_link_libraries(tedge-trust-test PRIVATE tedge::tedge)
  add_test(NAME trust COMMAND tedge-trust-test)
endif()

# Configuration management against a fake tenant on loopback.
if(TEDGE_CONFIG_MGMT AND TEDGE_TRUSTED_ROOTS)
  add_executable(tedge-config-test test_config.c)
  set_target_properties(tedge-config-test PROPERTIES C_STANDARD 11)
  target_compile_definitions(tedge-config-test PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_include_directories(tedge-config-test PRIVATE
    $<TARGET_PROPERTY:tedge,INCLUDE_DIRECTORIES>)
  target_link_libraries(tedge-config-test PRIVATE tedge::tedge)
  add_test(NAME config COMMAND tedge-config-test)
endif()
