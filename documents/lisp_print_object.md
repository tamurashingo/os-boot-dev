# print-object導入マイルストーン

## 目的

ユーザーから、`lisp_print`が印字する内容のうち、これまで区別されずに`#<closure>`へ
落ちていた種別を正しく表示するようにしたいという要望があった。要件は次の3点(意訳):

1. `print-object`を実装する。
2. これまで`#<closure>`と表示されていたものを、正しい種別として表示するようにする。
3. package含め、closureとして表示されていたが実際は区別されたobject種別であるものには、
   専用の`print-object`を用意する。

追加のヒアリングで以下2点が確定した:

- `print-object`はLisp拡張可能にする(`(defmethod print-object ((obj my-class)) ...)`で
  上書き可能)。milestone96/97で構築済みのCLOS `defmethod`/generic-function機構
  (`lisp_gf_select_method`等)をそのまま再利用する。
- package(および同様の非instance種別であるcompiled-function)は、milestone97で明示的に
  スコープ外とした「specializerはユーザー定義クラスのみ」という制約に抵触するため、
  defmethodでは上書きできないC側の専用分岐で固定表示する(class/instance/
  generic-functionの既存分岐と同じ非拡張パターン)。

規模と性質の違いから、マイルストーン**98(print-object総称関数・write-string・princ)**・
**99(package・compiled-functionの専用印字分岐)**の2つに分けて記録する。

マイルストーン番号は既存の1〜97に続く**98から**開始する。

## 現状把握

- `LispClosure`のエスケープハッチ12種のうち、package(`pkg_name!=0`)とcompiled-function
  (`bytecode!=0`、述語`lisp_is_compiled`はmilestone52頃から存在するが`lisp_print`内では
  一度も呼ばれていなかった)の2種が専用分岐を持たず、汎用の`#<closure>`フォールバックへ
  落ちていた。`documents/lisp_clos.md`68-70行目で既に把握されていたが、当時は
  対応されなかった既知の債務だった。
- この処理系には文字列連結・`format`等のLisp側文字列構築プリミティブが一切存在しない
  (全`LISP_REGISTER_BUILTIN`呼び出しを網羅的にgrepして確認)。`print-object`のmethod本体を
  文字列として組み立てて返す設計は取れないため、コンソールへ直接書き込む副作用的な設計とし、
  新規ビルトイン`write-string`(`write-line`の改行無し版)・`princ`(任意の`LispObject`を
  `lisp_print`経由でコンソールへ書き出す)を追加する。
- `lisp_print`のトップレベル呼び出し元はコード全体で`src/main.c`のREPLループの1箇所のみで
  常にコンソールストリームであるため、method本体が(渡された`stream`引数ではなく)常に
  コンソールへ直接書き込むことは現時点では挙動の後退にならない(将来Lisp側へ本格的な
  streamを導入する際は再検討が必要な既知の限定事項)。
- boot順序(`lisp_heap_init`→`lisp_packages_init`→`lisp_symbols_init`→`lisp_builtins_init`→
  compiler.lisp/stdlib.lisp読込→REPL)により、`defclass`マクロ自体がstdlib.lispで定義される
  ため、CLOS instanceはstdlib.lisp読込完了より前には存在し得ない。`print-object`の既定
  methodを`lisp_builtins_init`(stdlibロードより前に実行される)内でC側から直接登録して
  おけば、instanceが`lisp_print`へ到達する時点では常にbind済みであることが保証される。
- `lisp_method_applicable`は「specializerがNILなら常に適用可(実引数の型を問わない)」という
  実装のため、`print-object`の既定method(specializer-list=`(nil)`)はinstance以外の引数でも
  「適用可能」と判定されてしまう。既定method自身に`lisp_assert_instance`を入れ、
  `(print-object 5)`のような誤用をpanicで止める。
- **defun/lambdaの本体は単一formのみ**という既存の制約(milestone21で確認済みの
  "progn gotcha")は、テストコード自身が複数の副作用文を持つ場合にも適用される。本
  マイルストーンのテスト作成時、この制約を見落として複数formを`defun`直下に並べ、2つ目以降が
  黙って無視される(コンパイルもされず、後続の別top-levelフォームとしても評価されない)不具合
  にはまった。`defmethod`マクロは自動で`(cons 'progn body)`しているため気付きにくいが、
  生の`defun`は自動でprognしない。テストコードでも既存の`clos-move-point`と同じ要領で
  明示的に`progn`で包む必要がある。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 98 | print-object総称関数・write-string・princ | 完了 | `lisp_print`のinstance分岐を、ハードコードされた`#<name instance>`直接出力から`print-object`総称関数への委譲(`lisp_apply(print_object_gf, (obj))`)へ書き換える。新規ビルトイン`write-string`(文字列を要求し改行無しでコンソールへ書き出し、`t`を返す)・`princ`(任意の`LispObject`を`lisp_print`経由でコンソールへ改行無しで書き出し、引数自身を返す)を追加する。既定`print-object`method(`lisp_builtin_default_print_object`、`lisp_assert_instance`で非instanceをガードしつつ従来と同じ`#<name instance>`を出力)を`lisp_make_builtin`でbuiltin closure化し、`lisp_builtins_init`内で`%ensure-generic-function`/`%add-method`をCから直接呼び出して起動時に登録する(`defmethod`マクロはstdlib.lisp読込完了まで使えないため)。`*package*`切替後も同一symbolを指し続けるよう`lisp_sym_print_object`を`lisp_symbols_init`内でキャッシュする(milestone78と同じ理由)。 |
| 99 | package・compiled-functionの専用印字分岐 | 未着手 | `lisp_print`のgeneric-function分岐の直後に、package(`#<PACKAGE name>`)とcompiled-function(`#<COMPILED-FUNCTION>`、名前情報を持たないため`#<builtin>`/`#<macro>`と同様の無名形式)の2つの専用分岐を追加する。class/instance/generic-functionと同じ非拡張パターン(`defmethod print-object`では上書きできない)。 |

## スコープ外として明記する項目

- `format`/文字列連結プリミティブ(`concatenate`等)の追加。`print-object`は副作用的な
  「コンソールへ直接書き込む」設計に留める。
- `:before`/`:after`/`:around`メソッドコンビネーション、`call-next-method`
  (milestone97のスコープ外がそのまま引き継がれる)。
- package/compiled-function以外のビルトイン型(fixnum/cons/string/vector等)への
  `print-object`拡張。これらは既存の専用分岐(vector等)または`lisp_print`のcatch-all
  (`#<builtin>`/`#<macro>`/`#<closure>`)のまま変更しない。
- `stream`引数を実際に使い分けること。`write-string`/`princ`/既定`print-object`method
  は常にコンソールストリームへ直接書き込み、渡された`stream`引数を無視する(現時点で
  `lisp_print`のトップレベル呼び出し元がコンソール以外に存在しないため)。

## 検証方針

`make build`でクロスコンパイルが通ることを確認する。`make test`で既存フィクスチャ全PASS
(回帰無し)と新規`test-print-object.lisp`のPASSを確認する。

- **マイルストーン98**はディスパッチ挙動をt/nilの観測に落とせるため`make test`で検証する
  (`test/lisp/test-print-object.lisp`): ユーザー定義`(defmethod print-object ((obj
  my-class)) ...)`が既定methodではなく実際に呼ばれること、継承経由のフォールバック
  (specificity比較)、既定method(オーバーライド無し)がinstanceをpanic無しで返すこと、
  `write-string`/`princ`の戻り値(`t`/引数自身)。副作用として改行無しのコンソール出力を
  行うテストは、末尾で`write-line ""`を呼び、`scripts/run_test.py`が探す`RESULT ...`行と
  混ざらないようにする。
- **マイルストーン99は`make test`では検証不能**(印字テキストそのものを観測する手段が
  この処理系に無い)。個別QEMU対話で確認する:
  - `*package*`が`#<PACKAGE COMMON-LISP-USER>`と表示されること(`#<closure>`にならない)。
  - REPL上で`defun`した通常の関数の`symbol-function`が`#<COMPILED-FUNCTION>`と
    表示されること。比較として`(symbol-function 'car)`のようなCビルトインは
    `#<builtin>`のまま変化しないこと。
- 個別QEMU対話でのみ確認する項目(milestone98):
  - オーバーライド無しinstanceの表示が従来と同じ`#<name instance>`のままであること
    (視覚的な後退が無いことの確認)。
  - オーバーライドmethod内の`write-string`/`princ`呼び出しが実際にコンソールへ正しく
    出力されること。
  - `(print-object 5)`(非instance直接呼び出し)が`lisp_assert_instance`によりpanic
    (`expected a CLOS instance but got something else`)すること。
