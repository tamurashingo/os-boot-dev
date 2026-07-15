# パッケージシステム再設計（CommonLispサブセット化）マイルストーン

## 目的

このドキュメントは、`bare_metal_lisp.md`のマイルストーン23で導入した最小限のパッケージシステム
（`common-lisp-user`/`keyword`の2パッケージのみ、`*package*`なし、リーダー・プリンターの修飾子
構文なし）を、真のCommonLispサブセットへ再設計するためのマイルストーン群である。

現状は次の3点が特に不十分である:

- `lisp_intern`が常に`common-lisp-user`パッケージをハードコードした対象とし、「現在のパッケージ」
  という概念そのものが存在しない。
- リーダー・プリンターに`pkg:sym`/`pkg::sym`のような修飾子構文が一切なく、`export`/`use-package`
  もLispから呼び出す手段がない。
- `LispPackage`は生C構造体（固定8個までの静的配列、パッケージあたり最大512シンボルの固定配列）
  でGC追跡対象の外にあり、`LispSymbol.package`もGCが辿れない生ポインタである。

本ロードマップが目指すのは、単にマクロで見せかけの名前空間分離を作ることではなく、**シンボルの
アイデンティティ（`eq`同一性）そのものをパッケージごとに完全分離し、リーダーとGCをそれに対応
させる**ことである。これは将来のマルチプロセス化に向けた土台であり、プロセスごとに`*package*`の
コンテキストを切り替えるだけでメモリ空間全体のシンボル安全性が確保できることを最終的な狙いとする
（マルチプロセス機能自体は本ロードマップの対象外）。

前提となる制約は他のドキュメントと同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証は`make test`（`test/lisp/`配下の
  フィクスチャをQEMU/OVMFヘッドレス起動で実行するハーネス）とREPL基本動作の目視確認で行う。

マイルストーン番号は既存の1〜67に続く**68から**開始する（68〜82の非公式使用が無いことをgrepで
確認済み）。フェーズF（83〜84）はmilestone78で発見した非tail位置スタックリークの、フェーズG
（85〜86）はmilestone81着手時に発見した特殊形式トークンの可視性問題の、それぞれ切り出しにより
追加した。フェーズH（87）は本ロードマップのフェーズ構成（A〜G）とは無関係な内容（コンパイラ自身の
自己ブートストラップ関数のCスタック深度対策）だが、ユーザーの判断によりマイルストーン番号の
グローバルな連番管理を優先し、本ドキュメントへ追記する形とした。

### 設計にあたり確定している方針

- **可視性制御は`export`/`use-package`まで実装し、シャドーイングは対象外とする。** `use-package`
  時の名前衝突は最小限のエラー化のみとし、CLの`shadow`/`shadowing-import`相当の解決機構は作らない。
- **`LispPackage`は第一級のGC管理オブジェクトにする。** タグ空間（2bit、CONS/FIXNUM/SYMBOL/CLOSURE
  で使い切り）に空きがないため、文字列/float/bignum/vector/コンパイル済み関数が使う既存の
  `LISP_TAG_CLOSURE`エスケープハッチ（「どの任意フィールドが非NULLか」で型判別するパターン）を
  再利用する。これにより`(find-package ...)`等がLisp値として本物のパッケージオブジェクトを返せる。
- **パッケージのシンボル集合／exportリスト／useリストはすべてconsリストで表現する。** 新規の
  可変長ベクタ機構は作らず、既存の`lisp_gc_mark`がconsの`car`/`cdr`を汎用的に辿れる仕組みにそのまま
  乗せる。パッケージあたり最大512シンボルという既存の容量上限も同時に撤廃できる。
- **`*package*`の既定値は`common-lisp-user`パッケージオブジェクトとする。** これにより`lisp_intern`
  のハードコード先切替は挙動不変のカットオーバーになり、`lisp_vm_integration.md`の2軸フォール
  バックのような新規実行時分岐を作らずに済む。
- **`lisp_intern_keyword`は`*package*`/`use-package`の影響を受けず、常に`keyword`パッケージへ固定
  したままにする。** `:foo`がどの文脈から読んでも同一シンボルを指すという既存の自己評価・`eq`保証
  （マイルストーン23）を壊さないため。
- **パッケージ名の重複作成はエラーではなく既存オブジェクトを返す。** `defpackage`/`in-package`を
  含むファイルを`load`で再読込しても状態が壊れないようにする（`defvar`再load冪等性と同じ考え方）。
- **`compiler.lisp`/`stdlib.lisp`は新設パッケージへ分離せず、引き続き`common-lisp-user`へinternする。**
  全組み込みシンボルの付け替えは本設計の主目的（同一性分離の枠組み自体）と独立なリスクであり、
  将来の別マイルストーンに委ねる。
- **`OP_GLOBAL_REF`/`OP_GLOBAL_SET`のグローバル参照解決方式（`lisp_vm_integration.md`マイルストーン
  51、`global_env`をシンボル同一性で都度探索）は変更しない。** パッケージ再設計が影響するのは
  「internの結果どのシンボルオブジェクトが得られるか」だけであり、得られたシンボルの`global_env`
  解決方法とは無関係のまま保つ。

## ファイル構成

新規ソースファイルは追加しない。既存の3ファイル構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／
`src/main.c`）と`lisp/stdlib.lisp`（`defpackage`マクロの追加先）をそのまま使う。検証用フィクスチャは
既存の`test/lisp/test-package.lisp`を拡張する。

## マイルストーン一覧

### フェーズA: ヒープオブジェクト化とGC統合（新機能なし、内部表現の置き換えのみ）

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 68 | `LispPackage`のヒープオブジェクト化 | 完了 | `LispPackage`を生C構造体から`LISP_TAG_CLOSURE`のescape hatchを共有する新規フィールド群（`pkg_name`/`pkg_symbols`/`pkg_exports`/`pkg_uses`/`pkg_is_keyword`）に置き換え、`lisp_alloc_tracked`経由で確保する`lisp_make_package_object`を新設した。symbols/exports/usesは当初計画（マイルストーン71）を前倒しして最初からconsリストとして持つ設計にしたため、`LISP_MAX_SYMBOLS`/`LISP_PACKAGE_NAME_MAX`は本マイルストーンの時点で完全に撤廃した。`lisp_packages[LISP_MAX_PACKAGES]`/`lisp_cl_user_package`/`lisp_keyword_package`は生`LispPackage *`からタグ付き`LispObject`へ変更した。`LispSymbol.package`は`LispPackage *`から`struct LispClosure *`へ retype したが、タグ付き`LispObject`化（マイルストーン70の予定スコープ）は未対応で生ポインタのまま残している。パッケージ自体がGC追跡対象のヒープオブジェクトになったため、`lisp_gc_mark_roots`を各パッケージ本体＋`pkg_symbols`/`pkg_exports`/`pkg_uses`を個別にmarkする実装へ拡張した（`lisp_gc_mark`のclosure汎用分岐にパッケージ専用フィールドを教える一般化はマイルストーン72へ委ねる）。`test/lisp/test-package.lisp`は無修正のまま全パスしている。 |
| 69 | `global_packages`リストと`lisp_make_package`/`lisp_find_package`の一般化 | 完了 | `global_packages`（consリスト）を新設し、マイルストーン68時点でも残っていた`lisp_packages[LISP_MAX_PACKAGES]`/`lisp_package_count`/`LISP_MAX_PACKAGES`（8個上限とその超過時のpanic）を完全に撤廃した。当初デッドコードだった`lisp_find_package`を`global_packages`の線形探索として実装し、`lisp_make_package`はまずこれを呼んで名前重複時は既存オブジェクトを返す（冪等性）ようにした。`lisp_gc_mark_roots`は`lisp_gc_mark(global_packages)`1回でスパインの各consセル・各パッケージオブジェクト自体が連動して辿られるようになったため、配列インデックスループを廃した（symbols/exports/usesの個別markはマイルストーン72まで引き続き必要）。この時点ではLisp側APIはまだ公開していない。`test/lisp/test-package.lisp`は無修正のまま全パスしている。 |
| 70 | `LispSymbol.package`のタグ付き`LispObject`化 | 完了 | `LispSymbol.package`を`struct LispClosure *`（マイルストーン68で生`LispPackage *`から retype 済み）から`LispObject`（#68の新表現へのタグ付きポインタ）へ変更した。もはや不要になった前方宣言`struct LispClosure;`は削除した。`->package->pkg_is_keyword`を直接読んでいた既存3箇所（printer/eval自己評価分岐/`keywordp`）は新設の`lisp_symbol_package_is_keyword(LispSymbol *)`アクセサ経由に統一し、`sym->package != LISP_NIL`チェックを1箇所に集約した。書き込み側は`lisp_intern_in_package`の`sym->package = pkg_cell`を`sym->package = pkg`（タグ付き引数をそのまま代入）に、gensym経路の`lisp_make_uninterned_symbol`の`sym->package = 0`を`sym->package = LISP_NIL`に変更した。`grep -n '\->package\b\|\.package\b' src/lisp.c src/lisp.h src/main.c`で全参照箇所を洗い出し、漏れなく更新済みであることを確認した。`test/lisp/test-package.lisp`は無修正のまま全パスしている。 |
| 71 | パッケージ内シンボル集合のconsリスト化 | milestone68へ統合済み | 当初計画では本マイルストーンで固定配列`symbols[LISP_MAX_SYMBOLS]`+`symbol_count`からconsリストへ切り替える予定だったが、マイルストーン68の`LispPackage`ヒープオブジェクト化の実装時点で、新規フィールドを追加するならconsリストとして持つ方が固定配列を経由しない自然な設計になると判断し、`pkg_symbols`/`pkg_exports`/`pkg_uses`を最初からconsリストとして導入した。`LISP_MAX_SYMBOLS`の容量上限とパニックもマイルストーン68の時点で撤廃済み。本マイルストーンはその時点で完了済みとして統合し、個別の実装作業は発生しない。マイルストーン18の「名前は1回だけ切り詰めてcompare/storeの両方に同じバッファを使う」規律は`lisp_intern_in_package`にそのまま維持されている。 |
| 72 | GCルートスキャンの`global_packages`一本化 | 完了 | `lisp_gc_mark`のclosure分岐（`vec_data`/`constants`ループの直後）に、対象が`pkg_name != 0`のパッケージであればsymbols/exports/usesの各consリストも辿る処理を追加した。これにより`lisp_gc_mark_roots`側で個別に行っていた「`global_packages`を辿って各パッケージのpkg_symbols/pkg_exports/pkg_usesを直接mark」するループが不要になり、`lisp_gc_mark(global_packages)`1行のみへ置き換えた（`vec_data`/`constants`ループと対称な設計）。パッケージ自体・全所属シンボルがGCで回収されないことの確認は、`(gc)`をLisp側から呼ぶテストが`test/lisp/`配下のファイルに置けないため（下記の既知の制約参照）、QEMUを個別に起動しREPLへ直接（`load`を経由せず）`(quote milestone72-sym-NN)`で複数の新規シンボルをintern→`defvar`で捕捉→`(gc)`→`(eq ...)`の順に手入力し、GC後も全てtになることを目視確認した。 |

**既知の制約（対応せず明記）**: `(gc)`を`load`経由（＝`test/lisp/`配下のフィクスチャや`init.lisp`を含む本ロードマップの`make test`ハーネス全体）で呼ぶと`Lisp panic: expected a cons cell but got something else`でpanicする既存バグがある。原因は`lisp_load_eval_buffer`（milestone16）がファイル内の全トップレベルformを読み切ってCローカル変数`forms`/`reversed`のconsリストに積んでから1つずつ評価する実装になっており、この「評価待ちの残りform群」がGCルート集合（`lisp_gc_mark_roots`）に含まれていないため、evalの合間に`(gc)`が走ると未評価の残りformがまだ何にも参照されていないと誤認されて回収されてしまうこと。REPLループ（`src/main.c`）は1行読んだら即評価し次を読むまで何も保持しないため無関係でこのバグを踏まない。milestone33のGC実装時点から存在する無関係の既存不具合であり、本ロードマップの対象外として修正しない（`test/lisp/test-package.lisp`に`(gc)`呼び出しを含む自動テストを追加しない理由もこれに拠る）。

### フェーズB: `*package*`とリーダー拡張

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 73 | `*package*`動的変数の導入と`lisp_intern`の切替 | 完了 | `*package*`シンボル自身を`lisp_packages_init`内で`lisp_intern`ではなく`lisp_intern_in_package(lisp_cl_user_package, "*package*")`で直接internし、`*macroexpand-hook*`（マイルストーン21）と同じ形で`is_special = 1`・`value = lisp_cl_user_package`を直接セットアップした（`lisp_intern`自身が今後この値を読むため、`*package*`を確立するintern呼び出しが循環しないよう直接APIを使う必要があった）。`lisp_intern(name)`は`lisp_intern_in_package(lisp_cl_user_package, name)`のハードコードから`lisp_intern_in_package(lisp_symbol_cell(lisp_sym_package)->value, name)`に変更した（`lisp_intern_keyword`は無変更）。`lisp_intern_in_package`の定義がこれより後にあったため、循環importを避けつつ前方宣言を増やさないよう`lisp_packages_init`自体を`lisp_intern_in_package`の直後に定義順を移動した。既定値が旧ハードコード先（`common-lisp-user`）と同一のため、`in-package`が未実装な現時点では全既存挙動が不変であることを確認した。`test/lisp/test-package.lisp`に`run-test-package-star-package-var`（`special-variable-p`で`*package*`が動的変数として確立されていること、`(eq *package* nil)`が`nil`であることを確認）を追加し、`run-test-package`の`and`チェーンに組み込んだ。`make build`および`make test`（21件）全PASSを確認した。 |
| 74 | リーダーの`pkg:sym`/`pkg::sym`修飾子対応 | 完了 | `lisp_read`の既存トークン分類`if`チェイン（先頭`:`の既存keyword判定より後、文字スキャンループ自体は無変更）に、捕捉済みトークンを走査して最初の`:`の位置でパッケージ名部分を切り出す処理を追加した（`:`はもとから`lisp_reader_is_delim`の区切り文字ではないため、`pkg:sym`は最初から1つのtokenとして捕捉されている）。`::`（2文字目も`:`）ならinternal修飾子として`lisp_intern_in_package`へそのまま委譲（exportされていないシンボルも解決できる）、単一`:`ならexternal修飾子として対象パッケージの`pkg_exports`consリストを`lisp_streq`で線形探索し、見つからなければreaderエラーとする。パッケージ名が`lisp_find_package`で見つからない場合、および修飾子の前後どちらかが空文字列（`"foo:"`等）の場合もreaderエラーとする。 |

### フェーズC: パッケージ操作関数と`defpackage`/`in-package`

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 75 | `make-package`/`find-package`ビルトイン | 完了 | `lisp_builtin_make_package`/`lisp_builtin_find_package`を新設し、それぞれ引数（Lisp文字列）を`lisp_assert_string`で検証した上で、#69で用意済みの`lisp_make_package`/`lisp_find_package`（いずれも既にstaticでC内部専用だった関数）へそのまま委譲する薄いラッパーとして実装した。`lisp_make_package`は名前重複時に既存オブジェクトを返す冪等性を持つため、`make-package`をLispから同名で何度呼んでも同一オブジェクト（`eq`）が返る。`lisp_find_package`は未存在の名前に対してもとから`LISP_NIL`を返す実装だったため変更不要だった。`env`への登録は既存の`keywordp`の直後に追加した。 |
| 76 | `export`ビルトインとexportリスト | 完了 | `lisp_builtin_export`を新設した。`(export symbols &optional package)`の形で、`symbols`は単一のsymbolまたはsymbolのリスト、`package`省略時は`*package*`（現在のパッケージ）を対象にする。対象パッケージの`pkg_exports`consリストを`==`（`eq`と同じポインタ比較）で線形探索し、既に含まれていなければ追加する（重複追加しない）。#74のリーダーの単一コロン修飾子（`pkg:sym`）はこの`pkg_exports`をそのまま参照するため追加の変更は不要だった。「`export`を評価した後にその効果を要求する`pkg:sym`をリーダーで読む」という順序は、`lisp_load_eval_buffer`がファイル全体を読み切ってから評価する実装のため`test/lisp/`配下のファイル（`load`経由）では組めない（milestone72の既知の制約と同根）。この組み合わせの検証は、C内部から直接`lisp_builtin_export`と`lisp_read_from_buffer`を順に呼ぶ自己テスト`lisp_reader_export_selftest`（`main.c`起動シーケンスに組み込み）と、個別の対話REPLセッションでの目視確認（`(make-package ...)`→`(export ...)`→`(eq (quote pkg:sym) (quote pkg::sym))`→`t`）の両方で行った。`test/lisp/test-package.lisp`には、`load`の制約に抵触しない範囲（単一シンボル・シンボルのリスト・package明示指定・2回呼んでもエラーにならないことをいずれも`t`が返ることで確認）の`run-test-package-export`を追加した。 |
| 77 | `use-package`ビルトインとuse-list継承を反映したintern解決 | 完了 | `lisp_intern_in_package`の探索順序を「自パッケージのローカルシンボル（`pkg_symbols`）→useしている各パッケージのexportシンボル（`pkg_uses`の各要素の`pkg_exports`）→見つからなければ新規作成（自パッケージへ）」に拡張した（自パッケージのローカル探索と新規作成の間に挿入）。`lisp_resolve_package_designator`を新設し、パッケージオブジェクトそのもの・パッケージ名の文字列のいずれも受け付けるようにした（文字列で未存在の名前を渡すとpanic）。`lisp_builtin_use_package`を新設し、`(use-package packages-to-use &optional package)`の形で、`packages-to-use`は単一のパッケージ（オブジェクトまたは名前の文字列）またはそれらのリスト、`package`省略時は`*package*`を対象にする。対象パッケージの`pkg_uses`consリストへ`eq`（`==`）基準で重複なく追加する（同じパッケージを再度useしても冪等）。名前衝突は最小限のガードとして、追加しようとしているパッケージの各exportシンボルの名前文字列（`lisp_streq`で比較）が、(a)対象パッケージの既存ローカルシンボル、または(b)既にuse済みの別パッケージのexportシンボルと一致し、かつ`eq`で同一オブジェクトでない場合に`lisp_panic`する（shadowingは対象外）。「`use-package`を評価した後、その効果に依存する無修飾名の解決をリーダー/`lisp_intern_in_package`で行う」という順序は、`lisp_load_eval_buffer`がファイル全体を読み切ってから評価する実装のため`test/lisp/`配下のファイル（`load`経由）では組めない（milestone76と同根の制約）。加えて`*package*`を切り替える`in-package`が未実装（milestone78）のため、Lispから無修飾名の解決先を能動的に切り替える経路自体が現時点で無い。この組み合わせの検証は、C内部から直接`lisp_make_package`/`lisp_builtin_export`/`lisp_builtin_use_package`/`lisp_intern_in_package`/`lisp_intern`を順に呼ぶ自己テスト`lisp_reader_use_package_selftest`（`main.c`起動シーケンスに組み込み、use-list経由の解決・冪等性・パッケージ名文字列指定・リスト指定・package省略時の`*package*`対象化を検証）で行った。2つの名前衝突panicパス（既存ローカルシンボルとの衝突／use済み別パッケージ間の衝突）は、標準指示に従い`make test`ではなく個別の対話REPLセッションで確認し、いずれもpanic後にREPLが正常復帰し以降の評価（`(+ 1 2)` → `3`）に影響しないことを確認した。`test/lisp/test-package.lisp`には、`load`の制約に抵触しない範囲（単一パッケージ・パッケージのリスト・パッケージ名の文字列・package省略・同じパッケージの再use）で`use-package`がエラーなく`t`を返すことを確認する`run-test-package-use`を追加した。 |
| 78 | `defpackage`マクロと`in-package`関数 | 完了 | `intern`（`(intern name &optional package)`、`lisp_intern_in_package`の薄いラッパー）と`in-package`（パッケージ名を受け取り`find-package`で見つからなければpanic、見つかれば`*package*`のシンボルセルの`value`を直接書き換える）をビルトインとして追加した。`defpackage`は`lisp/stdlib.lisp`にLispマクロとして実装し、`:export`/`:use`句（いずれも文字列のみ、複数指定可）を`make-package`+`export`+`use-package`呼び出し列へ展開する（`:nicknames`等の他句は対象外）。同名`defpackage`の再実行は#69の重複名冪等性によりべき等になる。実装中に2件のバグを発見・修正した：(1) `lisp_eval_toplevel`が特殊形式シンボル`compile-and-run`を他の特殊形式シンボルと異なり毎回`lisp_intern`し直していたため、`*package*`が`common-lisp-user`以外に切り替わった後は毎回別オブジェクトが生成され`global_env`解決に失敗し、`in-package`後のREPLが完全にロックする重大バグがあった。他の特殊形式シンボルと同様`lisp_symbols_init`内で1度だけinternしキャッシュする`lisp_sym_compile_and_run`に置き換えて解決した。(2) `defpackage`の展開結果（複数statementの`progn`）を`eq`の引数として直接比較すると`nil`になる（本来`t`になるべき）バグを発見した。原因はpackagesとは無関係な既存のVM/コンパイラの一般的な欠陥で、`compile-progn-body`が複数statementの`progn`を`(let ((捨てる束縛 e1)) (progn e2...))`へ脱糖しており、`let`の`OP_MAKE_LOCAL`がボックス化した値をスタック上に永続的に残す（対応する解放命令が無い）ため、`progn`がtail位置以外（関数呼び出しの引数等）で使われるとスタックに残ったゴミが後続の計算（`OP_CALL`のnargsが期待するスタック深さ）を壊していた。この問題はVMに新設した「スタック最上位を単純に捨てる」専用命令`OP_POP`（`src/lisp.h`/`src/lisp.c`）を導入し、`compile-progn-body`の複数statementケースを`let`経由の脱糖からe1評価後に`OP_POP`を発行する形へ書き換えて解決した（`and`/`cond`/`when`/`unless`/`let*`の本体は最終的に`progn`へ委譲するため同時に修正される）。この過程で、`OP_POP`修正の対象外である`let`/`let*`/`or`（および`cond`のtest-onlyクローズ）自体も、実際に使われる束縛を持つ非tail位置の兄弟引数として2個以上使われると同種の原因でpanicするという、より広範囲な既存バグを発見した。ユーザーの明示的な判断により、本マイルストーンでは`progn`の修正のみを対象とし、`let`/`let*`/`or`側の根本修正は#83（フェーズF、新設）として切り出した。 |

### フェーズD: プリンタ対応とブートストラップ整理

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 79 | プリンタの修飾シンボル印字対応 | 完了 | `lisp_print`のシンボル分岐に3つの静的ヘルパー（`lisp_symbol_exported_from`/`lisp_symbol_is_exported`/`lisp_symbol_visible_in_current_package`）を新設して組み込んだ。可視性判定は`lisp_intern_in_package`（#77）の探索順序「自パッケージのローカルシンボル→useしている各パッケージのexportシンボル」と対称にし、プリンタがリーダーの逆変換になるようにした：対象シンボルのホームパッケージが現在の`*package*`と同一、またはuseしている先パッケージからexportされていれば可視＝無修飾で印字する。非可視ならexportされていれば`pkgname:symbol`、されていなければ`pkgname::symbol`で印字する。keywordパッケージのシンボルは既存の`":"`前置分岐が先に判定されるため無変更。gensymによるuninterned symbolは`sym->package == LISP_NIL`のためこの判定自体を経由せず、既存どおり無修飾のまま印字される。検証は`make test`によるフルスイート（17ファイル全PASS、`*package*`は常に`common-lisp-user`のままのため印字結果は完全に不変）と、個別の対話REPLセッション（QEMUシリアル経由）で行った：`(defpackage "print-test-a" (:export "shared-a") (:use "common-lisp-user"))`でexportありシンボルと`(intern "priv-a" ...)`でexportなしシンボルを用意し、`common-lisp-user`から`'print-test-a:shared-a`→`print-test-a:shared-a`（単一コロン）、`'print-test-a::priv-a`→`print-test-a::priv-a`（二重コロン）と印字されることを確認。さらに`print-test-a`をuseする`print-test-b`へ`in-package`し、`'shared-a`（useしている先のexportシンボル、可視）→無修飾`shared-a`、`'print-test-a::priv-a`（use先だが非export、非可視）→`print-test-a::priv-a`のまま、と可視性判定がuseリストのexportの有無だけで正しく切り分けられることを確認。`(gensym)`の結果は`*package*`に関わらず常に無修飾（例:`G16`）、キーワード`:foo`は既存どおり`:foo`のまま、いずれも変更なしを確認した。この対話検証の過程で、`common-lisp-user`のビルトイン（`intern`/`find-package`/`in-package`自身を含む）は一切`export`されていないため、`:use "common-lisp-user"`していても`*package*`を他パッケージへ切り替えた後は無修飾でビルトインを呼べなくなる（`in-package`ですら無修飾では呼び戻せず、`(common-lisp-user::in-package ...)`のような二重コロン修飾呼び出しでしか復帰できない）という、ブートストラップ文脈の実用上の制約を確認した。これは印字とは無関係な既存設計（cl-userの意図的な非export）の直接的な結果であり、#80（ブートストラップのパッケージ文脈確定）の検討時に踏まえるべき既知事項として記録する。 |
| 80 | ブートストラップのパッケージ文脈確定 | 完了 | `src/main.c`の`EfiMain`起動順序（`lisp_heap_init`→`lisp_packages_init`→`lisp_symbols_init`→`lisp_builtins_init`→`compiler.lisp`/`stdlib.lisp`のload→各自己テスト→`init.lisp`→REPL）を確認したところ、`lisp_packages_init`（milestone73で新設）は元から`global_packages`初期化・`*package*`（`lisp_sym_package`の`value`）のcommon-lisp-userへのseedまで完結させてから返る実装になっており、`lisp_symbols_init`はその後に呼ばれる既存の並びのままで前提を満たしていたため、`main.c`の呼び出し順序自体はコード変更不要だった。この「順序が既に正しいこと」と「`compiler.lisp`/`stdlib.lisp`のシンボルが引き続き`common-lisp-user`へinternされ、無修飾で再internした同名シンボルと`eq`であること」を明文化して固定するため、新規の自己テスト`lisp_bootstrap_package_context_selftest`（`src/lisp.c`/`src/lisp.h`、`main.c`起動シーケンスに`lisp_reader_defpackage_selftest_run`の直後・`lisp_load_init_file`の直前に組み込み）を追加した。検証内容は、(1) `*package*`の現在値が`common-lisp-user`パッケージオブジェクトであること、(2) `lisp_symbols_init`でintern済みの特殊形式シンボル`defun`が`common-lisp-user`へ帰属し、無修飾で再internすると同一オブジェクト(`eq`)を返すこと、(3) `stdlib.lisp`で`defun`定義された`list`のシンボルも`common-lisp-user`へ帰属し、`global_env`で実際にクロージャとして束縛されていること（`unbound variable`にならないこと）。`make test`でtest/lisp/配下の全17テストファイルが本自己テストの`PASS`ログを含めて成功することを確認した。 |

### フェーズE: 正しさ検証

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 81 | VM/コンパイラのグローバル参照とシンボル同一性の回帰検証 | 完了 | 着手時、`*package*`を`common-lisp-user`以外へ切り替えると`:use "common-lisp-user"`していても`defun`/`if`/`let`等の特殊形式トークンまで`unbound variable`でpanicすることを対話REPL検証で発見した。原因は特殊形式シンボル（`lisp_sym_defun`等）がeval側で`eq`同一性チェックされる一方、`common-lisp-user`自身のビルトイン・特殊形式トークンが一切`export`されていないため（milestone79で発見した制約と同根）、別パッケージで読んだ`defun`という語がuse-list経由で`lisp_sym_defun`へ解決されず、別の新規シンボルとして生成されてしまうことにある。ユーザーの明示的な判断により、この特殊形式可視性の問題自体はmilestone78の`progn`スタックリーク発見時と同様のパターンで新規マイルストーン（#85〜86、フェーズG）として切り出し、本マイルストーンは`*package*`導入後もグローバル参照がシンボル`eq`同一性で正しく解決されるかの回帰検証に限定してスコープを狭めた。新設した自己テスト`lisp_global_ref_package_identity_selftest`（`src/lisp.c`/`src/lisp.h`、`main.c`起動シーケンスに`lisp_bootstrap_package_context_selftest_run`の直後・`lisp_load_init_file`の直前に組み込み）は次の3点を検証する: (a) `common-lisp-user`内で`defun`による前方参照・相互再帰（`m81-even`/`m81-odd`の相互再帰関数を`lisp_eval`で定義し`(m81-even 10)`が`t`を返すこと）が既存カバレッジと同様に動作すること、(b) `*package*`を（`lisp_sym_package`のシンボルセルを直接書き換える形で）別パッケージへ切り替えてから`common-lisp-user`へ戻し、同名関数`m81-redef-fn`を再定義しても`lisp_intern`が常に同一シンボルオブジェクト（`eq`）を返し、`global_env`解決も再定義後の新しい関数本体に正しく追従すること（`defun`自体の評価は特殊形式可視性制約に触れないよう常に`*package*`が`common-lisp-user`の状態で行い、パッケージ切替はC内部でのポインタ代入に限定した）、(c) `*package*`が非既定のパッケージオブジェクトを指している最中に`lisp_gc()`を実行しても、`*package*`自身の値、およびその最中に`lisp_intern_in_package`でinternしたシンボル（`m81-probe`）のいずれも回収されないこと——これは`global_packages`が`lisp_gc_mark_roots`のルートであり、`*package*`の現在値に関わらず**全**パッケージ・全所属シンボルが常にmilestone72の設計で保護される、という既存の保証を明示的に固定するテストである。`make build`および`make test`（17ファイル全PASS、新規自己テストの`PASS`ログを含む）で確認した。 |
| 82 | パッケージシステム統合テスト・既存回帰の総点検 | 完了 | `test/lisp/test-package.lisp`に2件のテストを追加した。`run-test-package-qualifier-reader`は、`main.c`の自己テスト`lisp_reader_package_qualifier_selftest`（milestone74）が起動時に既に作成済みの`selftest-pkg74`（`exported-sym`をexport・`internal-sym`は非export）を利用し、`selftest-pkg74:exported-sym`（単一コロン）・`selftest-pkg74::internal-sym`（二重コロン）を読んだ結果が、`intern`関数で直接同じ名前をinternした結果と`eq`であることを確認する——このパッケージ・exportの用意自体を本ファイル内で行うと`load`のファイル全体読み切り実装（milestone72/76と同根の制約）に抵触するが、`main.c`の自己テストは本ファイルの`load`より前に実行済みのため制約に触れずに検証できることを利用した。`run-test-package-in-package-roundtrip`は、`in-package`で`selftest-pkg74`へ切り替え→切替中に`intern`→`common-lisp-user`へ復帰、という手続きを1つのdefun本体として書き（本ファイル自身は`*package*`が`common-lisp-user`のまま読み切られるため、この記述自体はmilestone79/81で発見した特殊形式可視性制約に触れない）、切替中にinternしたシンボルが復帰後も同一オブジェクト(`eq`)で見つかること、および`*package*`が確実に`common-lisp-user`へ復帰していることを確認する。修飾子プリンタの統合テストは、印字結果を文字列として捕捉するLisp APIが存在しないため`test/lisp/`への追加はできず、milestone79で実施済みの個別対話REPL検証（`print-test-a:shared-a`等）を引き続き正式な検証手段とする（本マイルストーンでの追加作業なし）。`make build`・`make test`（全19ファイル、新規2テストの`t`を含む`run-test-package`の`and`チェーン経由でのPASSと、他18ファイルの既存自己テスト群のPASSログ）で回帰が無いことを確認した。 |

### フェーズF: 非tail位置のlet系スタックリークの根本修正（milestone78で発見）

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 83 | ローカル変数領域とオペランドスタックの分離による非tail位置スタックリークの根本修正 | 完了 | milestone78の`progn`修正（`OP_POP`導入）では解決できなかった同根の問題——`OP_MAKE_LOCAL`がボックス化した値をスタック上に永続的に残し、対応する解放命令が無いこと——により、`let`/`let*`/`or`/`cond`のtest-onlyクローズ自体も非tail位置で2個以上の兄弟式として使われると異常終了することを確認済み（例: `(cons 1 (let ((y 2)) y))`）。当初検討した「ボックスから値だけ取り出してボックス自体をスタックから除去する」方式ではなく、ユーザー提案の設計——関数呼び出し時にそのフレームが使う全ローカル変数スロット（仮引数+`let`で束縛される全ローカル）分の固定領域を`vm_stack`上に一括で確保し、その上に一時値用のオペランドスタックを積む——を採用した。具体的には: (1) コンパイラの`scope-next-slot-box`（単調増加、再利用しない）の最終値を`max-locals`としてクロージャに記録する新規フィールドを追加し、`closure-template`のリスト構造・`vm-make-closure`ビルトインの引数を`(nargs max-locals ...)`へ拡張した。(2) 呼び出し元が`lisp_vm_run`を呼ぶ前に`vm_stack[fp..fp+max_locals)`をまるごと`LISP_NIL`で確保する新規ヘルパー`lisp_vm_reserve_frame`を追加し、`OP_CALL`・`OP_BLOCK`・`lisp_vm_exec`（トップレベル）・`lisp_apply`のコンパイル済みクロージャ呼び出しブリッジという4箇所すべての呼び出し経路に組み込んだ。(3) `OP_MAKE_LOCAL`をFP相対のスロットindexを2byteオペランドとして取る命令へ変更し、スタック最上位の初期化値を1つpopしてその場で`cons(値, nil)`にボックス化し、確保済みの固定スロット`vm_stack[fp+index]`へ直接書き込む（pushし直さない）ようにした。この結果、ローカル変数用の領域は呼び出し時に一括確保された不変の領域となり、兄弟式の一時値が残っていても`let`のボックス化がスタックの深さに影響を与えなくなるため、根本的にリークが発生しなくなった。 |
| 84 | milestone83修正の回帰検証と既存コードベースの棚卸し | 完了 | `src/lisp.c`/`src/lisp.h`/`src/main.c`（VM自己テスト5件のハンドコンパイル済みバイトコード配列・`lisp_make_compiled`呼び出し）・`lisp/compiler.lisp`（`compile-let-push-and-box`/`compile-lambda`/`vm-materialize-template`/`compile-and-run`）・`test/lisp/test-compile-expr.lisp`（`OP_MAKE_LOCAL`のオペランド幅変化・`closure-template`構造変化に伴うハードコード済み期待値を全面的に再計算）・`test/lisp/test-compile-and-run.lisp`（`vm-make-closure`呼び出し3箇所）を`max-locals`対応に合わせて修正した。`make build`・`make test`（新規`test-locals-region.lisp`を含む全20ファイル）で回帰が無いことを確認した。加えて、`make test`単体では検出できない異常終了系シナリオを個別に確認する既定の方針に従い、本マイルストーン用に新設した`make test-locals-region`（`(cons 1 (let ((y 2)) y))`という非tail位置の兄弟式としての`let`、3つ以上の兄弟位置での複数`let`、外側の`let`本体内での非tail位置`let`の3パターン）を個別にQEMUで実行し、PASSすることを確認した。ループ本体で繰り返し評価される`let`がスタックへボックスを積み増さないことの確認（milestone87で発見したネストしたwhile内での`let`繰り返しによるスタック蓄積の再現テスト）は、`do`/`while`が導入されるmilestone87側の`test-while.lisp`で扱うため本マイルストーンのスコープには含めない。`compiler.lisp`/`stdlib.lisp`自身のコード中に同種のパターン（`let`/`let*`/`or`/`cond`を非tail位置の兄弟引数として使う箇所）が無いかも確認し、既存コードが暗黙にこの制約に依存している箇所は見つからなかった。 |

### フェーズG: 特殊形式トークンの可視性修正（milestone81で発見）

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 85 | 特殊形式・ビルトイントークンの`common-lisp-user`からのexport対応 | 未着手 | milestone81の着手時に発見した制約——`*package*`を`common-lisp-user`以外へ切り替えると、`:use "common-lisp-user"`していても`defun`/`if`/`let`/`quote`/`lambda`/`defmacro`/`progn`/`setq`/`cond`/`and`/`or`/`when`/`unless`/`block`/`return-from`等の特殊形式トークン、および`car`/`cons`/`+`等のビルトインまで無修飾では解決できなくなる（`unbound variable`でpanicする、または特殊形式として認識されず関数呼び出しとして評価されて失敗する）——を解消する。`lisp_symbols_init`でintern済みの特殊形式シンボル群と`lisp_builtins_init`で`global_env`へ束縛する全ビルトインシンボルを、`common-lisp-user`の`pkg_exports`へ登録する（初期化時に`lisp_builtin_export`相当の処理をC内部から直接呼ぶ、または`pkg_exports`へ直接cons追加する）。既存の`:use "common-lisp-user"`をしているパッケージの動作は、追加のexportにより無修飾解決できる範囲が広がるだけで後方互換になることを確認する。`in-package`自身が無修飾で呼べないため二重コロン修飾（`common-lisp-user::in-package`）でしか復帰できない、というmilestone79で発見した制約もこれで解消され、通常の`in-package`呼び出しで復帰できるようになることを確認する。 |
| 86 | milestone85修正の回帰検証 | 未着手 | milestone85の修正後、`make test`全件（特に修飾子リーダー・修飾子プリンタ・`use-package`関連の既存自己テスト）に回帰が無いことを確認する。加えて、`*package*`を新規パッケージ（`:use "common-lisp-user"`のみ）へ切り替えた対話REPLセッションで、`defun`によるユーザー定義関数の作成、`in-package`による無修飾での`common-lisp-user`への復帰、`(quote 症状のあったビルトイン呼び出し)`が全て無修飾で成功することを目視確認する。milestone81の自己テストがC内部操作（`lisp_sym_package`の直接書き換え）に限定していた制約が本マイルストーンの完了により解消されるため、必要であれば同等のシナリオをLisp評価のみで再テストするかどうかを検討する。 |

### フェーズH: コンパイラ自己ブートストラップ関数のCスタック深度対策（フェーズA〜Gとは無関係、ユーザーの判断でmilestone番号を継続）

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 87 | `do`/`while`の導入とコンパイラ自己ブートストラップ関数のCスタック深度対策 | 完了 | `test/lisp/test-compile-expr.lisp`（28節の`and`を持つマクロ展開網羅テスト）を`make test`で実行するとCスタックオーバーフローでクラッシュする問題を修正した。原因は`macroexpand-all-forms`/`append`/`mapcar`が節数に比例した深さの非末尾再帰をCコールスタック上に積むこと——ユーザーコードの再帰ではなく、コンパイラ自身の自己ブートストラップ処理（milestone65でmacroexpand-all/compile-exprの前提として`lisp/compiler.lisp`へ移設した6関数のうちの3つ）が原因である点が特徴。まず`lisp_eval`（`src/lisp.c`/`src/lisp.h`）へ`do`特殊形式（並行step-form・省略可能なresult-forms、`while(1)`ループでCスタックを消費しない設計）を追加し、`macroexpand-all-special-form-p`へ特殊形式として登録、`lisp/compiler.lisp`の`compile-expr`に`do`のコンパイルを実装した。次に`lisp/stdlib.lisp`へ`(while test . body)`を`(do nil ((not test)) . body)`へ展開する`while`マクロを追加した。`lisp/compiler.lisp`の`append`/`mapcar`を`do`化して非末尾再帰を除去したが、初回実装（アキュムレータへ逆順に積んでから`reverse`を再度通す方式）はconsセル生成数を元の再帰実装の2倍（`|a|×2`個）にしてしまい、`test-compile-expr.lisp`ロード中にヒープを圧迫し「`Lisp panic: expected a cons cell but got something else`」という別のクラッシュ（スタックではなくヒープ枯渇由来）を新たに引き起こした。`rplacd`による head/tail連結方式（生成するconsセル数を元の再帰実装と同じ`|a|`個に戻す）へ書き直して解決した。この調査の過程で、ヒープ枯渇のもう一つの独立した原因を`lisp_load_eval_buffer`（`src/lisp.c`）に発見した——`load`はファイル中の全トップレベルフォームを1回のバッファ評価内で連続して評価するため、`main.c`のREPLループが入力の合間に得ているGC起動の機会を一度も得られず、個々には正当なゴミであっても`test-compile-expr.lisp`の28関数連続ロード中に回収されずヒープを枯渇させていた。評価待ちの残りフォームリストを保持する新規のGCルート変数`lisp_gc_extra_root`（`src/lisp.c`/`src/lisp.h`）を追加し、`lisp_load_eval_buffer`が1フォーム評価するたびに`lisp_heap_low()`を確認して必要なら`lisp_gc()`を起動するよう修正した（評価待ちフォームはCローカル変数にしか存在しないため、その間だけ`lisp_gc_extra_root`へ退避してGCから保護する）。この過程で、`lisp/compiler.lisp`の`compile-let`が`(car (cdr (cdr form)))`で`let`の先頭のbody-formしか取らず、2個目以降のbody-formを一切コンパイルしない（値が捨てられるのではなくバイトコードに存在しない）という既存の重大バグを発見した——`(let (...) (while ...) result)`のような、ループ本体を`let`で包み最後に結果を返すという典型的な命令型パターンが軒並み壊れていた。`compile-let-star`の既存の正しい脱糖方式（bodyを`(cons 'progn body)`で包み`compile-progn-body`へ委譲）へ揃えて修正した。この修正により、`test/lisp/test-macroexpand-1.lisp`の`run-test-macroexpand-1-hook-affects-eval`が真に失敗するようになった（従来は`compile-let`のバグが2個目のbody-form（本来の`eq`検査）を握り消し、1個目の`setq`の戻り値というtruthyな値で常にvacuousにPASSしていたことが判明した）。原因はマクロ呼び出しを`defun`本体に直接書くと、その`defun`自身が`load`時にコンパイルされる瞬間（＝`*macroexpand-hook*`の差し替えより前）に`macroexpand-all`で展開済みになってしまい、実行時のフック差し替えが手遅れになるという、事前展開コンパイル方式とテストの意図との不整合であり、`compile-and-run`（引数のS式を呼び出し時に`macroexpand-all`→compile→実行する）へマクロ呼び出しを`quote`したS式として渡す形へ書き直し、フック差し替え後の展開を真に検証するテストへ修正した。本マイルストーンは、`lisp_vm.md`/`lisp_vm_integration.md`が明記していた非ゴール「末尾呼び出し最適化・スタック深度対策」を、**コンパイラ自身の自己ブートストラップ関数**（`append`/`mapcar`/`macroexpand-all-forms`等、ユーザーコードではなくコンパイラ内部処理）に限定する形で部分的に覆すものである（ユーザーコード一般のスタック深度対策は引き続き対象外のまま）。検証は新設`test/lisp/test-do.lisp`（10項目: 基本的な合計・result-formsの複数評価・result-forms省略時の`nil`・本体の副作用・step-form省略時の固定値・var単体省略記法・並行step（`let`と同じ意味であり`let*`相当の逐次評価に落ちていないことの検出）・動的変数の束縛と復元・`return-from`による`block`への直接脱出・50000回反復でもCスタックを消費しないこと）・新設`test/lisp/test-while.lisp`（6項目: 基本的な合計・戻り値が`nil`であること・0回反復・`return-from`での脱出・ネスト・50000回反復でもCスタックを消費しないこと）と、修正した`test/lisp/test-macroexpand-1.lisp`を`make test`で確認し、milestone83/84の`test-locals-region.lisp`を含む全22フィクスチャが回帰無くPASSすることを確認した。加えて、`make test`単体では検出できない異常終了系シナリオを個別に確認する既定の方針に従い、`make test-do`・`make test-while`を個別にQEMUで実行しPASSすることを確認し、本マイルストーンの直接の動機であった`test-compile-expr.lisp`自体も個別に`make test-compile-expr`で実行し、Cスタックオーバーフローが再発しないことをQEMU実行で確認した。 |

### 既知のリスク: `LISP_LOAD_BUFFER_MAX`の再発パターン

`lisp/stdlib.lisp`のサイズが`LISP_LOAD_BUFFER_MAX`（現在131072byte）を静かに超えて末尾が読み捨てられる
事故が、マイルストーン41（8192→32768）・46（32768→65536）・61（65536→131072）と**3回連続で再発**
している（`lisp_load_eval_buffer`がEOF耐性設計のため症状が「unbound variable」等の形で遅れて現れる）。
`defpackage`マクロ（#78）や新規ビルトインの追加で`stdlib.lisp`がさらに増えるため、各フェーズの
`make test`実行時にこの既知のリスクとして意識し、必要なら`LISP_LOAD_BUFFER_MAX`を再拡張する。

## スコープ外として明記する項目

以下は本ロードマップの範囲を超えるため対象外とする:

- シャドーイング機構（`shadow`/`shadowing-import`、パッケージロック）。`use-package`時の名前衝突は
  最小限のエラー化のみで扱う。
- `:nicknames`（パッケージ別名）、`rename-package`、`delete-package`、`unintern`。
- `compiler.lisp`/`stdlib.lisp`を専用パッケージ（`common-lisp`/`system`等）へ分離するリネーム——
  将来の別マイルストーンに委ねる。
- シンボル探索のハッシュテーブル化等のパフォーマンス最適化——既存実装も元々線形探索であり、
  教育・組み込み用途の規模では不要と判断。consリスト化（#71）後も線形探索のままでよい。
- `*print-case*`等、印字方式の一般化・CLの完全な`*package*`/`*print-package*`印字仕様。
- マルチプロセス機能自体の実装——本ロードマップはその土台のみを提供する。

## 検証方針

各マイルストーン完了時に、`make build`でクロスコンパイルが通ることと、`make test`（`test/lisp/`配下の
全フィクスチャをQEMU/OVMFヘッドレスで実行するハーネス）および実際のQEMU/OVMF起動でのREPL基本動作の
両方に回帰が無いことを確認してから次のマイルストーンに進む。フェーズA（68〜72）は挙動不変な内部
置換のみのため、特に`test/lisp/test-package.lisp`が無修正のまま全項目パスし続けることを各マイル
ストーンの合格基準とする。フェーズの区切りごとに、それまでの全マイルストーンの回帰確認をまとめて
行う。マイルストーン82（フェーズA〜Eの完了条件）では、`test/lisp/test-package.lisp`を
`make-package`/`export`/`use-package`/`defpackage`/`in-package`/修飾子リーダー/修飾子プリンタを
一通り経由する統合シナリオへ拡張した上で、既存自己テスト群全件の回帰確認を行う。フェーズF（83〜84）
・フェーズG（85〜86）は、フェーズA〜Eの実装中に発見した既存バグの根本修正として独立に完了させる
ため、これらの完了までを本ロードマップ全体の完了条件とする。フェーズH（87）はフェーズA〜Gと無関係な
内容（コンパイラ自身の自己ブートストラップ関数のCスタック深度対策）だが、ユーザーの判断により本
ロードマップの完了条件へ同様に含める。
