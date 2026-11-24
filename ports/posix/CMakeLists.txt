# SPDX-License-Identifier: Apache-2.0
#
# POSIX port of tiny-edge: the thin-edge.io client as a library for a Linux
# or macOS process (and later QNX and VxWorks, which are POSIX too).
#
# Build it on its own, with the sample:
#
#   cmake -S ports/posix -B build -DTEDGE_C8Y_URL=example.cumulocity.com
#   cmake --build build
#
# or from an application's CMake project:
#
#   add_subdirectory(<tiny-edge>/ports/posix tiny-edge)
#   target_link_libraries(my_app PRIVATE tedge::tedge)
#
# The client's options are CMake variables (cmake/options.cmake).

cmake_minimum_required(VERSION 3.20)
project(tiny_edge_posix LANGUAGES C)

get_filename_component(TEDGE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
file(STRINGS ${TEDGE_ROOT}/VERSION TEDGE_VERSION LIMIT_COUNT 1)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(_top ON)
else()
  set(_top OFF)
endif()
option(TEDGE_POSIX_SAMPLES "Build the samples" ${_top})
option(TEDGE_POSIX_TESTS "Build the port's tests (ctest)" ${_top})

include(cmake/options.cmake)
include(cmake/mbedtls.cmake)
include(${TEDGE_ROOT}/core/sources.cmake)
include(${TEDGE_ROOT}/provisioning/sources.cmake)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

tedge_core_sources(_core_sources "")
tedge_provisioning_sources(_provisioning_sources "")
add_library(tedge STATIC
  ${_core_sources}
  ${_provisioning_sources}
  src/tedge_posix.c
  src/tedge_port_os.c
  src/tedge_port_log.c
  src/tedge_port_device.c
  src/tedge_port_kv.c
  src/tedge_port_net.c
  src/tedge_port_crypto.c
  # Shared with other ports that have BSD sockets and mbedTLS.
  ${TEDGE_ROOT}/ports/common/src/tedge_sockets.c
  ${TEDGE_ROOT}/ports/common/src/tedge_mbedtls_key.c
)
add_library(tedge::tedge ALIAS tedge)
if(TEDGE_SHELL_COMMAND)
  target_sources(tedge PRIVATE src/tedge_shell_cmd.c)
endif()

set_target_properties(tedge PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
target_compile_definitions(tedge PRIVATE
  TEDGE_VERSION_STRING="${TEDGE_VERSION}"
  # POSIX.1-2008 for the core (gmtime_r) and the port; the BSD and GNU
  # additions the port uses (getifaddrs, getentropy, MSG_NOSIGNAL) are on
  # by default without it on macOS, and with _DEFAULT_SOURCE on glibc.
  _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE
  _DARWIN_C_SOURCE
)
# For POSIX systems that are RTOSes (NuttX), off for Linux and macOS:
# threads get the stack sizes the core asks for instead of the system's
# default, the MAC address comes from SIOCGIFHWADDR when getifaddrs()
# lists no link-layer entries, and a restart needs no exec().
option(TEDGE_POSIX_THREAD_STACKS "Give each thread the stack size the core asks for" OFF)
option(TEDGE_POSIX_MAC_IOCTL "Read the MAC address with SIOCGIFHWADDR" OFF)
option(TEDGE_POSIX_NO_EXEC "The system has no exec(): restart only through tedge_posix_config.restart" OFF)
if(TEDGE_POSIX_MAC_IOCTL)
  # Linux and NuttX have it; macOS and the BSDs do not, and list the MAC
  # through getifaddrs() instead, which is what the port does without it.
  include(CheckSymbolExists)
  check_symbol_exists(SIOCGIFHWADDR "sys/ioctl.h;net/if.h" TEDGE_HAVE_SIOCGIFHWADDR)
  if(NOT TEDGE_HAVE_SIOCGIFHWADDR)
    message(FATAL_ERROR "TEDGE_POSIX_MAC_IOCTL needs SIOCGIFHWADDR, which "
      "this system does not have; leave it off here")
  endif()
endif()
foreach(_opt TEDGE_POSIX_THREAD_STACKS TEDGE_POSIX_MAC_IOCTL TEDGE_POSIX_NO_EXEC)
  if(${_opt})
    target_compile_definitions(tedge PUBLIC ${_opt})
  endif()
endforeach()
option(TEDGE_WERROR "Treat warnings in the client's own code as errors" OFF)
target_compile_options(tedge PRIVATE -Wall -Wextra -Wno-unused-parameter
  -Wno-sign-compare $<$<BOOL:${TEDGE_WERROR}>:-Werror>)

# The public API (include/tedge/tedge.h) and this port's (tedge/posix.h).
target_include_directories(tedge PUBLIC
  ${TEDGE_ROOT}/include
  ${CMAKE_CURRENT_LIST_DIR}/include
)
target_include_directories(tedge PRIVATE
  ${TEDGE_ROOT}/core/src
  ${TEDGE_ROOT}/ports/common/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(tedge PUBLIC ${TEDGE_MBEDTLS_LIBRARIES} Threads::Threads)

tedge_generate_config(${CMAKE_CURRENT_BINARY_DIR}/generated)

# Trust anchors: every PEM in TEDGE_C8Y_CA_FILES, embedded as
# tedge_ca_bundle.inc, and the download-only roots in
# TEDGE_DOWNLOAD_CA_FILES as tedge_download_ca.inc, as the other ports do.
include(${TEDGE_ROOT}/ports/common/cmake/tedge_ca_bundle.cmake)
tedge_ca_bundle(${CMAKE_CURRENT_BINARY_DIR}/generated "${TEDGE_C8Y_CA_FILES}")
tedge_download_ca_bundle(${CMAKE_CURRENT_BINARY_DIR}/generated
  "${TEDGE_DOWNLOAD_CA_FILES}")

if(TEDGE_POSIX_SAMPLES)
  add_subdirectory(samples/minimal)
endif()
if(TEDGE_POSIX_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
