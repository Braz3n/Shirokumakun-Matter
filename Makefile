BOARD     := xiao_ble/nrf52840/sense
BUILD_DIR := build

SRCS := $(wildcard src/*.cpp src/*.c)

DOCKER_IMAGE := matter-ac-builder:v3.3.0
NCS_VOLUME   := ncs-v3.3.0

DOCKER_RUN := docker run --rm \
    -v $(NCS_VOLUME):/ncs \
    -v $(PWD):/workspace \
    -e ZEPHYR_BASE=/ncs/zephyr \
    -e ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
    -e ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0 \
    $(DOCKER_IMAGE)

.PHONY: image init build pristine dfu format

# Build the Alpine-based builder image. Run once.
image:
	docker build -t $(DOCKER_IMAGE) .

# Initialise the NCS v3.3.0 workspace into the Docker volume. Run once per SDK version.
init:
	docker volume create $(NCS_VOLUME)
	docker run --rm \
	    -v $(NCS_VOLUME):/ncs \
	    -v $(PWD):/workspace \
	    -e ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0 \
	    $(DOCKER_IMAGE) sh -c "\
	    [ -d /ncs/zephyr ] && echo 'Volume already initialised; run docker volume rm $(NCS_VOLUME) to reset' && exit 0; \
	    [ -d /ncs/.west ] || west init --mf /workspace/west.yml /ncs; \
	    cd /ncs && west update"

build:
	$(DOCKER_RUN) west build -b $(BOARD) /workspace

pristine:
	$(DOCKER_RUN) sh -c "rm -rf /workspace/$(BUILD_DIR) && west build -b $(BOARD) /workspace"

JLINK       := JLinkExe
JLINK_FLAGS := -nogui 1 -if SWD -speed 4000 -device NRF52840_xxAA -autoconnect 1

# Full erase + flash via JLink. Erases the entire device (including settings_storage),
# so the device will need to re-commission after this.
flash:
	$(JLINK) $(JLINK_FLAGS) -CommanderScript scripts/jlink_flash.jlink

format:
	clang-format -i $(SRCS)

# DFU runs on the host — mcumgr-client accesses the USB serial port directly.
DFU_PORT := $(shell for d in /dev/ttyACM*; do \
	info=$$(udevadm info $$d 2>/dev/null); \
	echo "$$info" | grep -q 'ID_USB_VENDOR_ID=2fe3' && \
	echo "$$info" | grep -q 'ID_USB_INTERFACE_NUM=02' && \
	echo $$d && break; done)

dfu: build
	@test -n "$(DFU_PORT)" || (echo "ERROR: Zephyr DFU port not found"; exit 1)
	mcumgr-client -d $(DFU_PORT) -m 1024 -l 512 upload build/workspace/zephyr/zephyr.signed.bin
	@hash=$$(mcumgr-client -d $(DFU_PORT) list 2>/dev/null | awk '/^response:/{sub(/^response: /,""); p=1} p' | jq -r '.images[] | select(.slot==1) | .hash'); \
	test -n "$$hash" || (echo "ERROR: could not read slot 1 hash"; exit 1); \
	mcumgr-client -d $(DFU_PORT) test $$hash; \
	mcumgr-client -d $(DFU_PORT) reset
