# SPDX-License-Identifier: Apache-2.0
#
# The client's build options on POSIX, as CMake cache variables with the
# names the core reads (TEDGE_*). Set them like any CMake option:
#
#   cmake -DTEDGE_C8Y_URL=example.cumulocity.com -DTEDGE_REMOTE_ACCESS=ON ..
#
# or with set() before add_subdirectory(). The defaults are the Zephyr
# port's, so a build behaves the same on both. tedge_generate_config()
# writes them to <tedge_config.h>.
#
# Three Kconfig choices become string options here:
#
#   TEDGE_AUTH                     c8y-ca (default) | bootstrap
#   TEDGE_C8Y_MQTT                 service (default) | core
#   TEDGE_REMOTE_ACCESS_TARGETS    lan (default) | list | local

set_property(GLOBAL PROPERTY TEDGE_OPTIONS "")

# tedge_option(<name> BOOL|INT|STRING <default> <description>)
#
# Declares an option and records it for tedge_generate_config(). BOOL is a
# feature (defined to 1 when on, not at all when off), INT a number and
# STRING a C string.
function(tedge_option name type default doc)
  if(type STREQUAL "BOOL")
    option(${name} "${doc}" ${default})
  else()
    set(${name} "${default}" CACHE STRING "${doc}")
  endif()
  set_property(GLOBAL APPEND PROPERTY TEDGE_OPTIONS "${name}:${type}")
endfunction()

# An option the build derives from others: not in the cache, so a user
# cannot set it to something the features contradict.
macro(tedge_derived name type value)
  set(${name} ${value})
  set_property(GLOBAL APPEND PROPERTY TEDGE_OPTIONS "${name}:${type}")
endmacro()

# A string option that takes one of a fixed set of values.
function(tedge_choice name default doc)
  set(${name} "${default}" CACHE STRING "${doc}")
  set_property(CACHE ${name} PROPERTY STRINGS ${ARGN})
  if(NOT "${${name}}" IN_LIST ARGN)
    message(FATAL_ERROR "${name} is \"${${name}}\"; it must be one of: ${ARGN}")
  endif()
endfunction()

# --- General --------------------------------------------------------------

tedge_option(TEDGE_LOG_LEVEL INT 3 "Log level: 0 none, 1 error, 2 warning, 3 info, 4 debug")
tedge_option(TEDGE_HEAP_SIZE INT 16384 "Bytes the client may allocate (its own heap)")
tedge_option(TEDGE_DEVICE_ID_PREFIX STRING "tedge" "Default external ID prefix")
tedge_option(TEDGE_DEVICE_TYPE STRING "thin-edge.io-posix" "Default device type")
tedge_option(TEDGE_RECONNECT_BACKOFF_MAX_S INT 300 "Longest wait between reconnect attempts (s)")
# Thread sizes and the priority are the RTOS's concern: on POSIX each thread
# gets the system's default stack and scheduling. The core still names them.
tedge_derived(TEDGE_THREAD_STACK_SIZE INT 6144)
tedge_derived(TEDGE_THREAD_PRIORITY INT 0)

# --- Cumulocity -----------------------------------------------------------

tedge_derived(TEDGE_TRANSPORT_C8Y BOOL ON)
tedge_option(TEDGE_C8Y_URL STRING "" "Cumulocity tenant host (may also be set at run time)")
tedge_option(TEDGE_C8Y_CA_FILES STRING "certs/godaddy-root-g2.pem"
  "Trust anchors for the tenant (PEM files, space-separated, relative to the repository)")
tedge_option(TEDGE_C8Y_OPERATION_JSON BOOL ON "Receive operations with their id (devicecontrol/notifications)")
if(TEDGE_C8Y_OPERATION_JSON)
  set(_payload 2048)
else()
  set(_payload 1024)
endif()
tedge_option(TEDGE_C8Y_PAYLOAD_BYTES INT ${_payload} "Largest message the client accepts")
tedge_option(TEDGE_REQUIRED_INTERVAL_MIN INT 60 "Required interval reported to Cumulocity (minutes)")

tedge_choice(TEDGE_AUTH c8y-ca "How the device authenticates" c8y-ca bootstrap)
if(TEDGE_AUTH STREQUAL "c8y-ca")
  set(TEDGE_AUTH_C8Y_CA ON)
  set(TEDGE_AUTH_BOOTSTRAP OFF)
else()
  set(TEDGE_AUTH_C8Y_CA OFF)
  set(TEDGE_AUTH_BOOTSTRAP ON)
endif()
set_property(GLOBAL APPEND PROPERTY TEDGE_OPTIONS
  "TEDGE_AUTH_C8Y_CA:BOOL" "TEDGE_AUTH_BOOTSTRAP:BOOL")
tedge_option(TEDGE_ENROLL_POLL_S INT 10 "Seconds between enrollment attempts")
tedge_option(TEDGE_BOOTSTRAP_USER STRING "" "Bootstrap user (tenant/username)")
tedge_option(TEDGE_BOOTSTRAP_PASSWORD STRING "" "Bootstrap password")

tedge_choice(TEDGE_C8Y_MQTT service "Which Cumulocity MQTT endpoint" service core)
if(TEDGE_C8Y_MQTT STREQUAL "service")
  if(NOT TEDGE_AUTH_C8Y_CA)
    message(FATAL_ERROR "TEDGE_C8Y_MQTT=service needs TEDGE_AUTH=c8y-ca")
  endif()
  tedge_derived(TEDGE_C8Y_MQTT_SERVICE BOOL ON)
else()
  tedge_derived(TEDGE_C8Y_MQTT_SERVICE BOOL OFF)
endif()

# --- Features -------------------------------------------------------------

tedge_option(TEDGE_TELEMETRY BOOL ON "Telemetry (measurements, events, alarms)")
tedge_option(TEDGE_TELEMETRY_BUFFER_BYTES INT 2048 "Bytes held for telemetry that cannot be sent yet")
tedge_option(TEDGE_HEALTH BOOL ON "Device health measurements")
tedge_option(TEDGE_HEALTH_INTERVAL_S INT 900 "Seconds between health measurements")
tedge_option(TEDGE_RESTART BOOL ON "Restart operation (the process starts itself again)")

tedge_option(TEDGE_LOG_UPLOAD BOOL ON "Log upload")
tedge_option(TEDGE_LOG_RING_BYTES INT 2048 "The client's own log kept in RAM")
tedge_option(TEDGE_LOG_TYPE_SLOTS INT 4 "Log types the client can hold")
tedge_option(TEDGE_LOG_UPLOAD_MAX_BYTES INT 8192 "Most bytes one request may send")
tedge_option(TEDGE_LOG_UPLOAD_STACK_SIZE INT 8192 "Stack of the upload thread (unused on POSIX)")

tedge_option(TEDGE_PARAMETERS BOOL ON "Parameters")
tedge_option(TEDGE_PARAMETERS_MAX INT 16 "Maximum declared parameters, across all sets")
tedge_option(TEDGE_PARAMETERS_SELF BOOL ON "Offer the client's own tunables as a parameter set")

tedge_option(TEDGE_CERT_RENEWAL BOOL ON "Certificate renewal")
tedge_option(TEDGE_CERT_RENEW_BEFORE_DAYS INT 30 "Renew when this many days of lifetime are left")
tedge_option(TEDGE_CERT_RENEW_ALARM_DAYS INT 7 "Raise an alarm when this many days are left and renewal failed")
tedge_option(TEDGE_CERT_RENEW_CHECK_HOURS INT 24 "Hours between certificate checks")

tedge_option(TEDGE_REMOTE_ACCESS BOOL ON "Remote access (tunnel to the device and its network)")
tedge_choice(TEDGE_REMOTE_ACCESS_TARGETS lan "Which hosts a tunnel may reach" lan list local)
foreach(_policy LAN LIST LOCAL)
  string(TOLOWER ${_policy} _value)
  if(TEDGE_REMOTE_ACCESS_TARGETS STREQUAL _value)
    set(TEDGE_REMOTE_ACCESS_TARGETS_${_policy} ON)
  else()
    set(TEDGE_REMOTE_ACCESS_TARGETS_${_policy} OFF)
  endif()
endforeach()
set_property(GLOBAL APPEND PROPERTY TEDGE_OPTIONS
  "TEDGE_REMOTE_ACCESS_TARGETS_LAN:BOOL" "TEDGE_REMOTE_ACCESS_TARGETS_LIST:BOOL"
  "TEDGE_REMOTE_ACCESS_TARGETS_LOCAL:BOOL")
tedge_option(TEDGE_REMOTE_ACCESS_ALLOW_LIST STRING "" "Allow-list of host:port targets")
tedge_option(TEDGE_REMOTE_ACCESS_MAX_SESSIONS INT 1 "Maximum concurrent sessions")
tedge_option(TEDGE_REMOTE_ACCESS_BUF_SIZE INT 2048 "Bridge buffer size per direction (bytes)")
tedge_option(TEDGE_REMOTE_ACCESS_STACK_SIZE INT 8192 "Stack of each session's bridge thread (unused on POSIX)")
tedge_option(TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S INT 900 "Close a session after this many idle seconds (0 = never)")
tedge_option(TEDGE_REMOTE_ACCESS_TWIN_SESSIONS BOOL ON "List the open sessions' targets in the twin data")
tedge_option(TEDGE_REMOTE_ACCESS_MDNS BOOL ON "Resolve .local targets over mDNS")
tedge_option(TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS INT 5000 "How long each lookup of a target name waits (ms)")
tedge_option(TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS INT 3 "Lookups of a target name before the session fails")
tedge_option(TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS INT 500 "Pause between lookups of a target name (ms)")

tedge_option(TEDGE_HTTP_MAX_REDIRECTS INT 3 "HTTP redirects to follow")

# The provisioning library (include/tedge/provisioning.h), beside the client.
tedge_option(TEDGE_PROVISIONING BOOL OFF "Setting the device up over Improv (provisioning library)")
tedge_option(TEDGE_PROVISIONING_SERIAL BOOL ON "Improv Serial, fed by the application")
tedge_option(TEDGE_PROVISIONING_STACK_SIZE INT 4096 "Stack of the provisioning worker (unused on POSIX)")
if(NOT TEDGE_PROVISIONING)
  set(TEDGE_PROVISIONING_SERIAL OFF)
endif()

# Not in this port yet: each needs a POSIX design of its own (an image
# format and a launcher that can roll back; a command runner; crash dumps).
foreach(_missing TEDGE_FIRMWARE_UPDATE TEDGE_SHELL_COMMAND TEDGE_COREDUMP)
  if(${_missing})
    message(FATAL_ERROR "${_missing} is not available in the POSIX port yet")
  endif()
endforeach()

# --- What the features need -----------------------------------------------

if(TEDGE_HEALTH AND NOT TEDGE_TELEMETRY)
  message(STATUS "tedge: TEDGE_HEALTH turns TEDGE_TELEMETRY on")
  set(TEDGE_TELEMETRY ON CACHE BOOL "" FORCE)
endif()
foreach(_needs_ca TEDGE_CERT_RENEWAL TEDGE_REMOTE_ACCESS)
  if(${_needs_ca} AND NOT TEDGE_AUTH_C8Y_CA)
    message(FATAL_ERROR "${_needs_ca} needs TEDGE_AUTH=c8y-ca")
  endif()
endforeach()
if(NOT TEDGE_PARAMETERS AND TEDGE_PARAMETERS_SELF)
  set(TEDGE_PARAMETERS_SELF OFF)
endif()
if(NOT TEDGE_REMOTE_ACCESS)
  set(TEDGE_REMOTE_ACCESS_TWIN_SESSIONS OFF)
  set(TEDGE_REMOTE_ACCESS_MDNS OFF)
endif()

if(TEDGE_LOG_UPLOAD)
  tedge_derived(TEDGE_HTTP BOOL ON)
else()
  tedge_derived(TEDGE_HTTP BOOL OFF)
endif()
if(TEDGE_HTTP OR TEDGE_AUTH_C8Y_CA OR TEDGE_REMOTE_ACCESS)
  tedge_derived(TEDGE_HTTP_CLIENT BOOL ON)
else()
  tedge_derived(TEDGE_HTTP_CLIENT BOOL OFF)
endif()

# --- <tedge_config.h> -----------------------------------------------------

# Writes <out_dir>/tedge_config.h from the options above. The file is only
# rewritten when its content changes, so a reconfigure does not rebuild the
# whole client.
function(tedge_generate_config out_dir)
  get_property(_options GLOBAL PROPERTY TEDGE_OPTIONS)
  list(REMOVE_DUPLICATES _options)
  list(SORT _options)
  set(_body "")
  foreach(_entry ${_options})
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _type)
    set(_value "${${_name}}")
    if(_type STREQUAL "BOOL")
      if(_value)
        string(APPEND _body "#define ${_name} 1\n")
      endif()
    elseif(_type STREQUAL "INT")
      if(NOT _value MATCHES "^-?(0x[0-9A-Fa-f]+|[0-9]+)$")
        message(FATAL_ERROR "${_name} must be a number, not \"${_value}\"")
      endif()
      string(APPEND _body "#define ${_name} ${_value}\n")
    else()
      string(REPLACE "\\" "\\\\" _value "${_value}")
      string(REPLACE "\"" "\\\"" _value "${_value}")
      string(APPEND _body "#define ${_name} \"${_value}\"\n")
    endif()
  endforeach()

  file(CONFIGURE OUTPUT ${out_dir}/tedge_config.h CONTENT
"/* Generated by ports/posix/cmake/options.cmake. Do not edit. */
#ifndef TEDGE_CONFIG_H_
#define TEDGE_CONFIG_H_

/* How a user of this port names an option: the CMake variable. */
#define TEDGE_CONFIG_NAME(name) #name

${_body}
#endif /* TEDGE_CONFIG_H_ */
" @ONLY)
endfunction()
