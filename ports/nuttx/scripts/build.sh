#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build tiny-edge for NuttX on one board, in the container from
# ports/nuttx/docker:
#
#   ports/nuttx/scripts/build.sh <board> [--agent] [--simple-boot]
#                                [--version <x.y.z>] [--extra <fragment>]...
#                                [--out <dir>] [--fresh] [--cmake]
#
# <board> is esp32s3-devkit or esp32c6-devkitc. The script clones NuttX and
# nuttx-apps at the pinned release into $NUTTX_WS (default
# ports/nuttx/build-nuttx) once, adds this port to nuttx-apps as an external
# application, configures the board's "wifi" configuration with
# configs/common.conf and configs/<board>.conf on top, and builds. The image
# is copied to <dir> (default $NUTTX_WS/out/<board>). --agent adds
# configs/agent.conf: the agent starts instead of the shell, with set-up
# over USB. The image boots through MCUboot (configs/<board>.mcuboot.conf), for
# firmware update with rollback; --simple-boot leaves MCUboot out, for a
# development image flashed at 0x0. --version is the firmware version the
# agent reports and the MCUboot header carries (default: tiny-edge's
# VERSION). --extra adds a configuration fragment of your own, after the
# port's (configs/small.conf is the reduced set CI builds). --fresh starts
# the configuration again (make distclean). --cmake builds with NuttX's
# CMake build instead of Make (ports/nuttx/CMakeLists.txt), in a worktree of
# the same NuttX release ($NUTTX_WS/nuttx-cmake), since NuttX refuses a
# CMake build in a tree configured for Make, and the build directory
# $NUTTX_WS/build-cmake/<board>; it writes the same files.
#
# What it writes to <dir>:
#
#   nuttx.bin              the image to flash: at 0x0 with --simple-boot, or
#                          at the primary slot (0x20000) after MCUboot,
#                          padded and confirmed
#   mcuboot-<chip>.bin     MCUboot, to flash at 0x0
#   nuttx-update.bin       the same image unpadded and not confirmed: what
#                          Cumulocity offers as a firmware update
set -euo pipefail

NUTTX_RELEASE=nuttx-13.1.0

port=$(cd "$(dirname "$0")/.." && pwd)
board=${1:?usage: build.sh <board> [--out dir] [--fresh]}
shift
ws=${NUTTX_WS:-$port/build-nuttx}
out=
fresh=0
mcuboot=1
cmake=0
version=$(cat "$port/../../VERSION")
extra=()
fragments=("$port/configs/common.conf" "$port/configs/$board.conf")
while [[ $# -gt 0 ]]; do
	case $1 in
	--out) out=$2; shift 2 ;;
	--fresh) fresh=1; shift ;;
	--agent) fragments+=("$port/configs/agent.conf"); shift ;;
	--simple-boot) mcuboot=0; shift ;;
	--version) version=$2; shift 2 ;;
	--extra) extra+=("$2"); shift 2 ;;
	--cmake) cmake=1; shift ;;
	*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
done
out=${out:-$ws/out/$board}
[[ $mcuboot == 1 ]] && fragments+=("$port/configs/$board.mcuboot.conf")
fragments+=(${extra[@]+"${extra[@]}"})
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+].*)?$ ]] ||
	{ echo "--version must be MAJOR.MINOR.PATCH, not \"$version\"" >&2; exit 1; }
[[ -f $port/configs/$board.conf ]] ||
	{ echo "no configuration for $board in $port/configs" >&2; exit 1; }

mkdir -p "$ws"
for repo in nuttx nuttx-apps; do
	dir=$ws/${repo#nuttx-}
	[[ $repo == nuttx ]] && dir=$ws/nuttx
	if [[ ! -d $dir ]]; then
		git -c advice.detachedHead=false clone -q --depth 1 \
			--branch "$NUTTX_RELEASE" "https://github.com/apache/$repo.git" "$dir"
	fi
done

# The NuttX tree to build: the clone, or for --cmake a worktree of it.
nx=$ws/nuttx
if [[ $cmake == 1 ]]; then
	nx=$ws/nuttx-cmake
	[[ -d $nx ]] || git -C "$ws/nuttx" worktree add -q --detach "$nx" HEAD
fi

# Three changes to NuttX's driver for the ESP chips' USB serial port, which
# has no options for any of them:
#
# - It receives into a fixed 64-byte buffer and discards what does not fit,
#   so a set-up frame longer than 63 bytes arrives cut (tedge-onboard's
#   onboard command is about 90). The receive buffer is made 512 bytes.
# - Its transmit buffer is 64 bytes too. The agent writes its log lines
#   without blocking (see below), so a burst that does not fit is dropped,
#   even with a program reading the port. It is made 2 KB.
# - Kernel messages (syslog, and board_reset()'s "reboot status" line) wait
#   for room in the port's FIFO with no limit. With no program on the
#   computer reading the port, the FIFO never empties, so a restart from
#   the cloud hung. The wait is limited, as in ESP-IDF's driver: after one
#   wait times out, characters are dropped at once until the FIFO has room.
for f in "$nx/arch/risc-v/src/common/espressif/esp_usbserial.c" \
	 "$nx/arch/xtensa/src/esp32s3/esp32s3_usbserial.c"; do
	perl -0pi -e 's/static char g_rxbuffer\[\w+\];/static char g_rxbuffer[512];/;
		s/(\.recv\s*=\s*\{\s*\.size\s*=\s*)(?!sizeof)\w+/${1}sizeof(g_rxbuffer)/;
		s/static char g_txbuffer\[\w+\];/static char g_txbuffer[2048];/;
		s/(\.xmit\s*=\s*\{\s*\.size\s*=\s*)(?!sizeof)\w+/${1}sizeof(g_txbuffer)/;
		s{\n  while \(!(\w+)\(&g_uart_usbserial\)\);\n}{
  /* tedge: bounded wait, no host reading drops output */
  static bool host_absent;
  unsigned int spins = host_absent ? 0 : 400000;

  while (!$1(&g_uart_usbserial))
    {
      if (spins-- == 0)
        {
          host_absent = true;
          return;
        }
    }

  host_absent = false;
}' "$f"
	grep -q 'g_rxbuffer\[512\]' "$f" && grep -q 'sizeof(g_rxbuffer)' "$f" &&
		grep -q 'g_txbuffer\[2048\]' "$f" && grep -q 'sizeof(g_txbuffer)' "$f" &&
		grep -q 'tedge: bounded wait' "$f" ||
		{ echo "could not patch the USB serial driver in $f" >&2; exit 1; }
done

# The ESP32-S3's MCUboot build names its console UART only when the console
# is a UART; with the USB serial console the bootloader does not compile.
# The ESP32-C6's rules use UART 0 for that case, and so does this.
f=$nx/arch/xtensa/src/esp32s3/Bootloader.mk
if ! grep -q 'CONFIG_ESP32S3_USBSERIAL),$(call cfg_val,CONFIG_ESP_CONSOLE_UART_NUM' "$f"; then
	perl -pi -e 'print "\t\t\$(if \$(CONFIG_ESP32S3_USBSERIAL),\$(call cfg_val,CONFIG_ESP_CONSOLE_UART_NUM,0)) \\\n" if /CONFIG_UART0_SERIAL_CONSOLE\),\$\(call cfg_val,CONFIG_ESP_CONSOLE_UART_NUM,0\)/' "$f"
fi
grep -q 'CONFIG_ESP32S3_USBSERIAL),$(call cfg_val,CONFIG_ESP_CONSOLE_UART_NUM,0)' "$f" ||
	{ echo "could not patch $f" >&2; exit 1; }

# nuttx-apps finds applications through each top-level directory's
# Make.defs and Kconfig. The files here include this port's own, so objects
# are built in the NuttX tree and not in the checkout.
ext=$ws/apps/external
mkdir -p "$ext/tiny_edge"
cat > "$ext/Make.defs" <<'MK'
include $(wildcard $(APPDIR)/external/*/Make.defs)
MK
cat > "$ext/Makefile" <<'MK'
MENUDESC = "External"
include $(APPDIR)/Directory.mk
MK
printf 'source "%s/Kconfig"\n' "$port" > "$ext/tiny_edge/Kconfig"
printf 'include %s/Make.defs\n' "$port" > "$ext/tiny_edge/Make.defs"
printf 'include %s/Makefile\n' "$port" > "$ext/tiny_edge/Makefile"
# The same for NuttX's CMake build.
printf 'nuttx_add_subdirectory()\nnuttx_generate_kconfig(MENUDESC "External")\n' \
	> "$ext/CMakeLists.txt"
printf 'include(%s/CMakeLists.txt)\n' "$port" > "$ext/tiny_edge/CMakeLists.txt"

cd "$nx"
# Always from the board's configuration, so a fragment left out this time
# (--agent) does not linger from the last build.
defconfig=$(ls boards/*/*/"$board"/configs/wifi/defconfig)
if [[ $cmake == 1 ]]; then
	bd=$ws/build-cmake/$board
	cfg=$bd/.config
	if [[ $fresh == 1 || ! -f $bd/.config.board ]]; then
		rm -rf "$bd"
		cmake -B "$bd" -DBOARD_CONFIG="$board:wifi" \
			-DNUTTX_APPS_DIR="$ws/apps" -GNinja >/dev/null
		# The board's full configuration: CMake reads .config before
		# olddefconfig runs, so the bare defconfig, which lacks the
		# chip's derived options, cannot stand in for it as with Make.
		cp "$cfg" "$bd/.config.board"
	fi
	cp "$bd/.config.board" "$cfg"
else
	cfg=$nx/.config
	if [[ $fresh == 1 || ! -f .config ]] ||
	   ! grep -q "^CONFIG_ARCH_BOARD=\"$board\"" .config; then
		make distclean >/dev/null 2>&1 || true
		./tools/configure.sh -l "$board:wifi" >/dev/null
	else
		cp "$defconfig" .config
	fi
fi
# The version, as a fragment of its own so the check below covers it.
mkdir -p "$out"
version_conf=$out/version.conf
echo "CONFIG_TINY_EDGE_FIRMWARE_VERSION=\"$version\"" > "$version_conf"
fragments+=("$version_conf")
cat "${fragments[@]}" >> "$cfg"
if [[ $cmake == 1 ]]; then
	cmake --build "$bd" -t olddefconfig >/dev/null
else
	make olddefconfig >/dev/null
fi

# Every option the fragments set must have taken: a dependency Kconfig
# could not meet drops it without a word. The exception is an option of a
# feature that a fragment turns off on purpose, such as the shell command's
# allow-list (CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST in configs/agent.conf)
# when configs/small.conf turns CONFIG_TEDGE_SHELL_COMMAND off. Only the
# last setting of each option counts, as for Kconfig: a later fragment
# (--extra) may change what an earlier one set.
settings() {
	cat "${fragments[@]}" | awk '
		/^CONFIG_[A-Za-z0-9_]*=/ { sym = $0; sub(/=.*/, "", sym) }
		/^# CONFIG_[A-Za-z0-9_]* is not set$/ { sym = $2 }
		sym != "" { last[sym] = $0; order[sym] = NR; sym = "" }
		END { for (s in last) print order[s] "\t" last[s] }' |
		sort -n | cut -f2-
}
disabled=$(settings |
	sed -n 's/^# \(CONFIG_[A-Za-z0-9_]*\) is not set$/\1/p')
turned_off() {
	local sym=$1 off
	for off in $disabled; do
		[[ $sym == "$off"_* ]] && return 0
	done
	return 1
}
missing=0
while IFS= read -r line; do
	case $line in
	CONFIG_*=*) grep -qxF "$line" "$cfg" || turned_off "${line%%=*}" ||
		{ echo "not applied: $line" >&2; missing=1; } ;;
	"# CONFIG_"*" is not set")
		# Off, or gone with an option it depends on.
		sym=${line#\# }; sym=${sym%% *}
		! grep -q "^$sym=" "$cfg" ||
		{ echo "not applied: $line" >&2; missing=1; } ;;
	esac
done < <(settings)
[[ $missing == 0 ]] || exit 1

# nuttx-apps does not rebuild an application's objects when the
# configuration changes, so objects built for another configuration would
# be linked in, with other sizes for the system's types (a struct stat from
# another configuration gave the client a wrong file size). Clean whenever
# the configuration differs from the last build's.
chip=$(sed -n 's/^CONFIG_ARCH_CHIP="\(.*\)"$/\1/p' "$cfg")
if [[ $cmake == 1 ]]; then
	# NuttX's image step merges MCUboot from the NuttX tree, so the
	# bootloader is built first. CMake tracks the configuration itself.
	[[ $mcuboot == 1 ]] && cmake --build "$bd" -t bootloader >/dev/null
	cmake --build "$bd"
	img=$bd
else
	if ! cmp -s .config .config.built; then
		make clean >/dev/null 2>&1 || true
	fi
	# With MCUboot, NuttX signs nuttx.bin padded to the slot, so the build
	# fails here if the image does not fit.
	make -j"$(nproc)"
	cp .config .config.built
	[[ $mcuboot == 1 ]] && make bootloader >/dev/null
	img=$nx
fi
cp "$img/nuttx.bin" "$out/"
if [[ $mcuboot == 1 ]]; then
	cp "$nx/mcuboot-$chip.bin" "$out/"
	# Signed here rather than by NuttX, which signs version 1.0.0 on the C6
	# and 0 on the S3. The MCUboot header holds MAJOR.MINOR.PATCH only. The
	# option names differ by chip (CONFIG_ESPRESSIF_* or CONFIG_ESP32S3_*).
	slot=$(sed -nE 's/^CONFIG_(ESPRESSIF|ESP32S3)_OTA_SLOT_SIZE=//p' "$cfg")
	hdr=$(sed -nE 's/^CONFIG_(ESPRESSIF|ESP32S3)_APP_MCUBOOT_HEADER_SIZE=//p' "$cfg")
	sign=(--align 4 -v "${version%%[-+]*}" -s auto -H "$hdr" --pad-header -S "$slot")
	# To flash: padded to the slot and confirmed. imgtool fails if the
	# image does not fit.
	imgtool sign "${sign[@]}" --pad --confirm "$img/nuttx.hex" "$out/nuttx.bin" >/dev/null
	# The update: neither padded nor confirmed, so MCUboot boots it once on
	# trial and swaps back unless the client confirms it.
	imgtool sign "${sign[@]}" "$img/nuttx.hex" "$out/nuttx-update.bin" >/dev/null
	printf '%s\n' "$out/mcuboot-$chip.bin" "$out/nuttx-update.bin"
fi
echo "$out/nuttx.bin"
