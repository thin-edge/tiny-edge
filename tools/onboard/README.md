# tedge-onboard

Sets up a device for Cumulocity over a USB cable. It puts the device on
your Wi-Fi network, registers it in Cumulocity, and checks that it is
connected. It takes about a minute, and you don't need to type anything
technical.

## Before you start

You need:

- **The device**, with firmware that supports this tool. If you are not
  sure, try it: the tool tells you if the firmware cannot be set up this
  way.
- **A USB data cable.** Some cables only charge. If the tool cannot find
  your device, try another cable.
- **go-c8y-cli**, the Cumulocity command-line tool, logged in to your
  Cumulocity tenant. If it is not installed, the tool tells you how to
  install it. If it is not logged in, the tool helps you log in.
- **This computer on the Wi-Fi network** the device should use. Most
  devices only use 2.4 GHz networks.

## Download

Download the file for your computer from the
[tiny-edge releases](https://github.com/thin-edge/tiny-edge/releases)
(look under "Assets"):

| Computer | File |
|---|---|
| Mac with Apple silicon (M1 and later) | `tedge-onboard_<version>_darwin_arm64.tar.gz` |
| Mac with an Intel processor | `tedge-onboard_<version>_darwin_amd64.tar.gz` |
| Windows | `tedge-onboard_<version>_windows_amd64.zip` |
| Linux | `tedge-onboard_<version>_linux_amd64.tar.gz` |

Open the downloaded file to unpack it.

**On a Mac**, the first time you start it, macOS may say it "cannot be
opened because the developer cannot be verified". Open **System Settings**,
then **Privacy & Security**, and select **Open Anyway** next to the
message about `tedge-onboard`.

**On Linux**, your user needs permission to use USB serial ports. Most
distributions give it to the `dialout` group (`uucp` on Arch Linux). To
check, run `groups` in Terminal. If the group isn't listed, add yourself
to it, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"
```

The tool reads the Wi-Fi network through NetworkManager (`nmcli`). On a
computer without it, the tool asks you for the network and its password.

**On Windows**, devices with a built-in USB port, such as the ESP32-C6
and ESP32-S3, work without extra drivers on Windows 10 and later. Boards
with a separate USB chip, which includes most classic ESP32 boards, may
need its driver first: the
[CP210x driver](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers)
or the [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html).
The chip's name is usually printed on it, next to the USB socket.

## Set up a device

1. Plug the device into your computer with the USB cable.
2. Start `tedge-onboard`:
   - **Mac or Linux:** open Terminal, type `./tedge-onboard` in the
     folder where you unpacked it, and press Enter.
   - **Windows:** double-click `tedge-onboard.exe`.
3. Answer the questions. Each one has the most likely answer already
   selected, so you can usually just press Enter. Use the arrow keys to
   choose something else.

The tool:

1. checks your Cumulocity login;
2. finds the device (if several are plugged in, it lets you choose, and
   can make one blink so you can tell them apart);
3. offers this computer's Wi-Fi network;
4. asks for the device's name in Cumulocity;
5. shows a summary and asks you to confirm;
6. sets the device up, and shows a link to it in Cumulocity.

At the end you can set up another device.

## What the tool reads from your computer

- **The Wi-Fi network and its password.** The tool reads the password
  your computer already has stored, so you don't have to type it. On a
  Mac, macOS asks you to allow this: enter your Mac's password and select
  **Allow**. If you would rather type the Wi-Fi password, select **Deny**,
  and the tool asks you for it. The password goes only to the device,
  over the cable. The tool never shows it or saves it.
- **Your Cumulocity login, through go-c8y-cli.** The tool has no login of
  its own and stores no Cumulocity password.

The tool registers the device in Cumulocity with a new one-time password,
which the device uses once to get its certificate. It registers the device
with the device type its firmware reports, so firmware offered for that type
shows up in the device's **Firmware** tab. A device whose firmware predates
this gets the type `thin-edge.io`.

## If something goes wrong

| What you see | What to do |
|---|---|
| No device was found | Check the cable is a data cable, and plug it in again. Try another USB port. On Linux, check that your user can use serial ports. On Windows, check that the board's USB driver is installed (see [Download](#download)). |
| The firmware cannot be set up this way | The device needs firmware with tiny-edge's provisioning library. |
| The device could not join the Wi-Fi network | Check the password. Make sure the network is 2.4 GHz and the device is in range. |
| The device did not connect in time | Check that the Wi-Fi network reaches the internet. The device keeps trying, so it may still appear in Cumulocity a few minutes later. |
| go-c8y-cli is not logged in | Choose **Choose or create a Cumulocity login** when the tool asks. |
| The device is already set up | Choose **Yes** to set it up again. It gets a new certificate, and keeps its history in Cumulocity. |

## For scripts

With `--yes`, the tool asks no questions and fails where it would have
asked one, saying which option answers it.

```sh
tedge-onboard --yes --port /dev/ttyUSB0 --wifi-ssid lab-net --wifi-password-stdin < pw.txt
tedge-onboard --yes --skip-wifi --reonboard --json
```

| Option | |
|---|---|
| `--port` | The device's serial port (found automatically otherwise) |
| `--wifi-ssid` | The Wi-Fi network (default: this computer's) |
| `--wifi-password-stdin` | Read the Wi-Fi password from standard input |
| `--skip-wifi` | Leave the device's Wi-Fi as it is |
| `--name` | The device's name in Cumulocity (default: its ID) |
| `--reonboard` | Set up a device that is already in Cumulocity again |
| `--timeout` | How long to wait for the device to connect (default 3m) |
| `--json` | Print the result as JSON |
| `--yes` | Ask no questions |

`ACCESSIBLE=1` switches the questions to plain prompts that work with
screen readers.

## How it works

The tool speaks [Improv Wi-Fi](https://www.improv-wifi.com) to the device
over its serial console, plus a few tiny-edge commands for the Cumulocity
details. On the device, tiny-edge's provisioning library answers them
(`include/tedge/provisioning.h`). The design is in
`openspec/changes/serial-onboarding/`.

Checks for a real board:

```sh
go test -tags hardware ./internal/device -run Hardware -v
TEDGE_ONBOARD_WIFI_TEST=1 go test -tags hardware ./internal/device -run HardwareWiFi -v
```
