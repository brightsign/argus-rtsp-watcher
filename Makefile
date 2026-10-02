GO ?= go
WATCHER_BIN := build/watcher
TARGET_SOC ?= rk3588
ENGINE_BUILD_DIR := build/$(TARGET_SOC)
ENGINE_BIN := $(ENGINE_BUILD_DIR)/argus-engine
ENGINE_TEST_BUILD_DIR := build/host-tests
ENGINE_TEST_BIN := $(ENGINE_TEST_BUILD_DIR)/tests/run_engine_tests
IMAGE_SERVER_BIN := build/image-stream-server
MOSQUITTO_DIR := build/mosquitto
LIBS_DIR := build/libs
MODELS_DIR := build/models
SDK_ENV := sdk/environment-setup-aarch64-oe-linux
# Optional: path to the BrightSign cobra toolchain installer. If empty, fetch-sdk
# searches the repo root / CWD for brightsign-x86_64-cobra-toolchain-*.sh.
SDK_INSTALLER ?=

# Packaging: the engine, mosquitto broker, runtime libs, and image-stream-server
# are all generic aarch64 (same for every SoC), so each is built once and staged
# into every per-SoC directory. Only the .rknn models are SoC-specific. Every
# runtime asset is produced on demand from upstream sources (build-mosquitto,
# build-libs, build-models, build-image-server) or the SDK sysroot -- this repo
# has no dependency on a sibling argus install tree.
SOC_LIST := RK3588 RK3576 RK3568
STAGING_DIR := staging
EXT_NAME := npu_argus_rtsp
BUILD_TS := $(shell date +%s)
DEV_ZIP := argus-rtsp-watcher-$(BUILD_TS).zip
EXT_ZIP := argus-rtsp-watcher-ext-$(BUILD_TS).zip

.DEFAULT_GOAL := help

help:                ## Print available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-14s %s\n", $$1, $$2}'

build: package       ## Build watcher + engine, stage all SoCs, and package -> zip(s) in repo root

prep:                ## Fetch the Rockchip-origin RKNN headers + runtime into engine/include/ (needs network; not vendored)
	bash scripts/prep.sh

fetch-sdk:           ## Install the aarch64 cross-compile SDK into ./sdk (needs the cobra toolchain installer; pass SDK_INSTALLER=/path or drop it at the repo root)
	bash scripts/fetch-sdk.sh $(SDK_INSTALLER)

build-watcher:       ## Cross-compile the Go watcher for arm64
	GOOS=linux GOARCH=arm64 $(GO) build -o $(WATCHER_BIN) ./cmd/watcher

build-image-server:  ## Build the image-stream-server (MJPEG preview) from its upstream repo (needs git + go + network)
	bash scripts/build-image-server.sh $(CURDIR)/$(IMAGE_SERVER_BIN)

build-mosquitto: fetch-sdk ## Cross-compile the mosquitto broker + harvest client tools from the SDK sysroot
	bash scripts/build-mosquitto.sh $(CURDIR)/$(MOSQUITTO_DIR)

build-libs: prep fetch-sdk ## Harvest the aarch64 runtime libraries (OpenCV/GStreamer/RGA/RKNN/...) from the SDK sysroot
	bash scripts/build-libs.sh $(CURDIR)/$(LIBS_DIR)

build-models:        ## Compile the RKNN models per SoC from upstream ONNX (needs docker + ~10GB + network)
	bash scripts/build-models.sh $(CURDIR)/$(MODELS_DIR) $(SOC_LIST)

build-engine: prep fetch-sdk ## Cross-compile the C++ engine for TARGET_SOC (default rk3588)
	mkdir -p $(ENGINE_BUILD_DIR)
	bash -c 'source $(CURDIR)/$(SDK_ENV) && \
		cmake -S $(CURDIR)/engine -B $(CURDIR)/$(ENGINE_BUILD_DIR) \
			-DOECORE_TARGET_SYSROOT="$$OECORE_TARGET_SYSROOT" \
			-DTARGET_SOC=$(TARGET_SOC) -DBUILD_TESTS=OFF && \
		$(MAKE) -C $(CURDIR)/$(ENGINE_BUILD_DIR) -j$$(nproc)'

build-engine-tests: prep ## Build the C++ engine unit tests for the host (native, not cross-compiled)
	mkdir -p $(ENGINE_TEST_BUILD_DIR)
	cmake -S $(CURDIR)/engine -B $(CURDIR)/$(ENGINE_TEST_BUILD_DIR) -DBUILD_TESTS=ON
	$(MAKE) -C $(CURDIR)/$(ENGINE_TEST_BUILD_DIR) run_engine_tests -j$$(nproc)

run-engine-tests: build-engine-tests ## Build and run the C++ engine unit tests
	$(ENGINE_TEST_BIN)

package: build-watcher build-engine build-image-server build-mosquitto build-libs build-models ## Assemble staging/<SOC> for rk3588/rk3576/rk3568 and produce the dev + LVM extension zips
	SOC_LIST="$(SOC_LIST)" STAGING_DIR="$(STAGING_DIR)" \
		WATCHER_BIN="$(CURDIR)/$(WATCHER_BIN)" ENGINE_BIN="$(CURDIR)/$(ENGINE_BIN)" \
		IMAGE_SERVER_BIN="$(CURDIR)/$(IMAGE_SERVER_BIN)" \
		MOSQUITTO_DIR="$(CURDIR)/$(MOSQUITTO_DIR)" LIBS_DIR="$(CURDIR)/$(LIBS_DIR)" MODELS_DIR="$(CURDIR)/$(MODELS_DIR)" \
		EXT_NAME="$(EXT_NAME)" DEV_ZIP="$(DEV_ZIP)" EXT_ZIP="$(EXT_ZIP)" \
		scripts/build.sh

test: run-tests      ## Alias for run-tests

run-tests: run-engine-tests ## Run all Go tests + engine unit tests
	$(GO) test ./...

run-integration-tests: ## Run hardware/engine integration tests (needs `make build` staged assets; skips cleanly off-target)
	$(GO) test -tags integration -v ./test/...

clean:               ## Remove build artifacts
	rm -rf build staging *.zip

.PHONY: help build prep fetch-sdk build-watcher build-image-server build-mosquitto build-libs build-models build-engine build-engine-tests run-engine-tests package test run-tests run-integration-tests clean
