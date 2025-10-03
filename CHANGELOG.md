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
