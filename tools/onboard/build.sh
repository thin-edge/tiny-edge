#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build tedge-onboard for each platform into dist/, one archive each:
#
#   tools/onboard/build.sh <version> linux/amd64 windows/arm64 ...
#
# macOS needs cgo (the USB port list comes from IOKit), so build darwin/*
# on macOS. Linux and Windows build anywhere, without cgo.
set -euo pipefail
cd "$(dirname "$0")"
version=$1
shift
mkdir -p dist
for target in "$@"; do
	goos=${target%/*}
	goarch=${target#*/}
	name=tedge-onboard
	[ "$goos" = windows ] && name=tedge-onboard.exe
	cgo=0
	[ "$goos" = darwin ] && cgo=1
	out=dist/tedge-onboard_${version}_${goos}_${goarch}
	rm -rf "$out"
	mkdir -p "$out"
	CGO_ENABLED=$cgo GOOS=$goos GOARCH=$goarch \
		go build -trimpath -ldflags "-s -w -X main.version=$version" \
		-o "$out/$name" .
	cp README.md "$out/"
	if [ "$goos" = windows ]; then
		(cd dist && zip -qr "$(basename "$out").zip" "$(basename "$out")")
	else
		tar -C dist -czf "$out.tar.gz" "$(basename "$out")"
	fi
	rm -rf "$out"
	echo "built $out"
done
