
PWD = $(shell pwd)

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src
SRC = $(SRCDIR)/main.c $(SRCDIR)/lisp.c
HDRS = $(SRCDIR)/uefi.h $(SRCDIR)/lisp.h

TEST_LISP_SRC := $(wildcard test/lisp/*.lisp)
TEST_LISP_DST := $(patsubst test/lisp/%.lisp,esp_dir/test/%.lisp,$(TEST_LISP_SRC))
TEST_NAMES := $(patsubst test/lisp/test-%.lisp,%,$(TEST_LISP_SRC))

# test-%パターンルールの追加により、GNU Makeがディレクトリ部を外してtest/lisp/test-*.lisp
# のbasenameを"test-%"に誤マッチさせ、循環依存として警告を出す問題を防ぐための
# 明示的な空レシピ(既存ファイルであり生成不要であることを教える)
$(TEST_LISP_SRC): ;

LISP_SRC := $(wildcard lisp/*.lisp)
LISP_DST := $(patsubst lisp/%.lisp,esp_dir/%.lisp,$(LISP_SRC))

STAMP_DIR := .make-stamps
IMAGE_STAMP := $(STAMP_DIR)/image-built

.PHONY: all setup image build run test

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
	docker run --rm --user "$$(id -u):$$(id -g)" -v "$(PWD)":/workspace uefi-builder \
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

# make test-<name>: test/lisp/test-<name>.lispだけをQEMUで実行し合否判定する。
# make test: 上記をtest/lisp/配下の全ファイルに対して順に実行し、まとめて合否判定する。
# (テストごとに毎回QEMUを起動し直すのは、LISP_MAX_SYMBOLS(256)の制約により
# 全テストファイルを1回の起動でまとめてloadするとシンボルテーブルが枯渇するため)
test-%: build
	python3 scripts/run_test.py $*

test: build
	@fail=0; \
	for name in $(TEST_NAMES); do \
		echo "=== test-$$name ==="; \
		python3 scripts/run_test.py $$name || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "=== FAILED ==="; exit 1; else echo "=== ALL PASS ==="; fi
