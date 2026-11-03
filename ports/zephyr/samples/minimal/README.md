# tiny-edge minimal sample

The smallest application that includes the client. It prints the client's
version, brings up the network, and starts the client. It uses nothing outside
Zephyr and this module, so a successful build shows the module stands on its
own.

Build it with one of the client's profiles (`ports/zephyr/profiles/`), which
picks the features and the mbedTLS options. `boards/<board>.conf` adds the
board's network and settings storage:

```sh
west build -b native_sim/native/64 ports/zephyr/samples/minimal -- \
  -DEXTRA_CONF_FILE=$PWD/ports/zephyr/profiles/minimal.conf
./build/zephyr/zephyr.exe
```

On a board, add the board's sizing after the profile, and for a profile with
firmware update build MCUboot too:

```sh
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore ports/zephyr/samples/minimal -- \
  -DSB_CONFIG_BOOTLOADER_MCUBOOT=y \
  "-DEXTRA_CONF_FILE=$PWD/ports/zephyr/profiles/full.conf;$PWD/ports/zephyr/profiles/boards/esp32c6-devkitc.conf" \
  -DCONFIG_TEDGE_SAMPLE_WIFI_SSID=\"my-ssid\" -DCONFIG_TEDGE_SAMPLE_WIFI_PSK=\"my-psk\"
```

## In CI

`sample.yaml` lists each profile on each board. CI builds them all with
twister, warnings as errors, and runs the native_sim ones:

```sh
west twister -T ports/zephyr/samples --integration
```

native_sim has no MCUboot and no shell, so firmware update, crash dumps and
the shell command drop out there. The ESP32-C6 and ESP32-S3-DevKitC builds
carry every feature.
