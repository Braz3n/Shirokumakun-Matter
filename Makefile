BOARD     := xiao_ble/nrf52840/sense
BUILD_DIR := build

SRCS := $(wildcard src/*.cpp src/*.c)

# Docker image and NCS workspace volume names.
# The volume persists the west workspace so west update only runs once per SDK version.
DOCKER_IMAGE  := matter-ac-builder:v3.3.0
NCS_VOLUME    := ncs-v3.3.0

DOCKER_RUN := docker run --rm \
    -v $(NCS_VOLUME):/ncs \
    -v $(PWD):/workspace \
    -e ZEPHYR_BASE=/ncs/zephyr \
    -e ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
    -e ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0 \
    $(DOCKER_IMAGE)

.PHONY: build pristine flash flash-erase attach format \
        docker-image docker-init docker-build docker-pristine docker-dfu

# --- Docker targets (primary build path) ---

# Build the Alpine-based builder image. Run once.
docker-image:
	docker build -t $(DOCKER_IMAGE) .

# Initialise the NCS v3.3.0 workspace into the Docker volume. Run once per SDK version.
# Uses west.yml in this repo as the manifest source.
docker-init:
	docker volume create $(NCS_VOLUME)
	docker run --rm \
	    -v $(NCS_VOLUME):/ncs \
	    -v $(PWD):/workspace \
	    -e ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0 \
	    $(DOCKER_IMAGE) sh -c "\
	    [ -d /ncs/zephyr ] && echo 'Volume already initialised; run docker volume rm $(NCS_VOLUME) to reset' && exit 0; \
	    [ -d /ncs/.west ] || west init --mf /workspace/west.yml /ncs; \
	    cd /ncs && west update"

docker-build:
	$(DOCKER_RUN) west build -b $(BOARD) /workspace

docker-pristine:
	$(DOCKER_RUN) sh -c "rm -rf /workspace/$(BUILD_DIR) && west build -b $(BOARD) /workspace"

# DFU runs on the host (J-Link / mcumgr-client are not in the container).
docker-dfu: docker-build
	$(MAKE) _dfu

# --- Native targets (fallback for machines with NCS installed natively) ---
# Toolchain path for NCS v2.9.2 (last known working). Update TC hash after
# installing v3.3.0: nrfutil toolchain-manager install --ncs-version v3.3.0
TC   := /home/zane/ncs/toolchains/7795df4459
WEST := $(TC)/usr/local/bin/west

export PATH := $(TC)/bin:$(TC)/usr/bin:$(TC)/usr/local/bin:$(TC)/opt/bin:$(TC)/opt/zephyr-sdk/arm-zephyr-eabi/bin:$(PATH)
export LD_LIBRARY_PATH := $(TC)/lib:$(TC)/lib/x86_64-linux-gnu:$(TC)/usr/local/lib:$(TC)/opt/nanopb/generator-bin
export PYTHONHOME    := $(TC)/usr/local
export PYTHONPATH    := $(TC)/usr/local/lib/python3.12:$(TC)/usr/local/lib/python3.12/site-packages
export ZEPHYR_TOOLCHAIN_VARIANT := zephyr
export ZEPHYR_SDK_INSTALL_DIR   := $(TC)/opt/zephyr-sdk
export ZEPHYR_BASE              := /home/zane/ncs/zephyr

build:
	$(WEST) build -b $(BOARD) .

pristine:
	$(WEST) build --pristine -b $(BOARD) .

JLINK       := JLinkExe
JLINK_FLAGS := -nogui 1 -if SWD -speed 4000 -device NRF52840_xxAA -autoconnect 1

flash: build
	$(WEST) flash --build-dir $(BUILD_DIR)
	$(JLINK) $(JLINK_FLAGS) -CommanderScript scripts/jlink_go.jlink

flash-erase: build
	$(WEST) flash --build-dir $(BUILD_DIR) --erase
	$(JLINK) $(JLINK_FLAGS) -CommanderScript scripts/jlink_go.jlink

attach:
	$(WEST) attach

format:
	clang-format -i $(SRCS)

# --- Shared targets ---

DFU_PORT := $(shell for d in /dev/ttyACM*; do \
	info=$$(udevadm info $$d 2>/dev/null); \
	echo "$$info" | grep -q 'ID_USB_VENDOR_ID=2fe3' && \
	echo "$$info" | grep -q 'ID_USB_INTERFACE_NUM=02' && \
	echo $$d && break; done)

_dfu:
	@test -n "$(DFU_PORT)" || (echo "ERROR: Zephyr DFU port not found"; exit 1)
	mcumgr-client -d $(DFU_PORT) -m 1024 -l 512 upload build/matter-ac-ncs/zephyr/zephyr.signed.bin
	@hash=$$(mcumgr-client -d $(DFU_PORT) list 2>/dev/null | awk '/^response:/{sub(/^response: /,""); p=1} p' | jq -r '.images[] | select(.slot==1) | .hash'); \
	test -n "$$hash" || (echo "ERROR: could not read slot 1 hash"; exit 1); \
	mcumgr-client -d $(DFU_PORT) test $$hash; \
	mcumgr-client -d $(DFU_PORT) reset

dfu: build _dfu
