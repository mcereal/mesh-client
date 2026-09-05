SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE_ARGS ?=
# Build tree root. Container builds set BUILD_ROOT=build/linux so they never share a
# CMake cache with a host configure (the core is Linux-only; macOS cannot build it natively).
BUILD_ROOT ?= build
export BUILD_ROOT

DOCKER := ./scripts/docker.sh

.PHONY: help setup debug release relwithdebinfo build test package proto clean distclean run minui format \
        docker-image docker-cross-image docker-shell docker-debug docker-test docker-run docker-pak docker-clean \
        deploy deploy-run deploy-logs deploy-check deploy-shot deploy-shell deploy-key brick

help:
	@echo "Host targets (Linux):"
	@echo "  make setup          - Install build prerequisites natively (submodules, libdbus, protobuf)"
	@echo "  make debug          - Configure and build a Debug build ($(BUILD_ROOT)/debug)"
	@echo "  make release        - Configure and build a Release build"
	@echo "  make test           - Run unit tests against the Debug build"
	@echo "  make run            - Run the Debug binary in the foreground"
	@echo "  make package        - Produce dist/MeshClient.pak.zip from a Release build"
	@echo "  make proto          - Regenerate nanopb sources from proto/meshtastic"
	@echo "  make format         - clang-format all tracked .c/.h files"
	@echo "  make clean          - Remove build artifacts"
	@echo "  make distclean      - Remove build and dist outputs"
	@echo ""
	@echo "Container targets (macOS or any host with Docker; sources bind-mounted, builds in build/linux):"
	@echo "  make docker-test    - Debug build + unit tests in the dev container"
	@echo "  make docker-debug   - Debug build only"
	@echo "  make docker-run     - Run the container-built Debug binary (BLE unavailable; CLI backend)"
	@echo "  make docker-shell   - Interactive bash in the dev container"
	@echo "  make docker-pak     - Static aarch64 build + dist/MeshClient.pak.zip for the TrimUI Brick"
	@echo "  make docker-image   - (Re)build the dev image;  make docker-cross-image for the cross image"
	@echo "  make docker-clean   - Remove build/linux"
	@echo ""
	@echo "Device targets (TrimUI Brick over SSH; configure .brick.env, see docs/device.md):"
	@echo "  make deploy         - Push dist/MeshClient.pak to the Brick's Tools/tg5040/"
	@echo "  make brick          - docker-pak + deploy in one step"
	@echo "  make deploy-run     - Run launch.sh on the device, streaming output (ARGS=\"--list-devices\")"
	@echo "  make deploy-logs    - Tail the on-device MeshClient.txt log"
	@echo "  make deploy-check   - Report SD card / BlueZ / D-Bus / adapter / fb0 state on the device"
	@echo "  make deploy-shot    - Screenshot the device's screen to a PNG (ARGS=\"-d 10 -o nodes.png\")"
	@echo "  make deploy-shell   - SSH into the device"
	@echo "  make deploy-key     - Install your SSH public key on the device"

setup:
	./scripts/setup-linux.sh

build: debug

debug:
	./scripts/build.sh debug $(CMAKE_ARGS)

release:
	./scripts/build.sh release $(CMAKE_ARGS)

relwithdebinfo:
	./scripts/build.sh relwithdebinfo $(CMAKE_ARGS)

test: debug
	ctest --test-dir $(BUILD_ROOT)/debug --output-on-failure

package: release minui
	./scripts/package.sh release

minui:
	./scripts/build_minui_helpers.sh

proto: debug
	cmake --build $(BUILD_ROOT)/debug --target nanopb_codegen

run: debug
	./$(BUILD_ROOT)/debug/meshclient --foreground --log-level debug

# clang-format 18 is what ubuntu:24.04 ships, so the dev container and CI agree on it. A
# different major reflows code that is already normalised - trailing-comment alignment and how
# brace initialisers pack, mostly - which lands as churn that reads like a real diff and drifts
# the tree every time it is run on a host with a different version. Refuse rather than rewrite:
# the container always has the right one, and the override is there for when you mean it.
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_MAJOR ?= 18

format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
	    echo "$(CLANG_FORMAT) not found. Run './scripts/docker.sh make format' instead." >&2; \
	    exit 1; }
	@have=$$($(CLANG_FORMAT) --version | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p'); \
	if [ "$$have" != "$(CLANG_FORMAT_MAJOR)" ] && [ -z "$$CLANG_FORMAT_ANY_VERSION" ]; then \
	    echo "clang-format $$have found; this tree is normalised with $(CLANG_FORMAT_MAJOR)." >&2; \
	    echo "Formatting with another major rewrites files that are already correct." >&2; \
	    echo "Run './scripts/docker.sh make format', or set CLANG_FORMAT_ANY_VERSION=1." >&2; \
	    exit 1; \
	fi
	$(CLANG_FORMAT) -i $$(git ls-files '*.[ch]')

clean:
	rm -rf $(BUILD_ROOT)/debug $(BUILD_ROOT)/release $(BUILD_ROOT)/relwithdebinfo

distclean: clean
	rm -rf dist

# ---- Docker -----------------------------------------------------------------

docker-image:
	$(DOCKER) --rebuild true

docker-cross-image:
	$(DOCKER) --cross --rebuild true

docker-shell:
	$(DOCKER)

docker-debug:
	$(DOCKER) make debug CMAKE_ARGS="$(CMAKE_ARGS)"

docker-test:
	$(DOCKER) make test CMAKE_ARGS="$(CMAKE_ARGS)"

docker-run:
	$(DOCKER) make run

docker-pak:
	$(DOCKER) --cross ./scripts/cross-build.sh

docker-clean:
	rm -rf build/linux

# ---- Device (TrimUI Brick over SSH) -----------------------------------------

DEPLOY := ./scripts/deploy-device.sh
ARGS ?=

deploy:
	$(DEPLOY) push

brick: docker-pak deploy

deploy-run:
	$(DEPLOY) run -- $(ARGS)

deploy-logs:
	$(DEPLOY) logs

deploy-check:
	$(DEPLOY) check

deploy-shot:
	$(DEPLOY) shot -- $(ARGS)

deploy-shell:
	$(DEPLOY) shell

deploy-key:
	$(DEPLOY) setup-key
