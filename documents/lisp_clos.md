# 最小CLOSサブセット導入マイルストーン

## 目的

ユーザーから、この自作Lisp処理系にCommonLisp Object System(CLOS)の最小サブセットを組み込みたい
という要望があった。要件は次の4点(ユーザーの原文):

1. classの定義
2. 標準オブジェクトの定義
3. 引数に応じて適用する関数の実装を変える仕組みの導入
4. `defun`はどのオブジェクトでも動くようにする(=既存の挙動を変えない)、`defmethod`は適用する
   関数を実行する

追加のヒアリングで以下の4点をユーザーが選択し、CLOSのスコープを確定した:

- ディスパッチは複数引数(multiple dispatch、CL標準相当) — 任意の位置のパラメータを特殊化できる
- クラスの継承は単一継承を含める
- specializerはユーザー定義クラス(standard-object)のみ。fixnum/cons/string等のビルトイン型は
  specializerにならない
- `defgeneric`は不要。`defmethod`が同名の総称関数を暗黙に生成する

パッケージシステムの拡充(マイルストーン68〜92)と同様、規模が大きいためマイルストーン
**96(class/instance/継承、ディスパッチ無し)・97(defmethod/総称関数/多重ディスパッチ)**の
2つに分けて記録する。また、CLOSに先行して`documents/lisp2_conversion.md`
(マイルストーン93〜95)でLisp-1からLisp-2(変数と関数の名前空間分離)への移行を行うため、
`defmethod`が暗黙生成する総称関数は`global_env`ではなく対象symbolの**関数セル**に格納する
(`%ensure-generic-function`/`%add-method`は`symbol-function`/`%set-symbol-function`経由で
読み書きする)。これにより総称関数は`defun`した関数と全く同じ名前空間で自然にshadow/再定義され、
実際のCLの設計(総称関数も関数namespaceに属する)と一致する。それ以外の設計
(class/instanceの構造、multiple dispatch、単一継承)はLisp-2化の影響を受けない。

マイルストーン番号は既存の1〜95に続く**96から**開始する。

## ファイル構成

新規ソースファイルは追加しない。既存の`src/lisp.c`/`src/lisp.h`・`lisp/stdlib.lisp`を拡張する。
新規テストファイルとして`test/lisp/test-clos.lisp`を追加する。

## 現状把握

- タグ付きポインタは2bitで完全に使い切っている(`LISP_TAG_CONS`/`FIXNUM`/`SYMBOL`/`CLOSURE`)。
  新しいオブジェクト種別(class/instance/generic-function)は既存の`LISP_TAG_CLOSURE`エスケープ
  ハッチパターン(`LispClosure`構造体、interpreted closure/builtin/macro/string/float/bignum/
  vector/compiled function/packageの9種がどのフィールドが非NULL/非0かで判別される)を
  再利用する以外の選択肢がない。
- `LispClosure`をallocateする箇所は正確に10箇所。`lisp_alloc_tracked`はフリーリストから
  再利用したスロットをゼロクリアしないため、全箇所が既存の全エスケープハッチフィールドを
  明示的に中立値へリセットしている。新フィールドを追加する場合は全箇所への追記が必要。
- GCマーキング(`lisp_gc_mark`)は`LISP_TAG_CLOSURE`用の単一分岐の中にゲート付きブロックが
  並んでいる形。新種別も同じ形のブロックを追加する。rootは`lisp_gc_mark_roots`が
  `global_env`/`global_packages`等を保持している。
- `lisp_apply`は`bytecode!=0`(VM再入)/`builtin!=0`(C関数呼び出し)/それ以外(interpreted
  closure)の3分岐。`OP_CALL`の非compiled-calleeパスは既にこの`lisp_apply`へ無条件に委譲して
  いるため、generic-function objectをVM側から呼ぶ際にVM自体への変更は不要——`lisp_apply`に
  4番目の分岐を追加するだけでよい。
- ハッシュテーブルは存在しない(識別ハッシュ`hash-code`のみ)。`defstruct`/`type-of`/`typep`/
  `class-of`も存在しない。レジストリが必要な場合は`global_packages`と同様、file-scopeの
  consリストで実装する。
- クラス名は`global_env`/関数セルとは別の新規レジストリ`global_classes`で管理する(symbolのeq
  で照合、パッケージのようなnickname機構は無いのでstring designator解決は不要)。
- `defclass`/`defmethod`は新規のC特殊形式やVM opcodeを追加せず、`lisp/stdlib.lisp`の
  `defmacro`として実装し、少数の新規Cビルトインの呼び出しへ展開する。これは`defpackage`が
  `make-package`/`export`/`use-package`等へ展開される既存パターン
  (`documents/lisp_package_operations.md`)と同型。メソッド本体は固定引数の`lambda`として
  VM経由でコンパイルされる。
- `lambda`/`defun`/`defmacro`の本体は単一式のみ。`defmethod`マクロ展開時は複数bodyフォームを
  `(cons 'progn body)`で1式へ畳む必要がある(既存の`dolist`等が同じ扱いをしている)。
- `lisp_print`は現時点でpackage種別に対する専用分岐が存在せず`#<closure>`へ落ちている。
  新規のclass/instance/generic-function種別には専用の印字分岐を必ず追加する(既存の抜けを
  繰り返さない)。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 96 | defclass・instance・単一継承(ディスパッチ無し) | 完了 | `LispClosure`へ`class_name`(クラス名symbol、非クラスなら`LISP_NIL`)・`class_superclass`(直接の親クラス、無ければ`LISP_NIL`)・`class_direct_slots`・`class_all_slots`(superclassの`class_all_slots`++`direct_slots`、defclass時に計算・キャッシュ)・`inst_class`(生成元クラス、非instanceなら`LISP_NIL`)・`inst_slots`(スロット値を保持する独立したvectorオブジェクト)の6フィールドを追加する。全allocate箇所(既存10箇所+新規`lisp_make_class`/`lisp_make_instance`の2箇所、計12箇所)へ`LISP_NIL`初期化を追記し、GCマークに`class_name`/`inst_class`のゲート付きブロックを追加する。新規レジストリ`global_classes`(file-scope consリスト)+`lisp_find_class`(symbolのeq線形探索、rootとして`lisp_gc_mark_roots`へ追加)、`lisp_is_class`/`lisp_is_instance`述語、`lisp_make_class`/`lisp_make_instance`を追加する。新規Cビルトイン`%make-class`(name/superclass-or-nil/direct-slots-listを受け、`global_classes`に既存なら同一オブジェクトを書き換え、無ければ新規作成)・`find-class`(見つからなければpanic、`find-package`のnil-on-miss方針とは意図的に異なり、specializer解決でnilと「無指定」を区別できなくなるためfail-fastにする)・`make-instance`(symbolなら`find-class`で解決、全スロットnil初期化)・`slot-value`/`set-slot-value`(`class_all_slots`をeq線形探索してindexを求め`inst_slots`を読み書き、無ければpanic)・`class-of`(`lisp_is_instance`必須、`inst_class`を返す)を追加する。`lisp/stdlib.lisp`に`defclass`マクロ(`(defclass name (superclass) (slot1 slot2 ...))`、superclassは0または1個、スロットはbare symbolのみで全てnil初期化)を追加する。`lisp_print`へ`lisp_is_class`/`lisp_is_instance`の新規分岐(`#<STANDARD-CLASS name>`/`#<name instance>`)を追加する。 |
| 97 | defmethod・総称関数・多重ディスパッチ | 未着手 | `LispClosure`へ`gf_name`(総称関数名symbol、非generic-functionなら`LISP_NIL`)・`gf_methods`(`((specializer-list . method-closure) ...)`のconsリスト)の2フィールドを追加する。全allocate箇所(96までの12箇所+新規`lisp_make_generic_function`、計13箇所)へ`LISP_NIL`初期化を追記し、GCマークに`gf_name`のゲート付きブロックを追加する(専用rootは不要、generic-function objectは`defmethod`実行時に対象symbolの関数セルへ格納されるため既存の「全internされたsymbolの`fn`をGCマークする」経路がそのまま到達する)。単一継承での多重ディスパッチアルゴリズムとして`lisp_class_hops_to_ancestor`(`class_superclass`を辿ってancestorまでのホップ数を返す)・`lisp_method_applicable`(各位置のspecializerがnilなら常に適用可、非nilなら実引数がinstanceでホップ数が求まることが必要)・`lisp_compare_method_specificity`(「無指定より指定ありが詳細」「指定ありどうしはホップ数が少ない方が詳細」の部分順序比較)・`lisp_gf_select_method`(適用可能なmethodの中から他に一度も劣後しなかったものを探す、適用可能なmethodが無ければ"no applicable method"、非劣後なものが2つ以上残れば"ambiguous method call"でpanic)を実装する。`lisp_apply`へ`closure->gf_name != LISP_NIL`の4番目の分岐(`lisp_gf_select_method`で選んだmethodへ`lisp_apply`を再度呼ぶ)を追加する(`OP_CALL`側はゼロ変更)。新規Cビルトイン`%ensure-generic-function`(`fboundp`かつ既存の`symbol-function`がgeneric-functionであればそれを返す、無ければ新規作成して`%set-symbol-function`でbind)・`%add-method`(`symbol-function`がgeneric-functionであることを確認し、既存entryと`specializer-list`が要素ごとにeq一致すれば置き換え・無ければ新規追加、アリティが異なれば"incongruent lambda list"でpanic)を追加する。`lisp/stdlib.lisp`に`defmethod-param-name`/`defmethod-param-specializer-form`の補助defunと`defmethod`マクロ(`(defmethod name ((p1 c1) (p2 c2) p3) body...)`、各パラメータはbare symbol(無指定)または`(name class-name)`(指定)、`&optional`/`&rest`/`&key`は非対応)を追加する。`lisp_print`へ`lisp_is_generic_function`の新規分岐(`#<GENERIC-FUNCTION name>`)を追加する。`lisp_apply`の各分岐は引数の"種類"を一切問わないため、`defun`で定義した通常の関数へinstanceを渡す既存の呼び出し経路(interpreted/compiled問わず)は一切変更されないことを確認する。 |

## スコープ外として明記する項目

**マイルストーン96:**

- `:initarg`/`:initform`/`:accessor`スロットオプション(全スロットnil初期化のみ)。
- 多重継承・C3 MRO(superclassは常に0または1個として扱う、2個以上の指定はバリデーション無しで
  無視)。
- サブクラスが親のスロット名を再宣言した場合の挙動(`class_all_slots`に重複が生じ、
  `slot-value`は先頭=親側のoccurrenceを解決する、未定義動作として扱う)。
- `change-class`、既にinstanceが存在するクラスを再定義した場合のマイグレーション。
- `:allocation :class`(クラス共有スロット)。
- 汎用的な`type-of`/`typep`、`standard-object`/`t`をルートとする組み込みクラス階層。

**マイルストーン97:**

- `&optional`/`&rest`/`&key`/`&aux`/`&allow-other-keys`を含む`defmethod`パラメータリスト。
- ビルトイン型(fixnum/cons/string/symbol/vector等)をspecializerとして使うこと。
- `:before`/`:after`/`:around`メソッドコンビネーション、`call-next-method`、`next-method-p`。
- `defgeneric`(明示的な総称関数宣言フォーム)。
- クラス・総称関数の削除・GC(`delete-package`相当の機構は用意しない)。
- `find-class`の`errorp`引数(常にpanicする、`nil`を返す経路は無い)。

## 検証方針

`make build`でクロスコンパイルが通ることを確認する。実装後、
`grep -n "class_name\s*=\|inst_class\s*=\|gf_name\s*=" src/lisp.c`でリセット箇所数を数え、
想定したallocate箇所数(96時点で12箇所×3フィールド、97で13箇所×3フィールド)と一致することを
セルフチェックする。`make test`で既存フィクスチャ全PASS(既存コードパスへの回帰が無いこと)と、
新規`test-clos.lisp`(class定義・instance生成・スロット読み書き・継承・instance独立性・単一/
複数specializerディスパッチ・メソッド再定義・defun非影響の回帰確認)のPASSを確認する。
`make test`だけでは検証できないpanicシナリオ(milestone78/81等と同様の方針)は個別のQEMU対話で
確認する:

- 2引数の総称関数で、どちらの特殊化specializer集合も他方を支配しない2つのmethodを定義し、
  両方の位置がinstanceで一致する呼び出しが"ambiguous method call"でpanicすること。
- 定義済みmethodのどれにも一致しない引数で呼び出すと"no applicable method"でpanicすること。
- `(find-class 'no-such-class)`がpanicすること。
- 異なるアリティで同名`defmethod`を追加すると"incongruent lambda list"でpanicすること。
- `'point`・`(make-instance 'point)`・`area`(総称関数、Lisp-2化後は`(symbol-function 'area)`
  または`#'area`で確認)をREPLで評価した際、`#<STANDARD-CLASS point>`/`#<point instance>`/
  `#<GENERIC-FUNCTION area>`と印字され、`#<closure>`にならないこと、panicしないこと。
- クラス・instance・総称関数を作成後に`(gc)`を実行しても、参照が失われた値が誤って回収され
  ず、instanceのスロット値が変化しないこと(`global_classes`のroot化とGCマークブロックの検証)。
- `*package*`切替のような特殊な手順(letによる動的束縛等)は不要(CLOSはmilestone79/80の
  ブートストラップ制約とは無関係)。
