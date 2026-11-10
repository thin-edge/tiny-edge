# SPDX-License-Identifier: Apache-2.0

add_executable(tedge-port-test test_port.c)
set_target_properties(tedge-port-test PROPERTIES C_STANDARD 11)
target_compile_definitions(tedge-port-test PRIVATE _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_link_libraries(tedge-port-test PRIVATE tedge::tedge)
add_test(NAME port COMMAND tedge-port-test)

add_executable(tedge-lifecycle-test test_lifecycle.c)
set_target_properties(tedge-lifecycle-test PROPERTIES C_STANDARD 11)
target_compile_definitions(tedge-lifecycle-test PRIVATE _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_link_libraries(tedge-lifecycle-test PRIVATE tedge::tedge)
add_test(NAME lifecycle COMMAND tedge-lifecycle-test)
