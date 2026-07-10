
PWD = $(shell pwd)

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src
SRC = $(SRCDIR)/main.c

STAMP_DIR := .make-stamps
IMAGE_STAMP := $(STAMP_DIR)/image-built

.PHONY: all setup image build run

all: build

setup:
	mkdir -p esp_dir/EFI/BOOT

$(IMAGE_STAMP): Dockerfile
	mkdir -p $(STAMP_DIR)
	docker build -t uefi-builder .
	touch $(IMAGE_STAMP)

image: $(IMAGE_STAMP)

build: setup $(SRC) $(IMAGE_STAMP)
	docker run --rm -v "$(PWD)":/workspace uefi-builder \
		x86_64-w64-mingw32-gcc -nostdlib -mno-red-zone -shared \
		-mno-stack-arg-probe \
		-Wl,--subsystem,10 \
		-Wl,--entry,EfiMain \
		-o $(TARGET) $(SRC)

clean:
	rm -rf esp_dir

run:
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd \
		-drive format=raw,file=fat:rw:./esp_dir
