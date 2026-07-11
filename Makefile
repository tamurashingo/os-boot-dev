
PWD = $(shell pwd)

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src
SRC = $(SRCDIR)/main.c $(SRCDIR)/lisp.c
HDRS = $(SRCDIR)/uefi.h $(SRCDIR)/lisp.h

TEST_LISP_SRC := $(wildcard test/lisp/*.lisp)
TEST_LISP_DST := $(patsubst test/lisp/%.lisp,esp_dir/test/%.lisp,$(TEST_LISP_SRC))

LISP_SRC := $(wildcard lisp/*.lisp)
LISP_DST := $(patsubst lisp/%.lisp,esp_dir/%.lisp,$(LISP_SRC))

STAMP_DIR := .make-stamps
IMAGE_STAMP := $(STAMP_DIR)/image-built

.PHONY: all setup image build run

all: build

setup: $(TEST_LISP_DST) $(LISP_DST)
	mkdir -p esp_dir/EFI/BOOT

esp_dir/test/%.lisp: test/lisp/%.lisp
	mkdir -p esp_dir/test
	cp $< $@

esp_dir/%.lisp: lisp/%.lisp
	cp $< $@

$(IMAGE_STAMP): Dockerfile
	mkdir -p $(STAMP_DIR)
	docker build -t uefi-builder .
	touch $(IMAGE_STAMP)

image: $(IMAGE_STAMP)

build: setup $(SRC) $(HDRS) $(IMAGE_STAMP)
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
