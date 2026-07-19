# マルチプロセス化ロードマップ

## 目的

ユーザーから、このベアメタルUEFI LispOSを将来「マルチプロセスOS」として発展させたいという
最終目標が提示された。要件は次の通り(ユーザー原文の意訳):

- シングルアドレス空間を明示的な設計制約とする(そもそもUEFI/ページング設定変更無しの現状では
  自明に成立している)。
- CLOSの`process`クラスを新設し、全プロセスをそのインスタンスとする。システム全体のプロセス
  一覧を`os:*all-processes*`に保持し、`(os:get-all-processes)`で取得できる。`process`は
  `stackframe`・環境を持ち、`setf`で書き換え可能。
- `make-process`でプロセスを生成する。forkと同時に新規の隔離パッケージ(gensym的に必ず一意な
  名前、ユーザー指定名は衝突時に失敗)を自動生成し、ベースパッケージ(`common-lisp-user`)を
  `use-package`する。fork側で関数を書き換えたい場合は、ベースパッケージのシンボルは無傷の
  まま、fork側パッケージ内にローカルな新規シンボルを作り、そこへ`defun`する(コードコピー0、
  ベースパッケージからの委譲がデフォルト)。
- `common-lisp-user`をSBCLの`sb-ext:lock-package`同様にロックし、ロック中パッケージへの
  `defun`/`setq`が"Package is locked"エラーになるようにする。
- プロセスをファーストクラスのLispオブジェクトにし、`(process-suspend p)`/
  `(process-resume p)`/`(process-local-variable p 'x)`のようなC/VM組み込み関数を生やす。
- プロセスA(通常シェル)からフリーズしたプロセスBのパッケージ環境を覗き、バグった関数だけを
  ベースパッケージの元定義に「差し戻す」安全なリモートインスペクタREPLを作る。
- 現在1つしかないREPLを、Ctrlキーを2回連続で押すことでプロセス一覧から切替できるように
  したい。

「このようなことができるようなLispOSとしたい。これを最終目標として、マイルストンを作成し、
documents/配下にドキュメントを作成してほしい」という依頼であり、本ドキュメントはその
マイルストーン化・ロードマップ化の結果である。**本ドキュメント作成時点では、以下の
マイルストーンは一切実装されておらず、全て未着手である。**

事前調査(VM/スタックモデル・パッケージシステム・コンソール入力の3方向)で以下3点の重大な
既存制約が判明し、ユーザーへのヒアリングで方針を確定した:

1. **Ctrl単体2回押下の検知は現行の`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`では原理的に不可能**
   (単独修飾キー押下はScanCode/UnicodeCharのペアを生成しないアーキテクチャ上の制約)。
   → `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`を新規実装する方針を採用する(フェーズH)。
2. **VMはC言語の呼び出しスタックを暗黙のコールスタックとして使っており、割込み駆動の
   プリエンプションも存在しない**(UEFIブートサービス上で動作するため)。「フリーズした
   プロセスを外から覗く」を実現するには、真のfiber/コンテキスト切替が必要。
   → コルーチン方式(VM命令ディスパッチ毎のyieldチェック)を採用する(フェーズC)。既存の
   `main.c`起動時スタック切替(`AllocatePages`+インラインアセンブラでの`rsp`交換、
   `EfiMain`→`EfiMainImpl`)が同型の手法の実例として再利用できる。
3. **`*package*`を`common-lisp-user`以外に切り替えると特殊形式/ビルトインが無修飾で
   使えなくなる既存の未解消制約**(`documents/lisp_package_system.md`の旧マイルストーン
   85・86、フェーズG)が、fork側パッケージ(`use-package`経由で`common-lisp-user`を使う
   設計)にとって直接のブロッカーになる。
   → 本ロードマップの先頭フェーズとして組み込む(フェーズA)。グローバル連番の都合上、
   旧85・86番は本ロードマップ内で**100・101番として再実施**し、旧ドキュメント
   (`documents/lisp_package_system.md`)側の該当行は本ドキュメントへのリダイレクト注記に
   更新した(実装済みマイルストーンの番号を後から変更しない既存方針を維持するため、
   85・86という番号自体は「本ロードマップへ統合済み」として欠番のまま記録に残す)。

マイルストーン番号は既存の1〜99に続く**100から**開始する。

## 現状把握

- **VM/コールスタック**: `vm_stack`/`vm_sp`(`src/lisp.c:2018-2020`)は固定長グローバル配列
  だが、実際の呼び出し連鎖はVM実行(`lisp_vm_run`の`OP_CALL`再帰)・ツリーウォーク
  (`lisp_eval`/`lisp_apply`相互再帰)いずれもC言語の呼び出しスタックに乗っている。真の
  一時停止/再開にはプロセス毎に独立したCスタック領域+コンテキスト切替が必須。
- **既存のスタック切替前例**: `src/main.c:605-644`で`EfiMain`が16MBスタックを
  `AllocatePages`確保し、インラインアセンブラで`rsp`を差し替えて`EfiMainImpl`を呼び、
  戻り値後に元の`rsp`へ復帰している。この手法をプロセス数分・双方向に一般化するのが
  コンテキスト切替の土台になる。
- **`setjmp`/`longjmp`**: 手書きasm(`src/lisp.c:5706-5749`、`src/lisp.h:59-70`)。現在は
  単一グローバルの`lisp_active_trap`(`src/lisp.h:83`)のみで、`main.c:563`で1度設定される
  だけ。プロセス毎に個別のトラップが必要。`lisp_vm_reset_stack`(`src/lisp.c:2044-2046`)も
  単一グローバル`vm_sp`のリセットであり、プロセス毎に分離が必要。
- **動的/special変数はシンボル本体(`is_special`/`value`)に直接載っている**
  (`src/lisp.c:32-33`)。環境チェーンの外側にある単一グローバル可変状態であり、プロセス毎の
  独立性は現状無い。本ロードマップでは意図的にこの制約をスコープ外として残す(レキシカル
  環境・パッケージ分離のみを対象とする)。
- **レキシカル環境は素の`LispObject`連想リスト**(`lisp_env_extend`/`lisp_env_lookup`、
  `src/lisp.c:2550-2610+`)であり、`process`が`closure->env`同等の参照を持てば外部からの
  覗き見(`process-local-variable`)は自然に実現できる。
- **GCルート**(`lisp_gc_mark_roots`、`src/lisp.c:2477-2487`)は`global_env`/
  `global_packages`/`global_classes`/`vm_stack[0..vm_sp)`等をマークする。複数プロセスの
  スタックを扱うには、全プロセスの(将来的には独立した)スタック領域をルート走査対象に加える
  必要があり、GC発火条件も「全プロセスが安全点にいる時のみ」に拡張が必要。
- **パッケージ系**: `lisp_make_package`(`src/lisp.c:1056-1064`)は**同名パッケージが既に
  存在すると黙って既存を返す**(エラーにならない)ため、fork時のgensym的一意名保証は
  呼び出し側で厳密にチェックする必要がある(そのままでは名前衝突時に無言で同一パッケージを
  共有してしまう危険がある)。
- **`shadow`/`shadowing-import`/`import`の違い**: `shadow`は対象パッケージ内に**新規の
  別シンボルオブジェクト**を作る。`shadowing-import`は既存シンボルオブジェクトをそのまま
  (同一性を保って)ローカルへ差し込む。`import`は既存シンボルの衝突なし追加のみでどちらも
  差し替えない。fork側の「ベースを傷つけずに独自の`foo`を持つ」という要件には`shadow`が
  正しいプリミティブであり、本ロードマップでは`shadow`を使う(ユーザー原文の
  「shadowing-importで新しいシンボルを作る」という記述は、実際には`shadow`の挙動を
  指している)。
- **パッケージロック機構は現状皆無**。書き込みサイトは特定済み: ツリーウォーク`defun`
  (`src/lisp.c:3450`)、コンパイル済み`defun`の`establish-global-function`
  (`:3535-3541`、書込`:3539`)、`%set-symbol-function`(`:3559-3565`、書込`:3563`)、
  `setq`→`lisp_env_set`(`:2590-2618`、書込`:2593`。VM側`OP_GLOBAL_SET`も同経路)、
  `defvar`/`defparameter`直接書込(`:3411-3439`)。
- **コンソール入力**: 現行`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`(`src/uefi.h:178-189`、`ConIn`
  として`main.c`で使用)は単独修飾キーを一切報告できない。
  `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`(`ReadKeyStrokeEx`/`EFI_KEY_STATE.KeyShiftState`で
  修飾キー単体の状態を報告可能)は`uefi.h`に一切定義が無く、新規に型定義・GUID・
  `LocateProtocol`呼び出しを追加する必要がある。タイマーによる「一定時間内の2回押下」判定は
  既存の`lisp_builtin_sleep`(`:5135-5158`)と同じ`CreateEvent`/`SetTimer`/`WaitForEvent`
  パターンを再利用できる。

## マイルストーン一覧

### フェーズA: 前提条件解消(旧`documents/lisp_package_system.md`85・86番を統合)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 100 | `*package*`非既定時の特殊形式無修飾アクセス | 完了 | `lisp_symbols_init`末尾で、特殊形式ディスパッチシンボル(`quote`/`if`/`lambda`/`defun`/`defmacro`/`quasiquote`/`unquote`/`unquote-splicing`/`progn`/`let`/`let*`/`setq`/`cond`/`and`/`or`/`when`/`unless`/`defvar`/`defparameter`/`block`/`return-from`/`do`/`function`)・self-evalトークン`t`・ラムダリストキーワード(`&optional`/`&rest`/`&key`/`&aux`/`&allow-other-keys`)を`common-lisp-user`の`pkg_exports`へ直接追加した(`lisp_builtin_export`を経由せずCから直接`pkg_exports`へconsする、起動時1回限りの処理のため)。`compile-and-run`(トップレベル評価専用の内部トークン)と`*macroexpand-hook*`/`print-object`(特殊形式ではなく通常のグローバル束縛、milestone101のスコープ)は対象外とした。 |
| 101 | `*package*`非既定時のビルトイン無修飾アクセス | 完了 | `LISP_REGISTER_BUILTIN`マクロを、シンボルの関数セルを設定した直後に`common-lisp-user`の`pkg_exports`へも自動でconsする`lisp_register_and_export_builtin`ヘルパー経由に置き換え、以後`lisp_builtins_init`で登録される全ビルトイン(`car`/`cons`/`+`/`in-package`等)が無修飾アクセス可能になるようにした。同マクロを経由しない`print-object`(総称関数)・`*macroexpand-hook*`(動的変数)は個別に`pkg_exports`へ追加した。`in-package`自身が無修飾で呼べず二重コロン修飾でしか復帰できないという既存の制約(`lisp_package_system.md`milestone79参照)もこれで解消されたことをC自己テストで確認した。 |

### フェーズB: プロセスオブジェクトモデル(CLOS)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 102 | `os`パッケージ・`process`クラス・レジストリ | 完了 | 新規`lisp/os-package.lisp`(`defpackage "os"`で`process`/`*all-processes*`/`get-all-processes`をexport)と`lisp/os.lisp`(`defclass os:process`、`os:*all-processes*`、`os:get-all-processes`本体)の2ファイルに分割し、`main.c`から`compiler.lisp`/`stdlib.lisp`と同様に別々の`lisp_load_boot_file`呼び出しでロードした(`load`は1回の呼び出し内でファイル全体を読み切ってから評価するため、同一ファイル内では`defpackage`の効果を後続フォームの読み取り(`os:`修飾子解決)に反映できない、milestone72/76/78/79/81/100/101と同根の制約)。CLOS `defclass os:process`のスロット(`name`/`package`/`stackframe`/`env`/`status`)は無修飾のまま`common-lisp-user`所属とした(スロット照合はシンボルeqであり、`os`パッケージ外からも無修飾`'name`等で一貫して`slot-value`を呼べるようにする設計判断)。`os:*all-processes*`(グローバルリスト、`defvar`)と`os:get-all-processes`を追加した。この段階ではプロセスは単なるデータ構造であり、実行機構は持たない。 |
| 103 | `make-process`(名前一意性のみ、実行機構無し) | 完了 | Cビルトイン`%make-process`(`src/lisp.c`、`os`パッケージへexportされた`lisp/os.lisp`の`os:make-process`(`&optional name`)ラッパー経由で呼ぶ、`%make-class`と同じ「Cビルトイン+薄いLispラッパー」パターン)を実装した。名前省略時はgensymと同じカウンタ方式で`"PROCESS-<N>"`形式の一意名を生成する。ユーザー指定名は`os:*all-processes*`内の既存プロセス名と内容が一致すれば`lisp_panic`で失敗する(文字列の内容比較`lisp_streq`はstr_data直接アクセスが必要でLisp側に無いため、Cビルトインとして実装する必要があった)。生成した`process`インスタンスは`name`スロットを設定し`os:*all-processes*`へconsで登録して返す。この段階ではまだfork実行・パッケージ分離・スタック確保は行わない。 |

### フェーズC: 実行基盤(コンテキスト切替)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 104 | per-processスタック領域とコンテキスト保存構造 | 完了 | 新規構造体`LispProcessStack`(`regs`(既存の`lisp_jmp_buf`をそのまま再利用)・`stack_base`/`stack_pages`・`pending_entry`/`pending_arg`・`started`)を追加した。`lisp_process_stack_create`が`AllocatePages`で新規スタック領域を確保し、その領域のtopを16byte境界+call直後を模した8byte分差し引いた位置へ`rsp`を設定、`rip`を小さなトランポリン関数(`lisp_process_trampoline`、グローバルに一時退避した`entry`/`arg`を読んで呼ぶ)へ向けた「未開始」コンテキストとして初期化する(この段階ではまだ実行は始まらない)。`lisp_context_switch(from, to)`は`lisp_setjmp(&from->regs)`で現在地を保存した直後に`lisp_longjmp(&to->regs, 1)`で`to`へ切替える(`to`が未開始なら偽装した`rsp`/`rip`から新規スタック上で開始、開始済みなら前回の中断点から再開)、新規アセンブラを一切書かずmilestone30の既存`lisp_setjmp`/`lisp_longjmp`(`jmpq`ベースで戻り先を問わない)をそのまま再利用する設計とした。C自己テスト`lisp_context_switch_selftest`をmain/別スタックコンテキスト間で3往復させ、双方のカウンタが期待どおり増えることを確認する形で追加した(`main.c`の既存自己テスト群と同じPASS/FAIL即hangパターンで起動シーケンスに組み込み)。`process`インスタンスの`stackframe`スロットへの接続・スケジューラ・yieldチェックはmilestone105/106のスコープであり本マイルストーンでは行わない。 |
| 105 | per-process `vm_stack`/`vm_sp`・トラップ分離 | 完了 | `LispProcessStack`(milestone104)へ`vm_stack`(`VM_STACK_SIZE`個、この定数は`src/lisp.c`から`src/lisp.h`へ移した)・`vm_sp`・`active_trap`の3フィールドを追加した。`src/lisp.c`側の同名のグローバル`vm_stack`/`vm_sp`/`lisp_active_trap`は「今実際に実行中のプロセスのもの」を指す単一の作業領域のまま変更せず(`lisp_vm_run`等の既存コードは無変更)、`lisp_context_switch`が切替の都度、現在のグローバルの内容を`from`へ退避してから`to`に保存されていた内容をグローバルへ復元するようにした。`lisp_process_stack_create`は新規コンテキストを`vm_sp=0`・`active_trap=NULL`(空のVMデータスタック・トラップ未設置)で初期化する。C自己テスト`lisp_process_vm_state_selftest`で、別スタック上のコンテキストの開始直後にmain側の値・トラップが一切見えないこと、逆にmain側がbの実行によって値・トラップを書き換えられないこと、bを再度resumeすると自分専用の値・トラップがそのまま残っていることを確認した。`lisp_vm_reset_stack`は元から「現在実行中のプロセスのvm_spのみ」を操作する実装であり、per-process分離後もそのままプロセス単位のリセットとして機能するため変更不要だった。 |
| 106 | コルーチンyieldチェック | 完了 | `lisp_vm_run`のVM命令ディスパッチループ先頭に、毎命令チェックする軽量なyieldフックを追加した。新規グローバル`lisp_vm_current_process`/`lisp_vm_yield_target`(いずれも`LispProcessStack *`、デフォルトNULL)の両方が非NULLの時のみ「武装」され、武装中は`lisp_vm_yield_budget`(`UINTN`、デフォルト実質無制限)が1減るごとに0へ達した時点で`lisp_vm_current_process`から`lisp_vm_yield_target`へ`lisp_context_switch`する(milestone104/105の`vm_stack`/`vm_sp`/`lisp_active_trap`退避にそのまま相乗りするため、呼び出し元がbudgetを再設定してから逆方向へ`lisp_context_switch`すれば、pc・オペランドスタック・ローカル変数を含む実行状態を一切壊さず続きから再開できる)。両グローバルがNULLのデフォルト状態では既存の`lisp_vm_run`呼び出し経路(ブート・REPL・全self-test/test-lisp fixture)の挙動は一切変化しない。C自己テスト`lisp_vm_yield_selftest`を追加し、0からTARGET(=12)まで1ずつ数え上げるbytecodeを手書きで構築し(`OP_MAKE_LOCAL`/`OP_LOAD_LOCAL`/`OP_STORE_LOCAL`/`OP_ADD`/`OP_EQ`/`OP_JUMP_IF_FALSE`を使ったループ)、budgetの小さいquantum(=3)を使って`main`が1回ずつ`lisp_context_switch`で別スタック上の`b`へ制御を渡す、というのを`b`が完了フラグを立てるまで繰り返す形で検証した。これにより、(1)1回の切替では完了せず命令ディスパッチの「途中」で実際に複数回yield・resumeされたこと(切替回数>1)、(2)最終的にbytecodeが正しい結果(TARGET)まで数え上げを完了したこと(pc・オペランドスタック・ローカル変数がyieldを跨いで正しく保持されたこと)の両方を確認した。`main`側の`while (!done)`ループがyieldの回数に応じて動的に切替回数を合わせる設計にしたため、milestone105の固定回数往復と違い切替回数の対称性を手動で数え合わせる必要が無かった(`b`のentryは完了時に1回だけ明示的に`main`へ戻ればよい)。武装解除(両グローバルをNULLへ・budgetを無制限へ戻す)は、この自己テストのスタックローカルな`main_ctx`/`b_ctx`が関数を抜けると無効になる(直後に続く`compiler.lisp`/`stdlib.lisp`ロード等がダングリングポインタへ`lisp_context_switch`してしまう)ことを防ぐため、成功・失敗どちらの経路でも必ず実行するようにした。**yieldチェックは`lisp_vm_run`(コンパイル済みbytecode経路)内のみのスコープであり、ツリーウォーク経路(`lisp_eval`/`lisp_apply`、`defmacro`本体・rest-arg形式`defun`で使われる)は対象外のまま残る**(真のプリエンプションを導入しない限りこの経路上の無限ループはyield不可能な既知の制約)。実際のスケジューラ・複数プロセスの同時実行はmilestone107以降のスコープであり本マイルストーンでは行わない。 |
| 107 | GCルート拡張 | 完了 | `lisp_process_stack_register`/`lisp_process_stack_unregister`(引数`LispProcessStack *`)を新設し、「中断中の他プロセス」を明示的に登録・解除できるようにした(milestone87で発見した一時的Cローカルルート`lisp_gc_extra_root`と同じ手動登録パターン)。登録済みプロセスの配列は固定長16件(`LISP_MAX_REGISTERED_PROCESS_STACKS`)。`lisp_gc_mark_roots`を拡張し、既存の(今実行中のプロセスの)グローバル`vm_stack[0..vm_sp)`マーキングに加えて、登録済み各プロセスの`LispProcessStack.vm_stack[0..vm_sp)`もマークするようにした(今実行中のプロセスがたまたま登録されていた場合は同じ内容を二重にマークするだけで安全)。C自己テスト`lisp_process_gc_root_selftest`を追加し、`lisp_vm_gc_root_selftest`(milestone34、単一プロセス版)の複数プロセス拡張として、別スタック上のプロセス`b`だけが`b`専用の`vm_stack`にpushしたcons(mainのグローバル`vm_stack`からも、Cローカル変数からも到達不能)が、`b`をmainへ切り替えて中断させた直後に`lisp_gc()`を実行しても回収されない(その後64個のダミーconsを確保してフリーリスト再利用を強制しても内容が上書きされない)ことを確認した。`b_ctx`はテスト関数を抜けると無効になるため、成功・失敗どちらの経路でも`lisp_process_stack_unregister`を必ず呼ぶ(milestone106の武装解除と同じ安全設計)。**GC発火条件の「全プロセスが安全点にいる時のみ」という要求について**: 現状GCが発火するのはREPLループ先頭(`lisp_heap_low`が真の時)または`(gc)`組み込み関数の呼び出し時のみであり、いずれも「今実行中の唯一のプロセス」自身の安全地点でしか呼ばれない。他の全プロセスはmilestone106のyieldチェックが命令ディスパッチの先頭でのみ発生する設計上、中断中は常に「命令の境界」で止まっており、その`vm_stack`スナップショットは常に安全である。したがって本要求は新たな判定コード(「全プロセスが安全点にいるか」を実行時に確認するチェック等)を追加しなくても、既存のyieldチェック設計により**構造的に**満たされている(安全点以外で中断する経路がそもそも存在しない)と判断し、判定コードの追加は行わなかった。 |

### フェーズD: fork時パッケージ分離とコード書き換え

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 108 | fork時の一意パッケージ生成 | 完了 | `%make-process`(`os:make-process`の実体)を拡張した。`lisp_make_package`の「同名パッケージが既に存在すると黙って既存を返す」挙動はfork時の一意性保証には使えないため、新規`lisp_make_package_strict`(名前衝突時に黙って共有せずpanicする作成経路)を追加した。パッケージ名は新規`lisp_generate_fork_package_name`が独立カウンタ`lisp_fork_package_name_counter`から`"FORK-PKG-<N>"`形式で生成する(既存のプロセス名生成`lisp_process_name_counter`/`"PROCESS-<N>"`とは別カウンタ・別接頭辞のため、プロセス名とパッケージ名が互いに衝突する余地は無い)。生成したパッケージへは既存の`lisp_builtin_use_package`をそのまま呼び出して`common-lisp-user`を`use-package`し、結果の`os:process`インスタンスの`package`スロット(milestone102で新設・以前は常にnil)へ格納するようにした。Lisp側の`os:make-process`定義自体はシグネチャ変更なし(拡張は全てC側の`%make-process`内で透過的に行われる)。C自己テスト`lisp_process_fork_package_selftest`を追加し、(1)生成された`package`スロットの値が`LISP_NIL`ではないこと、(2)2回連続で`make-process`した際の2つのfork先パッケージ名が異なること、(3)fork先パッケージが`common-lisp-user`を`use-package`していること(`pkg_uses`経由)、(4)`lisp_intern_in_package(common-lisp-user, "car")`とfork先パッケージ内で無修飾`intern`した`"car"`が`eq`であること(=ベースパッケージの委譲が実際に機能している)の4点を確認した。本自己テストは`os:process`クラス定義に依存するため、`main.c`の起動シーケンス上`os.lisp`ロード後に実行するよう配置した(milestone104-107の自己テストが`.lisp`ロード前に置かれているのとは異なる制約)。 |
| 109 | fork側でのローカル関数再定義 | 完了 | `shadow`はmilestone92で既に実装済みだったため新規のCコードは不要だった。実際の運用手順は「`in-package`でfork先パッケージへ切替→`shadow`でベースと同名の別シンボルを確保→そのシンボルへ`defun`」の3ステップで、REPL/`load`が1トップレベルフォームずつ`read`→`eval`を繰り返す(前のフォームの実行結果である`*package*`の変更が次のフォームの`read`に反映される)ことに支えられている。この3ステップをそのまま1つのテスト関数の本体として書くと、関数全体が`common-lisp-user`のまま1度に読み切られてしまうため、本体中の`car`という無修飾トークンは(実行時に`in-package`していても)結局`common-lisp-user`の`car`として読まれてしまい検証にならない(milestone79/81で確認した`*package*`非既定時のreader可視性制約と対称の問題)。そこでテスト(`test/lisp/test-os.lisp`の`run-test-os-make-process-fork-redefine`)では、実際の再定義そのものは`%set-symbol-function`(milestone93)を使い、`shadow`で確保したシンボルを`intern`(文字列引数、`*package*`に対しランタイムに解決される)経由で取得して結び付けることで、reader可視性制約を回避しつつ手順自体(`in-package`/`shadow`実行)は忠実に踏襲した。(1)fork側の`car`シンボルとベースの`car`シンボルが`eq`でないこと、(2)fork側で再定義した関数を呼ぶと新しい結果が返ること、(3)ベース側の`car`(`(car (cons 1 2))`)が無傷のまま`1`を返すこと、(4)`common-lisp-user`へ復帰後の`(intern "car")`が変わらずベースシンボルと`eq`であることを確認した。併せてmilestone108(C自己テストのみで検証済み)についても`run-test-os-make-process-fork-package`をLisp側テストとして追加し、2つの独立したforkパッケージそれぞれで無修飾`car`が正しくベースパッケージの`car`へ委譲されることを確認した。 |

### フェーズE: ベースパッケージロック

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 110 | `lock-package`/`unlock-package` | 未着手 | パッケージへロックフラグ(`pkg_locked`相当の新規フィールド)を追加し、`lock-package`/`unlock-package`ビルトインを実装する。 |
| 111 | ロックチェックの全書込サイトへの追加 | 未着手 | 現状把握で特定済みの5書込サイト(ツリーウォーク`defun`・コンパイル済み`defun`経由の`establish-global-function`・`%set-symbol-function`・`setq`/`lisp_env_set`系・`defvar`/`defparameter`系)全てにロックチェックを追加し、ロック中パッケージへの書込が"Package is locked"でpanicするようにする。起動時に`common-lisp-user`をデフォルトでロックする。 |

### フェーズF: プロセスのファーストクラス化(C/VM組み込み)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 112 | `process-suspend`/`process-resume` | 未着手 | フェーズCのコンテキスト切替機構を使い、指定した`process`インスタンスの実行を実際に一時停止・再開するC組み込み関数を実装する。 |
| 113 | `process-local-variable` | 未着手 | 指定した`process`のレキシカル環境・fork側パッケージ内シンボルの値を外部から読み取るC組み込み関数を実装する。動的/special変数はプロセス毎に分離されていない既知の制約(現状把握参照)を明記する。 |

### フェーズG: 安全なリモートインスペクタREPL

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 114 | プロセス環境インスペクタ | 未着手 | 他プロセスのパッケージ内シンボル一覧・関数定義を覗くLispレベルの対話ユーティリティを実装する。既存の`do-symbols`系マクロ(`documents/lisp_package_operations.md`)と`process-local-variable`(113)を組み合わせる。 |
| 115 | 関数の「差し戻し」コマンド | 未着手 | fork側でshadowされた関数を、ベースパッケージの元の定義に戻すインタラクティブなデバッグコマンドを実装する。 |

### フェーズH: コンソール入力拡張とプロセス切替UI

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 116 | `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`導入 | 未着手 | `uefi.h`へ`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`・`EFI_KEY_DATA`・`EFI_KEY_STATE`等の型定義とGUIDを追加し、`LocateProtocol`で取得する。`ReadKeyStrokeEx`+`KeyState.KeyShiftState`により、Ctrlキー単体の押下を検知する。 |
| 117 | Ctrl2回連続判定 | 未着手 | `CreateEvent`/`SetTimer`/`WaitForEvent`パターン(`lisp_builtin_sleep`と同型)を使い、一定時間内にCtrl単体押下が2回発生したことを判定するロジックを実装する。 |
| 118 | プロセス一覧UI・REPL切替 | 未着手 | Ctrl2回連続押下(117)を検知したらプロセス一覧を表示・選択させ、フェーズCのコンテキスト切替機構(104-106)とフェーズBのプロセスレジストリ(102)を使ってアクティブなREPLを選択したプロセスへ切り替える。 |

## スコープ外として明記する項目

- **真のプリエンプション(割込み駆動スケジューリング)**: IDT/タイマー割込みのセットアップは
  行わない。yieldは常にVM命令ディスパッチループが「協調的に」チェックするコルーチン方式に
  限定する(フェーズC参照)。ツリーウォーク経路の無限ループはyield不可能な既知の制約として
  残す。
- **メモリ保護によるプロセス分離**: シングルアドレス空間・MMU保護無しはユーザー自身が明示
  した設計制約であり、プロセス間の「隔離」はパッケージ(名前空間)レベルのみである。どの
  プロセスも他プロセスのメモリを直接壊せる。
- **動的/special変数のプロセス毎分離**: `defvar`/`defparameter`はシンボル本体への単一
  グローバル書込のままとし、プロセス毎の再束縛機構は導入しない。
- **マルチコア/SMP**: 単一実行コンテキストの協調的切替のみを対象とする。
- **ネットワーク越しの「リモート」インスペクタ**: 「リモート」は同一イメージ内の別プロセスを
  指すのみで、ネットワーク越しの操作は対象外。
- **CLOSの`:before`/`:after`/`:around`メソッドコンビネーション・`call-next-method`**
  (`documents/lisp_clos.md`milestone97のスコープ外を継承)。
- **プロセスの終了・破棄(`process`インスタンス自体やfork側パッケージの解放・GC)**。本
  ロードマップはプロセスの生成・切替・書き換えのみを対象とし、ライフサイクルの終端は扱わない。

## 検証方針

各マイルストーン完了時に、`make build`でクロスコンパイルが通ることと、`make test`
(既存フィクスチャ全PASS、回帰無し)を確認する。フェーズC(コンテキスト切替・yield・GCルート
拡張)とフェーズH(Ex Protocol・Ctrl2回検知)は、`make test`のヘッドレスハーネスでは検証できない
(実行タイミング・キー入力・複数プロセスの並行状態を扱うため)ため、個別のQEMU対話セッション
(シリアル経由)での目視確認を主たる検証手段とする。フェーズA・D・E・F・G相当の機能追加
(パッケージexport・fork時パッケージ生成・ロック・`process-local-variable`等)は、既存の
`test/lisp/test-package.lisp`/`test-clos.lisp`と同様、t/nilの観測に落とし込める範囲は
`make test`で自動化し、panicシナリオ(ロック違反・名前衝突・存在しないプロセスの操作等)は
既存の方針(milestone78/81等)と同様に個別QEMU対話で確認する。フェーズ単位で完了条件とし、
後続フェーズへ進む前に直前フェーズの回帰確認をまとめて行う。
