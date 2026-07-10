# 最小Lispの拡張マイルストーン

## 目的

このドキュメントは、[`boot.md`](./boot.md)のマイルストーン1〜11で完成した最小のLisp REPL
（ブート直後にキーボードから対話的にS式を評価できる状態）を土台に、Lisp言語自体を拡張していく
ための道筋を、マイルストーン単位で整理したものである。`defun`による名前付き関数の定義、それを
支えるグローバル環境の永続化、`macro`、そしてディスク（FAT32のESP）上のLispファイルを読み込む
`load`を対象とする。`boot.md`と同様、実装の詳細設計は各マイルストーンに着手する際に別途行い、
本ドキュメントは全体の見取り図として保守する。

前提となる制約は`boot.md`と同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証はQEMU/OVMF上で実際に起動し、コンソール
  出力を目視確認することで行う。

## ファイル構成

2026-07-10のリファクタで、ソースは以下の3ファイルに分割されている（詳細は`boot.md`「各マイル
ストーンの参考実装位置」参照）。本ドキュメントのマイルストーン13〜16も、この構成の上に実装する。

- **`src/uefi.h`**: UEFIの型・構造体・プロトコル定義（ヘッダのみ）
- **`src/lisp.h`+`src/lisp.c`**: Lispインタプリタ本体（オブジェクトシステム・ヒープアロケータ・
  リーダー・評価器・プリンター・組み込みプリミティブ）
- **`src/main.c`**: boot処理・エントリポイント（`EfiMain`）・REPLループ

マイルストーンごとの実装対象ファイルの想定:

- 13 `defun`: `lisp.c`（評価器の特殊形式として追加し、`global_env`に束縛）
- 14 `macro`: `lisp.c`（`LispClosure`へのフラグ追加、評価器）
- 15 最小限のLisp文字列型: `lisp.c`（オブジェクトシステム・リーダー・プリンター）
- 16 `load`: `lisp.c`（`load`組み込み関数）＋`uefi.h`（`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`/
  `EFI_FILE_PROTOCOL`の新規定義）。`EfiMain`側でファイルシステムプロトコルの取得が必要になる場合は
  `main.c`も変更対象になる可能性がある（詳細はマイルストーン16着手時に決定）

新規ファイルを追加する場合は`Makefile`の`SRC`／`HDRS`定義も見直す。

## マイルストーン一覧

`boot.md`のマイルストーン1〜11（UEFIブート〜REPL）に続く番号で管理する。

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 12 | グローバル環境の永続化 | ✅ 完了 | `global_env`を`EfiMain`のローカル変数から、`// --- 評価器 (milestone 9) ---`セクション先頭で宣言する`static LispObject global_env`（ファイルスコープ）に変更した。`EfiMain`は`global_env = lisp_builtins_init();`と代入するだけで、以後は`defun`/`load`など今後の特殊形式がここを直接書き換えれば、その変更は次のREPL入力からも見える。関数呼び出し時の引数↔値バインディング自体（マイルストーン9の`lisp_env_bind_params`）は変更していない。検証は、REPLループ内で`lisp_eval`直後に一時的に`global_env = lisp_env_extend(global_env, lisp_intern("last"), result);`を追加し、`(+ 1 2)`→`3`の後に`last`→`3`、`(+ last 10)`→`13`の後に`last`→`13`と、ある入力での変更が次の入力から見えることをQEMU上で確認してから元に戻した。（2026-07-10のファイル分割リファクタで実装は`main.c`から`lisp.c`に移動し、`global_env`は非staticの`extern`共有変数になった。挙動は変更していない） |
| 13 | `defun`の実装 | ✅ 完了 | `(defun name (params...) body)`という特殊形式を`lisp_eval`に追加した。既存の`lisp_make_closure`（マイルストーン9）でクロージャを作り、マイルストーン12で永続化した`global_env`に`name`を束縛し、`name`自身を返す。同名での再定義は新しい束縛を先頭に追加するだけで良い（既存の`lisp_env_lookup`が最前面を優先して探索するため）。実装時に、`defun`が作るクロージャは自分自身の束縛が`global_env`に追加される前の環境スナップショットを捕捉するため、再帰呼び出しが`unbound variable`でpanicする問題を発見した。`lisp_env_lookup`が渡された`env`チェーン内で見つからなかった場合に現在の`global_env`も探すフォールバックを追加して解決した（レキシカルな束縛は変わらず優先され、グローバル関数の再帰・相互参照・後方参照が可能になった）。QEMU上で関数定義・呼び出し・再定義・再帰（`(defun sumto (n) (if (eq n 0) 0 (+ n (sumto (- n 1)))))`→`(sumto 3)`→`6`）・引数無し関数を確認した |
| 14 | macroの実装 | ✅ 完了 | `defmacro`特殊形式を追加し、`LispClosure`にマクロであることを示す`is_macro`フィールドを追加した（マイルストーン10の`builtin`フィールド追加と同じ手法でタグを増やさない）。`(defmacro name (params...) body)`は`lisp_make_macro`でマクロクロージャを作り`global_env`に束縛し、`name`自身を返す（`defun`と同型）。呼び出し対象がマクロの場合、`lisp_eval`は引数を評価せず未評価のまま仮引数に束縛してマクロ本文を評価し（マクロ展開）、その展開結果を呼び出し元の環境で通常のevalにかける2段階の評価とした。実装時、テスト用マクロのパラメータ名に`t`を使ったところ展開結果が意図せず壊れる現象に遭遇したが、これは`lisp_eval`がシンボル`t`を環境ルックアップより先に自己評価させる既存仕様（Common Lispの`T`/`NIL`同様、予約シンボルはローカル束縛で覆えない）によるもので、マクロ機能自体のバグではないと判明した。パラメータ名を変えて、`(defmacro my-if (test then else) (cons (quote if) (cons test (cons then (cons else nil)))))`を定義し、`(my-if 1 (+ 1 1) (car 5))`→`2`、`(my-if nil (car 5) (+ 2 2))`→`4`（いずれも選ばれなかった分岐の`(car 5)`が評価されずpanicしないこと）と、`my-if`単体の評価が`#<macro>`と表示されることをQEMU上で確認した |
| 15 | 最小限のLisp文字列型 | ✅ 完了 | ダブルクオートで囲んだ文字列リテラル（例: `"hello"`）をリーダーが読み取れるようにした。既存のタグ空間（cons/fixnum/symbol/closure）を使い切っているため、新しい2bitタグは増やさず、`LispClosure`に`str_data`（非NULLなら文字列データへのポインタ）/`str_len`フィールドを追加する方針（マイルストーン10の`builtin`フィールド、マイルストーン14の`is_macro`フィールドと同じescape hatch）を踏襲した。文字列はLISP_TAG_CLOSUREを共有するため、`lisp_eval`の変更は不要で、既存の「closureは自己評価する」という末尾のフォールバックだけで文字列も自己評価される。`lisp_make_string(chars, len)`がヒープに0終端バッファを確保してコピーし、`lisp_is_string`が`str_data != 0`で判定する。リーダーは`"`から次の`"`までを読み取る（エスケープシーケンスは未対応。`load`が使うファイル名程度の用途では不要と判断）。`lisp_reader_is_delim`にも`"`をデリミタとして追加した。プリンターは文字列を`"..."`形式で表示する（`lisp_print`の closure 分岐の先頭に追加）。QEMU上で`"hello"`→`"hello"`、`(atom "hello")`→`t`（consではないので atom）、`(eq "a" "a")`→`nil`（文字列は symbol と異なり intern されないため、内容が同じでも別オブジェクトで eq にならない）、`(cons "a" (cons "b" nil))`→`("a" "b")`を確認した |
| 16 | `load`の実装（FAT32のESPからのファイル読み込み） | 未着手 | `(load "filename")`という組み込み関数を追加し、UEFIの`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`/`EFI_FILE_PROTOCOL`（CLAUDE.mdの方針に従い必要なフィールドのみ新規に手書き）を使ってQEMUがマウントしているFAT32のESPからファイル内容を読み込む。読み込んだ内容は複数のトップレベルS式を含み得るため、マイルストーン8のリーダーを「バッファ終端に達したら`lisp_panic`せずに読み込み終了とみなす」ように拡張し、各S式を順にマイルストーン12の永続グローバル環境で評価する。読み込んだファイル内の`defun`/`defmacro`による定義がその後のREPL入力からも使えることを確認する |

## 追加機能: quote/quasiquoteのreader糖衣構文とmacroとの連携（2026-07-10）

マイルストーン12〜16の番号には含まれないが、macro（マイルストーン14）を書きやすくするための
拡張として、ユーザーの明示的な指示により以下を実装した。✅ 完了。

- **reader**: `lisp_read`に`'`/`` ` ``/`,`/`,@`の4つの糖衣構文を追加した。`'expr`→
  `(quote expr)`、`` `expr ``→`(quasiquote expr)`、`,expr`→`(unquote expr)`、
  `,@expr`→`(unquote-splicing expr)`と読み替える（`lisp_reader_is_delim`にもこの3文字を
  追加し、トークンの区切りとして機能するようにした）。
- **評価器**: `lisp_qq_expand`（`quasiquote`のテンプレートを再帰的に展開する内部ヘルパー）と
  `lisp_append`（`,@`のスプライシング用）を追加し、`lisp_eval`に`quasiquote`特殊形式を追加した。
  `(unquote x)`に一致した箇所はenv上で`x`を評価した値に置き換わり、リスト要素が
  `(unquote-splicing x)`の場合はその評価結果（リスト）を周囲のリスト構造にそのまま継ぎ足す。
  それ以外のconsは再帰的に展開して再構築し、atomはそのまま返す（自己クオート）。
  **スコープ上の制限**: ネストしたquasiquote（backquoteの中にさらにbackquoteが現れるケース）の
  深度追跡は実装していない。単純化のため、内側のquasiquoteも同じ深さのまま展開される
  （標準的なCommon Lisp/Schemeの`depth`を1段ずつ増減させる挙動とは異なる）。今回のmacro用途
  では単一階層のbackquoteしか使わないため、実用上は問題にならないと判断した。
- **QEMUでの確認**: `'a`→`a`、`'(1 2 3)`→`(1 2 3)`、`` `(1 2 3) ``→`(1 2 3)`、
  `` `(1 ,(+ 2 3) 4) ``→`(1 5 4)`、`` `(1 ,@(cons 2 (cons 3 nil)) 4) ``→`(1 2 3 4)`を確認した。
  さらにmacroとの連携として、マイルストーン14で`cons`/`quote`を手書きしていた`my-if`マクロを
  backquoteで書き直した`(defmacro my-if2 (test then else) `(if ,test ,then ,else))`を定義し、
  `(my-if2 1 (+ 1 1) (car 5))`→`2`、`(my-if2 nil (car 5) (+ 2 2))`→`4`
  （選ばれなかった分岐の`(car 5)`が評価されずpanicしないこと）を確認し、macro本文が
  quasiquoteで簡潔に書けることを実証した。

## 検証方針

`boot.md`と同じ方針を踏襲する。各マイルストーン完了時に、`make build`でクロスコンパイルが通ることと、
`make run`（Linux環境ではOVMFファームウェアパスを環境に合わせて差し替える）でQEMU上に起動し、想定した
出力・動作がコンソール上で確認できることの両方を確認してから次のマイルストーンに進む。
