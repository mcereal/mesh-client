## [1.3.0](https://github.com/mcereal/mesh-client/compare/v1.2.1...v1.3.0) (2026-09-04)

### Features

* **ui:** conversations, channel selection and an on-screen keyboard ([d867140](https://github.com/mcereal/mesh-client/commit/d867140da905b4b6c28a94a20fc1720db0336280))
* **ui:** navigate the HUD with the d-pad and send quick replies ([0e4b4ba](https://github.com/mcereal/mesh-client/commit/0e4b4ba047f0fbcde5d2cc0bc5d1d5d9a61a9c4d))

### Bug Fixes

* **ble:** keep the node you are talking to on the list ([f82485f](https://github.com/mcereal/mesh-client/commit/f82485f6c82ea47f1130b5c3e247ccd58bdf9941))
* **ble:** notice a dropped link, fail unsent messages, and pick targets from a list ([ef4a4c8](https://github.com/mcereal/mesh-client/commit/ef4a4c85bf23d7db633b6244c823421453b2b4dd))
* **ui:** rank message peers above MQTT-fed nodes so they stay listable ([690a592](https://github.com/mcereal/mesh-client/commit/690a592fc7ac79a34285fc295c3e7a7e6a660e8c))

## [1.2.1](https://github.com/mcereal/mesh-client/compare/v1.2.0...v1.2.1) (2026-09-04)

### Bug Fixes

* **ble:** keep the UI live while BlueZ connects ([c3254c1](https://github.com/mcereal/mesh-client/commit/c3254c14487cff754a751e275fadb3f41c5305bc))
* **ble:** wait for GATT service discovery and auto-connect on device ([6953063](https://github.com/mcereal/mesh-client/commit/695306357048967d0d56e554e71c5b83b70f873e))
* **ui:** pan the framebuffer to the page being drawn ([adf0e9f](https://github.com/mcereal/mesh-client/commit/adf0e9ffcb62f6bff4385ebd91571e10a406720a))
* **ui:** write opaque alpha so the HUD is visible on the Brick ([a7b4ebb](https://github.com/mcereal/mesh-client/commit/a7b4ebb6823db2dcd97aac5b364f761e953a6627))

## [1.2.0](https://github.com/mcereal/mesh-client/compare/v1.1.16...v1.2.0) (2026-09-03)

### Features

* **message:** send and receive Meshtastic text messages ([f2e4b58](https://github.com/mcereal/mesh-client/commit/f2e4b583fe00cacd18c343b698bcf006c1bff84d))

### Bug Fixes

* **message:** preserve the cached inbox and reject malformed UTF-8 ([3731a62](https://github.com/mcereal/mesh-client/commit/3731a62d85c692bcb09d17a59f9fb7c20d6381f6))

## [1.1.16](https://github.com/mcereal/mesh-client/compare/v1.1.15...v1.1.16) (2026-09-03)

### Bug Fixes

* **build:** fall through to the next libmsettings candidate on rejection ([025f1e2](https://github.com/mcereal/mesh-client/commit/025f1e2a6775551b9dd8d740f3d24e30ea2902cb))
* **build:** never stage host-arch helpers into the device pak tree ([99fde62](https://github.com/mcereal/mesh-client/commit/99fde623d4416c3a0b73ab41ca2aaaaf9c2c2ab0))

## [1.1.15](https://github.com/mcereal/mesh-client/compare/v1.1.14...v1.1.15) (2026-09-03)

### Bug Fixes

* **ui:** paint an initial frame and add a way to quit from the device ([8fe9bc8](https://github.com/mcereal/mesh-client/commit/8fe9bc81794403eb22336f9fb4e319a1a659e173))
* **ui:** print transport-only updates and harden the setup script ([712f3d9](https://github.com/mcereal/mesh-client/commit/712f3d9ffc71025260e0d6d4959e6e91b18a5d92))

## [1.1.14](https://github.com/mcereal/mesh-client/compare/v1.1.13...v1.1.14) (2026-09-03)

### Bug Fixes

* **ble:** yield between FromRadio reads and retry failed drains ([3ee5ed3](https://github.com/mcereal/mesh-client/commit/3ee5ed30bd4c95253a8181d2aec98c2233a14d24))
* speak the Meshtastic BLE GATT protocol instead of Nordic UART ([1fe2933](https://github.com/mcereal/mesh-client/commit/1fe29333d0d84ee2422083074758aa493e96d56a))

## [1.1.13](https://github.com/mcereal/mesh-client/compare/v1.1.12...v1.1.13) (2026-09-03)


### Bug Fixes

* remove orphaned SDL backend and link MinUI helpers statically ([3afd87d](https://github.com/mcereal/mesh-client/commit/3afd87dbc8c4b0268d3146ae8c6c384238e84e4a))


### Documentation

* sync docs with current code and add CLAUDE.md ([582674c](https://github.com/mcereal/mesh-client/commit/582674c14db9933d700d476b724ab9e83d062d92))

## [1.1.12](https://github.com/mcereal/mesh-client/compare/v1.1.11...v1.1.12) (2025-10-03)


### Bug Fixes

* framebuffer backend ([b62e931](https://github.com/mcereal/mesh-client/commit/b62e9313d91ddbec76d306412ed9d99d3d3f9240))

## [1.1.11](https://github.com/mcereal/mesh-client/compare/v1.1.10...v1.1.11) (2025-10-03)


### Bug Fixes

* implement SDL backend ([62fcb09](https://github.com/mcereal/mesh-client/commit/62fcb09f2d09855b0ea32a3a49c53edfd805c3f4))

## [1.1.10](https://github.com/mcereal/mesh-client/compare/v1.1.9...v1.1.10) (2025-10-03)


### Bug Fixes

* synchronous run printy to tty ([5976df9](https://github.com/mcereal/mesh-client/commit/5976df983b92640d1a0e137b5bda249cb15069bf))

## [1.1.9](https://github.com/mcereal/mesh-client/compare/v1.1.8...v1.1.9) (2025-10-03)


### Bug Fixes

* cli write to tty ([92668b9](https://github.com/mcereal/mesh-client/commit/92668b984c1d79db74f9f005fcffbf50a0d5d8ee))

## [1.1.8](https://github.com/mcereal/mesh-client/compare/v1.1.7...v1.1.8) (2025-10-03)


### Bug Fixes

* mirror output to tty ([800a606](https://github.com/mcereal/mesh-client/commit/800a6062efdcad0463635a5b63243aed1c74b5ed))

## [1.1.7](https://github.com/mcereal/mesh-client/compare/v1.1.6...v1.1.7) (2025-10-03)


### Bug Fixes

* reset launcher to simple synchronous run ([c8f95ce](https://github.com/mcereal/mesh-client/commit/c8f95ce008b117ea28d6c7ea37330233694c6461))

## [1.1.6](https://github.com/mcereal/mesh-client/compare/v1.1.5...v1.1.6) (2025-10-03)


### Bug Fixes

* tee to log file and run meshclient synchronbously ([2fb2381](https://github.com/mcereal/mesh-client/commit/2fb23815b51565ef36d7b5f0cb4e2f0a7078babb))

## [1.1.5](https://github.com/mcereal/mesh-client/compare/v1.1.4...v1.1.5) (2025-10-03)


### Bug Fixes

* create FIFO in pak userdata and run meshclient in the background ([e7ad1e2](https://github.com/mcereal/mesh-client/commit/e7ad1e229eca631fc5fe2d1427ffc8ae3c1e8332))

## [1.1.4](https://github.com/mcereal/mesh-client/compare/v1.1.3...v1.1.4) (2025-10-03)


### Bug Fixes

* set meshclient backend cli ([0637201](https://github.com/mcereal/mesh-client/commit/06372019dde25b7df58e7ed04d8dc70047c0e20d))

## [1.1.3](https://github.com/mcereal/mesh-client/compare/v1.1.2...v1.1.3) (2025-10-03)


### Bug Fixes

* helper directory path ([6f6aa10](https://github.com/mcereal/mesh-client/commit/6f6aa10754add28d9986c8057ab37c1696e648c7))

## [1.1.2](https://github.com/mcereal/mesh-client/compare/v1.1.1...v1.1.2) (2025-10-03)


### Bug Fixes

* trigger rebuild with helper paths ([38b6ecc](https://github.com/mcereal/mesh-client/commit/38b6ecc78fc2f7afd47155ce8e94b66e7fc973cd))

## [1.1.1](https://github.com/mcereal/mesh-client/compare/v1.1.0...v1.1.1) (2025-10-03)


### Bug Fixes

* cache toasting ([5ec62f3](https://github.com/mcereal/mesh-client/commit/5ec62f36ff533c2fca8a6b82db56b7dd3882083b))
* dbus system socket ([57091f9](https://github.com/mcereal/mesh-client/commit/57091f9ad8d57526658734653a7ef89cf3930d92))

## [1.1.0](https://github.com/mcereal/mesh-client/compare/v1.0.6...v1.1.0) (2025-10-03)


### Features

* build libdbus from source for static linking to enable BLE support ([c760b64](https://github.com/mcereal/mesh-client/commit/c760b64fdc8168664a6e78837248af9823fa8a03))


### Bug Fixes

* add dbus include and library paths to cmake build flags ([9baf7fb](https://github.com/mcereal/mesh-client/commit/9baf7fb328bf04a2aabf381c6cb4a740563b6080))
* build expat library before libdbus to satisfy dependencies ([833fdce](https://github.com/mcereal/mesh-client/commit/833fdcec72aa6de1728c55421491f219ee629811))
* explicitly set cross-compiler tools for libdbus configure ([c79753e](https://github.com/mcereal/mesh-client/commit/c79753e5f8444b545a257aedfd35d8bfa60bfbca))

## [1.0.6](https://github.com/mcereal/mesh-client/compare/v1.0.5...v1.0.6) (2025-10-03)


### Bug Fixes

* add --foreground flag to launch.sh to keep app running ([54db334](https://github.com/mcereal/mesh-client/commit/54db33465b1286a7c06b6ad2f9c70b6b4b01ae99))

## [1.0.5](https://github.com/mcereal/mesh-client/compare/v1.0.4...v1.0.5) (2025-10-03)


### Bug Fixes

* force cmake to use system python3 with protobuf packages installed ([814f71a](https://github.com/mcereal/mesh-client/commit/814f71a6c6f10c7a8475bcaefff0d17b68e70deb))
* install python protobuf packages before toolchain setup ([d2c40ee](https://github.com/mcereal/mesh-client/commit/d2c40ee2c9cb2c46cdf7a4281d3dc8a9b15864d9))
* repair corrupted YAML in semantic-release workflow ([1b0db55](https://github.com/mcereal/mesh-client/commit/1b0db554d5a5a63a05896aae137609a51532f668))
* switch to ubuntu-20.04 with glibc 2.31 for better TrimUI device compatibility ([8377a66](https://github.com/mcereal/mesh-client/commit/8377a66208d0dd0862dcd1b920929caf81fa6552))
* trigger rebuild with ubuntu-20.04 runner ([ee211fc](https://github.com/mcereal/mesh-client/commit/ee211fc56ed9de12640d8c50bbc4a468e78f726e))
* use bootlin musl toolchain instead of musl.cc for reliable downloads ([279b238](https://github.com/mcereal/mesh-client/commit/279b238cbcca23da9ebfa4767bb1ec08ef7470a5))
* use musl-libc cross-compiler for fully static ARM binary with no glibc dependency ([88fbe76](https://github.com/mcereal/mesh-client/commit/88fbe765a9596c6bb2bea340dcb278371d33741e))

## [1.0.4](https://github.com/mcereal/mesh-client/compare/v1.0.3...v1.0.4) (2025-10-03)


### Bug Fixes

* use static linking for libgcc/libstdc++ to avoid glibc version conflicts on TrimUI ([83c8136](https://github.com/mcereal/mesh-client/commit/83c81369751a6a97c71d0721a869d3653d075534))

## [1.0.3](https://github.com/mcereal/mesh-client/compare/v1.0.2...v1.0.3) (2025-10-03)


### Bug Fixes

* add ARM cross-compilation to semantic-release workflow for TrimUI Brick ([9838a21](https://github.com/mcereal/mesh-client/commit/9838a219adee0b0042050e1eb658d7387dbf4c9c))
* disable pkg-config for cross-compilation to skip dbus dependency check ([3fd0fdc](https://github.com/mcereal/mesh-client/commit/3fd0fdc9437f3a4bc8244231757ce8abf508def7))
* install ARM dbus libraries and configure cmake for proper cross-compilation ([dc6de56](https://github.com/mcereal/mesh-client/commit/dc6de56a239173584cff413e4fc15b40f6135fb2))

## [1.0.2](https://github.com/mcereal/mesh-client/compare/v1.0.1...v1.0.2) (2025-10-03)


### Bug Fixes

* bump minor to trigger build ([478ef6c](https://github.com/mcereal/mesh-client/commit/478ef6ca90a06d3fdf8c1ebcf45ca6004e1fad34))

## [1.0.1](https://github.com/mcereal/mesh-client/compare/v1.0.0...v1.0.1) (2025-10-03)


### Bug Fixes

* handshake snapshots restored on launch ([4d7b1c6](https://github.com/mcereal/mesh-client/commit/4d7b1c68ae1eafc6cf01a9fbb86280c3a8b022a3))

## 1.0.0 (2025-10-03)


### Bug Fixes

* add required python packaging ([f850fd6](https://github.com/mcereal/mesh-client/commit/f850fd6cf6599b8882e14e1d45e36c112f26e107))
* restore missing tools dir packaging script and add ble connection scaffolding ([b04634a](https://github.com/mcereal/mesh-client/commit/b04634abbe6e138ced7fa49e7fe0244f032583dc))

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
