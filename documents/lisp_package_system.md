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
確認済み）。

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
| 72 | GCルートスキャンの`global_packages`一本化 | 未着手 | `lisp_gc_mark_roots`の「配列を2重ループでシンボルだけ直接mark」を`lisp_gc_mark(global_packages)`1行へ置き換える。`lisp_gc_mark`のclosure分岐に、対象がパッケージ判別済みの場合はsymbols/exports/usesの各consリストも辿る処理を追加する（既存の`vec_data`/`constants`ループと対称）。パッケージ自体・全所属シンボルがGCで回収されないことを保証する新規テスト（多数のシンボルをinternした後`(gc)`を明示実行し`eq`と検索結果が保たれることを確認）を追加する——旧設計にはこの懸念自体が存在しなかったため新規カバレッジ。マイルストーン68の時点で`lisp_gc_mark_roots`はパッケージ本体とpkg_symbols/pkg_exports/pkg_usesを個別にmarkする暫定実装へ既に拡張済みであり、本マイルストーンはそれを`lisp_gc_mark`のclosure汎用分岐に一般化する仕上げ作業として残っている。 |

### フェーズB: `*package*`とリーダー拡張

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 73 | `*package*`動的変数の導入と`lisp_intern`の切替 | 未着手 | `*package*`を通常の`defvar`と同じ`is_special`/`value`機構で用意し、`lisp_packages_init`内で初期化して既定値を`common-lisp-user`パッケージオブジェクトにする。`lisp_intern(name)`のハードコード先を「`*package*`の現在値へintern」に変更する（`lisp_intern_keyword`は変更しない）。既定値が旧ハードコード先と同一のため、`in-package`を1度も呼ばなければ全既存挙動が完全に不変であることを回帰テストの合格基準にする。 |
| 74 | リーダーの`pkg:sym`/`pkg::sym`修飾子対応 | 未着手 | `lisp_read`の既存トークン分類`if`チェイン（文字スキャンループ自体は無変更）に、捕捉済みトークン内の`:`/`::`を検出して(パッケージ名, シンボル名, internal可否)へ分割する処理を追加する。マイルストーン18と同じ「1回だけ切り詰めた同一バッファをcompare/storeの双方に使う」規律を踏襲する。パッケージ未発見、または`pkg:sym`（単一コロン）でシンボルが対象パッケージのexportリストに無い場合はreaderエラーとする。先頭`:`（既存keyword構文）の判定はこれより先に行い無変更のままとする。この時点では`export`/`use-package`のLisp APIが未実装なので、テストはC内部APIで直接パッケージ・exportリストを組み立てて検証する。 |

### フェーズC: パッケージ操作関数と`defpackage`/`in-package`

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 75 | `make-package`/`find-package`ビルトイン | 未着手 | #69のC内部APIをラップしたLisp呼び出し可能な組み込み関数を追加する。パッケージオブジェクト自身（不透明ポインタではなく本物の第一級`LispObject`）を返す。`find-package`は未存在なら`nil`を返す。 |
| 76 | `export`ビルトインとexportリスト | 未着手 | パッケージ＋シンボル（複数可）を受け取り、対象パッケージのexport consリストへ`eq`基準で重複なく追加する組み込み関数を追加する。#74のリーダーが参照する「exportされているか」判定はこのリストを直接見る。 |
| 77 | `use-package`ビルトインとuse-list継承を反映したintern解決 | 未着手 | パッケージ間の`use-package`関係をuseリストへ追加する組み込み関数を追加し、`lisp_intern_in_package`の探索順序を「自パッケージのローカルシンボル→useしている各パッケージのexportシンボル→見つからなければ新規作成（自パッケージへ）」に拡張する。`(in-package :foo)`後に無修飾で書いた名前が、`foo`が`use-package`している別パッケージのexportシンボルと同一オブジェクト（`eq`）に解決される——本設計の核心部分。名前衝突（複数のuse対象が同名exportを持つ場合）は最小限のガードとしてエラーにする。`use-package`を呼ばない限り探索順序に追加ステップが挟まるだけで結果は不変であることを確認する。 |
| 78 | `defpackage`マクロと`in-package`関数 | 未着手 | `in-package`はパッケージ名を受け取り`(find-package name)`が見つからなければエラー、見つかれば`*package*`のシンボルセルの`value`を直接書き換える組み込み関数として実装する。`defpackage`は`lisp/stdlib.lisp`にLispマクロとして実装し、`:export`/`:use`句を`make-package`+`export`+`use-package`呼び出し列へ展開する（既存の`cond`等と同じ「マクロで既存プリミティブへ脱糖する」パターン。`:nicknames`等の他句は対象外）。同名`defpackage`の再実行は#69の重複名冪等性によりべき等になる。 |

### フェーズD: プリンタ対応とブートストラップ整理

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 79 | プリンタの修飾シンボル印字対応 | 未着手 | `lisp_print`のシンボル分岐（既存の`package != 0 && package->is_keyword_package`ガードの拡張ポイント）に、対象シンボルのホームパッケージが現在の`*package*`から見て可視（`*package*`自身のシンボルである、または`*package*`がuseしているパッケージからexportされている）かどうかの判定を追加する。可視なら無修飾のまま印字、非可視ならexportされていれば`pkgname:symbol`、されていなければ`pkgname::symbol`で印字する。gensymによるuninterned symbolは既存どおり無修飾で印字する（変更なし）。`*package*`が`common-lisp-user`のままであれば印字結果は完全に不変であることを確認する。 |
| 80 | ブートストラップのパッケージ文脈確定 | 未着手 | `src/main.c`の`EfiMain`順序（`lisp_heap_init`→`lisp_packages_init`→`lisp_symbols_init`→…）を見直し、`lisp_packages_init`内で`global_packages`初期化・`*package*`のseedまで完結させてから`lisp_symbols_init`（`*package*`依存の`lisp_intern`を呼ぶ特殊形式シンボル群のintern）に進むことを確定・コード化する。`compiler.lisp`/`stdlib.lisp`のシンボルが引き続き`common-lisp-user`へinternされ、REPLで打った同名シンボルと`eq`であることを確認する。 |

### フェーズE: 正しさ検証

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 81 | VM/コンパイラのグローバル参照とシンボル同一性の回帰検証 | 未着手 | `OP_GLOBAL_REF`/`OP_GLOBAL_SET`（`lisp_vm_integration.md`マイルストーン51）が`global_env`をシンボルの`eq`同一性で解決する前提が`*package*`導入後も壊れていないことを専用テストで確認する: 同一パッケージ内での`defun`前方参照・相互再帰（既存カバレッジの再確認）、`(in-package)`を挟んでも同一パッケージ・同一名の再読込みが常に同一シンボルオブジェクトに再解決されること、`*package*`が非既定値を指している最中に`(gc)`を実行しても`*package*`自身の値と束縛中のシンボル群が回収されないこと。 |
| 82 | パッケージシステム統合テスト・既存回帰の総点検 | 未着手 | `test/lisp/test-package.lisp`を`make-package`/`export`/`use-package`/`defpackage`/`in-package`/修飾子リーダー/修飾子プリンタを一通り経由する統合シナリオへ拡張し、既存自己テスト群（`run-test-*`）全件・QEMUシリアルログでのPASS/FAIL目視確認を再実施する。ロードマップ全体の完了条件。 |

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
行う。マイルストーン82（本ロードマップ全体の完了条件）では、`test/lisp/test-package.lisp`を
`make-package`/`export`/`use-package`/`defpackage`/`in-package`/修飾子リーダー/修飾子プリンタを
一通り経由する統合シナリオへ拡張した上で、既存自己テスト群全件の回帰確認を行う。
