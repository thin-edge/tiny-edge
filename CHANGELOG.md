# Changelog

## [0.6.0](https://github.com/thin-edge/tiny-edge/compare/v0.5.0...v0.6.0) (2026-11-12)


### Features

* **onboard:** register devices with the type their firmware reports ([98fc339](https://github.com/thin-edge/tiny-edge/commit/98fc339b46272819c9aaeb186c604e69854d807d))
* **onboard:** register devices with their firmware's type; fix the ESP-IDF component install ([fe360dd](https://github.com/thin-edge/tiny-edge/commit/fe360dda02f5a48d6e7b2a51d92206615f5eef82))


### Bug Fixes

* **esp-idf:** build the component when the component manager installs it ([57f96c9](https://github.com/thin-edge/tiny-edge/commit/57f96c9bd3b1ef71fd8e2b7d54ebc2189dedeb8b))

## [0.5.0](https://github.com/thin-edge/tiny-edge/compare/v0.4.0...v0.5.0) (2026-11-12)


### ⚠ BREAKING CHANGES

* the health service, the tedge_Agent name, the parameter sets, and the Smart Function rules are renamed. cumulocity/README.md, "Moving a tenant from tedge-zephyr names", lists what a tenant needs: the new rules (with the old ones deleted), the new DTM definitions, and the removal of the old fragments and service.

### Features

* **esp-idf:** set devices up over USB with tedge-onboard ([5ff2c4f](https://github.com/thin-edge/tiny-edge/commit/5ff2c4f0987ac187a32c63edb578386d5acf717a))
* **esp-idf:** set devices up over USB with tedge-onboard ([41d8bfc](https://github.com/thin-edge/tiny-edge/commit/41d8bfcc3ce8bfa0bc168e37ce4c225f3fd2f94a))
* move the OS to tedge_OS and drop duplicates from tedge_Agent ([12b24f2](https://github.com/thin-edge/tiny-edge/commit/12b24f2929d084d5f29b16bf5a16cb2c14c79263))
* report Cumulocity's c8y_Agent fragment ([b156c4a](https://github.com/thin-edge/tiny-edge/commit/b156c4a01910560816c238d86adb6f05d359df54))
* report tiny-edge names, the OS, and c8y_Hardware on every port ([bec9a28](https://github.com/thin-edge/tiny-edge/commit/bec9a284cbd8a3b8d827fb293f578c0bcca4d8ed))


### Bug Fixes

* report firmware versions up to 31 characters ([0a3f74f](https://github.com/thin-edge/tiny-edge/commit/0a3f74fee897ae8b465ae62b5ecf928da84d9b13))

## [0.4.0](https://github.com/thin-edge/tiny-edge/compare/v0.3.0...v0.4.0) (2026-11-11)


### Features

* build sample images without Wi-Fi credentials ([1bbb0e0](https://github.com/thin-edge/tiny-edge/commit/1bbb0e0c65ef59bbb2420702faec0cb34b828b52))
* FreeRTOS port, with ESP-IDF as its first platform ([57be8e1](https://github.com/thin-edge/tiny-edge/commit/57be8e15ead6cb266baf2fc243609ea17634671c))
* FreeRTOS port, with ESP-IDF as its first platform ([573007b](https://github.com/thin-edge/tiny-edge/commit/573007b138c70212ac0e02fde5f951e8ffbbd03e))
* run cloud shell commands (c8y_Command) on ESP-IDF ([a54ba1f](https://github.com/thin-edge/tiny-edge/commit/a54ba1f6470be58688eb2b3caf531c96c7789333))


### Bug Fixes

* report an operation interrupted by a reset as failed ([5282034](https://github.com/thin-edge/tiny-edge/commit/5282034180c22f71275e5f135ed101c1bcc9f05f))
* report an operation interrupted by a reset as failed ([e822b4d](https://github.com/thin-edge/tiny-edge/commit/e822b4d0378bb069439b3d5cbb19a1a458254313))

## [0.3.0](https://github.com/thin-edge/tiny-edge/compare/v0.2.0...v0.3.0) (2026-11-10)


### Features

* **core:** read the device ID and tenant through the public API ([baf7af4](https://github.com/thin-edge/tiny-edge/commit/baf7af4a05fbebe87e28d06868d484d8349b6533))
* **onboard:** offer the networks the board can see ([ee2735f](https://github.com/thin-edge/tiny-edge/commit/ee2735f37e3ba96bca4bd458d03cb41db4764d63))
* **onboard:** tedge-onboard, set a device up from a computer ([7637e06](https://github.com/thin-edge/tiny-edge/commit/7637e064728fd9fc7322a760f9bee07c3d11a889))
* **posix:** a POSIX port of the client ([1baacd9](https://github.com/thin-edge/tiny-edge/commit/1baacd9647610c8524fcc1562be3d7d9ed48a635))
* **posix:** a POSIX port of the client ([50dc48b](https://github.com/thin-edge/tiny-edge/commit/50dc48babb588313825f69b090e67d38ba4ec9c4))
* **posix:** sample application and port tests ([522c8c9](https://github.com/thin-edge/tiny-edge/commit/522c8c9f30205a7d7880c045cc8e9f0e24e34987))
* **provisioning:** an Improv Wi-Fi library for setting a device up ([96eeb7d](https://github.com/thin-edge/tiny-edge/commit/96eeb7d622043e28caa422a876397cae5d97c93b))
* **provisioning:** report the application's firmware name and version ([e2cdf67](https://github.com/thin-edge/tiny-edge/commit/e2cdf677978e70a190d810556e75a522246b24e3))
* **provisioning:** report the Wi-Fi networks the board can see ([872404d](https://github.com/thin-edge/tiny-edge/commit/872404d23b31fb199e03dbc412d511ef475a9bd0))
* set devices up over USB with tedge-onboard ([07a8559](https://github.com/thin-edge/tiny-edge/commit/07a855995778cb254bb22cc48c6d1420a702b038))
* **zephyr:** a ready-made Wi-Fi scan hook for provisioning ([5766f14](https://github.com/thin-edge/tiny-edge/commit/5766f14881e56fd40e477e6fecbf743842cccdd8))
* **zephyr:** Improv Serial on the console, and an onboarding sample ([6a537b1](https://github.com/thin-edge/tiny-edge/commit/6a537b1cabb15ba199a5944f8dac20c67a0fe3d6))
* **zephyr:** one call starts provisioning with the port's hooks ([0d9440a](https://github.com/thin-edge/tiny-edge/commit/0d9440ad7bd2c24b8cda56788ccfe03f726e8fe6))
* **zephyr:** one call starts provisioning with the port's hooks ([e5b14ba](https://github.com/thin-edge/tiny-edge/commit/e5b14ba1b4fdf44e086503da092f9bce4a3509db))


### Bug Fixes

* append formatted text without overrunning the buffer ([23350ab](https://github.com/thin-edge/tiny-edge/commit/23350ab6f348249f5d6ec09b54c3047fdcc3ea2e))
* append formatted text without overrunning the buffer ([1696f82](https://github.com/thin-edge/tiny-edge/commit/1696f826b9bf98bab0e8246a2cdd9b2d7ca8aff9))
* **core:** build without restart and firmware update with warnings as errors ([f3cb8b8](https://github.com/thin-edge/tiny-edge/commit/f3cb8b8e90c3e00962a28e722fd6cca1d663241e))
* **core:** stop the client promptly while it waits for registration ([fbd0219](https://github.com/thin-edge/tiny-edge/commit/fbd02193ef4f76953ad40f0c352105b8e7398d45))
* **remote-access:** relay what the target sent before it hung up ([26e87b3](https://github.com/thin-edge/tiny-edge/commit/26e87b30e90592052b9149c71924ee1ba6ef2a81))
* **zephyr:** read the console directly when the shell is not on it ([c9cb45c](https://github.com/thin-edge/tiny-edge/commit/c9cb45c06315f28c2c474f0848645db8dead089b))

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
