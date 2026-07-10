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

UEFIから起動し、ミニマムなLisp REPLが立ち上がって`defun`・`macro`・文字列型・ディスク上の
Lispファイルを読み込む`load`まで使える状態になっています。

- UEFIブート・Hello World・メモリマップ取得によるヒープ確保
- タグ付きポインタ方式のLispオブジェクトシステム（cons・fixnum・symbol・closure）と
  バンプアロケータ
- S式のリーダー／プリンター（`'`/`` ` ``/`,`/`,@`の糖衣構文、文字列リテラルにも対応）
- レキシカルスコープを持つ評価器（`quote`/`quasiquote`/`if`/`lambda`/`defun`/`defmacro`）
- `car`/`cdr`/`cons`/`eq`/`atom`/`+`/`-`/`load`などの組み込みプリミティブ
- グローバル環境が永続化されたREPL（キーボードから対話的にS式を評価できる）
- `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`/`EFI_FILE_PROTOCOL`を使い、FAT32のESP上に置いた
  Lispファイルを`(load "filename")`で読み込み、定義した関数・マクロをその後のREPL入力
  から使える

![QEMU上でLisp REPLが動いている様子](documents/images/screenshot-milestone12.png)

![外部ファイルをloadして中の関数を呼び出している様子](documents/images/screenshot-milestone16.png)

詳細なマイルストーンは[`documents/boot.md`](documents/boot.md)（〜REPLまで）と
[`documents/init_lisp.md`](documents/init_lisp.md)（`defun`〜`load`まで）にまとめています。
現時点で両ドキュメントの全マイルストーンが完了しています。今後の拡張は
[`documents/bare_metal_lisp.md`](documents/bare_metal_lisp.md)（CommonLisp相当を目指すロード
マップ、未着手）に整理しています。

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
