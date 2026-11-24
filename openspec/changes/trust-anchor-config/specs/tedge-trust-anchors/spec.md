## ADDED Requirements

### Requirement: The compiled-in tenant anchors are always trusted

The client SHALL trust the anchors compiled in from `TEDGE_C8Y_CA_FILES` for
every TLS connection, whatever configuration has been pushed. No
configuration operation SHALL remove, replace or disable them.

#### Scenario: A pushed file cannot cut the device off

- **WHEN** an operator pushes a `trusted-roots` or `tenant-roots` file that
  does not contain the tenant's root
- **THEN** the device still connects to its tenant after a reset or
  reconnect

#### Scenario: Clearing the extra anchors

- **WHEN** an operator pushes an empty `trusted-roots` file
- **THEN** the extra anchors are removed and the compiled-in anchors remain
  in force for every connection

### Requirement: Extra anchors are scoped

Anchors from `trusted-roots` and from `TEDGE_DOWNLOAD_CA_FILES` SHALL be
trusted only for HTTPS connections to hosts outside the tenant's domain, as
decided by the same rule that decides where the cloud token may be sent.
Anchors from `tenant-roots` SHALL be trusted for every TLS connection, and
SHALL exist only when the build enables `TEDGE_TRUSTED_ROOTS_TENANT`.

#### Scenario: A download root does not verify the tenant

- **WHEN** a root is stored in `trusted-roots` and something presents a
  certificate chaining only to that root for the tenant's MQTT endpoint
- **THEN** the TLS handshake fails

#### Scenario: A download root verifies a redirect target

- **WHEN** ISRG Root X1 is a download-scope anchor and a firmware URL
  redirects to release-assets.githubusercontent.com
- **THEN** the download succeeds

#### Scenario: Tenant scope disabled at build time

- **WHEN** the image is built without `TEDGE_TRUSTED_ROOTS_TENANT`
- **THEN** `tenant-roots` is not advertised, and an operation for it fails
  with a reason that names the missing feature

### Requirement: Compiled-in download roots

The build SHALL embed the PEM files listed in `TEDGE_DOWNLOAD_CA_FILES` as
download-scope anchors, independently of configuration management. The
default SHALL be ISRG Root X1 and USERTrust ECC Certification Authority. An
empty value SHALL embed none.

#### Scenario: GitHub release firmware with the default build

- **WHEN** a device built with the default options receives a firmware
  operation whose URL is a GitHub release asset
- **THEN** the TLS handshakes to github.com and to the redirect target
  succeed, and the image is installed only if its signature check passes

#### Scenario: Download roots removed

- **WHEN** the image is built with `TEDGE_DOWNLOAD_CA_FILES` empty
- **THEN** downloads outside the tenant are verified only against the tenant
  anchors and any stored `trusted-roots`

### Requirement: Certificates are validated before they are stored

Before storing a set, the client SHALL parse every certificate in the file
and SHALL reject the whole file when any certificate does not parse, is not
a CA certificate, is expired or not yet valid, has a key the build cannot
verify, is larger than `TEDGE_TRUSTED_ROOTS_MAX_DER`, or when the file holds
more than `TEDGE_TRUSTED_ROOTS_MAX` certificates after built-in duplicates
and repeats are dropped. A rejected file SHALL leave the stored set
unchanged, and the operation SHALL fail with a reason that names the
offending certificate by position and subject.

#### Scenario: Leaf certificate pushed by mistake

- **WHEN** the file contains a server certificate without the CA basic
  constraint
- **THEN** the operation fails with "certificate 1 (CN=...): not a CA
  certificate" and the previous anchors stay in force

#### Scenario: Too many certificates

- **WHEN** the file holds more certificates than `TEDGE_TRUSTED_ROOTS_MAX`
- **THEN** the operation fails with a reason that states the limit

#### Scenario: Built-in root included in the file

- **WHEN** the file contains a copy of a compiled-in root and one new root
- **THEN** the compiled-in copy is skipped, only the new root is stored, and
  the operation succeeds

### Requirement: Stored anchors survive a reset and a firmware update

The client SHALL store extra anchors through the port's key-value store, one
certificate per key, and SHALL switch from the old set to the new set with a
single final write, so that a reset at any point leaves either the complete
old set or the complete new set in force. The storage format SHALL be read
unchanged by later firmware versions.

#### Scenario: Reset during a set

- **WHEN** the device resets after some of the new certificates are written
  but before the switch
- **THEN** after boot the previous set is in force and the operation is
  reported as failed

#### Scenario: Firmware update keeps the anchors

- **WHEN** a device with stored anchors installs a new firmware image that
  includes the feature
- **THEN** the new image trusts the same extra anchors

### Requirement: New anchors apply from the next connection

A new set SHALL apply to TLS connections opened after the operation
succeeds. The client SHALL NOT close an open connection because the anchors
changed.

#### Scenario: Set followed by a firmware update

- **WHEN** a `trusted-roots` set succeeds and a firmware operation follows
- **THEN** the firmware download is verified against the new set, and the
  MQTT session stayed connected throughout

### Requirement: Extra anchors are bounded in memory

The client SHALL hold at most `TEDGE_TRUSTED_ROOTS_MAX` extra anchors per
scope in RAM, in DER, and SHALL parse download-scope anchors only for
download connections. A set operation that cannot get its working memory
SHALL fail with "out of memory" and leave the connection and stored set
unaffected.

#### Scenario: No room for the staging buffer

- **WHEN** a `trusted-roots` set arrives and the heap cannot provide the
  staging buffer
- **THEN** the operation fails with "out of memory" and the MQTT session
  stays connected
