#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Compiles every file of the core with the host's C compiler and C library,
# against a port that has only types (include/), with every feature on.
# It proves the core needs nothing from an RTOS: a Zephyr header, macro or
# libc extension it relied on would fail here even when the portability
# check's patterns miss it.
#
#   tests/host-compile/run.sh [include-dir...]
#
# The core parses certificates with mbedTLS, so its headers are needed: pass
# their directories (mbedTLS 4 splits them between mbedtls and
# tf-psa-crypto), or have them installed where the compiler looks.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cc=${CC:-cc}
extra=()
for dir in "$@"; do
	extra+=(-I "$dir")
done

failed=0
for src in "$root"/core/src/*.c; do
	if ! "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra \
		-Wno-unused-parameter -Wno-sign-compare -Werror \
		-DTEDGE_VERSION_STRING='"0.0.0"' \
		-I "$here/include" -I "$root/include" -I "$root/core/src" \
		${extra[@]+"${extra[@]}"} -c "$src" -o /dev/null; then
		echo "FAIL $(basename "$src")"
		failed=1
	fi
done
[ $failed -eq 0 ] && echo "every core file compiles without an RTOS"
exit $failed
