#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build one Data Preparation package (<rule>.tar.gz) per Smart Function, in
# the form the Data Preparation UI imports and
# PUT /service/dataprep/v1/rules/<rule>/deployed accepts.
#
#   cumulocity/smart-functions/package.sh <out-dir> [rule-dir...]   # default: every rule here
#
# A package holds data-prep.yaml, the JavaScript it names in
# smartFunctionFile, and tests/. Prints the path of each archive it writes.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
[[ $# -ge 1 ]] || { echo "usage: $0 <out-dir> [rule-dir...]" >&2; exit 2; }
mkdir -p "$1"
out=$(cd "$1" && pwd)
shift
rules=("$@")
if [[ ${#rules[@]} -eq 0 ]]; then
	for d in "$here"/*/; do
		[[ -f $d/data-prep.yaml ]] && rules+=("${d%/}")
	done
fi
for dir in "${rules[@]}"; do
	dir=${dir%/}
	name=$(basename "$dir")
	js=$(sed -n 's/^smartFunctionFile:[[:space:]]*["'\'']\{0,1\}\([^"'\'' ]*\).*/\1/p' "$dir/data-prep.yaml")
	if [[ -z $js || ! -f $dir/$js ]]; then
		echo "$name: data-prep.yaml names no smartFunctionFile that exists (got '${js}')" >&2
		exit 1
	fi
	files=(data-prep.yaml "$js")
	[[ -d $dir/tests ]] && files+=(tests)
	# COPYFILE_DISABLE: macOS tar otherwise adds ._* files the service rejects.
	COPYFILE_DISABLE=1 tar czf "$out/$name.tar.gz" -C "$dir" "${files[@]}"
	echo "$out/$name.tar.gz"
done
