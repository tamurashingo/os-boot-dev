# パッケージ操作関数群の拡充マイルストーン

## 目的

`documents/lisp_package_system.md`（68〜87完了）でパッケージを第一級のGC管理オブジェクト化し、
`*package*`・リーダー修飾子・`export`/`use-package`/`defpackage`/`in-package`までを実装した。
しかしパッケージ指定は文字列限定のままで、CommonLispの標準的なパッケージ操作関数（シンボル一覧の
走査・shadowing・削除・改名など）は一切無い状態だった。

ユーザーから次の拡充要望があり、本ドキュメントはこれをマイルストーン**91（designator拡張・
読み取り系）・92（破壊的操作系・shadowing）**の2つに分けて記録する
（`lisp_package_system.md`に68〜87がまとまっているのと同じ構成）。

1. パッケージ指定子をkeyword（および一般にsymbol）でも受け付けるようにする。
2. 読み取り系: `do-all-symbols`/`list-all-packages`/`do-external-symbols`/`do-symbols`/
   `package-name`/`package-nicknames`/`find-all-symbols`/`find-package`（拡張）/
   `package-use-list`/`use-package`（拡張）/`find-symbol`。
3. 破壊的操作系: `shadow`/`unexport`/`unuse-package`/`delete-package`/`rename-package`、
   および`defpackage`の`:shadow`/`:shadowing-import-from`/`:import-from`句
   （ユーザーからの追加要望によりスコープ拡張）。

マイルストーン番号は既存の1〜90に続く**91から**開始する。

## ファイル構成

新規ソースファイルは追加しない。既存の`src/lisp.c`/`src/lisp.h`・`lisp/stdlib.lisp`・
`test/lisp/test-package.lisp`を拡張する。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 91 | designator拡張・読み取り系パッケージ操作関数 | 完了 | `lisp_string_designator_name`（string→`str_data`、symbol/keyword→`sym->name`）と`lisp_package_designator_name`（package→`pkg_name`、他は`lisp_string_designator_name`へ委譲）を新設し、`find-package`/`make-package`/`in-package`/`export`/`use-package`をこれ経由に拡張してsymbol・keyword指定を受け付けるようにした（`export`の`package`引数がdesignator解決を一切通していなかった既存の抜けも同時に修正）。`LispClosure`へ`pkg_nicknames`（文字列のconsリスト）フィールドを追加し、全allocate箇所（closure/vector/string/bignum/float/コンパイル済み関数等のescape hatchを共有する約10箇所）へ`LISP_NIL`初期化を追記、GCマーク箇所にも追記した。`make-package`に第2引数`nicknames`（designatorのリスト、各要素を`lisp_string_designator_name`で文字列化）を追加し、`lisp_find_package`は`pkg_name`一致に加え`pkg_nicknames`も照合するようにした。新規Cビルトインとして`package-name`/`package-nicknames`/`package-use-list`/`list-all-packages`（対応フィールドをそのまま返す薄いラッパー）、内部用`%package-symbols`/`%package-exported-symbols`（`pkg_symbols`/`pkg_exports`をそのまま返す）、`find-symbol`（`name`文字列限定、ローカル→use先exportの2段階探索で新規作成はしない、見つかれば`(cons symbol status)`(`status`は`:internal`/`:external`/`:inherited`)・見つからなければ`nil`）、`find-all-symbols`（`name`文字列限定、全パッケージの`pkg_symbols`を走査し名前一致するシンボルをリストで返す）を追加した。`lisp/stdlib.lisp`には`do`を土台にした一般`dolist`マクロ（`while`と同じ明示的`cons`/`list`構築）を新設し、これを経由して`package-accessible-symbols-list`/`package-external-symbols-list`/`package-all-symbols-list`ヘルパー（`append`＋上記Cビルトインで実装）から`do-symbols`/`do-external-symbols`/`do-all-symbols`マクロを導出した（`(var [package [result]])`は既存の`defpackage`等と同じ手動destructuring、`do-all-symbols`はCL同様packageを取らない）。`defpackage`に`:nicknames`句を追加した（値は文字列のリストなので`(list 'quote nicknames)`でquoteしてmake-packageへ渡す）。 |
| 92 | 破壊的操作系パッケージ関数とshadowing | 完了 | `LispClosure`へ`pkg_shadowing_symbols`（`shadow`/`shadowing-import`で登録されたローカルシンボルのconsリスト、eq基準）フィールドを91と同様に全allocate箇所・GCマーク箇所へ追加した。`lisp_intern_in_package`のphase a（自パッケージのローカル探索）を`lisp_find_local_symbol(pkg_cell, name)`、phase c（新規作成）を`lisp_create_local_symbol(pkg, name)`として抽出するリファクタを行った（既存の`lisp_intern_in_package`自体の振る舞い＝a→use先export探索→cの順は不変）。`use-package`の2つの名前衝突panic分岐（既存ローカルシンボルとの衝突／use済み別パッケージ間の衝突）それぞれに、対象パッケージの既存ローカルシンボルが`pkg_shadowing_symbols`にeqで含まれる場合はpanicしない、という例外を新設ヘルパー`lisp_symbol_is_shadowing`で追加した。新規Cビルトインとして`shadow`（`names`は文字列designatorまたはリスト、`lisp_find_local_symbol`→無ければ`lisp_create_local_symbol`で解決し`pkg_shadowing_symbols`へeq重複なく追加。use先のexport探索は経由しない点が`lisp_intern_in_package`と異なる）、`unexport`（`pkg_exports`からeqで除去）、`unuse-package`（`pkg_uses`からeqで除去）、`import`（対象の`pkg_symbols`に同名だがeqでないシンボルが既にあればpanic、同名同一なら無処理、無ければそのまま追加。home packageは変更しない）、`shadowing-import`（対象の`pkg_symbols`に同名の既存シンボルがあれば（eq問わず）除去してから追加し、`pkg_shadowing_symbols`にも登録）、`delete-package`（`global_packages`から除去し他の全パッケージの`pkg_uses`からも除去。対象が現在の`*package*`ならpanic）、`rename-package`（`lisp_find_package(new-name)`が既存の別パッケージを指す場合はpanic。`pkg_name`を`lisp_alloc`で確保した新バッファに置き換え、既存バッファの明示的freeは無し。`new-nicknames`が渡された場合のみ`pkg_nicknames`を置き換え、省略時は既存のまま維持）を追加した。`import`されたシンボルは`sym->package`（home package）が変わらないため、既存の`lisp_symbol_visible_in_current_package`（milestone79）の同一性判定だけでは無修飾で印字されない不整合が生じていたが、フォールバックとして「現在の`*package*`の`pkg_symbols`にeqで含まれるか」を追加して解決した（既存のhome-package一致高速パスは変更していないため、import未使用の既存動作に回帰は無い）。`lisp/stdlib.lisp`の`defpackage`に`:shadow`（既存の`defpackage-clause-names`で名前抽出し`export`と同じ「1名前=1呼び出し」パターンで`shadow`呼び出し列を生成）と`:import-from`/`:shadowing-import-from`（新規ヘルパー`defpackage-clause-package-symbol-pairs`/`defpackage-pairs-for-clause`で句ごとに`(source-pkg . sym-name)`ペアを列挙してから`import`/`shadowing-import`呼び出し列を生成）を追加した。生成順序を`make-package`(nicknames込み)→`:shadow`→`:shadowing-import-from`→`:use`→`:import-from`→`:export`→`(find-package name)`に変更し、`:shadow`を`:use`より先に処理することで`use-package`実行時点でshadowingが既に有効になっている（`use-package`の名前衝突チェックがshadowingを見て例外扱いできる）ようにした。 |

## スコープ外として明記する項目

- `with-package-iterator`。
- `find-symbol`/`find-all-symbols`の`name`引数のsymbol designator対応（文字列限定のまま）。
- milestone85〜86（`*package*`切替後の特殊形式/ビルトイン可視性の解消、`lisp_package_system.md`で
  既に切り出し済みの別マイルストーン、未着手のまま）。
- パッケージロック（`lisp_package_system.md`のスコープ外指定を継続）。

## 検証方針

`make build`でクロスコンパイルが通ることと、`make test`（`test/lisp/test-package.lisp`拡張分を
含む全フィクスチャ）に回帰が無いことを確認した上で、`make test`だけでは検証できないpanicシナリオ
（milestone78/81等と同様の方針）を個別のQEMU対話で確認した:

- shadow未設定での通常の名前衝突が引き続きpanicすること（`use-package`の既存回帰確認）。
- shadow設定後は同名の別exportを`use-package`してもpanicしないこと。
- `delete-package`で`*package*`自身の削除がpanicすること（`in-package`経由では
  milestone79/80で確認済みのブートストラップ制約により`*package*`切替後に無修飾の
  ビルトイン自体が解決できなくなるため、`(let ((*package* ...)) (delete-package *package*))`
  という、対象シンボル自体は`*package*`切替前に読まれる形で確認した）。
- `rename-package`で既存の別名への改名がpanicすること。
- importしたシンボルが無修飾で印字されること（printerフォールバックの確認）。
