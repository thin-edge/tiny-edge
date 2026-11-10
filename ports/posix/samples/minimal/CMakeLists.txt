# SPDX-License-Identifier: Apache-2.0

add_executable(tedge-minimal main.c)
set_target_properties(tedge-minimal PROPERTIES C_STANDARD 11)
target_compile_definitions(tedge-minimal PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(tedge-minimal PRIVATE tedge::tedge)
