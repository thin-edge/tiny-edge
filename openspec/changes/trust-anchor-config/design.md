## Context

**How trust works today.** The build concatenates every PEM in
`TEDGE_C8Y_CA_FILES` into `tedge_ca_bundle.inc`
(`ports/common/cmake/tedge_ca_bundle.cmake`, `ports/zephyr/CMakeLists.txt`,
`ports/nuttx/tools/generate.cmake`). `core/src/tedge_c8y.c` embeds it as
`server_ca[]` and, on the first connect, calls
`tedge_port_tls_set_ca(server_ca, sizeof(server_ca))` once, gated by
`ca_registered`. Every TLS connection the client opens, whatever the host,
verifies against that one set:

| Caller | Security mode | Host |
|---|---|---|
| `tedge_c8y.c` (MQTT) | `TEDGE_PORT_TLS` / `TEDGE_PORT_TLS_DEVICE` | tenant |
| `tedge_bootstrap.c` | `TEDGE_PORT_TLS` | tenant |
| `tedge_remote_access.c` | `TEDGE_PORT_TLS_DEVICE` | tenant |
| `tedge_http.c` (`tedge_http_connect`) | `TEDGE_PORT_TLS` | tenant, or any host a download URL or redirect names |

**How ports hold the CA.**

- Zephyr (`ports/zephyr/src/tedge_port_net.c`): `tls_credential_add(TAG_SERVER_CA,
  TLS_CREDENTIAL_CA_CERTIFICATE, pem, len)`. Zephyr keeps the pointer, so the
  buffer must outlive the credential. Each socket lists its sec tags with
  `ZSOCK_TLS_SEC_TAG_LIST`; Zephyr holds one credential of each type per tag.
- POSIX, NuttX and ESP-IDF (`ports/common/src/tedge_sockets.c`): the pointer
  is stored under `cred_lock`, and `tls_setup()` calls
  `mbedtls_x509_crt_parse()` on it for every connection. A verify failure
  logs `mbedtls_x509_crt_verify_info()` and returns `-EACCES`.

**Storage.** `tedge_port_kv_*` stores opaque values under `tedge/...` keys.
Zephyr uses the settings subsystem on NVS, where one value must fit in an
NVS sector with its header (4 KB sectors on the shipped boards). ESP-IDF
uses NVS blobs (the port prefixes the full key, and a blob may span pages).
POSIX and NuttX write one file per key. One certificate per key, in DER, is
under 1.5 KB for every common root and fits all three.

**Downloads and uploads already exist.** `tedge_download()`
(`core/src/tedge_http_download.c`) streams a URL into a sink, follows
redirects, and sends the token only inside the tenant's domain.
`tedge_upload()` (`core/src/tedge_http_upload.c`) streams a body through a
sink with a known length, as log upload does (measure pass, then send pass).

**Memory.** Each anchor a connection trusts is parsed into an
`mbedtls_x509_crt` for that connection: about the DER size plus a few hundred
bytes of heap. Boards with no PSRAM keep the mbedTLS heap in internal RAM:
ESP32-C6-DevKitC, ESP32-DevKitC (WROOM-32) and ESP32-S3 without PSRAM (the
NuttX `small.conf`). The QT Py ESP32-S3 (N4R2) and ESP32-CAM put the heap in
PSRAM. A remote-access session is a second TLS context, so tenant-scope
anchors cost twice while it runs.

## Goals / Non-Goals

**Goals:**

- Read and extend the device's trust anchors from Cumulocity configuration
  management, with no reflash.
- Make it impossible for a pushed file to stop the device reaching its
  tenant.
- Keep anchors added for downloads out of the tenant connections unless the
  build opts in.
- One implementation in core/, a thin port change, and the same behavior on
  Zephyr, ESP-IDF, NuttX and POSIX.
- Bounded RAM, flash and NVS use, with defaults that suit boards without
  PSRAM.

**Non-Goals:**

- Removing or replacing a compiled-in root remotely (see "Follow-up: replace
  mode").
- Revocation, pinning, client certificates.
- A general configuration-file feature beyond what trust anchors need.

## Decisions

### 1. Two configuration types, two scopes

`trusted-roots` (download scope) is used only by `TEDGE_PORT_TLS_DOWNLOAD`
connections, which `tedge_http.c` opens for hosts outside the tenant's
domain. The predicate is the one `tedge_http_download.c` already uses to
decide whether to send the token, so "which hosts may get the token" and
"which hosts may be verified by a pushed root" are the same split.

`tenant-roots` (tenant scope, `TEDGE_TRUSTED_ROOTS_TENANT`, off by default)
is used by every connection: MQTT, bootstrap, remote access and HTTP to the
tenant, and downloads too.

*Why:* the incident is about downloads, and the least-privilege answer is
that a root someone adds for GitHub cannot be used to impersonate the
tenant. But tenant CA rotation needs tenant scope, so it exists as an opt-in.

*Alternatives:* one type with a per-certificate scope marker in PEM
comments (fragile, invisible in the Cumulocity UI); one type applied
everywhere (simplest, but widens the tenant's trust for a download problem).

### 2. Additive only: the compiled-in bundle is always trusted

The port receives the compiled-in tenant bundle exactly as today, plus
separate extra lists. Nothing a configuration push does can remove the
compiled-in anchors from any connection. Adding an anchor can never make a
chain that verified before fail to verify, because mbedTLS accepts a chain
that ends at any anchor in the set. The only ways an addition hurts are heap
(capped, decision 7) and trust (scoped, decision 1).

### 3. Port interface

```c
/* include/tedge/port/net.h */
enum tedge_port_ca_scope {
	TEDGE_PORT_CA_DOWNLOAD, /* TEDGE_PORT_TLS_DOWNLOAD only (group 1) */
	TEDGE_PORT_CA_TENANT,   /* every TLS mode (added with group 5) */
};

/* A DER certificate, or PEM text (one or more certificates) with its NUL. */
struct tedge_port_ca_buf {
	const void *data;
	size_t len;
};

/* Replace the extra anchors of one scope. The buffers must stay valid until
 * the next call for that scope (same rule as tedge_port_tls_set_ca).
 * count == 0 clears them. Takes effect for connections opened afterwards.
 * @return 0, or a negative errno (-ENOSPC: the port cannot hold that many). */
int tedge_port_tls_set_extra_ca(enum tedge_port_ca_scope scope,
				const struct tedge_port_ca_buf *bufs, size_t count);

enum tedge_port_security {
	TEDGE_PORT_TCP,
	TEDGE_PORT_TLS,
	TEDGE_PORT_TLS_DEVICE,
	TEDGE_PORT_TLS_DOWNLOAD, /* new: TLS plus the download-scope anchors */
};
```

The compiled-in download roots (`TEDGE_DOWNLOAD_CA_FILES`, decision 9) are
passed as the first entry of the download scope, one PEM buffer holding all
of them; stored anchors follow as one DER buffer each. A buffer may be
either format because `mbedtls_x509_crt_parse()`, which both kinds of port
end in, accepts a DER certificate or a NUL-terminated PEM bundle.

- **Zephyr:** one sec tag per buffer (`TAG_DOWNLOAD_CA_BASE + i`), since
  Zephyr holds one CA credential per tag. The tag list for a socket is built per connection: tenant
  tag, then tenant-scope tags, then download-scope tags for
  `TEDGE_PORT_TLS_DOWNLOAD`. The port's Kconfig raises
  `CONFIG_TLS_MAX_CREDENTIALS_NUMBER` and
  `CONFIG_NET_SOCKETS_TLS_MAX_CREDENTIALS` by the configured maxima; a call
  with more than fit returns `-ENOSPC`. Replacing a scope deletes its old
  credentials before adding the new ones, under a mutex; a connection
  already open has parsed its CA chain and is unaffected.
- **Common (`tedge_sockets.c`):** store the arrays under `cred_lock`; in
  `tls_setup()`, after parsing the bundle, call
  `mbedtls_x509_crt_parse()` for each extra buffer in scope. A certificate that
  fails to parse here is skipped with a warning rather than failing the
  connection (it was validated before storing, so this means a build with
  different mbedTLS options read an older value). The verify-failure log
  gains the host and the number of anchors tried, so the next incident like
  this one is diagnosable from the log alone.

*Alternative:* re-concatenate everything into one PEM and call
`tedge_port_tls_set_ca()` again. No port change, but no scopes, PEM costs
about a third more RAM than DER, and on Zephyr the swap of the single
credential would race with a connection being opened.

### 4. Configuration operations (`tedge_config_mgmt.c`)

A small registry of configuration types, each with:

```c
struct tedge_config_type {
	const char *name;
	/* Write the current file through sink; called twice (measure, send). */
	int (*read)(tedge_upload_sink_t sink, void *sink_ctx, void *user);
	/* Begin/feed/finish a new file; finish validates and commits, or
	 * returns -errno with a reason string. */
	int (*write_begin)(void *user);
	int (*write_chunk)(const void *data, size_t len, void *user);
	int (*write_end)(char *reason, size_t reason_cap, void *user);
	void *user;
};
```

The registry is internal (no public API in this change), so other types can
be added later without a design change.

- **On connect:** SmartREST `119,<type>,...` lists the compiled-in types.
- **`c8y_UploadConfigFile`** (`526,<device>,<type>`): `501`, then create an
  event `c8y_ConfigUpload` with the type, attach the binary through
  `tedge_upload()` (two passes, as log upload), and `503` with the binary's
  URL. Runs on the log-upload worker thread pattern (its own small stack, one
  job at a time). If log upload is compiled in, the same worker is shared.
- **`c8y_DownloadConfigFile`** (`524,<device>,<url>,<type>`): `501`, then
  `tedge_download()` into the type's `write_chunk`, then `write_end`; `503`
  on success, `502,<reason>` on failure. Runs on the thread that runs
  firmware downloads, and is refused (`502,busy`) while a firmware update is
  in progress, so the two never hold two download buffers at once.
- An unknown type fails with `502,unknown configuration type <type>`.

### 5. File format

Get produces PEM text:

```
# tiny-edge trusted-roots
# built-in (read-only): CN=Go Daddy Root Certificate Authority - G2
# built-in (read-only): CN=ISRG Root X1
# built-in (read-only): CN=USERTrust ECC Certification Authority
# extra 1: CN=Example Corp Root CA, expires 2041-03-01
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
```

Built-in anchors are listed by subject in comments only, so the file stays
small and set can never "re-add" them. Set accepts any PEM text: text outside
`BEGIN/END CERTIFICATE` blocks is ignored, a certificate identical to a
built-in one is skipped silently (so a file with the built-ins pasted in
still works), and duplicates are collapsed. The resulting list replaces the
type's extra anchors (whole-file semantics, as Cumulocity configuration
management expects). An empty or certificate-free file clears them.

### 6. Streaming parse and validation (`tedge_trust.c`)

The download is not held whole. `write_chunk` feeds a line splitter with a
buffer of one PEM block (about 2.2 KB with the default `MAX_DER`, from the
client's heap for the duration of the operation). At each `END CERTIFICATE`
the block is given to `mbedtls_x509_crt_parse()`, which decodes the PEM;
the DER kept is the parsed certificate's `raw`. It is checked at once:

1. It parses. mbedTLS refuses a key type or curve the build lacks here,
   so this is also the "key this device cannot verify" check.
2. It is at most `TEDGE_TRUSTED_ROOTS_MAX_DER` bytes.
3. A copy of a compiled-in root, or of a certificate earlier in the file,
   is skipped.
4. Basic constraints say CA (`mbedtls_x509_crt_get_ca_istrue()`; a
   certificate without the extension is refused), and the key usage, if
   present, allows certificate signing.
5. Not expired and not yet valid, when the clock is valid (the client only
   receives operations with a valid clock, so in practice always).
6. Not more than `TEDGE_TRUSTED_ROOTS_MAX` certificates to store.

The first failure aborts the whole file: `502` with a reason that names the
certificate by position and subject, e.g. `certificate 2 (CN=Foo): not a CA
certificate`. The set in force is unchanged. Self-signed is not required, so an
intermediate can be an anchor when that is what the operator wants.

There is no staging buffer for the whole set: each certificate that passes
is written to the inactive bank (decision 7) straight away, so a set needs
one PEM block of RAM however many certificates it holds. If the PEM block
cannot be allocated, the set fails with "out of memory" rather than risking
the connection.

### 7. Storage layout and atomic switch

```
tedge/trust/<scope>/active   "a" or "b"
tedge/trust/<scope>/a/0..n   DER, one certificate per key
tedge/trust/<scope>/b/0..n
```

A set writes the new certificates to the inactive bank, deletes that bank's
leftover higher indexes, then writes `active`. A reset before the `active`
write leaves the old set in force; after it, the new one. Reading the bank
stops at the first missing index. The format is a contract between firmware
versions (kv.h's rule): it is versioned by the key path, and a future change
uses a new path.

On the first connection (`tedge_trust_register()`, called where
`tedge_c8y.c` registers the tenant bundle, so before any download),
`tedge_trust` loads the active bank into one block of the client's heap and
calls `tedge_port_tls_set_extra_ca()` with the compiled-in download bundle
followed by the stored certificates. After a successful set, it loads the new
bank into a new block, calls the port, then frees the old block (the port
has replaced its pointers by then, which is the interface's rule).

### 8. When changes take effect

On the next connection that uses the scope. A firmware download started
after the set uses the new anchors. The MQTT session is not dropped for a
`tenant-roots` change: the new anchors matter only on reconnect, and
reconnecting on purpose would only add risk. `tedge_c8y.c` keeps
`ca_registered` for the compiled-in bundle; the extras are registered by
`tedge_trust` independently.

### 9. Compiled-in download roots (`TEDGE_DOWNLOAD_CA_FILES`)

A second PEM bundle, `tedge_download_ca.inc`, built by the same cmake helper
as the tenant bundle (`tedge_download_ca_bundle()` in
`ports/common/cmake/tedge_ca_bundle.cmake`), embedded in `tedge_c8y.c`
beside the tenant bundle and registered with the tenant bundle on the first
connect. When `tedge_trust.c` arrives (group 2) it takes over the
registration and passes the bundle as the first download-scope buffer. Default: ISRG Root X1 and USERTrust ECC Certification Authority,
which together cover github.com, release-assets.githubusercontent.com,
objects.githubusercontent.com and most public HTTPS hosts. Set the option
empty to drop them. They are not added to `TEDGE_C8Y_CA_FILES`, so the
tenant's trust does not widen.

They are independent of `TEDGE_CONFIG_MGMT`: a build without configuration
management still gets working GitHub downloads. This part can ship first, on
its own, and closes the reported incident.

### 10. Cost defaults

`TEDGE_TRUSTED_ROOTS_MAX = 4` and `MAX_DER = 1536`: four RSA-4096 roots in
the worst case, 6 KB DER, held once in RAM and parsed per connection. Boards
without PSRAM get a profile override of `MAX = 2` in
`ports/zephyr/profiles/boards/{esp32c6-devkitc,esp32-devkitc}.conf` and the
NuttX `small.conf`, where the extra parse during a download TLS session is
the tightest point (firmware download already runs beside the MQTT
session). The measured numbers go in the proposal.

### Follow-up: replace mode (not in this change)

Replacing the compiled-in tenant roots would be a separate mode of
`tenant-roots`, applied as firmware is: store as pending, open a
trial connection to the tenant using only the new set, commit on CONNACK,
roll back on failure or on a reset before commit, and report the operation
after the outcome. It needs the trial-connect hooks firmware update has in
`tedge_fw_decision.c`. It is left out because the additive model already
covers rotation as long as the new root is pushed in time, and a mistake in
replace mode is the one thing that strands a fleet.

## Risks / Trade-offs

- **A tenant CA rotation pushed too late strands devices.** If the tenant
  starts serving a chain to a root the device does not have, the device
  cannot connect, so it cannot receive the new root either. → Document that
  the new root must be pushed to `tenant-roots` (or shipped in firmware)
  before the old root stops being served, and that devices offline during
  the window need a reflash. Builds without `TEDGE_TRUSTED_ROOTS_TENANT`
  rely on firmware updates for rotation, which need the tenant connection
  too.
- **A malicious anchor in download scope lets someone serve a fake firmware
  or config file over TLS.** → The port still checks every image, and
  rejects a fake one when the bootloader requires signatures (MCUboot with a
  signing key, ESP-IDF secure boot). Without required signatures, any host
  the device trusts for downloads can supply firmware; the operator guide
  says to require them.
  Config files are not signed; the only config type is trust anchors, which
  cannot affect tenant connections from download scope. Whoever can push
  configuration already controls the device through the tenant.
- **Heap during a download.** Each extra anchor costs heap in every TLS
  session in its scope. → Caps, a smaller default on boards without PSRAM,
  and download-scope anchors are parsed only for downloads.
- **Zephyr credential slots.** Raising the credential limits costs a small
  static table per slot. → Raised only by the configured maxima, and only
  when the feature is on.
- **NVS wear.** Each set writes up to `MAX + 1` values. → Sets are rare
  operator actions; no periodic writes.
- **Expired anchors in storage.** A stored anchor can expire later. → It
  stays stored (mbedTLS ignores an expired anchor for verification); get
  marks it `EXPIRED` in its comment line so operators see it.
- **Clock wrong at validation.** → Configuration operations arrive only over
  an established tenant connection, which requires a valid clock.

## Migration Plan

- No migration: no stored anchors means today's behavior, plus the default
  download roots when `TEDGE_DOWNLOAD_CA_FILES` is left at its default.
- Rollout order: (1) `TEDGE_DOWNLOAD_CA_FILES` alone, which fixes GitHub
  firmware URLs; (2) configuration management with `trusted-roots`;
  (3) `tenant-roots`.
- Rollback: push an empty file to clear extra anchors, or flash an image
  without the feature. Stored keys are ignored by an image without it, and
  the next image with it reads them again.

## Open Questions

- Should `tenant-roots` also be off for remote access, so a pushed root can
  never verify the remote-access endpoint? Proposed: no, the endpoint is the
  tenant.
- Is SmartREST `119` the right advertisement on the MQTT Service endpoint,
  or does the JSON operation mode (`TEDGE_C8Y_OPERATION_JSON`) need the
  `c8y_SupportedConfigurations` fragment sent as inventory JSON instead?
  To verify in the first core task.
- Does Zephyr's `tls_credential_add` accept a DER CA credential on every
  mbedTLS configuration the boards use (PEM parsing disabled in some
  profiles)? If not, store PEM for Zephyr only.
- Should a config download from a host outside the tenant be allowed at all,
  or should `c8y_DownloadConfigFile` URLs be restricted to the tenant?
  Proposed: allowed, same rules as firmware.
