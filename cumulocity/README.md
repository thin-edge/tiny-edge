# Cumulocity tenant setup

What a Cumulocity tenant needs so that devices running tiny-edge show
their measurements, their state and an editable Parameters tab. The devices
talk to the **Cumulocity MQTT Service** on thin-edge.io `te/` topics
([ports/zephyr/README.md](../ports/zephyr/README.md), "What the client publishes"); on that transport Cumulocity only stores
what a **Smart Function** maps. Operations (restart, firmware, remote access,
parameters, shell) need no mapping.

| Directory | What it is | Needed for |
|---|---|---|
| `smart-functions/tiny-edge-measurements/` | `te/device/*///m/*` → measurements | any measurement, including `tedge_health` |
| `smart-functions/tiny-edge-twin/` | `te/device/*///twin/*` → managed-object fragments | `remoteAccess`, `c8y_Agent`, `tedge_OS`, `tedge_Agent`, `c8y_Hardware`, and the current parameter values the Parameters tab shows |
| `smart-functions/tiny-edge-firmware-progress/` | `te/device/*///progress/firmware` → `c8y_FirmwareDownload` events | download progress while a firmware update runs (optional) |
| `smart-functions/tiny-edge-events/` | `te/device/*///e/*` → events | any event, including `zephyr_Identify` from the sw0 identify pattern |
| `dtm/*.json` | Digital Twin Manager property definitions for the client's own parameter sets (`tiny_edge`, `tiny_edge_remote_access`); an application registers its own sets the same way | the Parameters tab |

## Set up a tenant

[c8y-tenant-sync](https://github.com/reubenmiller/c8y-tenant-sync), a
go-c8y-cli extension, sets up a tenant from [tenant.yaml](tenant.yaml). It
declares everything the devices need:

| Section | What it sets up |
|---|---|
| `features` | `mqtt-service.smartrest`, `certificate-authority`, and `data-prep.ui-visibility`, which shows the Data Preparation application |
| `microservices` | `mqtt-service`, `data-prep-ctrl` (Smart Functions), `dtm` and `device-parameter` (Parameters tab) |
| `applications` | Data Preparation and Digital Twin Manager |
| `digitalTwin` | the DTM parameter sets in `dtm/` |
| `smartFunctions` | every Smart Function in `smart-functions/` |

Without `mqtt-service.smartrest` the MQTT Service drops a device as soon as
it subscribes to `devicecontrol/notifications`; the device logs -113.

Each GitHub release (after 0.10.2) carries the manifest and its files as a
bundle, `tiny-edge-tenant.tar.gz`. Install the extension once, then, with a
go-c8y-cli session for the tenant (`set-session`), apply the latest release:

```sh
c8y extensions install reubenmiller/c8y-tenant-sync
c8y tenant-sync run https://github.com/thin-edge/tiny-edge/releases/latest/download/tiny-edge-tenant.tar.gz --dry   # what it would change; changes nothing
c8y tenant-sync run https://github.com/thin-edge/tiny-edge/releases/latest/download/tiny-edge-tenant.tar.gz
```

From a checkout, `c8y tenant-sync run --dry` in this directory applies the
checkout's `tenant.yaml`.

The run enables a feature that is not active, subscribes a missing
application, deploys a Smart Function whose code, configuration or tests
differ from the deployed rule, and creates or updates a DTM definition that
differs from its file. Anything already in place is left alone, so the run
can be repeated, and it fails when an item fails. The applications are owned
by the `management` tenant: a tenant which may not subscribe them itself
reports them as skipped, and its parent tenant (or the Cumulocity provider)
subscribes them. `--only digitalTwin,smartFunctions` applies only the DTM
sets and the Smart Functions.

To keep the tenant on the release its devices run, include that release's
bundle from the tenant's own manifest. An application adds its own
parameter sets there too:

```yaml
# tenant.yaml
include:
  - github:
      repo: thin-edge/tiny-edge
      release: v<version>   # or latest
      assets: ["tiny-edge-tenant.tar.gz"]

digitalTwin:
  propertyDefinitions:
    - source:
        path: ./cumulocity/dtm   # the application's sets: every *.json in it
```

```sh
c8y tenant-sync run -f tenant.yaml --dry
```

Set up the tenant before devices connect, or reconnect them afterwards
(a restart is enough). A device sends its twin fragments (`c8y_Agent`,
`c8y_Hardware`, `remoteAccess` and every parameter set) once per
connection. Without the twin Smart Function those messages are dropped, so a
device that connected first shows no Parameters tab until it reconnects.
Measurements recover on their own because the device keeps sending them.

## Smart Functions

Each directory is a Data Preparation rule package: `data-prep.yaml` (the
topic pattern), the JavaScript, and `tests/*.yaml`. The manifest deploys all
of them. Each GitHub release also carries one `<rule>.tar.gz` per Smart
Function, which you can import in the Data Preparation UI.
`smart-functions/package.sh <out-dir>` builds the same archives from a
checkout.

To deploy a package without c8y-tenant-sync, send it as the raw request body:

```sh
cumulocity/smart-functions/package.sh /tmp/rules
curl -X PUT -H "$C8Y_HEADER" \
  -H "Content-Type: application/gzip" --data-binary @/tmp/rules/tiny-edge-twin.tar.gz \
  "${C8Y_HOST%/}/service/dataprep/v1/rules/tiny-edge-twin/deployed"
```

`C8Y_HEADER`, which `set-session` sets, holds the whole
`Authorization: ...` line; `C8Y_HEADER_AUTHORIZATION` holds only its value
and does not work as a `-H` argument. `c8y api --file` cannot send the
package: it always sends a multipart form, which the service answers with
400 "Failed to read request body". A package built with macOS tar needs
`COPYFILE_DISABLE=1`, or it carries `._*` files the service rejects;
`package.sh` sets it.

`smart-functions/test.py` runs every rule's tests in a tenant's Data
Preparation runtime (`/service/dataprep/v1/run-tests`) without deploying
anything; run it after changing a function.

**Measurements.** Known types get a Cumulocity type, one fragment and named
series with units (the device sends none):

| Topic type | Cumulocity type | Fragment: series (unit) |
|---|---|---|
| `pump` | `c8y_Pump` | `pump`: flow (l/min), pressure (bar), motorTemperature (C), speed (rpm), vibration (mm/s), runHours (h) |
| `server` | `c8y_Server` | `server`: temperature (C), humidity (%RH), pressure (hPa) |
| `agent` | `c8y_Agent` | `agent`: uptime (s) |
| `device` | `c8y_Device` | `device`: uptime (s), freeHeap (bytes), rssi (dBm) |
| `tedge_health` | `c8y_TedgeHealth` | `tedge_Health`: uptime (s), freeHeap (bytes), droppedMessages, resetCause |

Any other type — an operator can rename the Modbus one — is kept with the
type as fragment and the keys as series, rather than dropped. The device's
own timestamp is used when it sends one.

**Twin.** Every `te/device/<id>///twin/<name>` message is written unchanged
under a fragment named `<name>`, except `tedge_RemoteAccess`, which becomes
`remoteAccess`. That is what puts a parameter set's current values
(`zephyr_modbus_telemetry`, `zephyr_modbus_control`, `zephyr_opcua_telemetry`,
`zephyr_snmp_telemetry`, `tiny_edge`) on the managed object, where the Parameters tab
reads them. It replaces a rule that mapped only `tedge_RemoteAccess`; do not
deploy both.

**Events.** Every `te/device/<id>///e/<type>` message becomes an event of
type `<type>` on that device, with the payload's `text` (the type when there
is none), the device's `time` when it sent one, and any other payload field
as a fragment. Without this function no event from a device on the MQTT
Service reaches the tenant. The one the firmware sends itself is
`zephyr_Identify`: an operator at a board presses `sw0` twice, and the event
shows which cloud device it is.

**Agent and hardware.** The device sends Cumulocity's `c8y_Agent` and
`c8y_Hardware` fragments as twin messages (`te/device/<id>///twin/c8y_Agent`
and `.../twin/c8y_Hardware`), so the twin function writes them with no
mapping of their own. `c8y_Agent` names the client (`tiny-edge`), its
version, its URL, and its maintainer. `c8y_Hardware` has `model` (the board on Zephyr, the chip
on ESP-IDF, the machine type on POSIX), `revision`, and `serialNumber` (the
MAC address), each unless the application names its own. On Core MQTT the
client writes both fragments itself.

## Parameters tab (DTM)

The tenant needs the `dtm` and `device-parameter` microservices (the
manifest subscribes them), and the user registering needs `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE` and `_ADMIN`.

The manifest registers the sets in `dtm/`, and updates a definition when its
file changes: after a declaration changed, regenerate the file and run the
manifest again.

| Identifier | Declared by | Parameters |
|---|---|---|
| `zephyr_modbus_telemetry` | `modbus-server` | `interval_s` (5–3600 s), `telemetry`, `measurement_type` (≤ 31 chars) |
| `zephyr_modbus_control` | `modbus-server` | `running`, `mode` (off/auto/manual), `speed_setpoint` (0–100 %), `local_writes` (whether Modbus clients may change the three) |
| `zephyr_opcua_telemetry` | `opcua-server` | `interval_s` (5–3600 s), `telemetry` |
| `zephyr_snmp_telemetry` | `snmp-agent` | `interval_s` (5–3600 s), `telemetry` |
| `tiny_edge` | tiny-edge, every image | `log_level` (off/err/wrn/inf/dbg), `health_interval_s` (0–86400 s), `required_interval_min` (0–1440 min) |
| `tiny_edge_remote_access` | tiny-edge, images with remote access | `remote_access` |

The client's own sets start with `tiny_edge`. An application's sets start
with a prefix of its own and the application's name (the microcontroller-devices
applications use `zephyr_`), so they cannot collide with a definition someone else
registers for another device (tedge-dot, for one, registers
`zephyr_modbus_pump_control_parameters` for the same pump, written over
Modbus). Each file's `tags` name the Cumulocity device types that declare
the set. `zephyr_modbus_control.local_writes` set to false refuses *every*
Modbus write to the controls, tedge-dot's included.

The identifier is the set's name, which is also the twin fragment and the
suffix of the `c8y_ParameterUpdate_<set>` operation, so all three have to
agree. The files were printed by the firmware
(`tedge params schema <set>`, from a build with a serial shell and
`CONFIG_TEDGE_PARAMETERS_SCHEMA`), so they match what the device validates.
A device refuses a change that names a parameter it does not declare, and
the Parameters tab sends the whole set, so every field in a definition has
to be declared by every device that declares the set. That is why
`remote_access` is a set of its own, `tiny_edge_remote_access`, declared
only by images with remote access: inside the main set it made every
change to that set fail on the WROOM and the ESP32-CAM.

The tab appears on a device whose managed object carries the set's fragment,
which the twin Smart Function writes when the device connects. A change the
device refuses (out of range, unknown name) fails the operation with the
reason, and nothing is applied.

## Shell diagnostics

The `tedge-full` Modbus and agent images of the ESP32-S3-DevKitC and the QT Py
ESP32-S3 carry the shell command (`ports/zephyr/profiles/extras/shell-diagnostics.conf`)
for general checks from the device's **Shell** tab. Send `help` to see what
the device runs:

| Command | Answers |
|---|---|
| `kernel uptime`, `kernel version` | how long it has run, what it runs |
| `net iface`, `net conn` | addresses, gateway, open sockets |
| `wifi status` | SSID, RSSI, channel, security |
| `tedge params list` | the parameter sets it declares |
| `tedge diag` | the client's state and uptime |

`net ping` is not offered: Zephyr prints the replies after the command
returns, so the cloud would only ever see the `PING <host>` line.

Anything else is refused with the reason. Output travels in one SmartREST
field, so a long answer is cut. The other boards' images leave it out: on
the ESP32-C6 its ~16 KB of RAM left too little for the Wi-Fi driver and the
firmware download (DEVICES.md, "Shell diagnostics measured").

## Moving a tenant from tedge-zephyr names

Releases up to 0.4 named the client after Zephyr. A tenant set up for
them needs these changes once its devices run a newer release:

- **Smart Functions:** deploy the `tiny-edge-*` rules, then delete the
  `tedge-zephyr-*` ones. They take the same topics, so with both deployed
  every message is stored twice.
- **Parameters tab:** register `tiny_edge` and `tiny_edge_remote_access`
  (the manifest does, see "Set up a tenant"). Keep the `zephyr_tedge` definitions while
  devices on older releases remain.
- **Old fragments:** an updated device writes `tiny_edge` and
  `tiny_edge_remote_access`, and leaves the old `zephyr_tedge` fragments on
  its managed object. Remove them so the Parameters tab only offers the sets
  the device still declares:

  ```sh
  c8y inventory update --id <device> \
    --data '{"zephyr_tedge":null,"zephyr_tedge_remote_access":null}'
  ```

- **Health:** the client's service is now `tiny-edge`
  (`te/device/<id>/service/tiny-edge/status/health`). A service called
  `tedge-zephyr` on a device stays as it was last reported; delete it.
