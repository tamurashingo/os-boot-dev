# os-boot-dev

フリースタンディングCでスクラッチから書く、UEFIブートローダー兼ミニマムなLisp OSです。
PE32+実行ファイル（`BOOTX64.EFI`）としてビルドし、QEMU/OVMF上で動作します。

## このプロジェクトについて

- 現代的なx86_64、UEFIの構成のPCでbootするOSを書いてみる
- UEFIの提供する機能をふんだんに使い、どのくらいラクにつくれるのかを試してみる
- インタラクティブなシェルとしてのLispを実装する

UEFIの型・構造体・プロトコルはいったん手書きでやってます。
目的はモダンなOSがどうやって起動するのかを一から学ぶことです。

## 現在の状態

UEFIから起動し、そのままミニマムなLisp REPLが立ち上がるところまで実装済みです。

- UEFIブート・Hello World・メモリマップ取得によるヒープ確保
- タグ付きポインタ方式のLispオブジェクトシステム（cons・fixnum・symbol・closure）と
  バンプアロケータ
- S式のリーダー／プリンター
- レキシカルスコープを持つ評価器（`quote`/`if`/`lambda`）
- `car`/`cdr`/`cons`/`eq`/`atom`/`+`/`-`などの組み込みプリミティブ
- グローバル環境が永続化されたREPL（キーボードから対話的にS式を評価できる）

![QEMU上でLisp REPLが動いている様子](documents/images/screenshot-milestone12.png)

今後は`defun`・`macro`・文字列型・ディスク上のLispファイルを読み込む`load`を実装していく
予定です。詳細なマイルストーンは[`documents/boot.md`](documents/boot.md)（〜REPLまで）と
[`documents/init_lisp.md`](documents/init_lisp.md)（`defun`以降）にまとめています。

## ソース構成

- `src/uefi.h` — UEFIの型・構造体・プロトコル定義（`EFI_SYSTEM_TABLE`・
  `EFI_BOOT_SERVICES`・`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`など）
- `src/lisp.h` / `src/lisp.c` — Lispインタプリタ本体（オブジェクトシステム・ヒープ
  アロケータ・リーダー・評価器・プリンター・組み込みプリミティブ）
- `src/main.c` — ブートエントリポイント`EfiMain`（メモリマップ取得・ヒープ初期化・
  REPLループ）


## ビルド・実行

クロスコンパイラ（`x86_64-w64-mingw32-gcc`）はDockerコンテナ内に用意しています。

```sh
make build   # Dockerイメージをビルドし、esp_dir/EFI/BOOT/BOOTX64.EFIを生成
make run     # QEMU/OVMFでBOOTX64.EFIを起動
```

`make run`のOVMFファームウェアパスはmacOS/Homebrew向けの設定になっているため、Linux上
では`Makefile`内のパスをローカルのOVMFコードファイル（例:
`/usr/share/OVMF/OVMF_CODE.fd`）に差し替えてから実行してください。

その他のターゲット:

- `make image` — Dockerイメージのみビルド／更新
- `make setup` — ESP用のディレクトリ構成のみ作成
- `make clean` — `esp_dir`を削除
