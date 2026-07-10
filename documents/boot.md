# ブートから最小Lisp起動までのマイルストーン

## 目的

このドキュメントは、`src/main.c`のスクラッチUEFIブートローダーを起点に、最終的に最小限の
Lisp（リーダー・評価器・プリンターを持ち、対話的にS式を実行できる状態）をブート直後に動かす
までの道筋を、マイルストーン単位で整理したものである。実装の詳細設計は各マイルストーンに着手
する際に別途行い、本ドキュメントは全体の見取り図として保守する。

前提となる制約（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src/main.c`にスクラッチで書く。
- UEFIのブートサービスが有効な間（`ExitBootServices`を呼ぶまで）は物理アドレスと仮想アドレスが
  一致（identity mapping）しているため、`PhysicalStart`をそのままポインタとして扱える。
- テストフレームワークは無いため、各マイルストーンの検証はQEMU/OVMF上で実際に起動し、コンソール
  出力を目視確認することで行う。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 1 | UEFIブート・Hello World | ✅ 完了 | PE32+として`BOOTX64.EFI`をビルドし、QEMU/OVMF上で`EfiMain`が呼ばれ、コンソールに文字列を表示できる |
| 2 | メモリマップ取得・最大空き領域の特定 | ✅ 完了 | `GetMemoryMap`で全メモリ記述子を取得し、`EfiConventionalMemory`のうち最大の連続領域（`heap_start`/`max_free_size`）を求める |
| 3 | Lispオブジェクトシステム＋簡易アロケータ | ✅ 完了 | タグ付きポインタ方式の`LispObject`（`cons`/`fixnum`）、`LispCons`、`lisp_heap_init`+`alloc_cons`によるバンプアロケータ |
| 4 | cons/car/cdrの構築・アクセサ関数 | ✅ 完了 | `alloc_cons()`を包んだ`lisp_cons(car, cdr)`構築ヘルパー、`lisp_car`/`lisp_cdr`/`lisp_set_car`/`lisp_set_cdr`アクセサを追加。各アクセサは`lisp_assert_cons`で引数がcons(TAG_CONS)であることを確認し、そうでなければ`lisp_panic`でエラーメッセージを表示して停止する |
| 5 | シンボルとNIL/T | ✅ 完了 | `LISP_TAG_SYMBOL`（下位2bit=10）と`LispSymbol`（固定長name配列）を追加。ヒープの汎用バンプアロケータ`lisp_alloc(size)`を切り出し、`alloc_cons`もこれを使うよう整理。`lisp_intern(name)`は線形探索の固定長テーブル（`lisp_symbol_table`）で同名シンボルを同一オブジェクトに束ね、`t`は起動時に`lisp_symbols_init()`でintern。`nil`は既存の即値`LISP_NIL`（アドレス0）のまま、intern対象にはしない設計とした（空リスト終端とfalse相当を1つの即値にまとめる簡略化） |
| 6 | 文字列・文字入力 | 未着手 | 既存の`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`（`ReadKeyStroke`）を使ってキーボードから1行分の入力を読み取り、入力バッファに文字列として保持する |
| 7 | プリンター | 未着手 | `LispObject`を人間が読める文字列に変換し、既存の`OutputString`パターンで出力する。`fixnum`は10進表示、`cons`は`(a . b)`または`(a b c)`形式、`nil`は`nil`と表示できるようにする |
| 8 | リーダー（S式パーサー） | 未着手 | 入力文字列をトークナイズし、括弧・シンボル・整数リテラルを解釈して`LispObject`の木（cons連結リスト）に変換する。ネストした括弧・空白区切り・負数程度をサポート。動作確認にはマイルストーン7のプリンターで結果を出力して目視確認する |
| 9 | 最小評価器（eval/apply） | 未着手 | まず自己評価（数値・`nil`/`t`）と`quote`のみを評価できるようにし、その後`if`、変数束縛（`lambda`/関数呼び出し）を段階的に追加する |
| 10 | 組み込みプリミティブ | 未着手 | `car`/`cdr`/`cons`/`eq`/`atom`/`+`/`-`など、evalが呼び出す基本関数をCで実装し、シンボルと結び付ける |
| 11 | REPL（最小Lisp起動） | 未着手 | 「入力読み取り→リーダー→eval→プリンター→次の入力待ち」のループを`EfiMain`から起動し、キーボードから対話的にS式を評価できる状態にする。ここに到達した時点を「最小のLispが動く」とみなす |

## 各マイルストーンの参考実装位置

- マイルストーン1〜5は`src/main.c`内に実装済み。
  - Hello World・画面クリア: `EfiMain`冒頭
  - メモリマップ取得・最大空き領域探索: `EfiMain`内、`GetMemoryMap`呼び出しから探索ループまで
  - Lispオブジェクトシステム: `// --- Lisp Object System ---`セクション（`LispObject`/`LispCons`/`lisp_make_fixnum`/`lisp_is_cons`/`lisp_heap_init`/`alloc_cons`など）
  - cons/car/cdr構築・アクセサ・panic: 同セクション内`lisp_panic`/`lisp_assert_cons`/`lisp_cons`/`lisp_car`/`lisp_cdr`/`lisp_set_car`/`lisp_set_cdr`
  - シンボル・intern: 同セクション内`LispSymbol`/`lisp_alloc`/`lisp_intern`/`lisp_symbols_init`/`lisp_sym_t`
- マイルストーン6以降は、既存の単一ファイル構成（`src/main.c`にすべて追記し、`Makefile`はビルド対象を増やさない）を維持しつつ実装していく想定。ファイルを分割する場合はその時点で`Makefile`の`SRC`定義も見直す。

## 検証方針

各マイルストーン完了時に、`make build`でクロスコンパイルが通ることと、`make run`（Linux環境では
OVMFファームウェアパスを環境に合わせて差し替える）でQEMU上に起動し、想定した出力・動作がコンソール
上で確認できることの両方を確認してから次のマイルストーンに進む。
