# Lispをスタックマシン型VMで動くコンパイラとして再定義するマイルストーン

## 目的

このドキュメントは、`boot.md`（マイルストーン1〜11）・`init_lisp.md`（12〜16）・
`bare_metal_lisp.md`（17〜29）・`lisp_robustness.md`（30〜33）で完成した現在のツリーウォーク型
Lispインタプリタ（`lisp_eval`/`lisp_apply`、`src/lisp.c`）を土台に、S式を1バイト命令のバイトコード
へコンパイルし、スタックマシン型VM（`lisp_vm_exec`）上で実行する方式へ拡張するためのマイルストーン群
である。既存のツリーウォーク・インタプリタは置き換えず併存させる。本ロードマップの範囲は「C側の
VM実行エンジンを実装し、手動構築したバイトコードで動作検証する」（目標1）と「Lisp側に`compile-expr`
コンパイラを実装し、同じ検証ケースをS式から生成したバイトコードで再現する」（目標2）の2つの大きな
ゴールに限定し、既存の`defun`/`eval`経路とコンパイル済み関数の完全な統合（自動コンパイル等）は
対象としない（詳細は「スコープ外として明記する項目」参照）。他の4文書と同様、実装の詳細設計は
各マイルストーンに着手する際に別途行い、本ドキュメントは全体の見取り図として保守する。

前提となる制約は他の4文書と同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証はQEMU/OVMF上で実際に起動し、コンソール
  出力（目標1）または`test/lisp/`フィクスチャの`load`結果（目標2）で確認する。

設計にあたり、以下3点をユーザーとの事前検討で確定している:

- **VMデータスタックとGCの統合:** `LispObject vm_stack[VM_STACK_SIZE]`をグローバルに固定長確保し、
  `lisp_gc_mark_roots`（milestone33）のルート集合に「スタック底から現在のSPまでの全要素」を追加する。
  これを忘れるとVMが計算途中で保持している中間オブジェクトがゴミ判定されて回収されてしまう。
- **レキシカル変数の高速化:** `lisp_env_lookup`のシンボル名探索を排除し、コンパイル時に決まる
  フレーム先頭からのオフセットで`OP_LOAD_LOCAL <offset>`/`OP_STORE_LOCAL <offset>`に静的解決する。
  ただし本ロードマップの`compile-expr`は**外側スコープの変数も捕捉できる完全なクロージャ**に対応する
  方針であり、そのため単純なFP相対オフセットだけでは済まず、ローカル変数は「ミュータブルなボックス
  （既存の`cons`を再利用、`car`=値）」を介した間接参照にする（詳細は各マイルストーンの概要を参照）。
  `setq`による書き換えがクロージャ間で正しく共有されるために必要な設計。
- **マクロ展開のタイミング:** `compile-expr`はLisp側に書き、その冒頭で既存の`lisp_macroexpand_1`/
  `*macroexpand-hook*`（milestone21）を再帰的に適用してマクロを全て展開してから、特殊形式と
  プリミティブ呼び出しだけになったS式をバイトコード生成に渡す。

コンパイル結果を保持する新しい値の表現は、新しい2bitタグを追加せず、milestone15（文字列）・
milestone22（bignum/float）・milestone26（ベクタ）と同じ「既存の`LispClosure`へフィールドを追加する
escape hatch」方式を踏襲する。

## ファイル構成

新規ソースファイルは追加しない。既存の3ファイル構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／
`src/main.c`）に加え、`compile-expr`本体はmilestone29の自己ホスティング先例に従い`lisp/stdlib.lisp`
にLisp自身で追加する。目標2の検証用フィクスチャは既存の`test/lisp/`配下に追加する。

## マイルストーン一覧

`lisp_robustness.md`のマイルストーン30〜33に続く番号で管理する。

### 目標1: C言語側のみでスタックマシンVMを実装する

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 34 | VMデータスタックの確保とGCルート統合 | ✅完了 | `src/lisp.c`に`LispObject vm_stack[VM_STACK_SIZE]`（`VM_STACK_SIZE=1024`）と`vm_sp`をstaticなグローバルとして確保。`lisp_gc_mark_roots`に`vm_stack[0..vm_sp)`を走査して各要素を`lisp_gc_mark`する処理を追加した。検証用に`lisp_vm_gc_root_selftest`（vm_stackのみから参照されるconsを積んでGCを実行し、その後大量にconsを確保してフリーリスト再利用を強制、元のconsが上書きされていないことを確認する）を追加し、`src/main.c`の起動時自己テスト群（milestone30のsetjmp自己テストと同様、毎回起動時に実行）に組み込んだ。QEMU/OVMF上で`VM stack GC root self-test: PASS`を確認済み。 |
| 35 | オペコード定義とVM実行ループの最小実装 | ✅完了 | `OP_CONST`/`OP_ADD`/`OP_RETURN`（`src/lisp.h`にマクロ定義、各命令は1byte opcode+0/1byteオペランド）を定義し、`lisp_vm_exec`の`switch`ループを実装した。`LispClosure`へ`bytecode`/`bytecode_len`/`constants`（LispObjectのベクタ、vec_dataと同型）/`constants_len`/`nargs`を追加（escape hatch方式、7つの既存コンストラクタすべてにデフォルト初期化を追加し、新規`lisp_make_compiled`コンストラクタを実装）。`lisp_gc_mark`に`constants`配列走査を追加。VMスタックの押し下ろしは`lisp_vm_push`/`lisp_vm_pop`（オーバーフローは資源枯渇として`lisp_panic_fatal`、アンダーフローは`lisp_panic`）に切り出した。`src/main.c`に`lisp_vm_arith_selftest_run`を追加し、手動構築した`OP_CONST 1, OP_CONST 2, OP_ADD, OP_RETURN`相当のバイトコードを`lisp_vm_exec`に渡して`3`が返ることをQEMUで確認済み（`VM arithmetic self-test: PASS`）。 |
| 36 | 制御フロー命令とボックス化ローカル変数の基盤 | ✅完了 | `OP_JUMP <target>`/`OP_JUMP_IF_FALSE <target>`（`target`は関数の`bytecode`先頭からの絶対バイト位置、1byte）、`OP_LOAD_LOCAL <i>`/`OP_STORE_LOCAL <i>`/`OP_MAKE_LOCAL`（`src/lisp.h`にマクロ定義）を追加した。ローカル変数はフレームスロットに生の値を直接置かず、既存の`lisp_cons`をボックス（`car`=値、`cdr`はNIL固定）として再利用し1段の間接参照を挟む設計を導入した（milestone38のクロージャ捕捉で外側変数の共有ミューテーションを正しく扱うための前段）。`lisp_vm_exec`にローカル変数`fp`（現時点では`OP_CALL`が無いため実行開始時の`vm_sp`と同値。milestone37でスタックフレーム機構が加わる際の前段）を追加し、`OP_LOAD_LOCAL`/`OP_STORE_LOCAL`は`vm_stack[fp+i]`が指すボックスに対し`lisp_car`/`lisp_set_car`（既存の`lisp_assert_cons`によるボックス未生成の検出も兼ねる）で読み書きする。`OP_MAKE_LOCAL`はスタック最上位をpopし`lisp_cons(値, NIL)`としてその場でボックス化しpushし直す。`src/main.c`に`lisp_vm_control_flow_selftest_run`を追加し、`(let ((x 10)) (if <test> (setq x 20) (setq x 30)) x)`相当の手動バイトコードを`test`がnil/非nilの両方の定数配列で`lisp_vm_exec`に渡し、`OP_JUMP_IF_FALSE`によるelse分岐（結果30）と、分岐後`OP_JUMP`でelseを飛び越すthen分岐（結果20）の両方が正しく動作することをQEMUで確認済み（`VM control flow self-test: PASS`）。 |
| 37 | 関数呼び出し命令とスタックフレーム構築 | ✅完了 | `OP_CALL <nargs>`（`src/lisp.h`にマクロ定義）を追加した。呼び出し規約: 呼び出し元がargを順にpushし最後に呼び出す関数オブジェクトをpushしてから`OP_CALL <nargs>`を発行する。実装のため`lisp_vm_exec`本体を`static LispObject lisp_vm_run(LispClosure *cl, UINTN fp)`という共有の実行ループへ切り出し（`lisp_vm_exec`はこれを`fp=vm_sp`で呼ぶだけの薄いラッパーになった）、`OP_CALL`のcase内でpopした関数オブジェクトの下にあるnargs個のスタック位置をその場で`lisp_cons(値, NIL)`によりボックス化して新フレームとし、`lisp_vm_run`をC再帰で呼び出す（Cコールスタック自体がVMの呼び出しネストを表現するため、明示的な呼び出しスタックは追加していない）。呼び出し前後の`nargs`不一致・スタック不足は`lisp_panic`とした。手動バイトコードでの階乗計算はOP_SUB/OP_MULやOP_EQ相当がまだ無く（milestone39で追加予定）実現できないため、代わりに`f(self, n)`という自己適用（self-application）方式の関数を構築し検証した——`self`を第1引数として受け取り、再帰呼び出し時は同じ`self`を関数参照兼引数として渡すことで、`defun`等の名前束縛に頼らずに同一バイトコードが自分自身を呼び出す（`n`がnilなら基底部で0を返す）。`src/main.c`に`lisp_vm_call_selftest_run`を追加し、`f(f, 5)`をドライバ経由で`lisp_vm_exec`に渡して`5`（基底部の0 + 自分のn=5）が返ることをQEMUで確認済み（`VM function call self-test: PASS`）。 |
| 38 | クロージャ生成とupvalue命令 | ✅完了 | `OP_MAKE_CLOSURE <const_idx>`/`OP_LOAD_UPVALUE <idx>`/`OP_STORE_UPVALUE <idx>`（`src/lisp.h`にマクロ定義）を追加した。`LispClosure`に`upvalue_descs`（コンパイル時に確定する捕捉元記述子、テンプレート側）と`upvalues`（実行時生成、インスタンス側）を追加し、いずれも生配列ではなく既存の`lisp_make_vector`で作るLispObjectのベクタとして表現した（`lisp_gc_mark`への追加は`params`/`body`/`env`と同型の呼び出し2つで済んだ）。`upvalue_descs`の各要素は`(kind . index)`のcons（`kind=0`は「クロージャ生成元フレームの`FP+index`のボックスをそのまま捕捉」、`kind=1`は「クロージャ生成元closure自身の`upvalues[index]`をそのままコピー」で2階層以上の捕捉をフラット化）。`OP_MAKE_CLOSURE`はテンプレートの`upvalue_descs`を解決して新しい`upvalues`ベクタを構築し、`bytecode`/`constants`/`nargs`/`upvalue_descs`をテンプレートと共有した新規インスタンスをpushする。`OP_LOAD_UPVALUE`/`OP_STORE_UPVALUE`は常に実行中のクロージャ自身の`upvalues`ベクタだけを見て読み書きする。手動バイトコードから`upvalue_descs`ベクタや後付け設定を組み立てるため、新規公開API`lisp_make_upvalue_descs`（kind/index配列からベクタを構築）と`lisp_compiled_set_upvalue_descs`（`lisp_make_compiled`後にテンプレートへ設定）を`src/lisp.h`/`src/lisp.c`に追加した。`src/main.c`に`lisp_vm_closure_selftest_run`を追加し、`(defun make-counter (n) (lambda () (setq n (+ n 1)) n))`相当のカウンタ生成関数を手動バイトコードで構築、`make-counter`を2回呼んで独立したクロージャ`c1`/`c2`を作り、`c1`を2回・`c2`を1回呼んだ結果の合計（`11+12+101=124`）が正しく返ることで、複数回呼び出し間での捕捉ボックスの状態持続と、インスタンス間の捕捉独立性を同時に検証した。QEMU/OVMF上で`VM closure/upvalue self-test: PASS`を確認済み。 |
| 39 | プリミティブ最適化命令 | ✅完了 | `OP_CONS`/`OP_CAR`/`OP_CDR`/`OP_EQ`（`src/lisp.h`にマクロ定義）を追加した。いずれも既存の`lisp_cons`/`lisp_car`/`lisp_cdr`/ポインタ同値比較（`eq`組み込みと同じ`lisp_sym_t`/`NIL`の返し方）をそのまま呼ぶだけで、関数呼び出し（`OP_CALL`）を経由しない。目標1の総合検証として、`src/main.c`に`lisp_vm_integrated_selftest_run`を追加し、`f(self, n, counter)`（`n`がeq 0でなければ`counter()`呼び出し・upvalue読み書き・`cons(n, step)`のローカル変数へのボックス化・`car`/`cdr`での取り出し・`n+(-1)`による真の減算・自己適用方式での再帰呼び出しを行い`carval+cdrval+rec`を返す）を手動バイトコードで構築した。これはmilestone37で「`OP_SUB`/`OP_MUL`/`OP_EQ`が無く階乗は実現できない」と記録した制約を、新設の`OP_EQ`と負の定数による`OP_ADD`減算で解消し、初めて本物のfixnum比較に基づく再帰終了判定を実現したものでもある。`make-counter`（milestone38）で作ったクロージャを`f`の第3引数として渡し、再帰の各段で`counter()`を呼んで得た値（1,2,3）と`n`（3,2,1）を`cons`/`car`/`cdr`経由で組み合わせて合計する設計とし、`f(f, 3, counter)`が`12`を返すことを確認した。これにより`OP_CONST`/`OP_ADD`/`OP_JUMP_IF_FALSE`/`OP_LOAD_LOCAL`/`OP_MAKE_LOCAL`/`OP_CALL`/`OP_MAKE_CLOSURE`/`OP_LOAD_UPVALUE`/`OP_STORE_UPVALUE`/`OP_CONS`/`OP_CAR`/`OP_CDR`/`OP_EQ`/`OP_RETURN`という目標1で追加した命令の大半を単一の検証で組み合わせて動作確認した。QEMU/OVMF上で`VM integrated self-test: PASS`を確認済み。これをもって**目標1（C言語側スタックマシンVMの実装）を完了**とする。 |

### 目標2: Lisp側にコンパイラ（compile-expr）を実装する

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 40 | コンパイラフロントエンド: マクロ展開 | ✅完了 | `lisp/stdlib.lisp`に`macroexpand-all`（既存の`macroexpand-1`、milestone21を式全体へ再帰的に適用してマクロ呼び出しを完全に消し去る関数）と、それを最初のステップとして呼ぶ`compile`関数を追加した。実装中に既存`lisp_macroexpand_1`の未知の制約を発見した: `macroexpand-1`は呼び出し式の演算子を無条件に`lisp_eval`で評価してマクロクロージャかどうか確認するため、`if`/`let`/`quote`など特殊形式のキーワードをそのまま渡すと「unbound variable」でpanicする（特殊形式のキーワードは`lisp_eval`内の専用チェックで関数呼び出し分岐に入る前に捕まえられており、通常の変数として束縛されていないため）。そのためまず`macroexpand-all-special-form-p`で現行の特殊形式キーワード19種（`quote`/`quasiquote`/`if`/`progn`/`let`/`let*`/`setq`/`cond`/`and`/`or`/`when`/`unless`/`block`/`return-from`/`lambda`/`defvar`/`defparameter`/`defun`/`defmacro`）に該当するかを判定し、該当する場合は`macroexpand-1`を呼ばずに直接`macroexpand-all-forms`（各特殊形式の構造に従って評価される位置のサブフォームだけを再帰展開する、変数名・タグ名などは保持）へ渡す。該当しない（マクロ呼び出しの可能性がある）formだけを`macroexpand-1`のループにかけ、展開が止まるまで（`eq`で判定）繰り返してから`macroexpand-all-forms`の関数呼び出しフォールバック分岐（演算子位置も含め全要素を再帰展開）に渡す。`quote`/`quasiquote`/`defmacro`の内側はデータ・コンパイル時コードとして展開せずそのまま保持する。`compile`はmilestone42以降で追加する本番のバイトコード生成（`compile-expr`）がまだ無いため、現時点では`macroexpand-all`の結果をそのまま返すだけの薄いラッパーとする。検証は`test/lisp/test-compile.lisp`を新設し、非マクロ式・単純マクロ展開・マクロが別マクロへ展開される再帰展開・`if`/`let`/`lambda`/`setq`/`cond`/`defun`各特殊形式に埋め込まれたマクロ呼び出しの展開・`quote`内側の非展開・`compile`自体の動作の11項目を、car/cdr/eqで構造を手でたどる`struct-eq`ヘルパーで確認した（`equal`が無いため）。QEMU/OVMFヘッドレス起動（socket-serialで対話的にREPLへ`load`＋各`run-test-compile-*`を送信）で全項目`t`を確認、既存の`(+ 1 2)`等のREPL基本動作に回帰が無いことも確認した。 |
| 41 | バイトコード中間表現とアセンブラ | 未着手 | コンパイラ内部で使う「ラベル付き命令リスト」というIR（Lispのリストで表現）と、ラベルをジャンプオフセットへ解決して`bytecode`のバイト列へ変換するアセンブラをLisp側に実装する。 |
| 42 | compile-expr: リテラル・quote・ifのコンパイル | 未着手 | 定数（`OP_CONST`、constantsベクタへの登録含む）と`if`（`OP_JUMP_IF_FALSE`/`OP_JUMP`）のコンパイルを実装する。 |
| 43 | compile-expr: コンパイル時レキシカル環境とlet | 未着手 | コンパイル時の「スコープ→スロットオフセット」対応表を実装し、`let`束縛（`OP_MAKE_LOCAL`）と変数参照・`setq`（`OP_LOAD_LOCAL`/`OP_STORE_LOCAL`）のコンパイルを実装する。 |
| 44 | compile-expr: lambdaとクロージャ捕捉 | 未着手 | lambda本体の自由変数解析（自分のローカルでない変数参照を列挙し、捕捉元が直接囲む関数のローカルかさらに外側のupvalueかを判定）を実装し、`OP_MAKE_CLOSURE`と捕捉記述子（`upvalue_descs`）の生成をコンパイルする。 |
| 45 | compile-expr: 関数呼び出し・プリミティブ呼び出し | 未着手 | `OP_CALL`と、milestone39でインライン化した各プリミティブへのコンパイルを実装する。 |
| 46 | 統合検証 | 未着手 | 目標1でCの手動バイトコードとして検証した一連のケース（算術・分岐・ローカル変数・再帰呼び出し・外側変数を捕捉するクロージャ）を、`compile-expr`経由で同じS式から生成したバイトコードで再現する。検証用の橋渡し（`lisp_vm_exec`へ直接渡す一時的なテスト用ビルトインなど）を用意して正しい結果が返ることを確認し、目標2を完了とする。 |

## スコープ外として明記する項目

以下は本ロードマップの範囲を超えるため対象外とする:

- `defun`/既存の`lisp_eval`/`lisp_apply`経路とコンパイル済み関数の完全統合（ソースを自動的に
  コンパイルして通常の関数呼び出しから使えるようにする配線）。本ロードマップは`lisp_vm_exec`への
  直接投入による検証までを範囲とする。
- 末尾呼び出し最適化・スタック深度対策（既存ロードマップの非ゴールを継承）。
- 可変長引数（rest引数）を取るコンパイル済み関数。
- Common Lispの`tagbody`/`go`そのもののコンパイル（`OP_JUMP`はif/loop相当の基盤にとどめる）。
- レキシカルスコープのマクロ（`macrolet`相当）。既存インタプリタ同様、マクロは常にグローバル。
- Direct Threaded Code等によるVM自体の高速化（`switch`ループの素朴な実装で十分とする）。

## 検証方針

`lisp_robustness.md`までと同じ方針を踏襲する。各マイルストーン完了時に、`make build`でクロス
コンパイルが通ることと、`make run`（Linux環境ではOVMFファームウェアパスを環境に合わせて差し替える）
でQEMU上に起動し、想定した出力・動作がコンソール上で確認できることの両方を確認してから次の
マイルストーンに進む。

目標1（34〜39）は`lisp_vm_exec`にC側で手動構築したバイトコード配列を直接渡す形の検証で、既存の
QEMU/OVMFヘッドレス起動（socket-serial、既存手法）でシリアル出力を確認する。目標2（40〜46）は
既存の`test/lisp/*.lisp`→`esp_dir/test/`の`load`+`run-test-xxxxx`規約（milestone16/17で確立）に
従い新規フィクスチャを追加して検証する。
