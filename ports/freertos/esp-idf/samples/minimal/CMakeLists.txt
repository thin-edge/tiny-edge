# SPDX-License-Identifier: Apache-2.0
#
# The smallest ESP-IDF application that hosts tiny-edge: join Wi-Fi, start
# the client, and publish a measurement now and then.
#
#   idf.py set-target esp32c6
#   idf.py menuconfig     # Wi-Fi and the tenant: "tiny-edge sample"
#   idf.py build flash monitor

cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tedge-minimal)
