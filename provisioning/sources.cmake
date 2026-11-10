# SPDX-License-Identifier: Apache-2.0
#
# The provisioning library's source files (include/tedge/provisioning.h),
# for every port, as core/sources.cmake lists the client's:
#
#   include(<tiny-edge>/provisioning/sources.cmake)
#   tedge_provisioning_sources(srcs "")        # options are TEDGE_* variables
#   tedge_provisioning_sources(srcs <prefix>)  # options are <prefix>TEDGE_*
#
# Empty unless TEDGE_PROVISIONING is on.

function(tedge_provisioning_sources out prefix)
  set(src "")
  if(${prefix}TEDGE_PROVISIONING)
    list(APPEND src tedge_provisioning.c)
    if(${prefix}TEDGE_PROVISIONING_SERIAL)
      list(APPEND src tedge_improv_serial.c)
    endif()
  endif()
  list(TRANSFORM src PREPEND ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/)
  set(${out} ${src} PARENT_SCOPE)
endfunction()
