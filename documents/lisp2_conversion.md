# Lisp-1からLisp-2への移行マイルストーン

## 目的

これまでのLisp処理系はSchemeと同じLisp-1(変数と関数が単一のシンボル名前空間を共有する設計)
だった。CLOS(`documents/lisp_clos.md`)の導入に先立ち、ユーザーから
「変数のシンボルと関数のシンボルをそれぞれ分けるLisp2にする対応も入れる」という要望があり、
完全なCommonLisp忠実(レキシカル変数に束縛された関数値を呼ぶには`funcall`が必須、呼び出し位置の
bare symbolは常に関数セルのみを見る)なLisp-2への移行を行う。

調査の結果、「グローバル関数名をbareで値として渡す」という現在の設計に依存するコードが、
コンパイラ自身のブートストラップ(`lisp/compiler.lisp`)・`lisp/stdlib.lisp`・
milestone61の専用テスト(`test/lisp/test-call-dispatch.lisp`)にまで広範囲に存在することが
判明した。この影響範囲をユーザーに提示し、了解を得た上で完全なLisp-2化を進める。

マイルストーン番号は既存の1〜92に続く**93から**開始し、CLOS(旧93・94)は本マイルストーンの後
**96・97**へ採番を繰り下げる。

## ファイル構成

新規ソースファイルは追加しない。既存の`src/lisp.c`/`src/lisp.h`/`src/main.c`・
`lisp/compiler.lisp`/`lisp/stdlib.lisp`・`test/lisp/test-call-dispatch.lisp`を拡張・改修する。
新規テストファイルとして`test/lisp/test-symbol-function.lisp`を追加する。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 93 | 関数セルの追加とfuncall系API整備(`defun`は無変更) | 完了 | 土台のみを作る独立マイルストーン。`defun`はまだ`global_env`へ書き続けるため既存動作は変わらず、`make test`は無改修でPASSし続ける前提。`LispSymbol`構造体(`src/lisp.c:28-37`)へ`value`と対になる新規`fn`フィールド(未束縛は`LISP_NIL`)を追加し、symbol生成箇所2箇所(`lisp_create_local_symbol`/`lisp_make_uninterned_symbol`)へリセットを追記、GCマーク(`lisp_gc_mark`のsymbol分岐)を`fn`も辿るように変更する。新規Cビルトインとして`symbol-function`(`fn`を返す、未束縛ならpanic)・`%set-symbol-function`(`fn`への内部書き込みAPI、94でdefun等から使う)・`fboundp`(`fn != LISP_NIL`か)・`symbol-value`(既存の値セル取得の薄いラッパー)を追加する。リーダーへ`#'`の2文字先読み分岐(既存の`,`/`,@`と同型)を追加し`#'foo`を`(function foo)`へ展開、`function`特殊形式をツリーウォーク(`lisp_eval`)・コンパイラ(`compile-expr`)双方に追加する。`funcall`/`apply`(milestone61で既存)は無変更。 |
| 94 | 名前空間分離の実行(defun/組み込み関数/呼び出し位置/コンパイラ/VM/ブートストラップの一括対応) | 完了 | ブートストラップの読み込み順序制約(`lisp/compiler.lisp`はコンパイラ準備前提のためツリーウォークで読み込まれ、`lisp/stdlib.lisp`はその後コンパイル経由で読み込まれる)により、下記の変更はいずれか1つだけでは起動できず、1つの不可分なマイルストーンとして一括実施する。(1)ツリーウォークの`defun`/`defmacro`特殊形式と`lisp_builtin_establish_global_function`の書き込み先を`global_env`から93で追加した関数セル直接書き込みへ変更。(2)`lisp_builtins_init`内の55個の`env = lisp_env_extend(env, ...)`登録行を全て関数セルへの直接書き込みへ書き換え、関数の戻り値をvoid化し`main.c`の代入も削除。(3)ツリーウォークの呼び出し位置解決(`lisp_eval`内`LispObject fn = lisp_eval(op, env);`)を、`op`がsymbolならレキシカル`env`を一切見ずに関数セルのみを読むよう変更(未束縛ならunbound-function相当のpanic)。(4)新規VM opcode`OP_GLOBAL_FUNCTION_REF`(21番、読み取り専用、対応する書き込み版は不要)を追加。(5)コンパイラに`compile-global-ref`と同型の`compile-function-ref`を追加し、`compile-call`の演算子がbare symbolならこれを使うよう分岐変更、93の`function`特殊形式のコンパイルも同じ命令を共有する。(6)これらの変更と同時に、`lisp/compiler.lisp`の`mapcar`本体の`fn`直接呼び出しを`funcall`化、`macroexpand-all`系約15箇所の`(mapcar macroexpand-all ...)`を`(mapcar #'macroexpand-all ...)`化、`lisp/stdlib.lisp`の`>`定義`(apply < (reverse args))`を`(apply #'< (reverse args))`化する(これらの修正が無いと起動自体が止まるため後続マイルストーンへ先送りできない)。この時点で`test-call-dispatch.lisp`(milestone61)は前提が崩れて失敗する見込みのため一時的に許容し、95で復帰させる。 |
| 95 | 既存コードの`#'`化とドキュメント | 未着手 | `test/lisp/test-call-dispatch.lisp`を全面書き換え、bareで渡している`car`/`+`/`<`/`cd-double`/`cd-add`/`cd-lt`全箇所へ`#'`を付与する(検証意図自体はmilestone61と同じ「funcall/apply/reduce/sortが呼び出し先の種別を問わず一貫動作する」を保つ)。`lisp/stdlib.lisp`・`test/lisp/*.lisp`全体を広く監査し、94着手前のgrep(`mapcar`/`reduce`/`sort`/`apply`/`funcall`の引数)でカバーされない他パターン(`defvar`/`defparameter`/`setq`/`let`でグローバル関数名をbareで保持・受け渡し)も洗い出して`#'`化する。本ドキュメント(`documents/lisp2_conversion.md`)を作成し、`README.md`にリンクとドキュメント数の更新を反映する。 |

## スコープ外として明記する項目

- `flet`/`labels`(ローカル関数定義)。既存コードにローカル関数束縛の構文は無く、導入すると
  呼び出し位置解決が「関数セルのみを見る」では済まなくなり設計が複雑化するため見送る。
- `(setf (symbol-function 'x) ...)`のような汎用`setf`マクロ経由の書き込み構文(`setf`自体が
  未実装のため、`%set-symbol-function`を直接呼ぶ形に留める)。
- `boundp`(`symbol-value`のみ導入)。
- 既存の`is_special`(動的変数)機構自体の変更(値セルの扱いは無変更)。

## 検証方針

`make build`でクロスコンパイルが通ることを都度確認する。93は既存`make test`が無改修で全PASS
すること、94は`test-call-dispatch.lisp`のみ既知の失敗を許容しそれ以外の全フィクスチャがPASS
すること、95で`test-call-dispatch.lisp`を含む全フィクスチャがPASSすることを確認する。
`make test`だけでは検証できないpanicシナリオ(milestone78/81等と同様の方針)は個別のQEMU対話で
確認する:

- `%set-symbol-function`→`symbol-function`/`fboundp`/`#'foo`/`(function foo)`の一貫性。
- 未設定symbolの`fboundp`がnilを返し、`symbol-function`はpanicすること。
- `(defun m94-f (x) x)`定義後、`(fboundp 'm94-f)`がt、`(symbol-function 'm94-f)`が閉包を返し、
  `m94-f`をbareで値位置評価するとunbound variableでpanicすること。
