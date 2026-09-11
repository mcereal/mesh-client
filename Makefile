SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE_ARGS ?=
# Build tree root. Container builds set BUILD_ROOT=build/linux so they never share a
# CMake cache with a host configure (the core is Linux-only; macOS cannot build it natively).
BUILD_ROOT ?= build
export BUILD_ROOT

DOCKER := ./scripts/docker.sh

.PHONY: help setup debug release relwithdebinfo build test package proto clean distclean run format fuzz \
        ui-capture screenshots \
        docker-image docker-cross-image docker-shell docker-debug docker-test docker-run docker-pak \
        docker-clean docker-ui-capture docker-screenshots docker-fuzz \
        deploy deploy-start deploy-stop deploy-run deploy-logs deploy-check deploy-shot deploy-clip deploy-input-map \
        deploy-shell deploy-key brick

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
	@echo "  make ui-capture     - Render a UI scene to a GIF without a device (ARGS=\"scene -o out.gif\")"
	@echo "  make screenshots    - Re-render the listing stills in .github/resources/screenshots"
	@echo "  make fuzz           - Build and run the libFuzzer harnesses (ARGS=\"--time 600\" to hunt)"
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
	@echo "  make docker-ui-capture - make ui-capture inside the dev container (use this on macOS)"
	@echo "  make docker-screenshots - make screenshots inside the dev container (use this on macOS)"
	@echo "  make docker-fuzz    - make fuzz inside the dev container (use this on macOS)"
	@echo ""
	@echo "Device targets (TrimUI Brick over WiFi/SSH or USB/adb; configure .brick.env, see docs/device.md):"
	@echo "  make deploy         - Push dist/MeshClient.pak to the Brick's Tools/tg5040/"
	@echo "  make brick          - docker-pak + deploy in one step"
	@echo "  make deploy-start   - Start MeshClient on the device as Tools > MeshClient does (NextUI steps aside)"
	@echo "  make deploy-stop    - Stop every MeshClient on the device; end every on-device test with this"
	@echo "  make deploy-run     - deploy-start + follow the log; Ctrl-C stops it (ARGS=\"--list-devices\" runs headless)"
	@echo "  make deploy-logs    - Tail the on-device MeshClient.txt log"
	@echo "  make deploy-check   - Report SD card / BlueZ / D-Bus / adapter / fb0 state on the device"
	@echo "  make deploy-shot    - Screenshot the device's screen to a PNG (ARGS=\"-d 10 -o nodes.png\")"
	@echo "  make deploy-clip    - Film the device's screen to a GIF (ARGS=\"-d 10 -n 30 -o open.gif\")"
	@echo "  make deploy-input-map - Identify the device's buttons: press them, read the codes"
	@echo "  make deploy-shell   - Open a shell on the device"
	@echo "  make deploy-key     - Install your SSH public key on the device (SSH transport only)"
	@echo "  (transport auto-detects USB when a cable is attached; force with BRICK_TRANSPORT=ssh|adb)"

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

package: release
	./scripts/package.sh release

proto: debug
	cmake --build $(BUILD_ROOT)/debug --target nanopb_codegen

run: debug
	./$(BUILD_ROOT)/debug/meshclient --foreground --log-level debug

# Render the HUD from a scene script, off-screen, with no Brick and no framebuffer involved.
# The companion to deploy-shot for a change that is about a transition; see docs/ui.md.
ui-capture:
	./scripts/ui-capture.sh $(ARGS)

# The libFuzzer harnesses over the two places bytes we did not write enter the client: the
# serial frame parser and the session's FromRadio decode. No arguments is the deterministic
# regression pass CI runs; ARGS="--time 600" is an actual hunt. See docs/testing.md.
fuzz:
	./scripts/fuzz.sh $(ARGS)

# The four stills the README and the Pak Store listing carry, from the scenes in
# devtools/ui_capture/scenes/shots/. Run it after a UI change rather than editing the pictures.
screenshots:
	./scripts/screenshots.sh $(ARGS)

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

docker-ui-capture:
	$(DOCKER) make ui-capture ARGS="$(ARGS)"

docker-screenshots:
	$(DOCKER) make screenshots ARGS="$(ARGS)"

docker-fuzz:
	$(DOCKER) make fuzz ARGS="$(ARGS)"

docker-clean:
	rm -rf build/linux

# ---- Device (TrimUI Brick over SSH) -----------------------------------------

DEPLOY := ./scripts/deploy-device.sh
ARGS ?=

deploy:
	$(DEPLOY) push

brick: docker-pak deploy

# Start and stop through NextUI's own launch loop, so the launcher is off the screen and off the
# buttons while the client runs; see scripts/deploy-device.sh.
deploy-start:
	$(DEPLOY) start -- $(ARGS)

deploy-stop:
	$(DEPLOY) stop

deploy-run:
	$(DEPLOY) run -- $(ARGS)

deploy-logs:
	$(DEPLOY) logs

deploy-check:
	$(DEPLOY) check

deploy-shot:
	$(DEPLOY) shot -- $(ARGS)

deploy-clip:
	$(DEPLOY) clip -- $(ARGS)

# Which button reports what, measured by pressing them; see docs/device.md.
deploy-input-map:
	$(DEPLOY) input-map -- $(ARGS)

deploy-shell:
	$(DEPLOY) shell

deploy-key:
	$(DEPLOY) setup-key
