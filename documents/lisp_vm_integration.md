# VM/コンパイラを既定の評価器に統合するマイルストーン

## 目的

このドキュメントは、`lisp_vm.md`（マイルストーン34〜46）で完成した、Lisp自身で書かれた
コンパイラ（`compile-expr`等、`lisp/stdlib.lisp`）とスタックマシン型VM（`lisp_vm_exec`、
`src/lisp.c`）を、`(compile-and-run expr)`を明示的に呼んだときだけ動く実験的な経路から、
REPL・`defun`・`load`がデフォルトで使う評価経路そのものへ格上げするためのマイルストーン群である。
現在は`lisp_eval`/`lisp_apply`（ツリーウォーク型インタプリタ、マイルストーン1〜33）が唯一の
デフォルト経路であり、コンパイラ/VMは完全に独立して並存しているだけで、`defun`が作る関数も
常にインタプリタ用クロージャである。

本ロードマップの完了時点で目指す状態は次の3点である:

1. REPL・`load`によるトップレベル評価が、既定で「マクロ展開→コンパイル→VM実行」を経由する
   （フェーズ1）。
2. `defun`が定義と同時に本体をコンパイルし、コンパイル済みクロージャを`global_env`へ束縛する
   （フェーズ2）。
3. 起動時の標準ライブラリ読み込み自体もコンパイル駆動の経路を通る（フェーズ3）。

ツリーウォーク・インタプリタ（`lisp_eval`/`lisp_apply`）は削除しない。マクロ展開
（`lisp_macroexpand_1`）はマクロ本体を未評価の引数フォームに対して実際に評価するインタプリタ
操作であり、コンパイル時（VM実行より前）に発生するため、本ロードマップ完了後も恒久的に
残り続ける（詳細は「設計にあたり確定している方針」参照）。

前提となる制約は他のドキュメントと同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証は`make test`（`test/lisp/`配下の
  フィクスチャをQEMU/OVMFヘッドレス起動で実行するハーネス）とREPL基本動作の目視確認で行う。

なお、`src/main.c`・`src/lisp.c`のコメントを確認したところ、「milestone47」は本ロードマップとは
無関係な既存機能（`init.lisp`自動読み込み・`write-file`等のテスト用ファイルI/O）に既に使われて
おり、`documents/*.md`に未記載のまま存在する。したがって本ロードマップのマイルストーン番号は
**48から**始める。

### 設計にあたり確定している方針

- **段階的フォールバック方式（2軸）。** 一度に全経路を切り替える「フラグデー」は行わない。
  - 軸1（トップレベル）: 統一評価ドライバは、対象フォームの先頭が`compile-expr`が未対応の
    特殊形式である場合、または`defmacro`である場合、そのフォーム全体を旧来の`lisp_eval`へ
    フォールバックする。この振り分け対象の集合は各マイルストーンの完了に伴って単調に縮小し、
    フェーズ1〜2の完了後は`defmacro`のみが残る（恒久的な唯一の想定ケース）。
  - 軸2（呼び出し境界、双方向）: VMの`OP_CALL`は呼び出し先がコンパイル済みクロージャでない場合
    `lisp_apply`へ委譲し、`lisp_apply`は呼び出し先がコンパイル済みクロージャの場合`lisp_vm_run`へ
    委譲する。これにより`mapcar`等の既存高階ビルトインやマクロ呼び出し自体が、コンパイル済み・
    インタプリタ済みいずれのクロージャも区別なく第一級の値として扱える。
  - この2軸のフォールバックにより、各マイルストーンを1つずつ有効化しても既存の`test/lisp/*.lisp`
    全フィクスチャとREPLの既存動作が回帰しないまま段階的に移行できる。
- **グローバル参照は実行時にシンボル同一性で再解決する。** 新設する`OP_GLOBAL_REF`/
  `OP_GLOBAL_SET`は、コンパイル時に静的な位置（フレームオフセット等）へ解決するのではなく、
  既存の`lisp_env_lookup`/`lisp_env_set`と同じ「呼び出しごとに`global_env`を捜査する」方式を
  踏襲する。これにより`defun`同士の前方参照・相互再帰という既存の（意図的な）挙動を破壊しない。
- **`lisp_eval`/`lisp_apply`は恒久的に削除しない。** マクロ展開は本質的にインタプリタ操作であり、
  `defmacro`は本ロードマップ完了後も恒久的にツリーウォーク経路へフォールバックし続ける、唯一
  かつ想定内のケースとして扱う。
- **コンパイラ準備状態ゲート。** `lisp/compiler.lisp`（フェーズ3で`stdlib.lisp`から分離する）
  自身の読み込みという鶏と卵問題を、ファイル名の特別扱いではなく明示的な準備状態フラグ（既定
  `false`）で解決する。フラグが`false`の間、統一評価ドライバは常にツリーウォークする。
- **`defun`の束縛機構自体は変えない。** コンパイル対象（クロージャの種類）が変わるだけで、
  `global_env`への束縛方法（`lisp_env_extend`、線形alist、呼び出し時の再スキャン）は変更しない。
- **VMデータスタックはpanicで必ずリセットされる。** `vm_sp`/`vm_stack`は現状panicのlongjmpで
  復元されず、panicのたびに状態が破損しうる。panic復帰点で明示的に`vm_sp`をゼロへ戻す修正を
  フェーズ1着手前の前提条件とし、それ以外の場所ではVM状態の生存を仮定しない。

## ファイル構成

新規ソースファイルは追加しない（フェーズ3の`lisp/compiler.lisp`分離を除く）。既存の3ファイル
構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／`src/main.c`）と、コンパイラ本体を持つ
`lisp/stdlib.lisp`（フェーズ3で`lisp/compiler.lisp`と`lisp/stdlib.lisp`に分割）をそのまま使う。
検証用フィクスチャは既存の`test/lisp/`配下に追加する。

## マイルストーン一覧

`lisp_vm.md`のマイルストーン34〜46に続く番号で管理する（47は本ロードマップと無関係な既存機能に
使用済みのため、48から開始する）。

### フェーズ1: 評価器の統合（前提整備を含む）

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 48 | VMスタックのpanic安全性修正 | 完了 | `vm_sp`/`vm_stack`がpanicのlongjmpで復元されない問題を修正する。REPLのpanic復帰点（`lisp_setjmp`によるトラップ復帰直後）で`vm_sp`を確実にゼロへリセットする処理を追加し、panicの繰り返しによるVM状態破損・GCルート汚染・最終的なVMスタックオーバーフローを防ぐ。VMが既定経路になる前に解決すべき安全性の前提条件。`lisp_vm_reset_stack()`を新設し、`src/main.c`のREPLループ先頭（`lisp_setjmp`直後）で毎回呼ぶ。 |
| 49 | VMブリッジ固定長バッファの拡張 | 完了 | `vm-make-closure`/`vm-exec`が使うCスタック上のステージング用固定長バッファ（`VM_BRIDGE_MAX_BYTECODE`/`_CONSTANTS`/`_UPVALUES`）を、実サイズの関数のコンパイル結果に耐える大きさへ拡張する。超過時のpanicメッセージも診断しやすい形にする。256/64/32を2048/256/128へ拡張し、超過時のpanicメッセージに上限値(16進)を含める`lisp_panic_vm_bridge_limit_exceeded`を新設した。 |
| 50 | compile-and-runのマクロ展開配線修正 | 完了 | `compile-and-run`が`macroexpand-all`を経由せず直接`compile-expr`を呼んでいる既存の配線漏れを修正し、マクロを含む式が正しく展開されてからコンパイルされることを回帰確認する。修正の過程で`lisp_macroexpand_1`(`src/lisp.c`)がop位置のsymbolを`lisp_eval`でフルに評価していたため、再帰呼び出し等でローカル変数が呼び出し位置に来ると「unbound variable」でpanicする既存バグを発見。マクロは常にglobal_envにのみ束縛される設計方針を踏まえ、global_envだけを見る非panicな`lisp_lookup_global_macro_candidate`に置き換えて修正した。 |
| 51 | グローバル参照解決（OP_GLOBAL_REF/OP_GLOBAL_SET） | 完了 | レキシカルスコープ外のシンボルを裸の定数としてpushしていた既存の（誤った）フォールバックを廃止し、新設`OP_GLOBAL_REF`/`OP_GLOBAL_SET`で実行時に`global_env`を解決するようにした。`src/lisp.h`/`src/lisp.c`に両opcodeを追加し、実装は既存の`lisp_env_lookup`/`lisp_env_set`をそのまま`global_env`に対して呼ぶだけとした（`lisp_vm_run`の定義位置がこの2関数より前のため前方宣言を追加）。`lisp/stdlib.lisp`の`compile-variable-ref`/`compile-setq`を、`compile-resolve`がnilを返した場合に旧来の（誤った）フォールバックへ落ちる実装から新opcodeを発行する実装へ書き換えた。`t`とkeywordパッケージのsymbolは引き続き自己評価する定数として`compile-literal`に回す必要があり、この判定用に新規Cビルトイン`symbolp`/`keywordp`を追加した。この変更に伴い、旧フォールバック挙動をそのまま期待値としていた`test-compile-expr.lisp`の既存2テスト（let内の未解決symbol参照）の期待バイト列を`OP_GLOBAL_REF`基準に更新し、`test-compile-and-run.lisp`にグローバル変数の参照・`setq`・グローバル関数呼び出しの回帰テストを追加した。 |
| 52 | OP_CALLの汎用ディスパッチ化（VM→インタプリタ方向） | 完了 | `src/lisp.c`の`lisp_vm_run`内`OP_CALL`が、呼び出し先が非コンパイル済み（`lisp_is_compiled`が偽）の場合に即panicしていた挙動を、`lisp_apply`へフォールバックする経路に変更した。フォールバック時はvm_stack上の生の引数値（コンパイル済み呼び出し用のボックス化はしない）をnargs個そのままLispの評価済みリストへ組み立て、`lisp_apply(fn_obj, args)`へ渡す（`lisp_apply`はCビルトインなら関数ポインタを直接呼び、従来のインタプリタクロージャなら`lisp_eval`で本体を評価する。非関数ならそこで既存通りpanicする）。`lisp_vm_run`は`lisp_apply`より前に定義されているため前方宣言を追加した。Lisp側の`compile-call`（`lisp/stdlib.lisp`）は元から呼び出し先の種別を区別せず汎用の`OP_CALL`を発行していたため変更不要だった。`test-compile-and-run.lisp`にCビルトイン（`atom`）呼び出しとstdlib.lispの`defun`によるインタプリタクロージャ（`list`）呼び出しの回帰テストを追加した。 |
| 53 | lisp_applyのコンパイル済みクロージャ対応（インタプリタ→VM方向） | 完了 | `src/lisp.c`の`lisp_apply`に、`closure->bytecode != 0`（コンパイル済み）ならOP_CALLと同じ規約（引数をvm_stackへ積み、FP相対でその場でボックス化してから`lisp_vm_run`を呼ぶ）で委譲する分岐を、既存の`builtin`分岐・通常クロージャ分岐より前に追加した。引数個数チェック（`closure->nargs`との不一致でpanic）もOP_CALLと同じ挙動にした。`lisp_apply`は`lisp_vm_run`より後方で定義されているため前方宣言は不要だった。`lisp_eval`の通常の関数呼び出し経路は元から`lisp_apply`を呼ぶだけなので変更不要で、ツリーウォークインタプリタから直接コンパイル済みクロージャを呼ぶケースと、`mapcar`（stdlib.lispのdefunによるインタプリタクロージャ）へコンパイル済みクロージャを渡すケースの双方を`test-compile-and-run.lisp`の回帰テストとして追加した。異常系（コンパイル済みクロージャを誤った引数個数で呼ぶ）は一時ファイルで個別に確認し、OP_CALLと同じ「VM function called with wrong number of arguments」panicで安全に停止することを確認した後、ファイルを削除した（`make test`には含めない）。 |
| 54 | compile-expr: progn/let*/cond/and/or/when/unless対応 | 完了 | VMには`dup`/`pop`に相当するopcodeが無く、`or`や`cond`のテスト専用クローズ（`(cond (5))`が5を返す等）はテスト値を再評価せずに条件分岐へ持ち込む必要があるため、生のIRを手で組み立てず「新しいS式（`if`/`let`/`progn`/相互再帰する`cond`/`and`/`or`）を組み立てて`compile-expr`へ再度渡す」というS式レベルの脱糖として実装した。これにより局所変数のボックス化・ラベル発行・ジャンプ先解決は既存の`compile-if`/`compile-let`にそのまま委譲される。生成する束縛名の衝突回避には既存の`gensym`（milestone20）を使用した。`macroexpand-all`がこの7フォームすべてを既に特殊形式として認識し本体を再帰的に展開済みのため（`compile-and-run`は`compile-expr`の前に1回`macroexpand-all`を通す、milestone50）、ここで組み立てる新しいラッパー式を再度展開する必要はない。`lisp/stdlib.lisp`に`compile-progn`/`compile-let-star`/`compile-and`/`compile-or`/`compile-cond`/`compile-when`/`compile-unless`とその補助関数を追加し、`compile-expr`のディスパッチにこの7フォームを追加した。実装作業中、`test-compile-expr.lisp`実行時に`stdlib.lisp`本体とテストファイル自身が定義するシンボルの合計が既存の`LISP_MAX_SYMBOLS`(256、パッケージごとの固定長シンボルテーブル)を超え「symbol table exhausted」でpanicする既存の容量限界に到達したため、`src/lisp.c`の`LISP_MAX_SYMBOLS`を512へ拡張して解消した（本milestoneのコード自体のバグではなく、`lisp/stdlib.lisp`が増え続けることで表面化した既存の容量制約）。テストは`test-special-forms.lisp`(milestone17、ツリーウォーク側の同じ7フォームのテスト)と同じ入出力の組を`test-compile-and-run.lisp`に`compile-and-run`経由で追加し、`and`/`or`の短絡評価（未評価branchが評価されるとpanicする式を使って安全性を確認）も含めて既存挙動と一致することを確認した。 |
| 55 | compile-expr: block/return-from対応 | 完了 | `if`/`let`/`lambda`に還元できない非局所脱出のため、milestone54とは異なりVMに新規opcode`OP_BLOCK`/`OP_RETURN_FROM`(`src/lisp.h`、18/19)を追加した。既存インタプリタの`lisp_return_tag`/`lisp_return_value`グローバル(milestone19、`src/lisp.c`)をVMからもそのまま共有し、新設のシグナル用グローバルは増やしていない。`block`の本体は`(lambda () (progn . body))`相当の引数無しクロージャとしてコンパイルし(`compile-lambda`の既存upvalue捕捉機構をそのまま再利用)、`OP_BLOCK`がそれを直接呼び出してから戻り値を見る前に`lisp_return_tag`を確認する(一致すれば捕捉してクリアし値をpush、不一致ならこのフレームも早期returnして伝播)。`OP_RETURN_FROM`はシグナルをセットしてから`OP_RETURN`と同様に即returnする。lambdaの本体は独立にassembleされた別のbytecode配列(閉じたC再帰呼び出し)であり生のジャンプ命令ではブロック境界を越えられないため、これがC呼び出しスタックを使った非局所脱出の唯一の手段になる。この機構が機能するには、`return-from`がblockに直接ラップされていない通常の関数呼び出しの奥深くで発生した場合にも中間のフレームを正しく素通りさせる必要があるため、既存の`OP_CALL`ハンドラの両分岐(コンパイル済み呼び出し・`lisp_apply`委譲)にも、ネストした呼び出しから戻った直後に`lisp_return_tag`を確認し立っていれば即座に伝播するチェックを追加した。`lisp/stdlib.lisp`に`compile-block`/`compile-return-from`と新opcode定数を追加し、`compile-expr`のディスパッチに追加した。`asm-instr-length`は既に1オペランド命令を汎用的に扱えるため変更不要、`macroexpand-all`も`block`/`return-from`を既に特殊形式として認識済みだったため変更不要だった。テストは`test-block-return.lisp`(milestone19)のケース1〜6(基本捕捉・return-from無しはprognと同じ・以降のbody評価をスキップ・入れ子blockの内側/外側タグ・再帰呼び出しを何段か経由した脱出)を`test-compile-and-run.lisp`に`compile-and-run`経由で追加し一致を確認した。ケース7〜9(let/let*の動的変数復元)は`compile-let`/`compile-let*`が特殊変数に未対応(milestone57)のため対象外とした。 |
| 56 | compile-expr: quasiquote対応 | 完了 | 既存インタプリタの`lisp_qq_expand`(`src/lisp.c`)と全く同じ再帰構造を、milestone54と同じ「新しいS式を組み立てて`compile-expr`へ再度渡す」脱糖として実装した(生のIRを自分で組み立てない)。`lisp/stdlib.lisp`に`compile-qq-desugar`(非コンスは`(quote form)`、`(unquote x)`はxそのもの、先頭要素が`(unquote-splicing x)`なら`(append x <残りを再帰>)`、それ以外のコンスは`(cons <carを再帰> <cdrを再帰>)`を組み立てる)と`compile-quasiquote`を追加し、`compile-expr`のディスパッチに`quasiquote`を追加した。`append`は`lisp/stdlib.lisp`本体で定義済みの通常のインタプリタクロージャで、milestone52のOP_CALL→`lisp_apply`委譲経由でそのまま呼べるため新規実装は不要だった。ネストしたquasiquote自体を特別扱いしない点(内側のunquote/unquote-splicingが外側と同じ深さで展開される)も`lisp_qq_expand`の既存の単純化をそのまま継承した。既存の`macroexpand-all`は`quasiquote`の内側を一切展開しないため(`macroexpand-all-forms`、opが`quasiquote`ならformをそのまま返す)、unquote内でマクロを呼ぶケースはツリーウォーク（`lisp_eval`が呼び出し式ごとに遅延的にマクロ判定するため展開される）とコンパイル経路（そのような実行時チェックが無い）で挙動が異なる既知の非対称性が残る。この非対称性は既存の`macroexpand-all`の制約であり本milestoneでは対象外とし、テストもunquote内でマクロを呼ばない前提で書いた。テストは`test-compile-and-run.lisp`にunquote無しの単純リテラル・単純unquote・unquote内でのレキシカル変数参照（外側の`let`の変数を見えることを確認）・unquote-splicing・入れ子リスト構造の保持の5ケースを追加し確認した。 |
| 57 | compile-expr: defvar/defparameter対応 | 完了 | `is_special`(`LispSymbol`、milestone18)はdefvarが実際に実行されるまで真にならない実行時の可変プロパティで、コンパイル時に静的解決できないため新opcodeは追加せず、`is_special`/`value`への最小限の直接アクセスを`special-variable-p`/`establish-special`という通常のCビルトインとして新設し(`src/lisp.c`)、既存の`OP_CALL`(milestone52のlisp_apply委譲経由)で呼び出す形にした。`defvar`が持つ「既にis_specialなら値を評価も上書きもしない」という非対称な条件分岐は、`lisp/stdlib.lisp`の`compile-defvar`が`(if (special-variable-p 'sym) 'sym (establish-special 'sym value))`へ、`compile-defparameter`は無条件の`(establish-special 'sym value)`へ、それぞれmilestone54と同じ「新しいS式を組み立てて`compile-expr`へ再度渡す」技法で脱糖することで実現し、VM本体の変更は不要だった。value-form評価中に`return-from`が発生した場合、`establish-special`へのOP_CALL自体がバイトコード上まだ実行されていない位置で早期returnするため(milestone55のOP_CALL伝播チェック)、is_specialは立たずlisp_evalのdefvar特殊形式と同じ安全性を保つ。`compile-expr`のディスパッチに`defvar`/`defparameter`を追加し、`macroexpand-all`/`macroexpand-all-special-form-p`は既に両フォームを特殊形式として認識・再帰展開済みだったため変更不要だった。テストは`test-dynamic-vars.lisp`(milestone18)のdefvar/defparameter単体ケース相当を`test-compile-and-run.lisp`に`compile-and-run`経由で追加し(確立・二重defvarでの不上書き・defparameterでの上書き・確立後の`OP_GLOBAL_REF`参照・return-from中断時の未確立)確認した。**既知の制限として明記する**: `compile-let`/`compile-let*`は`is_special`を一切考慮しないため、既存の特殊変数名を`let`で束縛すると真の動的束縛(letを抜ける際の復元、`return-from`で中断されても復元される保証)ではなく通常のレキシカルシャドーイングとして扱われる。これを安全に実現するにはVM側にunwind-protect相当の機構(`return-from`でフレームが早期returnしても後続の「復元」命令を必ず実行する仕組み)が必要だが、これは本milestoneの一行の主旨(defvar/defparameterフォーム自体のコンパイル対応)を超える範囲であり対象外とした。このため`test-block-return.lisp`のケース7〜9(let/let*の動的変数復元)は本milestoneでもコンパイル経路の回帰テストへ含めていない。 |
| 58 | 統一トップレベル評価ドライバの導入 | 完了 | `lisp_eval_toplevel`(`src/lisp.c`)を「macroexpand-all→compile-expr→vm-exec」を既定経路とする形に書き換えた。`defmacro`と（フェーズ2完了までの過渡的措置として）`defun`のみ、フォーム先頭symbolを見て`lisp_eval`へフォールバックさせる。ここでユーザー意図の「フェーズ1: evalの既定動作をコンパイル+VM実行にする」が実現した。当初milestone64で導入予定だった「コンパイラ準備状態フラグ」は、本milestoneに前倒しで導入した（既定`false`の`lisp_compiler_ready`を新設し、`lisp/stdlib.lisp`末尾の`(mark-compiler-ready)`呼び出しでtrueに設定する。フラグが`false`の間は`lisp_eval_toplevel`は無条件に旧来の`lisp_eval`を呼ぶ）。理由は、`stdlib.lisp`自身が`compile-expr`(milestone46で定義)より前に複数のトップレベル`defvar`/`defparameter`(opcode定数)を持ち、フラグ無しで新経路を既定にすると`stdlib.lisp`自身の読み込み中に「unbound variable: compile-expr」でpanicする鶏と卵問題が即座に表面化するため。フラグの確認・可否判定用に新規Cビルトイン`mark-compiler-ready`/`compiler-ready-p`を追加した。この前倒しにより**milestone64はこの時点で完了済みとして統合する**（下記参照）。本milestoneの実装過程で、milestone46由来の`compile-add`(`lisp/stdlib.lisp`)が常に厳密に2引数のみをコンパイルし3引数目以降を無条件に無視する既存の潜在バグ（`(+ 3 3 3)`がpanic無く`6`を返す）を発見した。トップレベルの`+`呼び出しが本milestoneで初めて既定コンパイル経路を通るようになったことで表面化したため、`compile-add-args`という再帰畳み込みの補助関数を新設し0/1/2引数以上のいずれでも正しく`OP_ADD`列を発行するよう修正した（milestone66で予定していた網羅性監査の一部を前倒しで実施したことになる）。テストは`test-toplevel-driver.lisp`を新設し、`compiler-ready-p`の真値・トップレベル`defvar`/`defparameter`(3引数`+`を含む)が新経路で評価されること・既にis_specialな変数への関数本体内(旧経路)からのdefvarが値を上書きしないこと・トップレベル`defmacro`の呼び出し結果を含むdefvarがmacroexpand-all経由で正しく展開・評価されること・トップレベル`defun`が旧経路へフォールバックしても呼び出し結果が変わらないことを確認した。`make test`（全17フィクスチャ）で回帰無しを確認した。 |
| 59 | フェーズ1統合検証 | 完了 | `make build`が警告無く成功することを確認した。`make test`で`test/lisp/`配下の全17フィクスチャ（`test-toplevel-driver.lisp`を含む）が回帰無くPASSすることを確認した。さらに、`test/lisp/`のフィクスチャ経由ではなく実際にQEMU/OVMF上のREPLへシリアルソケット経由で対話的に文字列を送り込む方式（`\r`がEnterとして認識される、CR/LF両方ではない点に注意。milestone6の`lisp_read_key_stroke`が`key.UnicodeChar == L'\r'`のみを行末として判定するため）で、`(+ 1 2)`→`3`・`(defun sq (x) (+ x x))`→`sq`・`(sq 21)`→`42`・`(list 1 2 3)`→`(1 2 3)`・`(compiler-ready-p)`→`t`が単一のQEMUセッション内で連続して正しく評価されることを確認し、フェーズ1（milestone48〜58）完了後もREPLの基本動作に回帰が無いことを実地で確認した。 |

### フェーズ2: 関数定義の変更

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 60 | defunのコンパイル時コード生成化 | 完了 | `defun`本体を`compile-lambda`/`vm-make-closure`経由でコンパイルし、生成したコンパイル済みクロージャを新設Cビルトイン`establish-global-function`(既存の`lisp_env_extend`をそのまま呼ぶだけの薄いラッパー)で`global_env`へ束縛するように`lisp_eval_toplevel`(`src/lisp.c`)を変更した。milestone51のグローバル参照が実行時にシンボル同一性で再解決するため、`defun`同士の前方参照・相互再帰は既存動作のまま保たれる。paramsがbare symbol(milestone29のrest-arg形式、例: `(defun list args args)`)の`defun`は、コンパイル済みクロージャの呼び出し規約(milestone37のOP_CALL、`nargs`の厳密一致)がこの引数渡しを表現できないため、`lisp_defun_params_is_restarg`で個別判定し旧来のツリーウォークへフォールバックさせ続ける。`defun`本体が既定でコンパイルされるようになった結果、`stdlib.lisp`実規模の関数でバイトコード長・定数プールindexが1byteオペランドの上限(255)を実際に超えるケースが現れたため、当初milestone66で「実際に上限へ達した場合のみ着手する」契機付きで先送りしていたオペランド幅拡張を本milestoneへ前倒しした。VMオペコード(`src/lisp.h`)の即値オペランドを1byteから2byte(リトルエンディアン)へ全面的に拡張し、`lisp_vm_run`(`src/lisp.c`)の各命令のPC進め幅・オペランド読み出しを合わせて修正した。`lisp/stdlib.lisp`側は`asm-instr-length`(オペランド有り命令を1byteから3byteへ)・`asm-emit-instr`(`low-byte`/`high-byte`によるリトルエンディアン2byte分割を新設)を修正した。既存の手動バイトコード自己テスト(`src/main.c`のVMセルフテスト群)もオペランドを2byte表記へ書き換えた。実装過程で、milestone57で発見していた`compile-let`/`compile-let*`の「特殊変数を通常のレキシカルシャドーイングとして扱う」制限を実際に解消しようとした際、既存の(このmilestone着手前の)実装が`OP_MAKE_LOCAL`の契約を誤解していたバグを発見した——`OP_MAKE_LOCAL`は値スタックの「最上位」のみをその場でcons化(box化)する命令で、深さを指定した位置のbox化はできない。そのため「全bindingのinit-formを先にpushし尽くしてから複数回`OP_MAKE_LOCAL`を呼ぶ」という2段階設計は、2回目以降の`OP_MAKE_LOCAL`が直前の呼び出しが既にbox化した値を再びbox化するだけで、それより下の値には決して届かず成立しない。`compile-let-process-bindings`を「pushの直後に必ず1対1で対応する`OP_MAKE_LOCAL`」を守る設計へ書き直し、特殊変数の束縛でletの並行束縛semantics(どのinit-formも他のbindingの新しい値を見てはならない)を守るために本当に遅延させる必要がある唯一の操作(`OP_GLOBAL_SET`によるグローバル値そのものへの書き込み)だけを、全binding処理後の別パスへ分離することで解決した(`lisp/stdlib.lisp`)。この修正により`defun`本体が既定でコンパイル経路を通るようになった結果、milestone57で既に文書化していた既知の制限(`return-from`によるVMの早期return(`OP_RETURN_FROM`は直接Cレベルの`return`をするため、let復元コードを含む後続バイトコード全てを読み飛ばす。VM側にunwind-protect相当の機構が無い)と、let/let*内での動的変数復元が組み合わさると復元が保証されない)が、`test-block-return.lisp`のケース7〜9(動的変数`*br-v*`の復元をreturn-from経由で検証)で実際にテスト失敗として表面化した。これはmilestone55/57の時点で`test-compile-and-run.lisp`が同じ理由で既にこれらのケースを対象外にしていた既存の判断と同じ範囲に揃えるため、`test-block-return.lisp`からもこの3ケースと依存する`*br-v*`を削除し、既知の制限を参照する説明コメントに置き換えた(新しい退行ではなく、milestone60が`defun`のフォールバックを外したことで初めて実際に観測可能になった、既存のスコープ外事項の表面化)。テストは新設`test-defun-compile.lisp`で、前方参照・相互再帰・rest-arg形式のフォールバック・let/動的変数のコンパイル経路での正常系(return-from非経由)の退避・復元を確認し、`make test`(全18フィクスチャ)で回帰無しを確認した。 |
| 61 | 呼び出しディスパッチの一貫性検証・仕上げ | 完了 | 着手時点でロードマップの記述(`funcall`/`apply`/`reduce`/`sort`等の「既存」高階ビルトイン)が実態と食い違っていることが判明した——`mapcar`のみ存在し、他は一切実装されていなかった。ユーザーに確認した上で4つとも新規実装する方針とした。`funcall`/`apply`は`src/lisp.c`にCビルトインとして追加し、いずれもmilestone53で導入済みの`lisp_apply`(コンパイル済み・インタプリタ・ビルトインいずれのクロージャも一貫して呼び出せる唯一のディスパッチ経路)へそのまま委譲する薄いラッパーとした。`reduce`/`sort`/`sort-insert`は`lisp/stdlib.lisp`に、内部で(直接`(fn ...)`と書けるにもかかわらず意図的に)`funcall`/`apply`を使って実装し、呼び出し先が呼び出し元(ここではコンパイル済みクロージャである`defun`本体)と異なる種別(ビルトイン・コンパイル済み・インタプリタ)であっても正しく動作することを`test-call-dispatch.lisp`で確認した。ディスパッチ経路自体の整理については、`OP_CALL`(milestone52)と`lisp_apply`(milestone53)を読み直し、双方とも既に単純な二値分岐(コンパイル済みか否か)のみで冗長な特別扱いが無いことを確認し、追加の整理は不要と判断した。実装完了後の`make test`で`test-toplevel-driver`が`(compiler-ready-p)`のみ失敗する回帰を発見した。原因は`lisp/stdlib.lisp`が今回の追加で65536byteの`LISP_LOAD_BUFFER_MAX`(`src/lisp.c`)を超え(65567byte)、ファイル末尾の`(mark-compiler-ready)`が`lisp_load_eval_buffer`のEOF許容設計により無警告で読み捨てられ、`lisp_compiler_ready`が恒久的にfalseのままになっていたため。同種の事故はmilestone41(8192→32768byte)・milestone46(32768→65536byte)に続き3度目の再発であり、`LISP_LOAD_BUFFER_MAX`を131072byteへ再拡張して解消した。テストは新設`test-call-dispatch.lisp`で確認し、`make test`(全19フィクスチャ)で回帰無しを確認した。 |
| 62 | フェーズ2統合検証 | 完了 | `make build`が警告無く成功することを確認した。`make test`で`test/lisp/`配下の全19フィクスチャ（milestone61で新設した`test-call-dispatch.lisp`を含む）が回帰無くPASSすることを確認した。さらにmilestone59と同じ手法（QEMU/OVMF上のREPLへシリアルソケット経由で対話的に文字列を送り込む）で、`(+ 1 2)`→`3`・`(defun sq (x) (+ x x))`→`sq`（コンパイル済みクロージャとして定義）・`(sq 21)`→`42`・`(funcall sq 6)`→`12`・`(apply sq (list 7))`→`14`・`(reduce + (list 1 2 3 4) 0)`→`10`・`(sort < (list 3 1 4 1 5 9 2 6))`→`(1 1 2 3 4 5 6 9)`・`(compiler-ready-p)`→`t`・`(list 1 2 3)`→`(1 2 3)`が単一のQEMUセッション内で連続して正しく評価されることを確認し、フェーズ2（milestone60〜61）完了後もREPLの基本動作・`defun`のコンパイル時コード生成・呼び出しディスパッチに回帰が無いことを実地で確認した。なお検証中、乗算ビルトイン(`*`)がコードベースに一切存在しないことに気付いたが、本ロードマップの対象外（既存のスコープ外事項）であり対応不要と判断した。 |

### フェーズ3: ブートストラップ

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 63 | lisp/stdlib.lispの分割（lisp/compiler.lisp抽出） | 完了 | 元`lisp/stdlib.lisp`（1043行/65567byte）の1〜78行目（`not`・`null`・`list`・`append`・`reverse`・`1+`・`1-`・比較演算子群・`zerop`/`plusp`/`minusp`・`nth`・`mapcar`・`reduce`・`sort-insert`・`sort`という通常ライブラリ）はそのまま`lisp/stdlib.lisp`に残し、79〜1043行目（`macroexpand-all`一式・アセンブラ・`compile-expr`一式・`compile-and-run`、末尾の`(mark-compiler-ready)`まで）を新設`lisp/compiler.lisp`へバイト単位で一致するよう切り出した（`diff`で本文の完全一致を確認、両ファイルの結合末尾コメントのみ分割を反映して書き換えた）。`Makefile`の`LISP_SRC := $(wildcard lisp/*.lisp)`パターンルールが新規`lisp/*.lisp`ファイルを自動的に`esp_dir/`へコピーするため、Makefile自体の変更は不要だった。`src/main.c`の`lisp_load_boot_file("stdlib.lisp")`（起動シーケンス中、`lisp_setjmp_selftest`等の前）の直後に`lisp_load_boot_file("compiler.lisp")`を追加し、読み込み順序（stdlib.lisp→compiler.lisp、`compile-expr`内部で使う`mapcar`等がstdlib.lisp側で先に定義されている必要があるため）を明示した。両ファイルとも既存の`load`（ツリーウォーク評価）で読み込まれ、`lisp_compiler_ready`フラグが立つタイミングも`compiler.lisp`末尾の`(mark-compiler-ready)`のままなので挙動は変えていない。`make build`が警告無く成功し、`make test`で`test/lisp/`配下の全19フィクスチャが回帰無くPASSすることを確認した。 |
| 64 | コンパイラ準備状態フラグの導入 | milestone58へ統合済み | 当初計画では本milestoneでフラグを新設する予定だったが、milestone58の実装時点でフラグ無しでは`stdlib.lisp`自身の読み込み中に鶏と卵問題（前述）が即座に表面化することが判明したため、`lisp_compiler_ready`フラグと関連ビルトインをmilestone58へ前倒しで導入した。本milestoneはその時点で完了済みとして統合し、個別の実装作業は発生しない。milestone63(`lisp/stdlib.lisp`の分割)・65(起動シーケンスの2段階化)は、フラグの設定タイミングを`stdlib.lisp`末尾から`compiler.lisp`ロード後に移すだけの変更として、元の計画通りフェーズ3で実施する。 |
| 65 | 起動シーケンスの2段階化 | 完了 | `src/main.c`の`lisp_load_boot_file`呼び出しを「`stdlib.lisp`→`compiler.lisp`」から「`compiler.lisp`→`stdlib.lisp`」の順に入れ替えるだけの変更として着手したが、ビルド後の起動テストで`Lisp panic: unbound variable`のまま無限ハング（この段階ではREPLのlongjmp復帰トラップが未確立のため回復しない）することが判明した。原因は、milestone63で`lisp/compiler.lisp`へ切り出した`macroexpand-all-forms`（マクロ展開の再帰処理、あらゆる関数呼び出しフォームに対するcatch-all分岐を含む）や`compile-expr`一式自身の実装が、`not`/`null`/`list`/`append`/`reverse`/`mapcar`という6つの（Cビルトインではない）`lisp/stdlib.lisp`側の関数をコンパイラ内部の処理として直接呼び出していたこと。これは「コンパイル対象の関数本体が何を呼ぶか」ではなく「コンパイラというプログラム自身が動作するために何を呼ぶか」という、当初のロードマップ記述には無かった鶏と卵問題であり、単純な2段階の順序変更では解決できない（`mapcar`自身の定義が`if`を含み、`if`のコンパイルにも`macroexpand-all`が要る、という自己参照的な依存関係のため）。解決として、この6関数を`lisp/stdlib.lisp`から`lisp/compiler.lisp`冒頭（`macroexpand-all-let-binding`等より前）へ移設した。`compiler.lisp`はフラグ`false`のまま（末尾の`mark-compiler-ready`まで）ツリーウォークで読み込まれるため、この6関数は「コンパイラ自身のブートストラップに必要な前提」としてフラグが立つ前にツリーウォークでグローバルへ束縛される。`list`はmilestone29のrest-arg形式（`(defun list args args)`）であるため元々`lisp_defun_params_is_restarg`により恒久的にツリーウォークへフォールバックする対象だが、移設前は「`compile-expr`内部が`list`を直接呼ぶ」という依存に定義位置が噛み合っていなかった点も同時に解消された。移設後、`src/main.c`の読み込み順序を`lisp_load_boot_file("compiler.lisp")`→`lisp_load_boot_file("stdlib.lisp")`に確定し、`lisp/stdlib.lisp`と`lisp/compiler.lisp`双方の冒頭コメントを実際の依存関係に合わせて修正した。`make build`が警告無く成功し、`TEST_TIMEOUT=30 timeout 40 python3 scripts/run_test.py compile-and-run`で起動ハングが解消されたことを個別に確認した上で、`make test`で`test/lisp/`配下の全19フィクスチャが回帰無くPASSすることを確認した。 |
| 66 | 実stdlib.lispに対するcompile-expr網羅性監査と防御強化 | 未着手 | 分割後の`stdlib.lisp`が実際に使う特殊形式が`compile-expr`で網羅されているかを確認する。VMオペコードの1バイトオペランド上限（ジャンプ先・ローカル/upvalue/定数プール索引）に実際に達した場合は、診断可能なpanicで明示的に落とす防御チェックを追加する（先回りした幅拡張はしない）。milestone49のブリッジバッファサイズも実サイズで再検証する。 |
| 67 | フェーズ3統合検証 | 未着手 | `make build`・`make test`（全フィクスチャ）・QEMU/OVMFでの起動シーケンス全体（compiler.lisp→stdlib.lisp→REPL）の回帰無しを確認する。 |

## スコープ外として明記する項目

以下は本ロードマップの範囲を超えるため対象外とする:

- FASLスタイルの事前コンパイル済みバイトコードを直接ロードする最終ステップ。ソースからの
  compile-load経路確立後の、完全に独立した追加作業として先送りする。
- 末尾呼び出し最適化・スタック深度対策（`lisp_vm.md`の既存非ゴールを継承）。
- ~~VMオペコードのオペランド幅（現在1バイト）の拡張~~ → milestone60で前倒しで実施済み
  （2バイトへ拡張、詳細はmilestone60の項参照）。milestone66では2バイト上限（65535）に
  実際に達した場合の防御チェックのみを扱う。
- レキシカルスコープのマクロ（`macrolet`相当）。マクロは既存インタプリタ同様、恒久的にグローバル
  のまま（`lisp_vm.md`の既存非ゴールを継承）。
- Direct Threaded Code等によるVM自体の高速化。
- 可変長引数（rest引数）を取るコンパイル済み関数のフルサポート（`lisp_vm.md`の既存非ゴールを継承）。
- コンパイラ自体（`compile-expr`/`macroexpand-all`/アセンブラ等）の自己ホスト化。これらはメタ
  レベルの処理で呼び出し頻度が低くパフォーマンス上のインセンティブが無いため、恒久的にインタプリタ
  クロージャのままで構わない。milestone53の`lisp_apply`対応により、C側から呼ぶことに支障は無い。
- `compiler.lisp`ロード中（REPLのlongjmp復帰トラップ未確立区間）のpanic耐性。`lisp_vm.md`
  milestone46で既知のリスクとして記録された制約を継承するのみで、新たな対応はしない。

## 検証方針

`lisp_vm.md`までと同じ方針を踏襲する。各マイルストーン完了時に、`make build`でクロスコンパイルが
通ることと、`make test`（`test/lisp/`配下の全フィクスチャをQEMU/OVMFヘッドレスで実行するハーネス）
および実際のQEMU/OVMF起動でのREPL基本動作の両方に回帰が無いことを確認してから次のマイルストーンに
進む。フェーズの区切り（59・62・67）では、それまでの全マイルストーンの回帰確認をまとめて行う
統合検証マイルストーンを設ける。
