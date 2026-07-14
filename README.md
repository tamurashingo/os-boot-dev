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

UEFIから起動し、自己ホスティングされた標準ライブラリを持つLisp REPLが立ち上がります。
`defun`・`defmacro`・動的スコープ変数・非局所脱出（`block`/`return-from`）・エラー時の
REPL自動復旧・数値タワー（fixnum/bignum/float）・文字列/ベクタ型・パッケージ・
マーク＆スイープGCまで実装済みで、トップレベルの評価はLisp自身で書かれたコンパイラ＋
スタックマシン型バイトコードVMが既定の経路になっています（`defmacro`とrest-arg形式の
`defun`のみ、従来のツリーウォーク評価器へフォールバックします）。

- UEFIブート・Hello World・メモリマップ取得によるヒープ確保
- タグ付きポインタ方式のLispオブジェクトシステム（cons・fixnum・bignum・float・symbol・
  string・vector・closure）とマーク＆スイープGC（ヒープ残量が少なくなると自動発火、
  `(gc)`で手動起動も可能）
- S式のリーダー／プリンター（`'`/`` ` ``/`,`/`,@`の糖衣構文、文字列リテラル、コメント
  `;`に対応）
- レキシカルスコープを持つ評価器（`quote`/`quasiquote`/`if`/`lambda`/`defun`/`defmacro`/
  `let`/`let*`/`progn`/`setq`/`cond`/`and`/`or`/`when`/`unless`/`block`/`return-from`）と
  `defvar`/`defparameter`による動的スコープ変数
- 自作`setjmp`/`longjmp`を使ったエラー復旧: 評価中に`panic`が起きてもREPLの入力待ちへ
  自動的に戻る（致命的なリソース枯渇時のみ本当に停止する）
- `car`/`cdr`/`cons`/`eq`/`atom`/`+`/`-`/`<`などの組み込みプリミティブに加え、
  `list`/`append`/`reverse`/`mapcar`/`nth`/比較演算子/`gensym`/`macroexpand-1`/
  `rplaca`/`rplacd`/`make-vector`/`svref`/`svset`/`hash-code`/`sleep`など
- パッケージシステム（`LispPackage`の登録APIとシンボルのパッケージ帰属）
- `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`/`EFI_FILE_PROTOCOL`を使い、FAT32のESP上のLispファイルを
  `(load "filename")`で読み込み・評価。`(write-file "filename" content)`でESPへの書き込みも
  できる（QEMUの`fat:rw:`ドライバは書き込みコミットが不安定なため、実機や生ディスクイメージ
  向けの機能として位置づけ）
- `(write-line "text")`でシリアルコンソールへ直接1行出力できる。これを使い、
  ブート時に`EFI/BOOT/BOOTX64.EFI`と同じディレクトリの`init.lisp`があれば
  stdlib読込・自己テストの直後、REPL開始前に自動で読み込んで評価する（無ければ何もしない）。
  テスト用のLispコードと`write-line`によるPASS/FAIL出力を`init.lisp`に置いておくだけで、
  対話操作なしにQEMUのシリアル出力を見る（または`-serial file:...`でログに落として`grep`
  する）だけでユニットテストの結果が分かる
- 起動時に`lisp/compiler.lisp`（コンパイラ本体、コンパイラ準備状態フラグが立つ前提として
  ツリーウォークで読み込む）→`lisp/stdlib.lisp`（標準的な関数・マクロ、以後はコンパイル
  経由で読み込む）の順に自動で`load`し、標準ライブラリをLisp自身で定義した状態でREPLが
  起動する（自己ホスティング）
- スタックマシン型バイトコードVM（`vm-exec`）とLisp自身で書かれたコンパイラ
  （`lisp/compiler.lisp`の`compile-expr`系関数、クロージャ・upvalue・関数呼び出しに対応）が
  トップレベル評価・`defun`本体のコンパイルの既定経路になっている。グローバル参照は
  実行時にシンボル同一性で再解決されるため、`defun`同士の前方参照・相互再帰は従来の
  ツリーウォーク評価器と同じ挙動を保つ

![QEMU上でLisp REPLが動いている様子](documents/images/screenshot-milestone12.png)

![外部ファイルをloadして中の関数を呼び出している様子](documents/images/screenshot-milestone16.png)

詳細なマイルストーンは以下の7ドキュメントにまとめています。1〜67まで全て完了済みです。

- [`documents/boot.md`](documents/boot.md)（1〜11: UEFIブート〜最小Lisp REPL）
- [`documents/init_lisp.md`](documents/init_lisp.md)（12〜16: `defun`〜`load`）
- [`documents/bare_metal_lisp.md`](documents/bare_metal_lisp.md)（17〜29: CommonLisp相当を
  目指す拡張、標準ライブラリの自己ホスティングまで）
- [`documents/lisp_robustness.md`](documents/lisp_robustness.md)（30〜33: 自作setjmp/longjmp
  によるエラー復旧とマーク＆スイープGC）
- [`documents/lisp_vm.md`](documents/lisp_vm.md)（34〜46: スタックマシン型VM＋Lisp自身による
  コンパイラ）
- [`documents/lisp_vm_integration.md`](documents/lisp_vm_integration.md)（48〜67: VM/コンパイラを
  既定の評価器に統合するロードマップ）
- [`documents/lisp_package_system.md`](documents/lisp_package_system.md)（68〜84: パッケージ
  システムをCommonLispサブセットへ再設計するロードマップ、68〜70,72〜80完了・81〜84未着手。
  78で発見したVM/コンパイラの非tail位置let系スタックリークのうち`progn`は78で修正済み、
  `let`/`let*`/`or`側の根本修正は83〜84として追加）

## ソース構成

- `src/uefi.h` — UEFIの型・構造体・プロトコル定義（`EFI_SYSTEM_TABLE`・
  `EFI_BOOT_SERVICES`・`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`・`EFI_FILE_PROTOCOL`など）
- `src/lisp.h` / `src/lisp.c` — Lispインタプリタ本体（オブジェクトシステム・GC・
  リーダー・評価器・プリンター・組み込みプリミティブ・VM実行エンジン）
- `src/main.c` — ブートエントリポイント`EfiMain`（メモリマップ取得・ヒープ初期化・
  自己テスト群・REPLループ）
- `lisp/compiler.lisp` — 起動時にstdlib.lispより先に自動読み込みされるコンパイラ本体
  （マクロ展開・アセンブラ・`compile-expr`一式、Lisp自身で書かれている）
- `lisp/stdlib.lisp` — 起動時にcompiler.lispの後に自動読み込みされる標準ライブラリ
  （Lisp自身で書かれている）
- `test/lisp/*.lisp` — `esp_dir/test/`へコピーされ、REPLから`(load "test\\xxx.lisp")`で
  読み込んで実行するテストファイル群

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

## テストの実行

`test/lisp/`配下のテストファイルはREPLから手動で`(load "test\\test-xxx.lisp")`し、
定義された`run-test-xxx`関数を呼んで確認するのが基本ですが、`esp_dir/EFI/BOOT/`に
`init.lisp`を置いておくと、ブート時（stdlib読込・自己テストの直後、REPL開始前）に
自動で読み込まれます。`write-line`でPASS/FAIL等を出力しておけば、QEMUを`-serial`で
ファイルやパイプに繋ぐだけで、対話操作なしにテスト結果を確認できます。

この仕組みを自動化したのが`make test`/`make test-<name>`です。`scripts/run_test.py`が
テストファイルを1つだけ読み込む`init.lisp`を動的生成し、QEMUをheadlessで起動して
シリアル出力（unixソケット経由）を監視、`RESULT <name> PASS`/`FAIL`の判定後にQEMUを
終了させます（テストファイルは1回のQEMU起動につき1つだけ読み込むため、テストごとに
毎回QEMUを起動し直します）。

```sh
make test-vector   # test/lisp/test-vector.lispだけを実行
make test           # test/lisp/配下の全テストファイルを順に実行
```

OVMFファームウェアのパスは環境変数`OVMF_CODE`（デフォルト`/usr/share/OVMF/OVMF_CODE.fd`）、
タイムアウトは`TEST_TIMEOUT`（デフォルト300秒）で変更できます。
