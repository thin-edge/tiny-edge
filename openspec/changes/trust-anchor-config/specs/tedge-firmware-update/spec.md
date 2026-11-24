## MODIFIED Requirements

### Requirement: Downloads follow redirects and keep the token to the tenant

The client SHALL follow HTTP redirects up to a configured limit, including to
another host or scheme, and SHALL handle a redirect target of at least one
kilobyte. It SHALL send its cloud token only to hosts within the tenant's own
domain, and SHALL drop it when a redirect leaves that domain. It SHALL
verify a host within the tenant's domain against the tenant trust anchors,
and a host outside it against the tenant trust anchors plus the
download-scope anchors (`TEDGE_DOWNLOAD_CA_FILES` and `trusted-roots`). It
SHALL write the image as the parser reports each body segment, so that a
chunked transfer is stored exactly as sent.

#### Scenario: A release asset on another host

- **WHEN** the firmware URL redirects to a download host outside the tenant
- **THEN** the client follows the redirect, sends no token to that host, and
  stores the image correctly

#### Scenario: A chunked transfer

- **WHEN** the cloud sends the image with chunked transfer encoding
- **THEN** the stored image matches what was sent, and the bootloader accepts
  it

#### Scenario: A host whose root the device does not trust

- **WHEN** the firmware URL names a host whose chain ends at a root in
  neither the tenant nor the download-scope anchors
- **THEN** the operation fails with a reason that names the host and says
  the certificate could not be verified
