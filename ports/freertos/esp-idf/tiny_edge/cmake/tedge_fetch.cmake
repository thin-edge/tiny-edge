# SPDX-License-Identifier: Apache-2.0
#
# The rest of tiny-edge for a component the component manager installed:
# it fetches only ports/freertos/esp-idf/tiny_edge, and the component builds
# the core, the provisioning library and the shared port code from beside it.
#
#   tedge_fetch(<out_var>)
#
# Clones the repository at the component's version (the commit a git
# dependency resolved to, or vX.Y.Z for a release version) into the build
# directory, once, and sets <out_var> to it. The URL is the git source in the
# project's dependencies.lock, so a fork is fetched from the fork;
# TINY_EDGE_GIT_URL overrides it.

function(tedge_fetch out_var)
  idf_component_get_property(_version ${COMPONENT_NAME} COMPONENT_VERSION)
  if(NOT _version)
    message(FATAL_ERROR "tiny_edge: the component has no version to fetch "
      "tiny-edge at. Set TINY_EDGE_DIR to a tiny-edge checkout.")
  endif()
  if(_version MATCHES "^[0-9a-f]+$" AND NOT _version MATCHES "^[0-9]+$")
    set(_ref ${_version})
  elseif(_version MATCHES "^v")
    set(_ref ${_version})
  else()
    set(_ref v${_version})
  endif()

  set(_url ${TINY_EDGE_GIT_URL})
  if(NOT _url)
    idf_build_get_property(_project_dir PROJECT_DIR)
    set(_lock ${_project_dir}/dependencies.lock)
    if(EXISTS ${_lock})
      file(READ ${_lock} _text)
      string(FIND "${_text}" "\n  ${COMPONENT_NAME}:" _at)
      if(_at GREATER_EQUAL 0)
        string(SUBSTRING "${_text}" ${_at} 1000 _entry)
        if(_entry MATCHES "\n +git: *([^\n]+)")
          string(STRIP "${CMAKE_MATCH_1}" _url)
        endif()
      endif()
    endif()
  endif()
  if(NOT _url)
    set(_url https://github.com/thin-edge/tiny-edge.git)
  endif()

  string(SUBSTRING "${_ref}" 0 12 _short)
  set(_dir ${CMAKE_BINARY_DIR}/tiny_edge-src-${_short})
  if(NOT EXISTS ${_dir}/core/sources.cmake)
    find_package(Git REQUIRED)
    message(STATUS "tiny_edge: fetching ${_url} at ${_ref}")
    file(REMOVE_RECURSE ${_dir})
    file(MAKE_DIRECTORY ${_dir})
    foreach(_cmd
        "init;-q"
        "fetch;-q;--depth;1;${_url};${_ref}"
        "-c;advice.detachedHead=false;checkout;-q;FETCH_HEAD")
      execute_process(COMMAND ${GIT_EXECUTABLE} ${_cmd}
        WORKING_DIRECTORY ${_dir} RESULT_VARIABLE _rc ERROR_VARIABLE _err)
      if(NOT _rc EQUAL 0)
        file(REMOVE_RECURSE ${_dir})
        message(FATAL_ERROR "tiny_edge: could not fetch ${_url} at ${_ref}: "
          "${_err}")
      endif()
    endforeach()
  endif()
  set(${out_var} ${_dir} PARENT_SCOPE)
endfunction()
