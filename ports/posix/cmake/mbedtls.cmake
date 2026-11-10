# SPDX-License-Identifier: Apache-2.0
#
# mbedTLS for the POSIX port: TLS, and the X.509 and ASN.1 parsers the core
# uses. The same major version as the Zephyr port's, so both ports use one
# API.
#
# An installed mbedTLS 4.1 or later (its CMake package) is used when there
# is one; it must be built with MBEDTLS_THREADING_C and
# MBEDTLS_THREADING_PTHREAD, which the port checks when it compiles.
# Otherwise the release is fetched, its checksum verified, and built with
# threading on. TEDGE_MBEDTLS_FETCH=ON skips the search.
#
# Defines TEDGE_MBEDTLS_LIBRARIES.

option(TEDGE_MBEDTLS_FETCH "Build mbedTLS from its release instead of using an installed one" OFF)

if(NOT TEDGE_MBEDTLS_FETCH)
  find_package(MbedTLS 4.1 CONFIG QUIET)
endif()

if(MbedTLS_FOUND)
  message(STATUS "tedge: mbedTLS ${MbedTLS_VERSION} (installed)")
  set(TEDGE_MBEDTLS_LIBRARIES MbedTLS::mbedtls MbedTLS::mbedx509)
else()
  include(FetchContent)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(TF_PSA_CRYPTO_USER_CONFIG_FILE
    ${CMAKE_CURRENT_LIST_DIR}/mbedtls_threading.h CACHE FILEPATH "" FORCE)
  set(LINK_WITH_PTHREAD ON CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.0/mbedtls-4.1.0.tar.bz2
    URL_HASH SHA256=377a09cf8eb81b5fb2707045e5522d5489d3309fed5006c9874e60558fc81d10
    DOWNLOAD_EXTRACT_TIMESTAMP ON
  )
  FetchContent_MakeAvailable(mbedtls)
  message(STATUS "tedge: mbedTLS 4.1.0 (built here)")
  set(TEDGE_MBEDTLS_LIBRARIES MbedTLS::mbedtls MbedTLS::mbedx509)
endif()
