# CommonLisp相当を目指す拡張マイルストーン

## 目的

このドキュメントは、[`boot.md`](./boot.md)（マイルストーン1〜11、UEFIブート〜最小Lisp REPL）と
[`init_lisp.md`](./init_lisp.md)（マイルストーン12〜16、`defun`/`macro`/文字列型/`load`）で完成
した現在のBareMetalLisp（最小Lisp）を土台に、CommonLisp（以下CL）相当の機能セットに近づけるための
拡張群を、マイルストーン単位で整理したものである。CLの全機能を再現することは目的とせず、あくまで
「最低限」必要と考えられる範囲に絞る（末尾の「スコープ外」節を参照）。他の2文書と同様、実装の詳細
設計は各マイルストーンに着手する際に別途行い、本ドキュメントは全体の見取り図として保守する。

前提となる制約は`boot.md`/`init_lisp.md`と同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証はQEMU/OVMF上で実際に起動し、コンソール
  出力を目視確認することで行う。

本ドキュメント作成にあたり「レキシカルスコープ」の対応方針を確認した。クロージャは生成時の環境
（`LispClosure.env`、マイルストーン9）を捕捉済みで、`lisp_env_lookup`（`src/lisp.c`）はクロージャの
環境チェーンを先にたどり、そこで見つからなければCグローバル変数として単一に保持された`global_env`
（環境チェーンに埋め込まれているわけではない、独立したテーブル）を探し、どちらにも無ければ
unbound variableエラーとする（マイルストーン13で再帰対応のために追加したフォールバック）。これは
そのまま「望ましいレキシカルスコープの挙動」と確認済みであり、この観点だけを理由にした新規
マイルストーンは設けない。

## ファイル構成

`init_lisp.md`と同じ3ファイル構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／`src/main.c`）の上に
実装する。新規ファイル（自己ホスティング用の`.lisp`ライブラリファイルなど）を追加する場合は
`Makefile`（`SRC`/`HDRS`、および`make setup`が作るESP配置）も見直す。

## マイルストーン一覧

`init_lisp.md`のマイルストーン12〜16に続く番号で管理する。

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 17 | 基本special formsの拡充 | ✅ 完了 | `let`/`let*`/`progn`/`setq`/`cond`/`and`/`or`/`when`/`unless`を`lisp_eval`（`src/lisp.c`）に追加した。`progn`は`lisp_eval_progn`（フォーム列を順に評価し最後の値を返す新設ヘルパー、フォームが無ければnil）を新設して実装し、`let`/`when`/`unless`/`cond`の各clauseの本体評価にもこの`lisp_eval_progn`を共通で使っている。`let`は各初期値式を**外側の**envで評価してから新しい束縛をまとめて積んだ環境で本体を評価する（束縛同士は互いを参照できない、並列束縛）。`let*`は各初期値式をそれまでに積んだ束縛が見える環境（`new_env`）で評価する（逐次束縛）。`setq`は既存の`lisp_env_lookup`と対になる`lisp_env_set`を新設して実装した。挙動は`lisp_env_lookup`と同じ探索順（渡された`env`チェーン→見つからなければ`global_env`）で既存の束縛ペアを探し、見つかった`(sym . value)`のcdrを`lisp_set_cdr`で破壊的に書き換える。どちらにも見つからない場合は新規変数の暗黙定義はせず`lisp_env_lookup`と同じ`unbound variable`でpanicする（`defvar`が無い現状ではsetqだけで新規のグローバル変数を作らせない、という意図的な制限）。`cond`は各clauseのtestを順に評価し、最初に非nilになったclauseの本体を`lisp_eval_progn`で評価する（本体が無いclauseはtestの値自身を返す、標準的なCLの`cond`と同じ挙動）。`and`/`or`は短絡評価が必須なため`if`の組み合わせマクロではなく直接`lisp_eval`に特殊形式として実装した（`(and)`はt、`(or)`はnilを返す）。検証は、QEMUをGUI無しでヘッドレス起動し（`-display none -vga none`、`-chardev socket`でシリアルポートをUnixソケットに繋いでPythonスクリプトから送受信）、以下を確認した: `(progn 1 2 3)`→`3`、`(progn)`→`nil`、`(let ((a 1) (b 2)) (+ a b))`→`3`、`(let ((x 1)) (let ((x 2) (y (+ x 1))) y))`→`2`（`let`はyの初期値式で外側の`x`=1を見る）、同じ式を`let*`にすると→`3`（`let*`は直前で束縛した`x`=2を見る、let/let*の違いを実証）、`(let ((x 1)) (setq x (+ x 10)) x)`→`11`、`(cond ((eq 1 2) 100) ((eq 1 1) 200) (t 300))`→`200`、`(cond ((eq 1 2) 100))`→`nil`、`(cond (5))`→`5`、`(and 1 2 3)`→`3`、`(and 1 nil 3)`→`nil`、`(and)`→`t`、`(or nil nil 5)`→`5`、`(or nil nil)`→`nil`、`(or)`→`nil`、`(or 1 (car 5))`→`1`（`(car 5)`が短絡により評価されずpanicしないこと）、`(and nil (car 5))`→`nil`（同様に短絡）、`(when (eq 1 1) 10 20)`→`20`、`(when (eq 1 2) 10)`→`nil`、`(unless (eq 1 2) 10 20)`→`20`、`(unless (eq 1 1) 10)`→`nil` |
| 18 | 動的変数（`defvar`/`defparameter`） | ✅ 完了 | `LispSymbol`（`src/lisp.c`）に`is_special`フラグと`value`フィールドを追加した。`lisp_env_lookup`/`lisp_env_set`は`is_special`が真のシンボルについてはenvチェーン/`global_env`のalistを一切探索せず、シンボル自身の`value`を直接読み書きする（動的変数はレキシカルな束縛経路から完全に外れる、という設計）。`defvar`は`is_special`が未設定のときだけ初期値式を評価して`value`にセットし`is_special`を立てる（既にspecialなら上書きしない、CLの`defvar`と同じ「再loadしても状態が保たれる」挙動）。`defparameter`は既存の値の有無に関わらず常に上書きする。`let`は動的変数の再束縛でも並列束縛の意味を保つため二段階評価（フェーズ1: 全初期値式を外側のenvで評価してから、フェーズ2: 通常変数はenvへ積み・特殊変数はシンボルの`value`を退避してから書き換え）に書き直した。`let*`は逐次束縛なので１パスで初期値式を評価しながら特殊変数はその場で`value`を書き換える（退避は`let`と同様、本体評価後にLIFOで復元）。`setq`は既存のまま（`lisp_env_set`が`is_special`を見て自動的に動的変数の直接書き換えに分岐するため変更不要）。検証は`test/lisp/test-dynamic-vars.lisp`の`run-test-dynamic-vars`で、`defvar`の初回セット・再load時の非上書き、`defparameter`の常時上書き、`let`での再束縛と復元（呼び出し先の別関数から見える動的スコープ、および`let`を抜けた後の復元）、`let`の並列束縛（同じ`let`内の他の初期値式は再束縛前の値を見る）、`let*`の逐次束縛（直前の初期値式での再束縛を後続の初期値式が見られる）、`let`内での`setq`、を確認しすべて`t`。実装・検証の途中、`run-test-let-star-sees-own-rebinding`のように32文字を超える関数名を呼び出すと`unbound variable`でpanicするバグを発見・修正した（後述） |
| 19 | 非局所的脱出の基盤（`block`/`return-from`） | ✅ 完了 | setjmp/longjmpが使えないため、`src/lisp.c`にファイルスコープの静的変数`lisp_return_tag`/`lisp_return_value`を新設し、非局所脱出中かどうかとその宛先タグ・値を保持するグローバルなシグナルとした（`lisp_return_tag == LISP_NIL`なら平常時）。`return-from`はタグシンボル（未評価）と値式（評価済み、省略時はnil）を受け取り、`lisp_return_tag`/`lisp_return_value`をセットしてそのまま値を返す。`block`は本体を評価した後、`lisp_return_tag`が自分のタグと一致していれば`lisp_return_tag`をnilに戻して`lisp_return_value`を返す（捕捉）。一致しない場合はシグナルを立てたまま評価結果をそのまま返す（自分より外側のblock宛として伝播）。共通ヘルパー`lisp_eval_progn`と`lisp_eval_list`をシグナル対応にした（各要素を評価した直後に`lisp_return_tag`をチェックし、セットされていれば残りの要素を評価せずそのまま返す）ことで、`progn`/`let`/`let*`/`when`/`unless`/`cond`の本体評価と、関数呼び出しの引数評価がまとめて対応済みになった。それ以外の`lisp_eval`内の評価経路（`if`の分岐選択、`setq`・`defvar`・`defparameter`の値式評価、`cond`/`and`/`or`/`when`/`unless`の各testの評価、関数呼び出しの被呼び出し式`fn`の評価、マクロ展開の2段目、`lisp_qq_expand`の`,`/`,@`）は、`lisp_eval`を呼んだ直後に個別に`lisp_return_tag`をチェックして即座に伝播するよう見直した。`let`/`let*`が動的変数（milestone 18）を退避・復元する処理は非局所脱出の有無に関わらず必ず実行される必要があるため特に注意して設計した: `let`はフェーズ1（初期値式の評価、まだ何も書き換えていない）でシグナルが立てば即座にそのまま返してよいが、`let*`は逐次束縛のため、あるbindingの初期値式評価中にシグナルが立った時点で既に前のbindingで動的変数を書き換えている可能性があり、その場合はループを`aborted`フラグを立てて抜けるだけにし、本体評価をスキップしつつ既存の`saved_specials`復元ループには必ず到達させてから結果を返すよう書き直した。対応する`block`が無いまま`return-from`のシグナルが残り続けると、次のトップレベル入力の評価が最初の一歩で無言のまま打ち切られ続けるため、`lisp_eval`をラップする`lisp_eval_toplevel`を新設し、REPLループ（`src/main.c`）と`lisp_load_eval_buffer`の両方をこれに差し替えた。トップレベル評価の直後に`lisp_return_tag`が残っていれば「対応するblockが存在しない」というユーザー側の誤りとしてpanicする。検証は`test/lisp/test-block-return.lisp`の`run-test-block-return`で、単純な早期脱出、`return-from`以降の本体formが評価されないこと、ネストした`block`で内側タグ宛は内側で捕捉されること・外側タグ宛は内側を素通りして外側まで伝播すること、再帰呼び出しを何段か経由した`return-from`が正しく呼び出し元の`block`まで伝播すること、`let`/`let*`の本体評価中および`let*`の束縛評価中に`return-from`が発生しても動的変数が正しく復元されることを確認しすべて`t`。加えて別のQEMU起動で`(return-from no-such-block 1)`が想定どおり`Lisp panic: return-from: no enclosing block for this tag`でpanicすることも確認した。関連拡張の`catch`/`throw`/`unwind-protect`は本マイルストーンでは実装しない（付記のみ） |
| 20 | `gensym` | ✅ 完了 | `src/lisp.c`に`lisp_make_uninterned_symbol(const char *name)`を新設した。`lisp_alloc`で`LispSymbol`を確保するだけで、既存の`lisp_intern`が使う`lisp_symbol_table`には一切登録しない。`eq`はオブジェクトの同一性そのもの（タグ付きポインタの値比較）であり、`lisp_intern`の名前一致判定はテーブルに載っているシンボルしか見つけられない。したがって「テーブルに載せない」というだけで、名前文字列が何であっても reader/`intern`経由の通常の探索では絶対に同じオブジェクトへ到達できない、というユニーク性を保証できる（名前文字列自体の一意性には依存しない設計）。組み込み関数`(gensym)`/`(gensym "prefix")`は`lisp_builtin_gensym`として実装し、省略可能な文字列引数（デフォルト`"G"`）とファイルスコープの静的カウンタ`lisp_gensym_counter`（呼ぶたびに+1）を使い、`lisp_print_fixnum`と同じ「桁を逆順に積んでから並べ直す」手法で「prefix + カウンタの10進数字」という名前を作って`lisp_make_uninterned_symbol`に渡す。名前文字列は読みやすさのためだけのものであり、一意性の根拠ではない。検証は`test/lisp/test-gensym.lisp`の`run-test-gensym`で、複数回の`(gensym)`が互いに`eq`にならないこと、`gensym`の結果が`atom`であること（consではない）、`nil`/`t`のいずれとも`eq`にならないこと、同じprefixを渡した`(gensym "foo")`同士も`eq`にならないことを確認しすべて`t`。milestone 17/18/19の既存テスト集約（`run-test-special-forms`/`run-test-dynamic-vars`/`run-test-block-return`）も回帰確認し、いずれも`t`のまま |
| 21 | `macroexpand-1`と`*macroexpand-hook*` | 未着手 | 18の動的変数機構が前提（`*macroexpand-hook*`は動的変数として実装する）。現在`lisp_eval`のマクロ呼び出し分岐に埋め込まれている「マクロ本体を評価して展開結果を得る」処理を、Lisp側から呼び出せる独立した関数として切り出す。マクロのデバッグ（展開結果を実行せず確認する）目的 |
| 22 | 数値タワーの拡張（bignum・float） | 未着手 | 既存のタグ空間（cons/fixnum/symbol/closure）は使い切っているため、マイルストーン15の文字列と同じ「`LispClosure`にフィールドを追加するescape hatch」を踏襲するか、`LispClosure`がすでに`builtin`/`is_macro`/`str_data`/`str_len`と肥大化しているため、この時点でタグ付きポインタ全体をtype-code付きの共通ヘッダ構造に見直すか、を実装着手時に判断する。既存の`+`/`-`と、標準ライブラリ化で追加される`*`/`/`について、fixnum/bignum/floatが混在した際の型昇格規則（例: fixnumオーバーフロー時にbignumへ、floatが混じれば結果もfloatへ）にも触れる |
| 23 | packageシステム（最小限） | 未着手 | 現在`intern`が単一のグローバルシンボルテーブルである点を、パッケージごとに分離したテーブルへ拡張する。最低限、通常のシンボルを置く`common-lisp-user`相当のパッケージと、`:foo`のように自己評価する`keyword`パッケージの分離のみをスコープとする。`export`/`use-package`/シャドーイングなど完全なCLパッケージ仕様は対象外とする（後述「スコープ外」参照） |
| 24 | 汎用出力ストリームの抽象化 | 未着手 | 現在`lisp_print`が`SystemTable->ConOut`を直接呼んでいる構造を、ストリームオブジェクト経由の出力に変更する。将来、文字列出力ストリームなど他の出力先を追加する際の土台とする |
| 25 | タイマー支援 | 未着手 | マイルストーン16（`load`）で型を付けずvoid\*のまま`EFI_BOOT_SERVICES`に追加した`CreateEvent`/`SetTimer`/`WaitForEvent`等に実型を与え、`sleep`相当のLisp関数を実装する |
| 26 | 汎用vector primitive（`make-vector`/`svref`/`svset`） | 未着手 | 22（数値タワー拡張）と同様、タグ空間をどう確保するかの論点が再度関わるため、22での判断と一貫した方針を採る |
| 27 | 破壊的変更（`rplaca`/`rplacd`） | 未着手 | `(rplaca cons-cell new-car)`/`(rplacd cons-cell new-cdr)`。既存の`lisp_cons_cell`アクセサ経由でcar/cdrスロットを書き換えるだけの小さな追加 |
| 28 | ハッシュ計算（`hash-code`） | 未着手 | `(hash-code object)`。アイデンティティ（ポインタ値ベース）または構造（内容ベース）のハッシュ値を返す関数のみを最低限の対象とする。本格的な`hash-table`（`make-hash-table`/`gethash`/`sethash`）は自然な発展として触れるが、「最低限」の範囲としては任意拡張と位置づける |
| 29 | 標準ライブラリの自己ホスティング化 | 未着手 | マイルストーン16の`load`を活かし、`list`/`append`/`reverse`/`mapcar`/`nth`/`null`/`not`/`1+`/`1-`/`zerop`/比較演算子（`<`/`>`/`=`等）など、C実装が必須ではない関数群を、起動時に読み込む`.lisp`ファイルにLisp自身で定義する。C側の組み込みは本当に低レベルな操作（cons/car/cdr、型判定、算術の核、IO）だけに絞る、既存の最小主義（`CLAUDE.md`）と一貫した方針。他のマイルストーンで追加される機能（`let`、動的変数など）の検証にも、この標準ライブラリが早い段階から使えると書きやすくなる |

## スコープ外として明記する項目

以下はCL相当と呼ぶには本来重要だが、「最低限」の範囲を超えるため本ドキュメントの対象外とする:

- CLの完全な条件システム（`handler-case`/`handler-bind`/restart）
- `format`のディレクティブ言語
- CLOS（`defclass`/メソッドディスパッチ）
- `defstruct`
- 完全なパッケージ仕様（`export`/`shadow`/`use-package`等）
- 有理数・複素数
- 末尾呼び出し最適化・スタック深度対策

## 検証方針

`boot.md`/`init_lisp.md`と同じ方針を踏襲する。各マイルストーン完了時に、`make build`でクロス
コンパイルが通ることと、`make run`（Linux環境ではOVMFファームウェアパスを環境に合わせて差し替える）
でQEMU上に起動し、想定した出力・動作がコンソール上で確認できることの両方を確認してから次の
マイルストーンに進む。

動作確認用のLispファイルは`test/lisp/*.lisp`に置き、`make build`/`make setup`が`esp_dir/test/`配下に
コピーする（`Makefile`の`esp_dir/test/%.lisp: test/lisp/%.lisp`ルール）。QEMU起動後、REPLから
`(load "test\test-special-forms.lisp")`（UEFIの`EFI_FILE_PROTOCOL`はパス区切りに`\`を要求し`/`は
使えない）で読み込み、ファイル内で定義した`run-test-xxxxx`関数を呼び出して`t`が返ることを確認する。
milestone 17の検証時にこの方式で`;`〜行末のコメントを含むテストファイルを読ませたところ、
readerが`;`をコメントとして扱わずシンボルの一部として読んでしまい、トップレベルの裸シンボルが
評価されて`unbound variable`でpanicする問題が見つかったため、`lisp_reader_skip_ws`
（`src/lisp.c`）を「空白と`;`から行末までのコメントを交互に読み飛ばすループ」に拡張した
（milestone番号は振らない小さな追加。quasiquote同様、既存機能への直接の追加のため）。

milestone 18の検証時には、`run-test-let-star-sees-own-rebinding`という関数を`defun`で定義した後
呼び出すと`unbound variable`でpanicするが、同じ`let*`式をREPLで直接評価すると成功する、という
一見不可解な不整合が見つかった。切り分けの結果、原因は関数名の文字数にあった:
`LISP_SYMBOL_NAME_MAX`（当時32）を超える名前は`lisp_intern`（`src/lisp.c`）内で単に切り詰めて
格納する一方、既存シンボルとの一致判定`lisp_streq(sym->name, name)`には切り詰め前の`name`を
そのまま渡していたため、格納済みの（切り詰め済みの）名前と絶対に一致せず、同じ長い名前を
読むたびに`eq`にならない別シンボルが新規生成されてしまっていた（`defun`で束縛したシンボルと、
後から呼び出す際に読み直したシンボルが別物になり、`global_env`から見つからずpanicする）。
`lisp_intern`を「比較・格納の両方で先に同じロジックで切り詰めてから扱う」よう修正し、
`LISP_SYMBOL_NAME_MAX`も`LISP_TOKEN_MAX`（64）に合わせて拡張した。32文字前後の説明的な関数名を
テストファイルで使う、という一見無関係な操作が既存のinternロジックの潜在バグを表面化させた例。
