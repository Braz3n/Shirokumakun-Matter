TC := /home/zane/ncs/toolchains/7795df4459
WEST := $(TC)/usr/local/bin/west

export PATH := $(TC)/bin:$(TC)/usr/bin:$(TC)/usr/local/bin:$(TC)/opt/bin:$(TC)/opt/zephyr-sdk/arm-zephyr-eabi/bin:$(PATH)
export LD_LIBRARY_PATH := $(TC)/lib:$(TC)/lib/x86_64-linux-gnu:$(TC)/usr/local/lib:$(TC)/opt/nanopb/generator-bin
export PYTHONHOME := $(TC)/usr/local
export PYTHONPATH := $(TC)/usr/local/lib/python3.8:$(TC)/usr/local/lib/python3.8/site-packages
export ZEPHYR_TOOLCHAIN_VARIANT := zephyr
export ZEPHYR_SDK_INSTALL_DIR := $(TC)/opt/zephyr-sdk
export ZEPHYR_BASE := /home/zane/ncs/zephyr

BOARD := xiao_ble/nrf52840/sense
BUILD_DIR := build

.PHONY: build pristine flash flash-erase attach

build:
	$(WEST) build -b $(BOARD) .

pristine:
	$(WEST) build --pristine -b $(BOARD) .

JLINK := JLinkExe
JLINK_FLAGS := -nogui 1 -if SWD -speed 4000 -device NRF52840_xxAA -autoconnect 1

flash: build
	$(WEST) flash --build-dir $(BUILD_DIR)
	$(JLINK) $(JLINK_FLAGS) -CommanderScript scripts/jlink_go.jlink

flash-erase: build
	$(WEST) flash --build-dir $(BUILD_DIR) --erase
	$(JLINK) $(JLINK_FLAGS) -CommanderScript scripts/jlink_go.jlink

attach:
	$(WEST) attach

# DFU over USB-CDC ACM1 using mcumgr-client: https://github.com/vouch-opensource/mcumgr-client
DFU_PORT := $(shell for d in /dev/ttyACM*; do \
	info=$$(udevadm info $$d 2>/dev/null); \
	echo "$$info" | grep -q 'ID_USB_VENDOR_ID=2fe3' && \
	echo "$$info" | grep -q 'ID_USB_INTERFACE_NUM=02' && \
	echo $$d && break; done)

dfu: build
	@test -n "$(DFU_PORT)" || (echo "ERROR: Zephyr DFU port not found"; exit 1)
	mcumgr-client -d $(DFU_PORT) -m 1024 -l 512 upload build/matter-ac-ncs/zephyr/zephyr.signed.bin
	@hash=$$(mcumgr-client -d $(DFU_PORT) list 2>/dev/null | awk '/^response:/{sub(/^response: /,""); p=1} p' | jq -r '.images[] | select(.slot==1) | .hash'); \
	test -n "$$hash" || (echo "ERROR: could not read slot 1 hash"; exit 1); \
	mcumgr-client -d $(DFU_PORT) test $$hash; \
	mcumgr-client -d $(DFU_PORT) reset
