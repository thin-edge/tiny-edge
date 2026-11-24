## MODIFIED Requirements

### Requirement: One MQTT session to Cumulocity, on the endpoint that matches the authentication

The client SHALL hold one MQTTS session to Cumulocity: the MQTT Service (port
9883) with certificate authentication, or Core MQTT (port 8883) with basic
authentication or when the Core MQTT endpoint is selected. It SHALL validate
the server certificate against the compiled-in tenant trust anchors and,
only when the build enables `TEDGE_TRUSTED_ROOTS_TENANT`, the stored
`tenant-roots` anchors; it SHALL NOT use download-scope anchors for this
session. It SHALL only connect once the realtime clock is valid (set by the
application or by the client's SNTP query). It SHALL subscribe with one
topic filter per SUBSCRIBE packet, and SHALL NOT subscribe to wildcard
free-form topics.

#### Scenario: Certificate device connects

- **WHEN** an enrolled device has an IPv4 address and a valid clock
- **THEN** the client connects to port 9883 with mutual TLS and reaches the
  connected state

#### Scenario: Clock not yet set

- **WHEN** the network is up but the realtime clock predates the build
- **THEN** the client sets it with SNTP before opening the TLS session

#### Scenario: Tenant chain to a pushed tenant root

- **WHEN** the build enables `TEDGE_TRUSTED_ROOTS_TENANT`, the tenant's new
  root is stored in `tenant-roots`, and the tenant starts serving a chain to
  that root
- **THEN** the client reconnects successfully
