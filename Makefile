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
BUILD_DIR := build/matter-ac-ncs

.PHONY: build pristine flash flash-erase attach

build:
	$(WEST) build -b $(BOARD) .

pristine:
	$(WEST) build --pristine -b $(BOARD) .

flash:
	$(WEST) flash --build-dir $(BUILD_DIR)

flash-erase:
	$(WEST) flash --build-dir $(BUILD_DIR) --erase

attach:
	$(WEST) attach
