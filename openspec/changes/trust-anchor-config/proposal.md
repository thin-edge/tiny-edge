## Why

Every port trusts exactly the roots compiled in from `TEDGE_C8Y_CA_FILES`
(default `certs/godaddy-root-g2.pem`, the Cumulocity tenant's CA), for every
TLS connection. A firmware update from a GitHub release URL therefore fails
on all ports: on an ESP32-C6 running NuttX (microcontroller-devices v0.17.2,
tiny-edge 0.8.1) it ends with "download failed (-13)", the `-EACCES` that
`tls_handshake` in `ports/common/src/tedge_sockets.c` returns on
`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`. github.com chains to USERTrust ECC and
the redirect target, release-assets.githubusercontent.com, chains to ISRG
Root X1. Fixing that today means a new build and a reflash, and so does any
future change of root, including a tenant CA rotation.

Operators already manage device files through Cumulocity configuration
management. The device's trust anchors should be one of those files, so the
root of trust can be read and extended remotely without reflashing.

## What Changes

- **A configuration type for extra trust anchors**, `trusted-roots`,
  advertised in `c8y_SupportedConfigurations`. `c8y_UploadConfigFile` uploads
  the device's anchors as a PEM file (built-in ones marked read-only in
  comments). `c8y_DownloadConfigFile` replaces the set of extra anchors with
  the certificates in the downloaded PEM file. An empty file removes them.
- **Additive only.** The compiled-in bundle stays trusted whatever is pushed,
  so a bad file can never cut the device off its tenant. Removing or
  replacing a built-in root is a non-goal of this change.
- **Scoped.** Anchors from `trusted-roots` verify HTTPS downloads from hosts
  outside the tenant's domain (firmware and configuration URLs, after
  redirects). They are never used for the MQTT session, the token, remote
  access, or HTTP calls to the tenant.
- **A second, opt-in type, `tenant-roots`**, behind its own build option (off
  by default), whose anchors are also trusted for the tenant connections.
  This is what lets an operator stage a tenant CA rotation remotely.
- **Validation before storing**: each certificate is parsed with mbedTLS and
  rejected when it is not a CA certificate, is expired, uses a key the build
  cannot verify, or the set exceeds the configured count or size. The
  operation fails with the reason, and the stored set is left unchanged.
- **Persistent storage** of the anchors through `tedge_port_kv_*`, one DER
  certificate per key, written so that a reset part-way through a set leaves
  the previous set in force.
- **Applied on the next connection.** No running session is torn down.
- **A complementary build option, `TEDGE_DOWNLOAD_CA_FILES`**: compiled-in
  roots used only for downloads outside the tenant, defaulting to ISRG Root
  X1 and USERTrust ECC, so GitHub release URLs work out of the box. The
  tenant bundle (`TEDGE_C8Y_CA_FILES`) is unchanged. MCUboot (or the port's
  own image check) still verifies the signature of every firmware image, so
  a wider download trust does not weaken image integrity.

### Non-goals

- Replacing or removing the compiled-in tenant roots remotely. A "replace"
  mode would need a trial connection and rollback, as firmware has; it is
  sketched in design.md as a possible follow-up change and is not built here.
- Client certificates, device keys, or certificate revocation (CRL/OCSP).
- Pinning a host to a specific anchor. A trusted anchor is trusted for every
  host in its scope.
- Configuration management for anything other than trust anchors. The
  config download/upload plumbing is written so other types can be added
  later, but none is.
- A filesystem. Anchors live in the key-value store only.

### Build options added

| Option | Default | What it adds |
|---|---|---|
| `TEDGE_CONFIG_MGMT` | off | `c8y_SupportedConfigurations`, `c8y_UploadConfigFile` and `c8y_DownloadConfigFile` handling (needs firmware update's download path and log upload's upload path) |
| `TEDGE_TRUSTED_ROOTS` | on with `TEDGE_CONFIG_MGMT` | The `trusted-roots` type, storage and validation |
| `TEDGE_TRUSTED_ROOTS_TENANT` | off | The `tenant-roots` type: extra anchors also trusted for tenant connections |
| `TEDGE_TRUSTED_ROOTS_MAX` | 4 | Most extra anchors stored and loaded, per type |
| `TEDGE_TRUSTED_ROOTS_MAX_DER` | 1536 | Largest accepted certificate, DER bytes |
| `TEDGE_DOWNLOAD_CA_FILES` | `certs/isrg-root-x1.pem certs/usertrust-ecc.pem` | Compiled-in roots for downloads outside the tenant |

Measured cost per board is not available yet; it is a task (8.x) and this
section is updated with the numbers before the change leaves draft. Expected
order of magnitude, from the code that is reused: about 4 to 6 KB flash for
the config operations and PEM/DER handling (mbedTLS PEM and base64 are
already linked on every port), about 3 KB flash for the two default download
roots, and per extra anchor at runtime its DER size (about 0.5 to 1.4 KB)
held in RAM, plus about the same again in mbedTLS heap in every TLS
connection that trusts it. Target boards: ESP32-S3-DevKitC, ESP32-C6-DevKitC,
ESP32-DevKitC (WROOM-32), QT Py ESP32-S3, ESP32-CAM, and the NuttX
ESP32-C6 and ESP32-S3 builds.

### Interface changes

- **Public `tedge_*` API**: none. The feature is driven from Cumulocity.
- **Port interface** (`include/tedge/port/net.h`): one new call,
  `tedge_port_tls_set_extra_ca()`, which hands the port the list of extra DER
  anchors for each scope, and one new `enum tedge_port_security` value,
  `TEDGE_PORT_TLS_DOWNLOAD`, which verifies against the tenant anchors plus
  the download-scope anchors. Existing values keep their meaning; with
  `TEDGE_TRUSTED_ROOTS_TENANT`, `TEDGE_PORT_TLS` and `TEDGE_PORT_TLS_DEVICE`
  also include the tenant-scope extras.
- **Messages the device sends**: SmartREST `119` (supported configurations)
  on connect; the config upload event and binary, and `501`/`502`/`503` for
  the two operations; no change to any existing message.

### Where the change lives

Both. core/: the configuration operations, PEM parsing, validation, storage
layout and the choice of security mode per connection (`tedge_http.c`).
Ports: the new port call and security mode in Zephyr's `tedge_port_net.c`
and in `ports/common/src/tedge_sockets.c` (used by POSIX, NuttX and
ESP-IDF), plus each port's build options and Kconfig. No RTOS call is added
to core/.

## Capabilities

### New Capabilities

- `tedge-trust-anchors`: which roots the device trusts for which
  connections, the compiled-in and remotely managed sets, their scopes,
  validation, storage, and when changes take effect.
- `tedge-config-management`: the Cumulocity configuration operations
  (supported types, upload, download) and how a configuration type plugs
  into them.

### Modified Capabilities

- `tedge-c8y-connection`: the MQTT session validates against the compiled-in
  tenant anchors plus, only when `TEDGE_TRUSTED_ROOTS_TENANT` is enabled, the
  `tenant-roots` anchors.
- `tedge-firmware-update`: downloads from hosts outside the tenant's domain
  are also verified against the download-scope anchors.
- `device-management-features`: configuration management joins the list of
  independently selectable features.

## Impact

- core/: new `tedge_config_mgmt.c` and `tedge_trust.c`; changes to
  `tedge_c8y.c` (registration, supported types, operation dispatch),
  `tedge_http.c` (security mode by host) and the internal headers.
- Ports: `include/tedge/port/net.h`; `ports/zephyr/src/tedge_port_net.c`
  (one more sec tag per extra anchor, `CONFIG_TLS_MAX_CREDENTIALS_NUMBER` and
  `CONFIG_NET_SOCKETS_TLS_MAX_CREDENTIALS` raised accordingly);
  `ports/common/src/tedge_sockets.c`; Kconfig and CMake options for Zephyr,
  ESP-IDF, NuttX and POSIX; `ports/common/cmake/tedge_ca_bundle.cmake` gains
  a second bundle.
- New PEM files in `certs/`: ISRG Root X1 and USERTrust ECC.
- Documentation: each port's README, and an operator guide covering the
  rotation caveat (push a new tenant root before the old one stops being
  served).
