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

With a go-c8y-cli session for the tenant (`set-session`), one command does
everything below:

```sh
cumulocity/setup-tenant.sh --dry-run   # what it would change; changes nothing
cumulocity/setup-tenant.sh
```

It prints the host it is about to change, then:

1. Checks that the tenant features `mqtt-service.smartrest` and
   `certificate-authority` are active, and stops if one is not, naming the
   command that enables it (`c8y features enable --key <feature>`).
   `--enable-features` enables them instead. Without
   `mqtt-service.smartrest` the MQTT Service drops a device as soon as it
   subscribes to `devicecontrol/notifications`; the device logs -113.
2. Deploys every Smart Function under `smart-functions/`, and reports each
   rule's HTTP status. Any status other than 200 fails the script.
3. Registers the DTM parameter sets with `dtm/register.sh`. A set that is
   already there is left alone; `--replace` re-registers it.

An application registers its own parameter sets with `--dtm-dir`, alone or
with the rest:

```sh
path/to/tiny-edge/cumulocity/setup-tenant.sh --dtm-dir cumulocity/dtm
path/to/tiny-edge/cumulocity/dtm/register.sh --dir cumulocity/dtm   # only the application's sets
```

`--no-smart-functions` and `--no-dtm` skip steps 2 and 3 (`--dtm-dir` sets
are still registered). `--help` lists every option.

## Smart Functions

Each directory is a Data Preparation rule package: `data-prep.yaml` (the
topic pattern), the JavaScript, and `tests/*.yaml`. `setup-tenant.sh`
deploys all of them. Each GitHub release also carries one
`<rule>.tar.gz` per Smart Function, which you can import in the Data
Preparation UI or deploy as it is. `smart-functions/package.sh <out-dir>`
builds the same archives from a checkout.

To deploy one package by hand, send it as the raw request body:

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

The tenant needs the `dtm` and `device-parameter` microservices, and the
user registering needs `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE` and `_ADMIN`.

```sh
cumulocity/dtm/register.sh             # registers every set not yet there
cumulocity/dtm/register.sh --replace zephyr_modbus_telemetry   # after a declaration changed
cumulocity/dtm/register.sh --dry-run   # what it would register; changes nothing
```

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
  (`cumulocity/dtm/register.sh`). Keep the `zephyr_tedge` definitions while
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
