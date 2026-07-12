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
import tempfile
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

# 起動確認・進行状況はstdout(実際のテストログ)と混ざらないようstderrに出す。
PROGRESS_INTERVAL = 5.0


def debug(msg):
    print("[debug] {}".format(msg), file=sys.stderr, flush=True)


def check_prerequisites():
    ok = True

    qemu_path = shutil.which("qemu-system-x86_64")
    if qemu_path is None:
        debug("qemu-system-x86_64 が見つかりません(PATH上に存在しない)")
        ok = False
    else:
        debug("qemu-system-x86_64: {}".format(qemu_path))

    if os.path.isfile(OVMF_CODE):
        debug("OVMF_CODE: {} ({} bytes)".format(OVMF_CODE, os.path.getsize(OVMF_CODE)))
    else:
        debug("OVMF_CODE が見つかりません: {}".format(OVMF_CODE))
        ovmf_dir = os.path.dirname(OVMF_CODE)
        if os.path.isdir(ovmf_dir):
            debug("{} の内容: {}".format(ovmf_dir, sorted(os.listdir(ovmf_dir))))
        else:
            debug("ディレクトリ自体が存在しません: {}".format(ovmf_dir))
        ok = False

    boot_efi = os.path.join(ESP_DIR, "EFI", "BOOT", "BOOTX64.EFI")
    if os.path.isfile(boot_efi):
        debug("BOOTX64.EFI: {} ({} bytes)".format(boot_efi, os.path.getsize(boot_efi)))
    else:
        debug("BOOTX64.EFI が見つかりません: {}".format(boot_efi))
        ok = False

    return ok


def make_init_lisp(name):
    return (
        '(load "test\\test-{name}.lisp")\n'
        '(write-line (if (run-test-{name}) "RESULT {name} PASS" "RESULT {name} FAIL"))\n'
        '(write-line "{done}")\n'
    ).format(name=name, done=DONE_MARKER)


def run_qemu_and_capture(sock_path):
    qemu_log = tempfile.NamedTemporaryFile(prefix="qemu-log-", suffix=".txt", delete=False)
    cmd = [
        "qemu-system-x86_64",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "format=raw,file=fat:rw:" + ESP_DIR,
        "-display", "none",
        "-no-reboot",
        "-serial", "unix:" + sock_path + ",server,nowait",
    ]
    debug("qemu起動: {}".format(" ".join(cmd)))
    qemu = subprocess.Popen(cmd, stdout=qemu_log, stderr=subprocess.STDOUT)
    debug("qemu pid={}".format(qemu.pid))

    def dump_qemu_log():
        qemu_log.flush()
        try:
            with open(qemu_log.name, "r", errors="replace") as f:
                content = f.read().strip()
        except OSError:
            content = ""
        if content:
            debug("qemuの標準出力/エラー:\n{}".format(content))
        else:
            debug("qemuは標準出力/エラーを何も出力していません")

    lines = []
    start = time.time()
    deadline = start + TIMEOUT_SECONDS
    sock = None
    try:
        last_progress = start
        while time.time() < deadline:
            if qemu.poll() is not None:
                debug("qemuがソケット接続前に終了しました(exit code={})".format(qemu.returncode))
                dump_qemu_log()
                return lines, False
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(sock_path)
                debug("シリアルソケットに接続しました ({:.1f}s経過)".format(time.time() - start))
                break
            except OSError:
                if time.time() - last_progress >= PROGRESS_INTERVAL:
                    debug("シリアルソケット接続待ち... ({:.0f}s経過, qemu存続中)".format(time.time() - start))
                    last_progress = time.time()
                time.sleep(0.2)
        if sock is None:
            debug("シリアルソケットへの接続がタイムアウトしました")
            dump_qemu_log()
            return lines, False

        sock.settimeout(1.0)
        buf = b""
        done = False
        last_progress = time.time()
        last_data = time.time()
        while time.time() < deadline:
            if qemu.poll() is not None:
                debug("qemuがテスト完了前に終了しました(exit code={})".format(qemu.returncode))
                dump_qemu_log()
                break
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    debug("シリアルソケットがクローズされました")
                    break
                last_data = time.time()
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
                now = time.time()
                if now - last_progress >= PROGRESS_INTERVAL:
                    debug("出力待ち... ({:.0f}s経過, 最後の受信から{:.0f}s)".format(
                        now - start, now - last_data))
                    last_progress = now
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
        try:
            qemu_log.close()
            os.remove(qemu_log.name)
        except OSError:
            pass


def main():
    if len(sys.argv) != 2:
        print("usage: run_test.py <test-name>", file=sys.stderr)
        return 2
    name = sys.argv[1]

    test_file = os.path.join(REPO_ROOT, "test", "lisp", "test-{}.lisp".format(name))
    if not os.path.isfile(test_file):
        print("no such test file: {}".format(test_file), file=sys.stderr)
        return 2

    if not check_prerequisites():
        print("test-{}: SETUP ERROR (前提条件が満たされていません、上記のdebugログを確認)".format(name))
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
