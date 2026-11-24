## ADDED Requirements

### Requirement: The device advertises its configuration types

When configuration management is compiled in, the device SHALL advertise
every configuration type compiled into the image in
`c8y_SupportedConfigurations` each time its session starts, and SHALL
advertise `c8y_UploadConfigFile` and `c8y_DownloadConfigFile` among its
supported operations.

#### Scenario: Default build with configuration management

- **WHEN** a device built with `TEDGE_CONFIG_MGMT` connects
- **THEN** `c8y_SupportedConfigurations` lists `trusted-roots`, and lists
  `tenant-roots` only when `TEDGE_TRUSTED_ROOTS_TENANT` is enabled

#### Scenario: Built without configuration management

- **WHEN** a device built without `TEDGE_CONFIG_MGMT` receives
  `c8y_DownloadConfigFile`
- **THEN** it fails the operation with a reason that names the missing
  feature

### Requirement: Get uploads the current configuration file

On `c8y_UploadConfigFile` for a known type, the device SHALL mark the
operation executing, upload the type's current file as an event binary with
a correct Content-Length without holding the file whole in RAM, and mark the
operation successful with the binary's URL. For `trusted-roots` and
`tenant-roots`, the file SHALL list the compiled-in anchors by subject in
comment lines and contain each extra anchor as a PEM certificate preceded by
a comment with its subject and expiry.

#### Scenario: Reading the trust anchors

- **WHEN** an operator requests the `trusted-roots` file
- **THEN** the uploaded file names the compiled-in roots in comments and
  contains every stored extra anchor as PEM

### Requirement: Set downloads and applies a configuration file

On `c8y_DownloadConfigFile` for a known type, the device SHALL mark the
operation executing, download the URL with the same redirect and token rules
as firmware downloads, pass the body to the type as it arrives, and mark the
operation successful only after the type has validated and stored it. On any
failure it SHALL mark the operation failed with the type's reason, and the
previous configuration SHALL stay in force.

#### Scenario: Unknown type

- **WHEN** the operation names a type the image does not have
- **THEN** it fails with "unknown configuration type <type>"

#### Scenario: Download fails

- **WHEN** the download ends with a TLS or HTTP error
- **THEN** the operation fails with the error, and the stored configuration
  is unchanged

#### Scenario: Firmware update in progress

- **WHEN** a configuration download arrives while a firmware update is
  downloading
- **THEN** the configuration operation fails with "busy" and the firmware
  update continues
