SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE_ARGS ?=
# Build tree root. Container builds set BUILD_ROOT=build/linux so they never share a
# CMake cache with a host configure (the core is Linux-only; macOS cannot build it natively).
BUILD_ROOT ?= build
export BUILD_ROOT

DOCKER := ./scripts/docker.sh

.PHONY: help debug release relwithdebinfo build test package proto clean distclean run minui format \
        docker-image docker-cross-image docker-shell docker-debug docker-test docker-run docker-pak docker-clean

help:
	@echo "Host targets (Linux):"
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

format:
	clang-format -i $$(git ls-files '*.[ch]')

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
