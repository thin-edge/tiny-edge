#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Set up the Cumulocity tenant of the active go-c8y-cli session for tiny-edge
# devices, in one go:
#
#   1. check the tenant features the devices need (mqtt-service.smartrest,
#      certificate-authority);
#   2. deploy every Smart Function under cumulocity/smart-functions/;
#   3. register the DTM parameter sets (cumulocity/dtm/register.sh).
#
#   cumulocity/setup-tenant.sh [options]
#
# Options:
#   --dry-run              read the tenant and say what would change; change nothing
#   --enable-features      enable a missing tenant feature instead of stopping
#   --replace              re-register DTM sets that already exist
#   --dtm-dir <dir>        also register the *.json sets in <dir> (repeatable),
#                          e.g. an application's own cumulocity/dtm
#   --no-smart-functions   skip step 2
#   --no-dtm               skip tiny-edge's own DTM sets (--dtm-dir still runs)
#
# Start a session first (set-session), which sets C8Y_HOST and C8Y_HEADER.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

dry=0
enable=0
replace=()
deploy_rules=1
own_dtm=1
dtm_dirs=()
while [[ $# -gt 0 ]]; do
	case $1 in
	--dry-run) dry=1 ;;
	--enable-features) enable=1 ;;
	--replace) replace=(--replace) ;;
	--dtm-dir) dtm_dirs+=("$(cd "${2:?--dtm-dir needs a directory}" && pwd)"); shift ;;
	--no-smart-functions) deploy_rules=0 ;;
	--no-dtm) own_dtm=0 ;;
	-h | --help) sed -n '3,24s/^# \{0,1\}//p' "$0"; exit 0 ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
[[ $own_dtm -eq 1 ]] && dtm_dirs=("$here/dtm" "${dtm_dirs[@]+"${dtm_dirs[@]}"}")
[[ $dry -eq 1 ]] && dtm_flags=(--dry-run) || dtm_flags=()

# --- session --------------------------------------------------------------
# c8y reads stdin when it is not a terminal, so every call gets < /dev/null
# or a script hangs.
if [[ -z ${C8Y_HOST:-} || -z ${C8Y_HEADER:-} ]]; then
	echo "No go-c8y-cli session: run set-session (it sets C8Y_HOST and C8Y_HEADER), then run this again." >&2
	exit 1
fi
host=${C8Y_HOST%/}
[[ $host == *://* ]] || host="https://$host"
tenant=$(c8y currenttenant get --select name --output csv < /dev/null) || {
	echo "The go-c8y-cli session for $host does not work (c8y currenttenant get failed)." >&2
	exit 1
}
if [[ $dry -eq 1 ]]; then
	echo "Dry run against $host (tenant $tenant): nothing will be changed."
else
	echo "Setting up $host (tenant $tenant) for tiny-edge."
fi

# --- 1. tenant features -----------------------------------------------------
# Without mqtt-service.smartrest the MQTT Service drops a device as soon as
# it subscribes to devicecontrol/notifications (the device logs -113).
feature_active() {
	c8y features get --key "$1" --raw < /dev/null 2>/dev/null |
		python3 -c 'import json,sys; print("yes" if json.load(sys.stdin).get("active") else "no")' 2>/dev/null ||
		echo no
}
missing=0
for f in mqtt-service.smartrest certificate-authority; do
	if [[ $(feature_active "$f") == yes ]]; then
		echo "feature $f: active"
	elif [[ $enable -eq 1 && $dry -eq 0 ]]; then
		if c8y features enable --key "$f" --force < /dev/null >/dev/null; then
			echo "feature $f: enabled"
		else
			echo "feature $f: could not enable it; ask whoever manages the tenant to enable it." >&2
			missing=1
		fi
	elif [[ $enable -eq 1 ]]; then
		echo "feature $f: not active, would enable it"
	else
		echo "feature $f: not active. Enable it with:  c8y features enable --key $f" >&2
		missing=1
	fi
done
if [[ $missing -eq 1 ]]; then
	if [[ $dry -eq 1 ]]; then
		echo "(a real run stops here; pass --enable-features to have the script enable them)" >&2
	else
		echo "Stopping: enable the features above, or run again with --enable-features." >&2
		exit 1
	fi
fi

failed=0

# --- 2. Smart Functions -----------------------------------------------------
# The package has to be the raw request body: `c8y api --file` always sends
# multipart, which the service answers with 400 "Failed to read request
# body". So curl it, with C8Y_HEADER (the whole "Authorization: ..." line).
if [[ $deploy_rules -eq 1 ]]; then
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT
	pkgs=$("$here/smart-functions/package.sh" "$work")
	while read -r pkg; do
		name=$(basename "$pkg" .tar.gz)
		url="$host/service/dataprep/v1/rules/$name/deployed"
		if [[ $dry -eq 1 ]]; then
			echo "smart function $name: would PUT $(wc -c < "$pkg" | tr -d ' ') bytes to $url"
			continue
		fi
		status=$(curl -sS -o "$work/$name.out" -w '%{http_code}' -X PUT \
			-H "$C8Y_HEADER" -H "Content-Type: application/gzip" \
			--data-binary "@$pkg" "$url") || status=000  # curl printed why
		if [[ $status == 200 ]]; then
			echo "smart function $name: deployed (HTTP 200)"
		else
			echo "smart function $name: HTTP $status: $(head -c 500 "$work/$name.out" 2>/dev/null)" >&2
			failed=1
		fi
	done <<< "$pkgs"
fi

# --- 3. DTM parameter sets ----------------------------------------------------
for d in "${dtm_dirs[@]+"${dtm_dirs[@]}"}"; do
	echo "DTM sets in $d:"
	"$here/dtm/register.sh" "${replace[@]+"${replace[@]}"}" "${dtm_flags[@]+"${dtm_flags[@]}"}" --dir "$d" || failed=1
done

if [[ $failed -eq 1 ]]; then
	echo "Done, with failures (see above)." >&2
	exit 1
fi
echo "Done."
if [[ $deploy_rules -eq 1 && $dry -eq 0 ]]; then
	echo "Devices that were already connected send their twin fragments again only when they reconnect: restart them to get the Parameters tab."
fi
