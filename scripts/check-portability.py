#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the boundary between the portable core and the RTOS ports.

Rules:

1. core/ and include/tedge/tedge.h use no RTOS API: no RTOS or vendor SDK
   headers and no Kconfig symbols (CONFIG_*). They are the code every port
   shares, so they may only use C99, the client's own headers and the port
   interface.
2. A port never reaches into another port (ports/<a> must not name ports/<b>).
3. Nothing reaches outside the repository with "../" paths.

The core was extracted from the Zephyr-only client, so some of its files
still break rule 1. They are listed in KNOWN_RTOS_BOUND. The list may only
shrink: a listed file that has become clean fails the check, so whoever
cleaned it also takes it off the list.

Documentation (*.md) is not checked.

Exit status is 0 when clean, 1 when any rule is broken.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Core files that still use Zephyr directly. Remove an entry when its file
# moves its RTOS calls behind the port interface. Never add one.
KNOWN_RTOS_BOUND = {
    "core/src/tedge_backoff.c",
    "core/src/tedge_bootstrap.c",
    "core/src/tedge_c8y.c",
    "core/src/tedge_cert_renew.c",
    "core/src/tedge_core.c",
    "core/src/tedge_enroll.c",
    "core/src/tedge_firmware.c",
    "core/src/tedge_health.c",
    "core/src/tedge_http_download.c",
    "core/src/tedge_http_upload.c",
    "core/src/tedge_internal.h",
    "core/src/tedge_log_upload.c",
    "core/src/tedge_op_json.c",
    "core/src/tedge_parameters.c",
    "core/src/tedge_ra_target.c",
    "core/src/tedge_remote_access.c",
    "core/src/tedge_self_params.c",
    "core/src/tedge_shell_allow_list.c",
    "core/src/tedge_telemetry.c",
    "include/tedge/tedge.h",
}

# Headers of the RTOSes and vendor SDKs the client runs on or may run on.
RTOS_HEADER = re.compile(
    r"#\s*include\s*[<\"]"
    r"(?:zephyr/|FreeRTOS\.h|task\.h|semphr\.h|queue\.h|tx_api\.h|nx_api\.h"
    r"|vxWorks\.h|sys/neutrino\.h|esp_[\w/]*\.h|freertos/)"
)
KCONFIG = re.compile(r"\bCONFIG_\w+")
PORT_REF = re.compile(r"\bports/(\w[\w-]*)")
RELATIVE = re.compile(r"(?:\.\./)+[^\s\"'()<>;]*")

CODE_SUFFIXES = {
    ".c", ".h", ".cmake", ".conf", ".yml", ".yaml", ".overlay", ".dts",
    ".dtsi", ".sh", ".py", ".ld",
}
CODE_NAMES = {"CMakeLists.txt", "Kconfig"}
SKIP_DIRS = {".git", "build", "openspec"}


def is_code(path: Path) -> bool:
    return path.name in CODE_NAMES or path.name.startswith("Kconfig") \
        or path.suffix in CODE_SUFFIXES


def is_portable(rel: str) -> bool:
    return rel.startswith("core/") or rel == "include/tedge/tedge.h"


def rtos_bound(text: str) -> bool:
    return bool(RTOS_HEADER.search(text) or KCONFIG.search(text))


def check_file(path: Path) -> list[str]:
    rel = path.relative_to(ROOT).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return []
    problems = []

    if is_portable(rel) and rel not in KNOWN_RTOS_BOUND:
        for lineno, line in enumerate(text.splitlines(), start=1):
            if RTOS_HEADER.search(line) or KCONFIG.search(line):
                problems.append(f"{rel}:{lineno}: RTOS API in portable code: "
                                f"{line.strip()}")

    if rel.startswith("ports/"):
        own = rel.split("/")[1]
        for lineno, line in enumerate(text.splitlines(), start=1):
            for match in PORT_REF.finditer(line):
                if match.group(1) != own:
                    problems.append(f"{rel}:{lineno}: port '{own}' refers to "
                                    f"port '{match.group(1)}'")

    for lineno, line in enumerate(text.splitlines(), start=1):
        for match in RELATIVE.finditer(line):
            target = (path.parent / match.group(0)).resolve()
            if ROOT not in target.parents and target != ROOT:
                problems.append(f"{rel}:{lineno}: path leaves the repository: "
                                f"{match.group(0)}")
    return problems


def main() -> int:
    problems = []
    for path in sorted(ROOT.rglob("*")):
        parts = path.relative_to(ROOT).parts
        if not path.is_file() or not is_code(path) or SKIP_DIRS & set(parts) \
                or parts[0].startswith("build"):
            continue
        problems.extend(check_file(path))

    for rel in sorted(KNOWN_RTOS_BOUND):
        path = ROOT / rel
        if not path.exists():
            problems.append(f"{rel}: listed in KNOWN_RTOS_BOUND but gone; "
                            "remove the entry")
        elif not rtos_bound(path.read_text(encoding="utf-8")):
            problems.append(f"{rel}: now portable; remove it from "
                            "KNOWN_RTOS_BOUND")

    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"core is portable apart from {len(KNOWN_RTOS_BOUND)} known "
          "file(s); ports are independent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
