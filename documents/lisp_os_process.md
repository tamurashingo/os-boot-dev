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
| 110 | `lock-package`/`unlock-package` | 完了 | `LispClosure`(パッケージのescape hatch実装)へ新規`int pkg_locked`フィールドを追加した(既存の`pkg_*`系フィールドと同様、closure/文字列/vector/クラス等を初期化する全13箇所で`0`初期化する必要があった)。`lock-package`/`unlock-package`(いずれも`lisp_resolve_package_designator`でパッケージ本体/文字列/symbol/keywordのいずれのdesignatorも受け付け、フラグを立てる/下ろすだけの冪等な操作)と、フラグを読み取る`package-locked-p`の3ビルトインを追加した。書込サイトへのロックチェック追加自体はmilestone111のスコープであり、本マイルストーンではフラグの読み書きのみを実装した。`test/lisp/test-package.lisp`に`run-test-package-lock`を追加し、初期状態が`nil`(未ロック)であること、`lock-package`後に`package-locked-p`が`t`になること、`unlock-package`後に`nil`へ戻ることを確認した。 |
| 111 | ロックチェックの全書込サイトへの追加 | 完了 | ロック済みパッケージへの書込を"Package is locked"でpanicさせるチェックを追加し、起動時に`common-lisp-user`をデフォルトでロックした。**設計判断(redefinition-onlyセマンティクス)**: ロードマップ原文どおり「起動時に`common-lisp-user`をデフォルトでロックする」を素朴に「ロック中パッケージへの書込を全て禁止」と解釈すると、既存テストフィクスチャの`(defun run-test-xxx ...)`(全て新規シンボルへの初回定義)まで壊れてしまう。そこで**新規シンボルへの初回定義は常に許可し、既に確立済みの束縛(関数セル`fn`が非nil、または動的変数`is_special`が真)を上書きする場合のみ、そのシンボルのホームパッケージ(`sym->package`。呼び出し時の`*package*`ではない)がロック済みならpanicする**という"redefinition-only"セマンティクスを採用した。判定用ヘルパー`lisp_symbol_home_package_locked`/`lisp_check_function_redefine_allowed`を追加し、(1)ツリーウォーク`defun`、(2)コンパイル済み`defun`経由の`establish-global-function`、(3)`%set-symbol-function`の3箇所は「`fn`が既に非nilかつロック済みならpanic」、(4)ツリーウォーク`defparameter`と(5)コンパイル済み`defparameter`/`defvar`が共通で呼ぶ`establish-special`は「上書き前の`is_special`が真かつロック済みならpanic」とした。`defvar`自体は「既にis_specialなら値を評価も上書きもしない」という既存の非対称な条件分岐(milestone18)により構造的に既存束縛を上書きできないため、変更不要とした。**設計判断(`setq`/`lisp_env_set`はチェック対象外)**: ロードマップは`setq`系も書込サイトとして挙げていたが、`setq`は「既に確立済みの変数の値を書き換える」通常の実行時操作であり、CommonLisp/SBCLのpackage lockもここは対象にしない。ここへチェックを入れると`(let ((*dv* ...)) (setq *dv* ...))`のような、ロック対象の`common-lisp-user`自身が持つ動的変数への通常の再束縛・代入(既存の`test-dynamic-vars.lisp`の`run-test-let-rebinds-dynamic`/`run-test-setq-dynamic`等が検証している正常動作)まで塞いでしまうため、意図的にチェック対象外とした。`lisp_lock_cl_user_package`(`src/lisp.c`/`src/lisp.h`)を追加し、`main.c`の起動シーケンス末尾(`compiler.lisp`/`stdlib.lisp`/`os-package.lisp`/`os.lisp`読み込み・全C自己テスト完了後、`lisp_load_init_file`直前)から呼び出すことで、milestone81の自己テスト(`common-lisp-user`内での同名関数再定義によるシンボル同一性検証)等、起動処理自体が行う`common-lisp-user`内の再定義はロックの影響を受けないようにした。この結果、既存テストのうち`common-lisp-user`上で意図的に既存定義を上書きしていた3箇所(`test-dynamic-vars.lisp`の`run-test-defparameter-overwrite`、`test-compile-and-run.lisp`の`run-test-compile-and-run-defparameter-overwrite`、`test-symbol-function.lisp`の`run-test-symbol-function-redefine`)は、上書き直前に`unlock-package`、直後に`lock-package`で挟むよう修正した(通常の開発者が既存定義を意図的に再定義する際の想定手順と同じ)。`test/lisp/test-package.lisp`に`run-test-package-lock-redefinition-only`を追加し、ロック中の新規パッケージへ新規シンボルを初回定義することは成功し、アンロック後の再定義も成功することを確認した。ロック中に既存定義を再定義してpanicするシナリオそのものは検証方針どおり`make test`では検証せず、個別のQEMU対話セッションでの確認に委ねる。`make build`/`make test`(28ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |

### フェーズF: プロセスのファーストクラス化(C/VM組み込み)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 112 | `process-suspend`/`process-resume` | 完了 | フェーズC(milestone104-107)のコンテキスト切替機構を使い、`os:process`インスタンスに実際の実行機構を与えた。Cビルトイン`%process-resume`/`%process-suspend`(`src/lisp.c`)を実装し、`lisp/os.lisp`に薄いラッパー`os:process-resume`/`os:process-suspend`を追加した(`%make-process`と同じ「Cビルトイン+薄いLispラッパー」パターン)。**設計**: `process`の`stackframe`スロット(milestone102で新設・以前は常にnil)を、固定長16件のコンテキストプール(`lisp_process_context_pool`、1プロセスあたり1MiB)内のindexを表すfixnumとして再利用する(未起動ならnil)。`status`スロットは`:active`/`:suspended`/`:finished`を格納する。`%process-resume(process &optional thunk)`は、未起動(`stackframe`がnil)なら`thunk`(0引数のLisp関数)を新規コンテキストで`lisp_process_stack_create`経由で開始し、起動済みなら直前の`%process-suspend`の中断点から再開する(再開時はmilestone107のGCルート登録を`lisp_process_stack_unregister`で外す)。いずれの場合も`lisp_context_switch`が呼び出し元へ戻るまでブロックする。`%process-suspend(process)`は「今実際に実行中のコンテキスト自身」からのみ許可される(自分自身をsuspendする設計。他プロセスの強制停止はできない、`target != lisp_current_process_stack`ならpanic)。中断直前に`lisp_process_stack_register`でGCルートへ登録してから、`LispProcessStack.resumer`フィールド(新設。resume側が毎回セットする「直前に自分をresumeした側」への参照)へ`lisp_context_switch`で戻る。**`&optional thunk`という、ロードマップ原文の`(process-resume p)`より引数が増えたシグネチャ差異の理由**: 未起動プロセスを開始するには「どの0引数関数から実行を始めるか」を必ずどこかで渡す必要があり、`make-process`時点では実行内容を持たない(ロードマップの設計どおり`stackframe`/`env`は生成時は空)ため、開始時にのみ`thunk`を渡す形にした(起動済みプロセスの再開時は`thunk`は無視される)。**単一実行コンテキストの協調的切替である以上、`%process-resume`の呼び出し元がその呼び出しから戻ってきた時点で観測できる`status`は常に`:suspended`か`:finished`のいずれかであり、`:active`を外部から観測することは原理的にできない**(自分自身の`status`を自分の実行中に読む場合を除く)ことをC自己テスト・`src/lisp.h`のコメント・`lisp/os.lisp`のコメントに明記した(当初この不変条件に反する`:active`を自己テストで直接アサートする設計ミスがあり、単一の実行コンテキストしか無い以上検証不可能なアサーションが常にFAILし続けていたことが判明、修正した)。**既知のGC安全性の制約(スコープ内に留める、未解消)**: milestone107のGCルート拡張は中断中プロセスの`vm_stack`(コンパイル済みbytecode経路のVMデータスタック)のみを対象とし、ツリーウォーク経路(`lisp_eval`/`lisp_apply`)のC局所変数はGCルート集合の対象外のままである(自己再帰する`lisp_eval`/`lisp_apply`のCスタックフレーム上のLispObjectは、milestone104でプロセス毎に独立したOSスタック領域に乗っているため、そのメモリ自体は破壊されないが、GCのマーク走査対象にはなっていない)ため、あるプロセスがツリーウォーク経路の途中で中断している間に別プロセスが`(gc)`を誘発すると、C局所変数だけから到達可能なオブジェクトが理論上回収され得るという既知の未解消リスクが残る。C自己テスト`lisp_process_suspend_resume_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`の起動シーケンスへ`os.lisp`読込後に組み込み)を追加し、ダイナミック変数`m112-counter`をインクリメント→自分自身に`%process-suspend`→再度インクリメントする0引数閉包を新規プロセスに対し開始し、(1)1回目のインクリメント後・suspend直後まで実行が呼び出し元へ返ってきて`status`が`:suspended`であること、(2)再度resumeすると2回目のインクリメント後に閉包が正常に戻り`status`が`:finished`になること、(3)2回のインクリメントの結果が期待どおり`2`であることを確認した。Lisp側テスト`run-test-os-process-suspend-resume`(`test/lisp/test-os.lisp`)も同様の検証をlambdaクロージャ(レキシカルに`counter`/`p`を捕捉、ダイナミック変数不要)で追加した。**デバッグ経緯として記録**: 実装当初、この閉包の本体を`(defun ... () (setq ...) (%process-suspend ...) (setq ...))`という3formのまま複数form並べただけで書いてしまい、`defun`/`lambda`の本体は単一formのみ(milestone21で既に確認済みの「prognの落とし穴」、`lisp_make_closure`へ渡す`body`が`cddr`の`car`のみを取り残りを無条件に無視する仕様)という既存制約を踏み外し、2番目・3番目のformが黙って無視されてsuspendが一切呼ばれないまま1個目のformだけで即座に`:finished`してしまうバグを起こした。QEMUシリアル出力に一時的な`m112 debug: checkN ...`診断print(`lisp_print`経由)を仕込んでどの検証が失敗しているか・実際の`status`値を特定し、原因を`(progn ...)`で明示的に束ねていない複数form本体と断定してから、C自己テスト・Lisp側テストの両方の本体を`(progn ...)`で束ねる形に修正した(診断printはデバッグ後に削除済み)。`make build`/`make test`(28ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |
| 113 | `process-local-variable` | 完了 | Cビルトイン`%process-local-variable(process symbol)`(`src/lisp.c`)を実装し、`lisp/os.lisp`に薄いラッパー`os:process-local-variable`を追加した(`%process-resume`/`%process-suspend`と同じパターン)。**設計**: `process`の`env`スロット(milestone102で新設・以前は常にnil)へ、`%process-resume`が初回起動時(`stackframe`がnilの分岐)にthunkクロージャ自身の`env`フィールド(`lisp_closure_cell(thunk)->env`、生成時点で捕捉したレキシカル環境のalist)をコピーするようにした。`%process-local-variable`はその`env`スロットを`lisp_env_lookup`(既存関数、動的/special変数ならシンボル自身の`value`を返し、レキシカル変数ならenvチェーン→`global_env`の順に探し、無ければ`lisp_panic`)にそのまま渡すだけで実装できる。`stackframe`がnil(未起動)ならpanicする。ロードマップ原文の「`process-local-variable`」は`make-process`時点のレキシカル環境と読めるが、Lisp組み込み関数は呼び出し元の評価時`env`を受け取れない(評価済み`args`のみ)ため、実際に捕捉できるのは`make-process`より後に呼ばれる`thunk`引数自身の生成時点の環境である(この差異は`&optional thunk`という`%process-resume`のシグネチャ自体に既に現れている、milestone112参照)。**重要な制約(実装中に発見)**: `env`アリストを持つのはツリーウォーク(`lisp_eval`)経由で作られたクロージャのみ。通常のdefun/lambda(ラムダリストキーワード無し)は`lisp_eval_toplevel`がデフォルトで`compile-and-run`経路(VMバイトコード、milestone60)へコンパイルし、コンパイル済みクロージャはレキシカル変数を`env`アリストではなく位置ベースの`upvalue_descs`/`upvalues`(milestone38、変数名を保持しない`kind`/`index`のみ)で捕捉するため、そのようなthunkに対しては`process-local-variable`は(動的変数を除き)何も見つけられず`unbound variable`でpanicする。したがって現状`process-local-variable`が機能するのは、thunk自身の生成箇所が`&optional`/`&rest`等のラムダリストキーワードを持つ`defun`本体全体、またはCから直接`lisp_eval`を呼ぶ経路など、ツリーウォークへフォールバックする場合に限られる(`lisp_defun_params_needs_interpreter`、milestone89)。C自己テスト`lisp_process_local_variable_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`の起動シーケンスへ`os.lisp`読込後・`%process-resume`/`%process-suspend`自己テストの直後に組み込み)を追加し、`let`でレキシカル変数`m113-x`(値42)を束縛した内側で生成した`(lambda () nil)`をthunkとして起動し、`%process-local-variable`で`m113-x`の値を外部から読み取れることを確認した。Lisp側テスト`run-test-os-process-local-variable`(`test/lisp/test-os.lisp`)も同様の検証を`os:process-local-variable`経由で追加したが、**このテスト関数自身に未使用の`&optional dummy`引数を付けて`lisp_defun_params_needs_interpreter`を真にし、本体全体をツリーウォークへ強制フォールバックさせる必要があった**(実装中、通常の`defun`のまま書いて実際に`unbound variable`パニックを起こし、上記の制約を発見した経緯そのもの)。`make build`/`make test`(28ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |

### フェーズG: 安全なリモートインスペクタREPL

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 114 | プロセス環境インスペクタ | 完了 | 他プロセスのパッケージ内シンボル一覧・関数定義・レキシカル変数を覗くLispレベルの対話ユーティリティを`lisp/os.lisp`に実装した。既存の`do-symbols`マクロ(milestone91、`documents/lisp_package_operations.md`)と`process-local-variable`(113)の組み合わせのみで実現でき、**新規Cビルトインは一切追加していない**。`os:process-package`(`slot-value`の薄いアクセサ)、`os:process-function-definitions`(対象プロセスのfork側パッケージから`do-symbols`でアクセス可能な全シンボルを走査し、`fboundp`なものを`(symbol . function)`の連想リストとしてsetq累積で組み立てる。`do-symbols`自体は値集約機能を持たない素朴なループマクロなので、`test-package.lisp`の`run-test-package-do-symbols`等と同じ既存パターンを踏襲した)、`os:process-lexical-variables`(内部ヘルパー`process-lexical-variable-names`が`env`スロット(素のalist、milestone113)を`(mapcar #'car ...)`で直接覗いて変数名一覧を得て、`process-local-variable`(113、公式アクセサ)経由で値を読み取る。`process-local-variable`自体は「名前を渡して値を読む」一方向APIなので、対象の名前一覧を得る手段としてenvスロットの直接参照を組み合わせる必要があった)、`os:inspect-process`(name/status/package/package-name/functions/lexical-variablesを1つの連想リストにまとめたトップレベルエントリポイント)をそれぞれ追加し、`process-package`/`process-function-definitions`/`process-lexical-variables`/`inspect-process`を`os`パッケージからexportした(内部ヘルパー2つはexportしない無修飾のcommon-lisp-userシンボルのまま)。**設計判断(構造化データを返す、整形出力しない)**: 本処理系にはprintf的な整形出力手段が無いため、「対話ユーティリティ」は呼び出し元(通常のシェル/REPL)が`slot-value`/`car`/`cdr`で覗ける構造化データを返す関数として実装した(既存のprocess関連ビルトイン群と同じ設計方針)。**実装中に発見した既存の制約(バグ修正)**: `package-name`(milestone91)は`pkg_name`から`lisp_make_string`で毎回新規の文字列オブジェクトを作って返す実装であり、2回の呼び出し結果は内容が同じでも`eq`にならない(本処理系に`string=`が無いため、これまで`eq`比較に晒される用途では使われていなかった未発見の性質だった)。当初`os:inspect-process`の戻り値に`package-name`の文字列のみを含め、テスト側で別途呼んだ`package-name`の結果と`eq`比較する設計で実装・検証した結果、`RESULT os FAIL`で失敗し発覚した。修正として、`os:inspect-process`に生のパッケージオブジェクト自体を返す`'package`エントリを追加し(表示用の`'package-name`文字列とは役割を分離)、テストは`'package`の方を`eq`比較に使うように変更した。Lisp側テスト`run-test-os-inspect-process`(`test/lisp/test-os.lisp`)を追加し、`&optional dummy`(milestone113と同じ理由でツリーウォークへ強制フォールバックさせるため必須)、fork側パッケージへの新規関数`m114-marker-fn`定義(`%set-symbol-function`経由、ベースとの名前衝突が無い単純な新規internなので`shadow`手順は不要)、レキシカル変数`secret`(999)を捕捉したthunkを使い、`os:inspect-process`の戻り値から`package`がfork側パッケージ自体と`eq`であること、`functions`に`m114-marker-fn`の`(symbol . function)`ペアが含まれその`cdr`が`symbol-function`の戻り値と`eq`であること、`lexical-variables`に`secret`が999として含まれることを確認した(`assoc`相当の組み込みが本処理系に無いため、ローカルヘルパー`find-pair-eq`を用意した)。`make build`/`make test`(28ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |
| 115 | 関数の「差し戻し」コマンド | 完了 | fork側でshadowされた関数をベースパッケージ(`common-lisp-user`)の元の定義に戻すデバッグコマンド`os:revert-function(p name)`を`lisp/os.lisp`に実装した。114と同じく**新規Cビルトインは一切追加していない**。既存の`find-symbol`(milestone91)・`intern`・`symbol-function`・`%set-symbol-function`(milestone93)のみを組み合わせる。**設計**: `(find-symbol name fork-pkg)`のstatusが`:inherited`(fork側にローカルな別シンボルを持たず、useしている`common-lisp-user`のシンボルをそのまま共有している=そもそもshadowされておらず差し戻す対象が無い)なら`nil`を返して何もしない。`:internal`/`:external`の場合(shadow等でfork側パッケージ内にローカルに確保された別シンボルオブジェクトが実在する)は、そのシンボルの関数セルを`common-lisp-user`側の同名シンボルの関数定義で`%set-symbol-function`により上書きし、対象のシンボル自身を返す。`%set-symbol-function`は対象シンボルの**ホームパッケージ**(常にfork-pkg。`common-lisp-user`自身のシンボルではない)がロック済みでない限りブロックしない(milestone111のredefinition-onlyセマンティクス)ため、`common-lisp-user`が起動時にロックされていても無関係に通る。ベース側にその名前の関数定義が存在しない場合は、既存の`symbol-function`の仕様どおり`unbound variable`ではなく専用のpanicメッセージで停止する(変更不要、既存動作に委ねた)。Lisp側テスト`run-test-os-revert-function`(`test/lisp/test-os.lisp`)を追加し、`run-test-os-make-process-fork-redefine`(109)と同じ`in-package`→`shadow`→文字列`intern`経由の手順でfork側の`car`をshadow・再定義してから`os:revert-function`で差し戻し、(1)戻り値がfork側の`car`シンボル自身と`eq`であること、(2)差し戻し後もfork側`car`シンボルがベースの`car`シンボルとは別オブジェクトのまま(`eq`にならない)であること、(3)差し戻し後に`(funcall (symbol-function fork-car-sym) (cons 1 2))`がベースの`car`と同じ挙動(`1`)を返すこと、(4)そもそもshadowされていない名前`"cons"`を渡すと`nil`が返ること(no-opの確認)を検証した。`make build`/`make test`(28ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |

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
