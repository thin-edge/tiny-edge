# ##############################################################################
# SPDX-License-Identifier: Apache-2.0
#
# tiny-edge as a nuttx-apps application, for NuttX's CMake build: the same
# program as the Makefile builds. The core, the POSIX port, the code ports
# share (ports/common), what NuttX needs of its own (src/), mbedTLS 4, and the
# sample agent as the program.
#
# nuttx-apps adds this directory from apps/external/tiny_edge (see
# scripts/build.sh --cmake), so objects are built in NuttX's build directory.
# ##############################################################################

if(CONFIG_TINY_EDGE)
  get_filename_component(TINY_EDGE_PORT ${CMAKE_CURRENT_LIST_DIR} REALPATH)
  get_filename_component(TINY_EDGE_ROOT ${TINY_EDGE_PORT}/../.. ABSOLUTE)
  set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)

  # --- Generated: tedge_config.h and the CA bundle ---------------------------

  # The same generator as the Make build, from the configuration this build
  # uses. Its sources.mk is for Make; the source lists come from
  # core/sources.cmake below.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DCONFIG=${CMAKE_BINARY_DIR}/.config
            -DOUT=${GENERATED} -P ${TINY_EDGE_PORT}/tools/generate.cmake
    RESULT_VARIABLE _generated)
  if(NOT _generated EQUAL 0)
    message(FATAL_ERROR "tiny-edge: generate.cmake failed")
  endif()
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${TINY_EDGE_PORT}/tools/generate.cmake
             ${TINY_EDGE_PORT}/Kconfig)

  # --- mbedTLS 4.1 -----------------------------------------------------------

  set(MBEDTLS_VERSION 4.1.0)
  set(MBEDTLS_SHA256
      377a09cf8eb81b5fb2707045e5522d5489d3309fed5006c9874e60558fc81d10)
  string(REGEX REPLACE "^\"(.*)\"$" "\\1" MBEDTLS_DIR
                       "${CONFIG_TINY_EDGE_MBEDTLS_DIR}")
  if(NOT MBEDTLS_DIR)
    set(MBEDTLS_DIR ${CMAKE_CURRENT_BINARY_DIR}/mbedtls-${MBEDTLS_VERSION})
  endif()
  if(NOT EXISTS ${MBEDTLS_DIR}/library)
    set(_tarball ${CMAKE_CURRENT_BINARY_DIR}/mbedtls-${MBEDTLS_VERSION}.tar.bz2)
    file(
      DOWNLOAD
      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2
      ${_tarball}
      EXPECTED_HASH SHA256=${MBEDTLS_SHA256}
      TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT ${_tarball} DESTINATION
         ${CMAKE_CURRENT_BINARY_DIR})
    file(REMOVE ${_tarball})
  endif()

  set(TF_PSA ${MBEDTLS_DIR}/tf-psa-crypto)
  set(MBEDTLS_SRCDIRS
      ${MBEDTLS_DIR}/library ${TF_PSA}/core ${TF_PSA}/drivers/builtin/src
      ${TF_PSA}/platform ${TF_PSA}/utilities ${TF_PSA}/extras)
  set(MBEDTLS_SRCS "")
  foreach(_dir ${MBEDTLS_SRCDIRS})
    file(GLOB _srcs ${_dir}/*.c)
    list(APPEND MBEDTLS_SRCS ${_srcs})
  endforeach()

  # --- Sources -----------------------------------------------------------------

  # The core's and the provisioning library's files for the options that are
  # on: the list every port builds from, read with NuttX's CONFIG_ names.
  include(${TINY_EDGE_ROOT}/core/sources.cmake)
  include(${TINY_EDGE_ROOT}/provisioning/sources.cmake)
  tedge_core_sources(CORE_SRCS CONFIG_)
  tedge_provisioning_sources(PROVISIONING_SRCS CONFIG_)

  # The POSIX port as it is: every file, apart from those this port replaces.
  file(GLOB NUTTX_SRCS ${TINY_EDGE_PORT}/src/*.c)
  file(GLOB POSIX_SRCS ${TINY_EDGE_ROOT}/ports/posix/src/*.c)
  file(GLOB COMMON_SRCS ${TINY_EDGE_ROOT}/ports/common/src/*.c)
  foreach(_src ${NUTTX_SRCS})
    get_filename_component(_name ${_src} NAME)
    list(REMOVE_ITEM POSIX_SRCS ${TINY_EDGE_ROOT}/ports/posix/src/${_name})
  endforeach()

  file(STRINGS ${TINY_EDGE_ROOT}/VERSION TEDGE_VERSION LIMIT_COUNT 1)

  # --- The program -------------------------------------------------------------

  nuttx_add_application(
    NAME
    ${CONFIG_TINY_EDGE_PROGNAME}
    PRIORITY
    ${CONFIG_TINY_EDGE_PRIORITY}
    STACKSIZE
    ${CONFIG_TINY_EDGE_STACKSIZE}
    MODULE
    ${CONFIG_TINY_EDGE}
    SRCS
    ${TINY_EDGE_PORT}/samples/agent/agent_main.c
    ${CORE_SRCS}
    ${PROVISIONING_SRCS}
    ${POSIX_SRCS}
    ${COMMON_SRCS}
    ${NUTTX_SRCS}
    ${MBEDTLS_SRCS}
    INCLUDE_DIRECTORIES
    ${GENERATED}
    ${TINY_EDGE_ROOT}/include
    ${TINY_EDGE_PORT}/include
    ${TINY_EDGE_ROOT}/ports/posix/include
    ${TINY_EDGE_ROOT}/ports/posix/src
    ${TINY_EDGE_ROOT}/ports/common/include
    ${TINY_EDGE_ROOT}/core/src
    ${TINY_EDGE_ROOT}/provisioning/src
    ${MBEDTLS_DIR}/include
    ${TF_PSA}/include
    ${TF_PSA}/drivers/builtin/include
    ${TF_PSA}/dispatch
    ${MBEDTLS_SRCDIRS}
    DEFINITIONS
    TEDGE_VERSION_STRING="${TEDGE_VERSION}"
    # The POSIX port's options for a POSIX RTOS, as in the Makefile: the
    # core's stack sizes, the MAC from SIOCGIFHWADDR, and no exec().
    TEDGE_POSIX_THREAD_STACKS
    TEDGE_POSIX_MAC_IOCTL
    TEDGE_POSIX_NO_EXEC
    # mbedTLS's Unix code paths; threading and the memory settings come
    # from the user configs.
    unix
    TF_PSA_CRYPTO_USER_CONFIG_FILE="tedge_mbedtls_user_config.h"
    MBEDTLS_USER_CONFIG_FILE="tedge_mbedtls_tls_config.h"
    COMPILE_FLAGS
    -std=gnu11)
endif()
