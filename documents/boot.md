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
| 6 | 文字列・文字入力 | ✅ 完了 | 既存の`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`（`ReadKeyStroke`）をポーリングし、Enterまでの1行を8bit char（ASCII）の静的バッファ`input_buffer`に格納する`lisp_read_line`を追加。Backspaceは1文字削除し画面表示も`\b \b`で戻す。矢印キーなど`UnicodeChar==0`の制御キーは無視。入力中は打った文字をそのままエコー表示し、`lisp_print_ascii`でASCII文字列をCHAR16に変換してコンソールに再表示できるようにした。エンコーディングはASCIIを採用し、`LispSymbol`の`name`など既存コードと型を揃え、次のリーダー（トークナイザ）がそのまま`char *`として扱えるようにした。まだLispオブジェクトとしての文字列型（新タグ）は導入していない |
| 7 | プリンター | ✅ 完了 | `lisp_print(SystemTable, obj)`が`LispObject`を人間が読める形式でコンソールに出力する。`fixnum`は`lisp_print_fixnum`（符号付き10進、libcのitoa相当を自前実装）、`symbol`は`lisp_symbol_cell(obj)->name`を`lisp_print_ascii`で表示、`cons`は`car`を順に走査して`(a b c)`形式（末尾が`nil`のとき）または`(a . b)`ドット対形式（末尾がconsでもnilでもない値のとき）で表示、`nil`は`nil`と表示する。想定外のタグは`lisp_panic`で停止する |
| 8 | リーダー（S式パーサー） | ✅ 完了 | `lisp_read_from_buffer(str)`が文字列の先頭から1つのS式を読み取り`LispObject`を返す。`lisp_read`と`lisp_read_list`が相互再帰で括弧を処理し、括弧以外は`lisp_token_is_fixnum`/`lisp_token_to_fixnum`で整数リテラルかどうかを判定して`fixnum`または`lisp_intern`によるシンボルにする。空白（スペース・タブ・CR・LF）をトークン区切りとし、ネストした括弧・先頭`-`付きの負数をサポート。不正な入力（閉じ括弧の不足・単独の`)`など）は`lisp_panic`で停止する。動作確認はマイルストーン7のプリンターで結果を出力して目視確認した |
| 9 | 最小評価器（eval/apply） | ✅ 完了 | `lisp_eval(expr, env)`が`fixnum`/`nil`/`t`を自己評価し、シンボルは環境から変数として検索する。`quote`/`if`/`lambda`を特殊形式として（起動時にintern済みの`lisp_sym_quote`/`lisp_sym_if`/`lisp_sym_lambda`との`eq`比較で）判定し、それ以外のconsは関数呼び出しとして演算子を評価した上で`lisp_apply`する。関数値は新設の第4タグ`LISP_TAG_CLOSURE`（`LispClosure{params, body, env}`）で表現し、`lambda`は生成時の環境を閉じ込めたクロージャを返す（レキシカルスコープ、本文は単一式のみサポート）。環境は新しい構造体を増やさず既存のconsで作る連想リスト（`(symbol . value)`のリスト、`lisp_env_extend`/`lisp_env_lookup`/`lisp_env_bind_params`）として表現した。未束縛変数・関数でないものへの適用・引数個数不一致は`lisp_panic`で停止する。実装中に、マイルストーン8のリーダーが`nil`トークンを（`LISP_NIL`の代わりに）新規のシンボルとしてinternしてしまうバグを発見し修正した（プリンターでは両者が同じ`"nil"`という文字列で表示されるため見た目上区別できず、evalが`nil`をポインタ等価で真偽判定するようになって初めて表面化した） |
| 10 | 組み込みプリミティブ | ✅ 完了 | `car`/`cdr`/`cons`/`eq`/`atom`/`+`/`-`の7つをCで実装。マイルストーン9終了時点でタグ付きポインタの下位2bitは`cons`/`fixnum`/`symbol`/`closure`の4値すべてを使い切っていたため、新しいタグは増やさず`LispClosure`に4番目のフィールド`builtin`（Cの関数ポインタ、`typedef LispObject (*LispBuiltinFn)(LispObject args);`）を追加した。`lambda`由来の通常クロージャは`builtin == NULL`、組み込み関数は`params`/`body`/`env`を使わず`builtin`にC実装関数を設定したもの（`lisp_make_builtin`で生成）とし、両方とも同じ`LISP_TAG_CLOSURE`・同じ`lisp_apply`経路に乗る。`lisp_apply`は`closure->builtin != NULL`なら評価済み引数リストを渡してC関数を直接呼ぶ分岐を追加した。`+`/`-`は複数引数対応（`+`は0個以上の合計、`-`は1個なら符号反転・2個以上なら左から順に減算）。`lisp_builtins_init()`が7つのプリミティブをinternしグローバル環境に束縛して返し、`EfiMain`はこれを`global_env`として使う |
| 11 | REPL（最小Lisp起動） | 未着手 | 「入力読み取り→リーダー→eval→プリンター→次の入力待ち」のループを`EfiMain`から起動し、キーボードから対話的にS式を評価できる状態にする。ここに到達した時点を「最小のLispが動く」とみなす |

## 各マイルストーンの参考実装位置

- マイルストーン1〜10は`src/main.c`内に実装済み。
  - Hello World・画面クリア: `EfiMain`冒頭
  - メモリマップ取得・最大空き領域探索: `EfiMain`内、`GetMemoryMap`呼び出しから探索ループまで
  - Lispオブジェクトシステム: `// --- Lisp Object System ---`セクション（`LispObject`/`LispCons`/`LispClosure`（`builtin`フィールド含む）/`LispBuiltinFn`/`lisp_make_fixnum`/`lisp_is_cons`/`lisp_is_closure`/`lisp_heap_init`/`alloc_cons`/`lisp_make_closure`/`lisp_make_builtin`など）
  - cons/car/cdr構築・アクセサ・panic: 同セクション内`lisp_panic`/`lisp_assert_cons`/`lisp_cons`/`lisp_car`/`lisp_cdr`/`lisp_set_car`/`lisp_set_cdr`
  - シンボル・intern: 同セクション内`LispSymbol`/`lisp_alloc`/`lisp_intern`/`lisp_symbols_init`/`lisp_sym_t`/`lisp_sym_quote`/`lisp_sym_if`/`lisp_sym_lambda`
  - 文字列・文字入力: `// --- 文字入力 (milestone 6) ---`セクション（`input_buffer`/`lisp_read_line`/`lisp_print_ascii`）
  - プリンター: `// --- プリンター (milestone 7) ---`セクション（`lisp_print_fixnum`/`lisp_print`）
  - リーダー: `// --- リーダー (milestone 8) ---`セクション（`lisp_read`/`lisp_read_list`/`lisp_token_is_fixnum`/`lisp_token_to_fixnum`/`lisp_read_from_buffer`）
  - 評価器: `// --- 評価器 (milestone 9) ---`セクション（`lisp_env_extend`/`lisp_env_lookup`/`lisp_env_bind_params`/`lisp_eval`/`lisp_eval_list`/`lisp_apply`）
  - 組み込みプリミティブ: `// --- 組み込みプリミティブ (milestone 10) ---`セクション（`lisp_builtin_car`/`lisp_builtin_cdr`/`lisp_builtin_cons`/`lisp_builtin_eq`/`lisp_builtin_atom`/`lisp_builtin_add`/`lisp_builtin_sub`/`lisp_builtins_init`）
- マイルストーン11以降は、既存の単一ファイル構成（`src/main.c`にすべて追記し、`Makefile`はビルド対象を増やさない）を維持しつつ実装していく想定。ファイルを分割する場合はその時点で`Makefile`の`SRC`定義も見直す。

## 検証方針

各マイルストーン完了時に、`make build`でクロスコンパイルが通ることと、`make run`（Linux環境では
OVMFファームウェアパスを環境に合わせて差し替える）でQEMU上に起動し、想定した出力・動作がコンソール
上で確認できることの両方を確認してから次のマイルストーンに進む。
