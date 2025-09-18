SHELL := /bin/bash
.DEFAULT_GOAL := help

CMAKE_ARGS ?=

.PHONY: help debug release relwithdebinfo build test package proto clean distclean run minui

help:
	@echo "Common targets:"
	@echo "  make debug          - Configure and build a Debug build"
	@echo "  make release        - Configure and build a Release build"
	@echo "  make test           - Run unit tests against the Debug build"
	@echo "  make package        - Produce dist/MeshClient.pak.zip from a Release build"
	@echo "  make clean          - Remove build artifacts"
	@echo "  make distclean      - Remove build and dist outputs"

build: debug

debug:
	./scripts/build.sh debug $(CMAKE_ARGS)

release:
	./scripts/build.sh release $(CMAKE_ARGS)

relwithdebinfo:
	./scripts/build.sh relwithdebinfo $(CMAKE_ARGS)

test: debug
	ctest --test-dir build/debug --output-on-failure

package: release minui
	./scripts/package.sh release

minui:
	./scripts/build_minui_helpers.sh

proto: debug
	cmake --build build/debug --target nanopb_codegen

run: debug
	./build/debug/meshclient --foreground --log-level debug

clean:
	rm -rf build/debug build/release build/relwithdebinfo

distclean: clean
	rm -rf dist
