# SPDX-License-Identifier: Apache-2.0
#
# The trust anchors the core embeds, as comma-separated bytes of every PEM
# file in a space-separated list, concatenated:
#
#   include(<tiny-edge>/ports/common/cmake/tedge_ca_bundle.cmake)
#   tedge_ca_bundle(<out_dir> "<files>")
#       -> <out_dir>/tedge_ca_bundle.inc    (TEDGE_C8Y_CA_FILES: the tenant,
#                                            core/src/tedge_c8y.c)
#   tedge_download_ca_bundle(<out_dir> "<files>")
#       -> <out_dir>/tedge_download_ca.inc  (TEDGE_DOWNLOAD_CA_FILES: hosts
#                                            outside the tenant only)
#
# A relative path is relative to the repository. An empty list writes an
# empty file. A change to a file reconfigures the build. Also usable from a
# script (cmake -P), as the NuttX port's generator does.

function(tedge_pem_bundle out_file files option)
  get_filename_component(_root ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.. ABSOLUTE)
  separate_arguments(_files UNIX_COMMAND "${files}")
  set(_bundle "")
  foreach(_pem ${_files})
    if(NOT IS_ABSOLUTE ${_pem})
      set(_pem ${_root}/${_pem})
    endif()
    if(NOT EXISTS ${_pem})
      message(FATAL_ERROR "${option}: no such file: ${_pem}")
    endif()
    file(READ ${_pem} _content HEX)
    string(APPEND _bundle "${_content}")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pem})
    endif()
  endforeach()
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bundle "${_bundle}")
  file(CONFIGURE OUTPUT ${out_file} CONTENT "${_bundle}\n")
endfunction()

function(tedge_ca_bundle out_dir files)
  tedge_pem_bundle(${out_dir}/tedge_ca_bundle.inc "${files}" TEDGE_C8Y_CA_FILES)
endfunction()

function(tedge_download_ca_bundle out_dir files)
  tedge_pem_bundle(${out_dir}/tedge_download_ca.inc "${files}"
    TEDGE_DOWNLOAD_CA_FILES)
endfunction()
