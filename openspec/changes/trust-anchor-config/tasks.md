## 1. Compiled-in download roots (ships first, fixes GitHub firmware URLs)

- [x] 1.1 Add `certs/isrg-root-x1.pem` and `certs/usertrust-ecc.pem`, each with a comment giving source URL, SHA-256 fingerprint and expiry
- [x] 1.2 Extend `ports/common/cmake/tedge_ca_bundle.cmake` to emit a second PEM bundle (`tedge_download_ca.inc`, usable from `cmake -P`), and wire it into every port's build (Zephyr, ESP-IDF, NuttX `tools/generate.cmake`, POSIX)
- [x] 1.3 Add `TEDGE_DOWNLOAD_CA_FILES` to the Zephyr Kconfig, ESP-IDF Kconfig, NuttX Kconfig and `ports/posix/cmake/options.cmake`, with the two roots as default and `tests/host-compile/include/tedge_config.h` updated
- [x] 1.4 Port interface: add `enum tedge_port_ca_scope` (download scope only for now), `struct tedge_port_ca_buf` (DER or PEM), `tedge_port_tls_set_extra_ca()` and `TEDGE_PORT_TLS_DOWNLOAD` to `include/tedge/port/net.h`, with doc comments (lifetime rule as for `tedge_port_tls_set_ca`)
- [x] 1.5 Common port: implement `tedge_port_tls_set_extra_ca()` and `TEDGE_PORT_TLS_DOWNLOAD` in `ports/common/src/tedge_sockets.c` (parse extras per connection under `cred_lock`, skip unparsable ones with a warning); log host and anchor count on verify failure
- [x] 1.6 Zephyr port: implement both in `ports/zephyr/src/tedge_port_net.c` with one sec tag per buffer and a per-connection tag list (raising `CONFIG_TLS_MAX_CREDENTIALS_NUMBER` for stored anchors is left to group 4; the compiled-in bundle uses the 4th of the default 4 slots)
- [x] 1.7 Core: in `core/src/tedge_http.c`, open hosts outside the tenant's domain with `TEDGE_PORT_TLS_DOWNLOAD` (reuse the token-domain predicate from `tedge_http_download.c`); register the compiled-in download roots with the tenant bundle on the first connect
- [x] 1.8 Host tests (POSIX): a local HTTPS server with a test CA added through the port call verifies only in download mode; the tenant mode rejects it
- [x] 1.9 Hardware check on the NuttX ESP32-C6 (the incident's device, 2026-11-24): an image from this branch (0.17.3-trustroots) installed from the tenant, then a firmware update from the GitHub release URL of microcontroller-devices v0.17.2 downloaded through the redirect (1,341,360 B in 22 s), installed and confirmed; the same URL had failed three times with -13 on the old image. Recorded in the PR
- [ ] 1.10 Same check on Zephyr ESP32-S3-DevKitC

## 2. Trust store in core (`core/src/tedge_trust.c`)

- [x] 2.1 Storage layout: banked keys `tedge/trust/download/{active,a/N,b/N}`; load the active bank on the first connection (`tedge_trust_register()`) into one block of the client's heap and call `tedge_port_tls_set_extra_ca()`
- [x] 2.2 Atomic commit: write each checked certificate to the inactive bank as it arrives (no staging buffer), delete leftover indexes, write `active`, load the new block, call the port, free the old block
- [x] 2.3 Streaming PEM splitter with a one-block buffer; `mbedtls_x509_crt_parse()` decodes each block
- [x] 2.4 Validation: parse, CA basic constraint and key usage, validity period, key type supported by the build, size and count caps, skip built-in duplicates and repeats; reasons naming position and subject
- [x] 2.5 PEM writer for get: built-in subjects as comments, each extra as subject/expiry comment plus PEM (two passes through an upload sink, as log upload)
- [x] 2.6 Tests (POSIX `tests/test_trust.c`, on the real key-value store and TLS stack): valid file, round trip of the get file, leaf cert, expired, not yet valid, no keyCertSign, too large, too many, unparsable, unterminated, built-in duplicate, CRLF and byte-at-a-time input, empty file clears, an unfinished set keeps the old one, and a stored root verifies a loopback download server. Not added to the Zephyr unit tests: the option is not in the Zephyr Kconfig until 4.1

## 3. Configuration operations in core (`core/src/tedge_config_mgmt.c`)

- [x] 3.1 Internal table of configuration types, with `trusted-roots` from `tedge_trust.c` (`tenant-roots` comes with group 5)
- [x] 3.2 Advertise types with SmartREST `119` on session start and add the two operations to the supported list; map the JSON operations (`TEDGE_C8Y_OPERATION_JSON`) to 524/526. Not yet confirmed against a real tenant: needs a device build with the options (group 4)
- [x] 3.3 `c8y_UploadConfigFile`: executing, create an event of the configuration's type, attach the binary with `tedge_upload()`, succeed with the URL. Own thread rather than the log-upload worker; the two never run at once
- [x] 3.4 `c8y_DownloadConfigFile`: executing, `tedge_download()` into the type's writer, success or failure with the reason; refused with "busy" during a firmware download or log upload, and those refuse while it runs
- [x] 3.5 Failure paths: unknown type, feature not compiled in, out of memory, download error; each reported with the reason in the 502
- [x] 3.6 POSIX `tests/test_config.c` against a fake HTTPS tenant on loopback: types line, unknown type, malformed request, a set, busy, a refused certificate, 404, oversize, and a get (identity lookup, event of the configuration type on the device, binary with the file, result URL, operation ids)
- [ ] 3.7 Check both operations against a real tenant on a device. The NuttX image (0.17.4-config) and a script (`ports/nuttx/build-nuttx/devtest/run.sh`, not committed) are ready; not run yet

## 4. Build options

- [x] 4.1 Zephyr Kconfig: `TEDGE_CONFIG_MGMT` (off by default, selects `TEDGE_HTTP`), `TEDGE_CONFIG_MGMT_STACK_SIZE`, `TEDGE_CONFIG_MAX_BYTES`, `TEDGE_TRUSTED_ROOTS`, `TEDGE_TRUSTED_ROOTS_MAX` (4 with PSRAM, 2 without), `TEDGE_TRUSTED_ROOTS_MAX_DER`; `TEDGE_HEAP_SIZE` +4 KB with trusted roots; the port registers only the roots that fit the credential pool and a socket's tag list; Kconfig rule cases `config-mgmt-defaults` and `config-mgmt-absent`
- [x] 4.2 ESP-IDF Kconfig for the same options (on by default, 2 roots without PSRAM, heap +4 KB); `sdkconfig.ci.small` turns it off. Built for esp32c6, full and small
- [x] 4.3 NuttX Kconfig (on by default, 4 roots, heap 20 KB with trusted roots, `TEDGE_HTTP` follows it); `configs/small.conf` turns configuration management off instead of lowering the cap, since the PSRAM-less S3 has no room for its second TLS session. esp32c6 agent image built
- [x] 4.4 POSIX `cmake/options.cmake` (all options, on by default; `TEDGE_HTTP` follows `TEDGE_CONFIG_MGMT`); Zephyr `profiles/full.conf` turns the feature on and raises `CONFIG_TLS_MAX_CREDENTIALS_NUMBER` to 8 and `CONFIG_NET_SOCKETS_TLS_MAX_CREDENTIALS` to 6. The no-PSRAM cap comes from the Kconfig default rather than board files

## 5. Tenant scope (`tenant-roots`)

- [ ] 5.1 Core: register `tenant-roots` only with `TEDGE_TRUSTED_ROOTS_TENANT`; pass the tenant-scope list at start-up and after each set
- [ ] 5.2 Common port: include tenant-scope extras for `TEDGE_PORT_TLS` and `TEDGE_PORT_TLS_DEVICE`
- [ ] 5.3 Zephyr port: include tenant-scope tags in both tag lists
- [ ] 5.4 POSIX system test: a local MQTT broker with a certificate from a second CA connects only after that CA is pushed to `tenant-roots`, and the built-in anchor still works after an empty push

## 6. System tests against Cumulocity

- [ ] 6.1 Robot test: get `trusted-roots`, push a file with a new root, get again and compare, push an empty file
- [ ] 6.2 Robot test: push a leaf certificate and an oversized file; both operations fail with the expected reason and the stored set is unchanged
- [ ] 6.3 Robot test: firmware update from a URL that needs a pushed root succeeds only after the push

## 7. Documentation

- [ ] 7.1 Operator guide: config types, file format, scopes, limits, and the rotation caveat (push the new tenant root before the old one stops being served; offline devices need a reflash)
- [ ] 7.2 Port READMEs: new options, Zephyr credential limits, RAM cost per anchor and the defaults on boards without PSRAM
- [ ] 7.3 Update the NuttX and Zephyr READMEs and REPORT.md where they say the client trusts only the tenant CA

## 8. Measurements

- [ ] 8.1 Measure flash and static RAM for `TEDGE_DOWNLOAD_CA_FILES` alone, and for `TEDGE_CONFIG_MGMT` with default options, on ESP32-S3-DevKitC, ESP32-C6-DevKitC, ESP32-DevKitC, QT Py ESP32-S3, ESP32-CAM and the NuttX ESP32-C6/S3 builds
- [ ] 8.2 Measure peak heap during a firmware download with 0, 2 and 4 extra anchors on ESP32-C6-DevKitC (no PSRAM); adjust the defaults if the download fails
- [ ] 8.3 Fill the measured numbers into proposal.md's build-options section
- [ ] 8.4 Measure the configuration thread's stack high-water mark on each RTOS port (a get and a set). 64-bit Linux uses 22.6 KB (POSIX default now 32 KB); the defaults of 8 KB (Zephyr) and 12 KB (ESP-IDF, NuttX) are unmeasured
