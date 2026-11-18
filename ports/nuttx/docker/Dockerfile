# SPDX-License-Identifier: Apache-2.0
#
# Builds tiny-edge for NuttX on the ESP32 chips: the Xtensa toolchain for
# the ESP32-S3 and the RISC-V toolchain for the ESP32-C3 and ESP32-C6, at
# the versions NuttX's own documentation names for the pinned release.
# Runs on x86-64 and arm64 hosts.
#
#   docker build -t tiny-edge-nuttx ports/nuttx/docker
#   docker run --rm -v "$PWD":/ws/tiny-edge tiny-edge-nuttx \
#       /ws/tiny-edge/ports/nuttx/scripts/build.sh esp32s3-devkit

FROM ubuntu:24.04

ARG XTENSA_VERSION=esp-14.2.0_20241119
ARG XTENSA_GCC=14.2.0_20241119
ARG RISCV_VERSION=13.2.0-2

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      bison build-essential ca-certificates cmake curl file flex genromfs \
      git gperf libncurses-dev ninja-build python3 python3-pip python3-venv \
      unzip xxd xz-utils \
    && rm -rf /var/lib/apt/lists/*

# kconfiglib for NuttX's configuration, esptool for images, imgtool for
# MCUboot signatures.
RUN python3 -m venv /opt/py \
    && /opt/py/bin/pip install --no-cache-dir kconfiglib esptool imgtool
ENV PATH=/opt/py/bin:$PATH

# kconfig-tweak, which NuttX's configure step calls: the Linux kernel's
# scripts/config, which kconfig-frontends ships under that name.
RUN curl -fsSL -o /usr/local/bin/kconfig-tweak \
      https://raw.githubusercontent.com/torvalds/linux/v6.6/scripts/config \
    && chmod +x /usr/local/bin/kconfig-tweak

RUN set -eu; arch=$(uname -m); \
    case "$arch" in \
      x86_64) xt=x86_64-linux-gnu; rv=linux-x64 ;; \
      aarch64) xt=aarch64-linux-gnu; rv=linux-arm64 ;; \
      *) echo "unsupported host $arch" >&2; exit 1 ;; \
    esac; \
    mkdir -p /opt/xtensa-esp-elf /opt/riscv-none-elf; \
    curl -fsSL "https://github.com/espressif/crosstool-NG/releases/download/${XTENSA_VERSION}/xtensa-esp-elf-${XTENSA_GCC}-${xt}.tar.xz" \
      | tar -C /opt/xtensa-esp-elf --strip-components 1 -xJ; \
    curl -fsSL "https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${RISCV_VERSION}/xpack-riscv-none-elf-gcc-${RISCV_VERSION}-${rv}.tar.gz" \
      | tar -C /opt/riscv-none-elf --strip-components 1 -xz
ENV PATH=/opt/xtensa-esp-elf/bin:/opt/riscv-none-elf/bin:$PATH

WORKDIR /ws
