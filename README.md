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

詳細なマイルストーンは以下の14ドキュメントにまとめています。1〜67まで全て完了済みです。

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
- [`documents/lisp_package_system.md`](documents/lisp_package_system.md)（68〜87: パッケージ
  システムをCommonLispサブセットへ再設計するロードマップ、68〜70,72〜84,87完了（フェーズA〜F・
  Hが完了）・85〜86未着手。78で発見したVM/コンパイラの非tail位置let系スタックリークのうち
  `progn`は78で修正済み、`let`/`let*`/`or`側の根本修正はローカル変数領域とオペランドスタックの
  分離という設計で83〜84として完了した。81着手時に発見した、
  `*package*`切替後は`common-lisp-user`の特殊形式・ビルトインが無修飾で使えなくなる制約の解消は
  85〜86として追加。フェーズH（87）はA〜Gと無関係な内容（コンパイラ自身の自己ブートストラップ関数
  の`do`/`while`化によるCスタック深度対策）だが、マイルストーン番号のグローバル連番管理を優先し
  本ドキュメントへ追記した）
- [`documents/lisp_lambda_list_keywords.md`](documents/lisp_lambda_list_keywords.md)（89:
  CommonLisp相当のラムダリストキーワード`&optional`/`&rest`/`&key`/`&aux`/`&allow-other-keys`の
  導入、完了。VM/バイトコードには手を入れずツリーウォークインタプリタのみを拡張し、コンパイル済み
  コードに直接ネストした`lambda`での使用は安全のため`lisp_panic`する設計）
- [`documents/lisp_variadic_comparison_operators.md`](documents/lisp_variadic_comparison_operators.md)
  （90: 比較演算子`>`/`=`/`<=`/`>=`/`/=`のCommonLisp相当の可変長引数対応、完了。`<`自体は
  milestone29から可変長対応済みで、89で導入した`&rest`を使いstdlib.lisp側の残り5演算子を
  書き換えた）
- [`documents/lisp_package_operations.md`](documents/lisp_package_operations.md)（91〜92:
  パッケージ指定子のsymbol/keyword対応、`do-symbols`系マクロ・`package-name`/`find-symbol`等の
  読み取り系関数、`shadow`/`import`/`delete-package`/`rename-package`等の破壊的操作系関数と
  `defpackage`の`:shadow`/`:import-from`/`:shadowing-import-from`句の追加、完了）
- [`documents/lisp2_conversion.md`](documents/lisp2_conversion.md)（93〜95: Lisp-1(Scheme相当の
  単一名前空間)からLisp-2(CommonLisp相当の変数/関数の名前空間分離)への移行、完了。93で
  シンボルへ関数セルを追加し`symbol-function`/`fboundp`/`#'`等のAPIを整備、94で`defun`・
  組み込み関数・呼び出し位置解決・コンパイラ・VM・ブートストラップを1マイルストーンとして一括で
  切り替え、95で既存コードの`#'`/`funcall`化を仕上げた）
- [`documents/lisp_clos.md`](documents/lisp_clos.md)（96〜97: CommonLisp Object System(CLOS)の
  最小サブセット導入、完了。96で`defclass`/`make-instance`/`slot-value`と単一継承を
  `LispClosure`の既存エスケープハッチパターンで実装、97で`defmethod`が暗黙生成する総称関数
  （Lisp-2化後の規約により対象symbolの関数セルへ格納）と、単一継承下での複数引数
  multiple dispatch（specificityの部分順序比較による`lisp_gf_select_method`）を追加した）
- [`documents/lisp_print_object.md`](documents/lisp_print_object.md)（98〜99: printerの
  未対応種別を解消するマイルストーン、完了。98で`print-object`をLisp拡張可能な総称関数として
  導入し（`defmethod print-object`でオーバーライド可能）、instanceの印字をこれに委譲。文字列
  連結プリミティブが無いためmethod本体がコンソールへ直接書き込む設計とし、新規ビルトイン
  `write-string`/`princ`を追加した。99でpackage（`#<PACKAGE name>`）とcompiled-function
  （`#<COMPILED-FUNCTION>`）の専用印字分岐を追加した。両者はCLOSのspecializerがユーザー
  定義クラスのみという制約(97)によりdefmethodでは上書きできない、class/instance/
  generic-functionと同じ非拡張パターンで実装した）
- [`documents/lisp_os_process.md`](documents/lisp_os_process.md)（100〜118: マルチプロセス化を
  最終目標としたロードマップ、100〜104完了・105〜118未着手。`process`クラス（CLOS）・
  `os:*all-processes*`レジストリ・`make-process`によるプロセス毎の隔離パッケージ生成
  （ベースパッケージ`common-lisp-user`を`use-package`し、fork側は`shadow`でローカル再定義）・
  `lock-package`によるベースパッケージロック・`process-suspend`/`process-resume`/
  `process-local-variable`・安全なリモートインスペクタREPL・Ctrl2回連続押下によるプロセス
  切替UIを対象とする。フェーズA（100・101）は`lisp_package_system.md`の旧85・86番
  （*package*切替時の特殊形式/ビルトイン可視性問題）を統合した前提条件フェーズ。100で
  `defun`/`if`/`let`等の特殊形式ディスパッチシンボル・`t`・`&optional`等のラムダリスト
  キーワードを`common-lisp-user`からexportし、`*package*`切替＋`use-package`済みなら
  無修飾で特殊形式が使えるようにした。101では`LISP_REGISTER_BUILTIN`マクロ自体を
  自動export経由に置き換え、`car`/`cons`/`in-package`等の全ビルトイン関数も同様に
  無修飾で使えるようにした。102では新規`os`パッケージ（`process`/`*all-processes*`/
  `get-all-processes`をexport）を`lisp/os-package.lisp`で作成し、続く`lisp/os.lisp`で
  `defclass os:process`（スロット`name`/`package`/`stackframe`/`env`/`status`、
  スロット名自体は`common-lisp-user`所属のまま）・`os:*all-processes*`・
  `os:get-all-processes`を定義した。`compiler.lisp`/`stdlib.lisp`と同様2ファイルに
  分割したのは、`load`がファイル全体を読み切ってから評価するため同一ファイル内では
  `defpackage`の効果が後続フォームの読み取りに反映されない制約を回避するため。103では
  Cビルトイン`%make-process`（`%make-class`と同じCビルトイン+薄いLispラッパーの
  パターン）を実装し、`os:make-process`（`&optional name`）から呼ぶようにした。名前
  省略時はgensymと同じカウンタ方式で`PROCESS-<N>`形式の一意名を生成し、ユーザー指定名は
  `os:*all-processes*`内の既存名と内容が一致すると`lisp_panic`する（文字列内容比較は
  str_data直接アクセスが必要でLisp側に無いため）。104ではper-processスタック領域と
  コンテキスト保存構造`LispProcessStack`を追加した。`lisp_process_stack_create`が
  `AllocatePages`で新規スタック領域を確保し、既存の`lisp_setjmp`/`lisp_longjmp`
  （milestone30、`jmpq`ベースで戻り先を問わない）を新規アセンブラ無しに再利用する形で、
  「未開始」コンテキスト（偽装した`rsp`/`rip`でトランポリン関数へ着地）と「開始済み」
  コンテキスト（前回の中断点から再開）の双方を扱える`lisp_context_switch(from, to)`を
  実装した。main/別スタックコンテキスト間で3往復するC自己テストで確認した

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
