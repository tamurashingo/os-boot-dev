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
| 21 | `macroexpand-1`と`*macroexpand-hook*` | ✅ 完了 | 従来`lisp_eval`のマクロ呼び出し分岐に直接埋め込まれていた「呼び出し式を評価してマクロクロージャか確認し、本体を評価して展開結果を得る」処理を、独立した`lisp_macroexpand_1(expr, env)`（`src/lisp.c`）として切り出した。マクロ呼び出しでなければ`expr`を**同じオブジェクトとして**（`eq`が真になる形で）そのまま返す設計にし、多値が無いこのLispでCLの2番目の戻り値`expanded-p`の代わりに`(eq form (macroexpand-1 form))`で「展開されなかった」ことを判定できるようにした。実際の展開処理（マクロの仮引数への束縛→本文評価）自体は動的変数`*macroexpand-hook*`（18の`is_special`機構を使い、`defvar`のLispフォームを経由せず`lisp_builtins_init`内でC側から直接`is_special=1`と初期値をセットして用意した）に委譲し、そのデフォルト値`lisp_default_macroexpand_hook`が従来の展開処理そのものを行う。呼び出し規約はCLの`*macroexpand-hook*`（展開器・フォーム・環境の3引数）に合わせたが、このLispのマクロはCLの「関数」とは異なりユーザー定義マクロの`LispClosure`そのものを1番目の引数（展開器）として渡す点、また3番目の環境引数はCLとの見た目合わせのためだけに受け取り実際には使わない点（`macrolet`が無く、マクロは常に自分の定義時の環境`macro->env`に閉じているため）が単純化。組み込み関数`(macroexpand-1 form)`は`lisp_macroexpand_1(form, global_env)`を呼ぶだけの薄いラッパーで、CLと異なり2番目の環境引数は受け取らない（レキシカルなマクロ環境という概念自体が無いため）。`lisp_eval`のマクロ呼び出し分岐自体もこの新関数を呼ぶだけに簡潔化された。検証は`test/lisp/test-macroexpand-1.lisp`の`run-test-macroexpand-1`で、マクロ呼び出しの展開結果の構造（car/cdrを手でたどって確認、`equal`が無いため）、マクロ呼び出しでない式は同一オブジェクトが返ること、atomはそのまま返ること、`*macroexpand-hook*`を差し替えると`macroexpand-1`の結果が変わること、その差し替えが`macroexpand-1`経由だけでなく通常のマクロ呼び出しの評価そのものにも反映されることを確認しすべて`t`。milestone 17/18/19/20の既存テスト集約と既存の`defmacro`呼び出しも回帰確認し、いずれも問題なし。`funcall`/`apply`がまだLisp側に無いため、ユーザーが`*macroexpand-hook*`をLispコードから差し替える際に「元のhookを呼び出しつつ追加処理する」形のラッパーは書けず、完全な置き換えのみ可能という制約が残る（付記のみ、本マイルストーンの範囲外） |
| 22 | 数値タワーの拡張（bignum・float） | ✅ 完了 | タグ空間の再設計は行わず、マイルストーン15の文字列と同じ「`LispClosure`にフィールドを追加するescape hatch」を踏襲した（`is_float`/`float_value`/`big_digits`/`big_len`/`big_negative`の5フィールド追加。再設計は全アクセサ・全`lisp_eval`分岐に影響する範囲の広さがmilestoneの目的に対して不釣り合いと判断）。bignumは固定幅ではない**真の多倍長**で、2^32進の桁配列（little-endian、先頭ゼロ桁はtrim済み、0は常にfixnumで表すためbignumが0を表すことはない）として持つ。桁配列は`LISP_BIGNUM_MAX_LIMBS`（64、2048bit・10進約617桁）を上限とする固定長バッファ上でschoolbook算法（`lisp_bignum_add_mag`/`_sub_mag`/`_compare_mag`）で計算し、超えたら`lisp_panic`する（`*`/`/`が無く`+`/`-`のみのため1回の演算で増える桁は最大1桁、上限到達は非現実的）。全てのbignum生成は正規化エントリポイント`lisp_make_number_from_magnitude`を経由し、結果がfixnum表現範囲（約±2^61）に収まれば必ずfixnumへdemoteする——これにより「小さい結果は常にfixnum」という不変条件が保たれ、`eq`による同一性テストが従来通り機能する。型混在の昇格規則（`lisp_num_add`/`lisp_num_negate`/`lisp_num_sub`、`sub(a,b)=add(a,negate(b))`として実装）: どちらかがfloatなら両辺を`lisp_number_to_double`でdoubleへ変換してfloat結果（bignumを含む巨大な値をdouble化する精度劣化は既知の制約として許容）、両方fixnumで結果が範囲を超える場合のみbignumへ昇格、fixnum/bignum・bignum/bignum混在は桁配列演算し結果は自動でfixnumへ正規化されうる。`lisp_builtin_add`/`lisp_builtin_sub`をこの汎用層の上に書き直し、非数値引数には`lisp_assert_number`で汎用のpanicメッセージを出すよう更新した。printerは`lisp_print_bignum`（最上位桁からの長除法`lisp_bignum_divmod_small`で10進化）と`lisp_print_float`（整数部は既存`lisp_print_fixnum`を再利用、小数部は固定6桁を`frac*=10; digit=(int)frac; frac-=digit;`の手計算で生成し末尾ゼロは1桁残るまでtrim、指数表記は非対応）を追加。readerは`lisp_token_is_float`/`lisp_token_to_float`で`-?digit+.digit+`形式のみ対応（`.5`や`5.`、指数表記は非対応な簡略化）。数値比較（`=`/`<`等）が無い（milestone29まで実装しない方針）ため、bignum/floatの値そのものはheapオブジェクトで`eq`比較できず自動テストできない。よって検証は`eq`で自動確認できる範囲（`test/lisp/test-numeric-tower.lisp`の`run-test-numeric-tower`: 通常サイズfixnum演算・単項`-`・`(+)`の回帰、および「2^61-1を2回加算してbignumへ昇格させた後、十分小さい値を引いてfixnum範囲に戻すと正規化により`eq`で`3`と一致する」というbignum→fixnum正規化往復の唯一の自動確認経路）と、QEMU REPLへの直接入力による目視確認のハイブリッドで行った。目視確認結果: `(+ 2305843009213693951 2305843009213693951)`→`4611686018427387902`（bignumへ昇格）、`(+ 4611686018427387902 1)`→`4611686018427387903`、`(- 4611686018427387902 4611686018427387899)`→`3`（bignumからfixnumへ正規化復帰）、`(+ 1.5 2.5)`→`4.0`、`(- 3.0)`→`-3.0`、`(+ 1 2.5)`→`3.5`、`(+ 4611686018427387902 1.0)`→`4611686018427387904.0`、105桁の巨大なbignum同士の加算も正しく10進表示された。**実装中に発見・修正した重要な既存バグ（milestone22で新規に導入したものではない）**: 旧`lisp_token_to_fixnum`（reader）は解析した10進リテラルの絶対値がfixnum表現範囲（約2^61）に収まるかを一度も検証しないまま`lisp_make_fixnum`（`value << 2`で下位2bitにタグを詰める処理）に渡していたため、2^61以上のリテラルはbit 61がbit 63（符号bit）に押し出され、無関係な負数へ静かに破損していた（加算・減算に関して群同型を保つ破損のため、たまたま結果が正しく見える組み合わせも存在し発見が遅れた）。`lisp_token_to_fixnum`を廃し、桁文字列を直接bignum桁配列へ読み込んでから`lisp_make_number_from_magnitude`で正規化する`lisp_token_to_number`に置き換えることで修正——巨大なリテラルは今後正しくbignumになり、通常サイズのリテラルは変わらずfixnumへ正規化される。milestone 17〜21の既存テスト集約（`run-test-special-forms`/`run-test-dynamic-vars`/`run-test-block-return`/`run-test-gensym`/`run-test-macroexpand-1`）も回帰確認し、いずれも`t` |
| 23 | packageシステム（最小限） | ✅ 完了 | 当初案（`lisp_symbol_table`に加えて`lisp_keyword_table`をもう1つハードコードし、`LispSymbol`に`is_keyword`真偽フラグを持たせる案）はユーザーから「将来的にパッケージを増やすことを前提に設計して。これだとcl-userとkeywordの二つしかパッケージを定義できないのでは」という指摘を受け却下し、パッケージを固定長配列で管理する汎用的な仕組みに再設計した。新設した`LispPackage`構造体（`src/lisp.c`）は`name`/`symbols`（既存`LISP_MAX_SYMBOLS`=256をそのまま再利用）/`symbol_count`/`is_keyword_package`を持ち、`static LispPackage lisp_packages[LISP_MAX_PACKAGES]`（`LISP_MAX_PACKAGES`=8）という配列と`lisp_package_count`で管理する。登録・検索は汎用API`lisp_make_package(name, is_keyword_package)`/`lisp_find_package(name)`のみで行い、3つ目以降のパッケージが必要になった際も配列要素を1つ使って`lisp_make_package`を1回呼ぶだけで追加でき、構造体やテーブルの再設計は不要（ユーザー指摘の解消）。`LispSymbol`には`is_keyword`のような単一フラグではなく`LispPackage *package`ポインタを追加した——「どのパッケージに属するか」を直接持たせることで、将来別の特別なパッケージが必要になった場合も`LispSymbol`側は無変更で済む設計判断。`lisp_intern`の中核ロジックは`lisp_intern_in_package(LispPackage *pkg, const char *name)`に汎用化し（探索・格納先を`pkg->symbols`/`pkg->symbol_count`に一般化、新規作成時`sym->package = pkg`をセット）、既存の`lisp_intern(name)`は`lisp_intern_in_package(lisp_cl_user_package, name)`を呼ぶ薄いラッパーに、新設の`lisp_intern_keyword(name)`は`lisp_keyword_package`向けの薄いラッパーとした（`*package*`のような可変の「現在のパッケージ」概念は非ゴールのため導入せず、cl-userへの固定は`lisp_intern`内にハードコード）。ブートストラップ`lisp_packages_init`（`src/lisp.c`新設、`src/lisp.h`に宣言追加）は`lisp_make_package("common-lisp-user", 0)`と`lisp_make_package("keyword", 1)`を呼び`lisp_cl_user_package`/`lisp_keyword_package`にキャッシュする。`src/main.c`の`EfiMain`では`lisp_heap_init`の直後・`lisp_symbols_init`の直前（symbol internがcl-userパッケージの存在を前提とするため）に`lisp_packages_init()`を追加した。reader（`lisp_read`）はトークン先頭が`:`の場合、先頭を除いた残りを`lisp_intern_keyword`に渡すよう分岐を追加した（CL同様、`symbol-name`自体には`:`を含めない——`:foo`の名前は`"foo"`で、`:`は印字/読み取り時の記法）。自己評価は既存の`t`専用扱いと同じ「evalの手前でハードコードして横取りする」方式を横展開して実装した（`lisp_eval`のシンボル分岐: `if (expr == lisp_sym_t \|\| (cell->package != 0 && cell->package->is_keyword_package)) return expr;`、`is_special`/`value`の動的変数機構は使わない）。この結果`(setq :foo 1)`は特別なガードを追加せずとも通常シンボルと同じ経路で`lisp_env_set`に到達し`unbound variable`でpanicする（keywordが変更不能という挙動を副作用として得る）。印字は`lisp_print`のシンボル分岐に同じ`package != 0 && package->is_keyword_package`ガードを追加し、名前の前に`:`を出力するようにした。gensym（milestone 20）の`lisp_make_uninterned_symbol`は`sym->package = 0`（NULL）を設定し、CLのuninterned symbol相当とした——`lisp_eval`/`lisp_print`の両方の`package`参照箇所はNULLガード済み。検証は`test/lisp/test-package.lisp`の`run-test-package`（`eq`で自動確認できる範囲: `(eq :foo :foo)`、`(eq :foo (quote :foo))`、cl-userの`foo`とkeywordの`:foo`が同一でないことの二重`eq`確認、通常シンボルinternの回帰）と、QEMU REPLへの直接入力による目視確認（自己評価・印字結果は`eq`では見えないため）のハイブリッドで実施し、いずれも成功: `:foo`を評価→エラーにならず`:foo`と印字、`(car (quote (:a :b :c)))`→`:a`、`(cdr (quote (:a :b :c)))`→`(:b :c)`（リスト内要素も正しく印字）、`(quote foo)`→`foo`（`:`が付かないこと）。milestone 17〜22の既存テスト集約（`run-test-special-forms`/`run-test-dynamic-vars`/`run-test-block-return`/`run-test-gensym`/`run-test-macroexpand-1`/`run-test-numeric-tower`）も回帰確認し、いずれも`t`のまま。`*package*`変数・Lisp呼び出し可能な`make-package`/`find-package`/`in-package`・`export`/`use-package`/シャドーイング・修飾シンボル印字（`pkgname:symbol`）は明記された非ゴールとして実装せず（`lisp_make_package`/`lisp_find_package`はC内部APIのみ） |
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
