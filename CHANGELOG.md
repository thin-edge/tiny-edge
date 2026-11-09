# Changelog

## [0.2.0](https://github.com/thin-edge/tiny-edge/compare/v0.1.0...v0.2.0) (2026-11-09)


### ⚠ BREAKING CHANGES

* **core:** struct tedge_remote_target, passed to the remote_access_allow hook, carries the target as `host` (as the operator named it) and `ipv4` (4 bytes, network order) instead of a Zephyr `struct sockaddr *addr`. tedge.h no longer includes a Zephyr network header for it.

### Bug Fixes

* build on a 64-bit host and with warnings as errors ([f2f7551](https://github.com/thin-edge/tiny-edge/commit/f2f7551933b000d72707a0bb3b31825dbbc61a4f))
* **firmware:** never cut the installed-version message ([035262d](https://github.com/thin-edge/tiny-edge/commit/035262d5ad79e24d80f4bf0f6323256bb50573e4))
* print 64-bit durations portably ([93ccd70](https://github.com/thin-edge/tiny-edge/commit/93ccd70096f2cfcef9cf23f4b06947fddb917acd))
* **telemetry:** refuse a type longer than 39 characters instead of cutting it ([f442f4c](https://github.com/thin-edge/tiny-edge/commit/f442f4c0898bc3adc343e51c8ff0f8809c67a7cc))


### Refactoring

* **core:** speak WebSocket in the core, and move remote access onto the port ([cfe1e1c](https://github.com/thin-edge/tiny-edge/commit/cfe1e1cf9da5fe4cbdfe66e4701887a681643ef3))

## [0.1.0](https://github.com/thin-edge/tiny-edge/compare/v0.0.2...v0.1.0) (2026-11-03)


### Features

* **c8y-spike:** PSRAM on the S3, WROOM as a remote-access enabler ([2c0dc64](https://github.com/thin-edge/tiny-edge/commit/2c0dc6450001f2ae7ae78be9c45c7b417af01438))
* **c8y-spike:** section 8, footprint per feature and draft profiles ([35142c9](https://github.com/thin-edge/tiny-edge/commit/35142c923cac454e322b15df0ceb100a68ca7819))
* **c8y-spike:** Spike A, MQTTS from the device to Cumulocity ([a68f484](https://github.com/thin-edge/tiny-edge/commit/a68f484b751b28208dc05579ad966c63e405c262))
* **c8y-spike:** Spike B, firmware update into slot1 via MCUboot ([7a5ae86](https://github.com/thin-edge/tiny-edge/commit/7a5ae86aad0c4e070db62c0641bf7934a039bbdd))
* **c8y-spike:** Spike C, Cumulocity CA enrollment on the device ([7d7fa87](https://github.com/thin-edge/tiny-edge/commit/7d7fa87e13715936724595f56f65a8fc2db0f88a))
* **c8y-spike:** Spike F, remote access through the device ([4c1985c](https://github.com/thin-edge/tiny-edge/commit/4c1985c84f5da0407af2e5fc752b1606ca0c7608))
* **cumulocity:** Smart Functions and DTM definitions for tedge-zephyr devices ([7ad78bb](https://github.com/thin-edge/tiny-edge/commit/7ad78bb907770eaf00b2c4c527272d248cabcb84))
* **identify:** identify a device from the cloud and from its button (thin-edge/microcontroller-devices[#12](https://github.com/thin-edge/tiny-edge/issues/12)) ([f75d937](https://github.com/thin-edge/tiny-edge/commit/f75d93705f0e4913f3520d18a0be05d624be0706))
* **modbus-server:** host the tedge-zephyr client (group 6, 8.1-8.2) ([2366b24](https://github.com/thin-edge/tiny-edge/commit/2366b245dac791854b3d7eaf2bb77e755c8b2212))
* **params:** zephyr_-prefixed parameter sets and cloud pump control ([e9f68a4](https://github.com/thin-edge/tiny-edge/commit/e9f68a4a1a4f4e760c43c7a5519eb3b66e2bdc81))
* **profiles:** certificate renewal in the ota profile ([469e244](https://github.com/thin-edge/tiny-edge/commit/469e244b415e5e0bf910ac8cd688f5a47f48f850))
* provision Wi-Fi and Cumulocity in one BLE session with lab-ztp-provisioner ([5ec6096](https://github.com/thin-edge/tiny-edge/commit/5ec609641e7edbee7be2c7e4aa148836b448eff4))
* reduce RAM and flash footprint, gate sizes against a baseline ([1f6d6d9](https://github.com/thin-edge/tiny-edge/commit/1f6d6d970c186576011aa7f9582a7442ed340843))
* reduce RAM/flash footprint, size baseline gate, zephyr_ parameter sets ([56c4011](https://github.com/thin-edge/tiny-edge/commit/56c4011351eaab2f9e9dc03510230e245b5d193c))
* **release:** shell diagnostics on the QT Py tedge-full images ([c291fec](https://github.com/thin-edge/tiny-edge/commit/c291fec1d3e8ffccedde6f7f1c663a61e19c819c))
* **release:** shell diagnostics on the S3-DevKitC tedge-full images ([826a554](https://github.com/thin-edge/tiny-edge/commit/826a5546c22d11efd90db50b78d23e9bb78e6ac1))
* **release:** ship every tedge build that ran on its board ([adfe265](https://github.com/thin-edge/tiny-edge/commit/adfe265701ec0aa5867da87a71b5fe857664d77c))
* **remote-access:** reach targets by their mDNS .local name (thin-edge/microcontroller-devices[#15](https://github.com/thin-edge/tiny-edge/issues/15)) ([ae3d738](https://github.com/thin-edge/tiny-edge/commit/ae3d738e94079ddc79c889bcda2f3b8e7ce39675))
* shared tedge board settings, ota profile and apps/tedge-agent ([904f5a5](https://github.com/thin-edge/tiny-edge/commit/904f5a5923b736a2dc8425b6bc2448732e605505))
* shell diagnostics overlay; tedge diag builds without MBEDTLS_MEMORY_DEBUG ([cd47844](https://github.com/thin-edge/tiny-edge/commit/cd478449b4b12cae09c2a608b68c72ebcdfa8b1a))
* tedge zephyr client ([7865fea](https://github.com/thin-edge/tiny-edge/commit/7865feaa5c79d155f549a308fc5d53931619c89f))
* **tedge-zephyr:** "help" lists what the shell command may run ([0d2f4fd](https://github.com/thin-edge/tiny-edge/commit/0d2f4fd54617dc10498da792f9b87fc33e29bd3e))
* **tedge-zephyr:** a test-booted image resets itself if it cannot confirm ([b65c436](https://github.com/thin-edge/tiny-edge/commit/b65c4369ec1fe7115bdf08ed10a43f942c36e6cc))
* **tedge-zephyr:** answer operations by id, not by "the oldest one" ([17d027d](https://github.com/thin-edge/tiny-edge/commit/17d027d0b6df6097b831c78c34511c053a58bca8))
* **tedge-zephyr:** certificate renewal before expiry (roadmap P6) ([b1f657f](https://github.com/thin-edge/tiny-edge/commit/b1f657fe88b95f09a9e9a6b15ca90aff42c53a9e))
* **tedge-zephyr:** Cumulocity transport, onboarding and device state ([162dae3](https://github.com/thin-edge/tiny-edge/commit/162dae33e305e6657294def6e764ca85fb677924))
* **tedge-zephyr:** diagnostics — logs, crash dumps and a guarded command (P5) ([c750f94](https://github.com/thin-edge/tiny-edge/commit/c750f9410916aa1ba5505616b2b739e5734657dd))
* **tedge-zephyr:** enroll with a one-time password issued outside the device ([8a554b1](https://github.com/thin-edge/tiny-edge/commit/8a554b1bd4ab052c8854c6f5ddc2981fa8e79735))
* **tedge-zephyr:** firmware update through MCUboot ([be1cb34](https://github.com/thin-edge/tiny-edge/commit/be1cb34ec2940530588c8819d66604fe821668f1))
* **tedge-zephyr:** learn a download's size from a range request ([192cf1f](https://github.com/thin-edge/tiny-edge/commit/192cf1f0a8bee9fe4f8dbdb8f2cbe4274ec28f46))
* **tedge-zephyr:** module foundations (c8y-direct-core group 1) ([1610abe](https://github.com/thin-edge/tiny-edge/commit/1610abeef52350f9590c74fe9c5b34dc02546059))
* **tedge-zephyr:** module skeleton, spike app and tenant findings ([52aa7a4](https://github.com/thin-edge/tiny-edge/commit/52aa7a41c4e968b7f919e39d8709b3d63b10de74))
* **tedge-zephyr:** remote access in the module (c8y-direct-remote-access) ([994383f](https://github.com/thin-edge/tiny-edge/commit/994383f97127583cae3f3b1f2fac3ba6fab0a119))
* **tedge-zephyr:** report the application's full firmware version ([3b437a8](https://github.com/thin-edge/tiny-edge/commit/3b437a8cbb66018478388c5700074316ff0f4ba7))
* **tedge-zephyr:** telemetry, alarms and the client's own health (P4) ([09f3ddc](https://github.com/thin-edge/tiny-edge/commit/09f3ddcf2b7b87b9f3ef8832fc4b9744af86b604))
* **tedge-zephyr:** the Cumulocity endpoint follows the auth method ([0595d06](https://github.com/thin-edge/tiny-edge/commit/0595d065474c18ed6ab4dd56fd1db8db9826fa68))
* **tedge-zephyr:** typed parameters the cloud can see and change (P7) ([8c0159a](https://github.com/thin-edge/tiny-edge/commit/8c0159a8d0a9755afadccacd862d5d32a21f4af1))
* **tedge-zephyr:** verify telemetry on hardware, and stamp SmartREST too ([751c5e9](https://github.com/thin-edge/tiny-edge/commit/751c5e9486ec88bb3be55114475003247cd4a1b3))
* **wifi-provisioner:** SoftAP captive-portal spike (c8y-direct-spikes §9) ([91ea78c](https://github.com/thin-edge/tiny-edge/commit/91ea78cc1296d183d63a8c4370a8f8dd0d33b3dd))


### Bug Fixes

* **c8y-spike:** echo in Cumulocity's web telnet terminal ([886ad98](https://github.com/thin-edge/tiny-edge/commit/886ad9820b81d6668c1f0666716931b0eb576f20))
* **ci:** limit GITHUB_TOKEN permissions in test workflows (thin-edge/microcontroller-devices[#8](https://github.com/thin-edge/tiny-edge/issues/8)) ([12b679a](https://github.com/thin-edge/tiny-edge/commit/12b679a415180c27df8e08b8406a03fa5c07b590))
* **tedge-boards:** state the S3-DevKitC PSRAM size ([11890eb](https://github.com/thin-edge/tiny-edge/commit/11890eb0c322300a4b65ef38d4e1c17ca756d959))
* **tedge-zephyr:** ask Cumulocity for pending operations on connect ([4926bb0](https://github.com/thin-edge/tiny-edge/commit/4926bb05617c64196d68dfe0e1d917314226b1ef))
* **tedge-zephyr:** bootstrap client ID; verify Core MQTT and the S3 ([34e27be](https://github.com/thin-edge/tiny-edge/commit/34e27bee68bd7bf89338af882e344fd164b190ba))
* **tedge-zephyr:** do not cut long descriptions in the parameter schema ([fbc7de0](https://github.com/thin-edge/tiny-edge/commit/fbc7de0d497f53734e7319be146330669764ad29))
* **tedge-zephyr:** keep the firmware marker's format stable ([d5a0516](https://github.com/thin-edge/tiny-edge/commit/d5a05168f3e31555dac20d517fbecec5f0477e05))
* **tedge-zephyr:** make a remote-access tunnel usable for an interactive session ([a876212](https://github.com/thin-edge/tiny-edge/commit/a8762125f832e784046dfaa7038af1ec419e1240))
* **tedge:** keep line breaks in shell command results (thin-edge/microcontroller-devices[#10](https://github.com/thin-edge/tiny-edge/issues/10)) ([0e851f8](https://github.com/thin-edge/tiny-edge/commit/0e851f843c40c594e53f2a3638f27cd2be83eb1e))
