# SPDX-License-Identifier: Apache-2.0
#
# The FreeRTOS port of tiny-edge on ESP-IDF, as an ESP-IDF component: the
# thin-edge.io client for ESP32 applications. Add it to a project with the
# component manager (idf_component.yml):
#
#   dependencies:
#     tiny_edge:
#       git: https://github.com/thin-edge/tiny-edge.git
#       path: ports/freertos/esp-idf/tiny_edge
#
# and REQUIRES tiny_edge in the application's component. The options are in
# Kconfig (idf.py menuconfig, "tiny-edge (thin-edge.io client)").
#
# The sources come from four places: the portable core (core/), the
# FreeRTOS kernel layer every FreeRTOS platform shares (ports/freertos/),
# the BSD-sockets and mbedTLS code shared with the POSIX port
# (ports/common/), and ESP-IDF's own (src/ here).

get_filename_component(TEDGE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
set(TEDGE_FREERTOS_DIR ${TEDGE_ROOT}/ports/freertos)
set(TEDGE_COMMON_DIR ${TEDGE_ROOT}/ports/common)

include(${TEDGE_ROOT}/core/sources.cmake)
tedge_core_sources(_core_sources CONFIG_)

set(_srcs
  ${_core_sources}
  ${TEDGE_FREERTOS_DIR}/src/tedge_port_os.c
  ${TEDGE_COMMON_DIR}/src/tedge_sockets.c
  src/tedge_port_device.c
  src/tedge_port_log.c
  src/tedge_port_kv.c
  src/tedge_port_net.c
  src/tedge_port_time.c
)
if(CONFIG_TEDGE_AUTH_C8Y_CA)
  list(APPEND _srcs
    ${TEDGE_COMMON_DIR}/src/tedge_mbedtls_key.c
    src/tedge_port_crypto.c
  )
endif()
if(CONFIG_TEDGE_FIRMWARE_UPDATE)
  list(APPEND _srcs src/tedge_port_fw.c)
endif()

idf_component_register(
  SRCS ${_srcs}
  # The public API (tedge/tedge.h) and the port's headers the core includes
  # (<tedge_port/os.h>, <tedge_port/log.h>).
  INCLUDE_DIRS ${TEDGE_ROOT}/include ${TEDGE_FREERTOS_DIR}/include include
  PRIV_INCLUDE_DIRS ${TEDGE_ROOT}/core/src ${TEDGE_COMMON_DIR}/include
  REQUIRES freertos log mbedtls
  PRIV_REQUIRES esp_netif esp_event esp_wifi esp_app_format app_update
    esp_hw_support esp_system nvs_flash lwip
  # The log capture registers itself in a constructor, which nothing
  # references by name.
  WHOLE_ARCHIVE
)

file(STRINGS ${TEDGE_ROOT}/VERSION TEDGE_VERSION LIMIT_COUNT 1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  TEDGE_VERSION_STRING="${TEDGE_VERSION}"
  # lwIP's resolver reports a timeout and an unknown name alike.
  TEDGE_COMMON_LWIP=1
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)

# <tedge_config.h> from this component's Kconfig, and the trust anchors.
set(_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/tedge_config.cmake)
tedge_generate_config(${CMAKE_CURRENT_LIST_DIR}/Kconfig ${_generated})
include(${TEDGE_COMMON_DIR}/cmake/tedge_ca_bundle.cmake)
tedge_ca_bundle(${_generated} "${CONFIG_TEDGE_C8Y_CA_FILES}")
target_include_directories(${COMPONENT_LIB} PUBLIC ${_generated})
