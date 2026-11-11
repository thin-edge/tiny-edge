# SPDX-License-Identifier: Apache-2.0
#
# The trust anchors the core embeds (core/src/tedge_c8y.c includes
# tedge_ca_bundle.inc): every PEM file in a space-separated list,
# concatenated, as comma-separated bytes.
#
#   include(<tiny-edge>/ports/common/cmake/tedge_ca_bundle.cmake)
#   tedge_ca_bundle(<out_dir> "<files>")
#
# A relative path is relative to the repository. A change to a file
# reconfigures the build.

function(tedge_ca_bundle out_dir files)
  get_filename_component(_root ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.. ABSOLUTE)
  separate_arguments(_files UNIX_COMMAND "${files}")
  set(_bundle "")
  foreach(_pem ${_files})
    if(NOT IS_ABSOLUTE ${_pem})
      set(_pem ${_root}/${_pem})
    endif()
    if(NOT EXISTS ${_pem})
      message(FATAL_ERROR "TEDGE_C8Y_CA_FILES: no such file: ${_pem}")
    endif()
    file(READ ${_pem} _content HEX)
    string(APPEND _bundle "${_content}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pem})
  endforeach()
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bundle "${_bundle}")
  file(CONFIGURE OUTPUT ${out_dir}/tedge_ca_bundle.inc CONTENT "${_bundle}\n")
endfunction()
