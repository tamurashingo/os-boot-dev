#!/usr/bin/env python3
"""1つのテストファイル(test/lisp/test-<name>.lisp)をQEMU上で実行し、結果を判定する。

esp_dir/EFI/BOOT/init.lisp を動的に生成してテスト対象ファイルをloadし、
run-test-<name>関数の結果をwrite-lineでシリアル出力させる。QEMUを
headlessで起動し、シリアル出力(unix socket)を監視して合否を判定したら
QEMUを終了させる。LISP_MAX_SYMBOLS(256)の制約上、テストファイルは
1回のQEMU起動につき1つだけ読み込む。
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESP_DIR = os.path.join(REPO_ROOT, "esp_dir")
INIT_LISP_PATH = os.path.join(ESP_DIR, "EFI", "BOOT", "init.lisp")
OVMF_CODE = os.environ.get("OVMF_CODE", "/usr/share/OVMF/OVMF_CODE.fd")
TIMEOUT_SECONDS = float(os.environ.get("TEST_TIMEOUT", "300"))
DONE_MARKER = "TEST-DONE"

# OVMFはConOutをシリアルにも複製するため、画面クリア(ESC[2J)やカーソル移動(ESC[H)などの
# ANSI/VT100エスケープシーケンスがそのままシリアル出力に混入する。これをprintでそのまま
# 端末に出すと呼び出し元のターミナルまでクリアされてしまうため、表示前に除去する。
ANSI_ESCAPE_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def make_init_lisp(name):
    return (
        '(load "test\\test-{name}.lisp")\n'
        '(write-line (if (run-test-{name}) "RESULT {name} PASS" "RESULT {name} FAIL"))\n'
        '(write-line "{done}")\n'
    ).format(name=name, done=DONE_MARKER)


def run_qemu_and_capture(sock_path):
    qemu = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
            "-drive", "format=raw,file=fat:rw:" + ESP_DIR,
            "-display", "none",
            "-no-reboot",
            "-serial", "unix:" + sock_path + ",server,nowait",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    lines = []
    deadline = time.time() + TIMEOUT_SECONDS
    sock = None
    try:
        while time.time() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(sock_path)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            return lines, False

        sock.settimeout(1.0)
        buf = b""
        done = False
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                buf = ANSI_ESCAPE_RE.sub(b"", buf)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode(errors="replace").strip()
                    if text:
                        lines.append(text)
                        print(text, flush=True)
                    if DONE_MARKER in text:
                        done = True
                if done:
                    break
            except socket.timeout:
                continue
        return lines, done
    finally:
        if sock is not None:
            sock.close()
        if qemu.poll() is None:
            qemu.send_signal(signal.SIGTERM)
            try:
                qemu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                qemu.kill()
                qemu.wait(timeout=10)


def main():
    if len(sys.argv) != 2:
        print("usage: run_test.py <test-name>", file=sys.stderr)
        return 2
    name = sys.argv[1]

    test_file = os.path.join(REPO_ROOT, "test", "lisp", "test-{}.lisp".format(name))
    if not os.path.isfile(test_file):
        print("no such test file: {}".format(test_file), file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(INIT_LISP_PATH), exist_ok=True)
    with open(INIT_LISP_PATH, "w") as f:
        f.write(make_init_lisp(name))

    sock_path = "/tmp/os-boot-dev-test-{}.sock".format(name)
    if os.path.exists(sock_path):
        os.remove(sock_path)

    try:
        lines, done = run_qemu_and_capture(sock_path)
    finally:
        if os.path.exists(INIT_LISP_PATH):
            os.remove(INIT_LISP_PATH)
        if os.path.exists(sock_path):
            os.remove(sock_path)

    result_prefix = "RESULT {} ".format(name)
    result_line = next((l for l in lines if l.startswith(result_prefix)), None)

    if not done or result_line is None:
        print("test-{}: TIMEOUT (no result received within {}s)".format(name, TIMEOUT_SECONDS))
        return 1
    if result_line.endswith("PASS"):
        print("test-{}: PASS".format(name))
        return 0
    print("test-{}: FAIL".format(name))
    return 1


if __name__ == "__main__":
    sys.exit(main())
