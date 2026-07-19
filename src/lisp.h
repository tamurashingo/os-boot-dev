#ifndef OS_BOOT_DEV_LISP_H
#define OS_BOOT_DEV_LISP_H

#include "uefi.h"

// --- Lisp Object System ---
typedef UINT64 LispObject;

#define LISP_INPUT_BUFFER_MAX 256

// panic時にConOutへ出力するためのシステムテーブル。EfiMainが起動時に設定する
extern EFI_SYSTEM_TABLE *g_system_table;

// load組み込み関数がHandleProtocolでファイルシステムを取得する際に使う。
// EfiMainが起動時に設定する (milestone 16)
extern EFI_HANDLE g_image_handle;

// トップレベルの永続グローバル環境 (milestone 12)。milestone94のLisp-2化により、
// defun/defmacro/組み込み関数はここではなく各symbolの関数セル(fn、milestone93)へ
// 書き込むようになったため、global_envは「関数namespaceと独立した、レキシカルにも
// is_specialにも属さないグローバル変数」専用の値namespace用alistとして残る
// (setqは新規束縛を暗黙に作らないため、現時点でこの経路で実際に値が入ることはない)
extern LispObject global_env;

// --- 文字入力 (milestone 6) ---
extern char input_buffer[LISP_INPUT_BUFFER_MAX];
extern UINTN input_length;

void lisp_heap_init(UINT64 start, UINT64 size);
void lisp_packages_init(void);
void lisp_symbols_init(void);
void lisp_builtins_init(void); // milestone94: 各symbolの関数セル(fn)へ直接登録するためvoid化
void lisp_load_boot_file(const char *filename); // milestone 29
void lisp_load_init_file(void); // milestone 47
void lisp_lock_cl_user_package(void); // milestone 111: 起動シーケンス末尾でcommon-lisp-userをロックする

void lisp_read_line(EFI_SYSTEM_TABLE *SystemTable);
LispObject lisp_read_from_buffer(const char *str);

LispObject lisp_eval(LispObject expr, LispObject env);
LispObject lisp_eval_toplevel(LispObject expr);

// --- 出力ストリーム (milestone 24) ---
// 関数ポインタ+contextという最小限のインターフェース。将来、文字列バッファへ
// 書き込むストリームなど別の出力先を追加する際は、writeに別実装を渡すだけで済む
typedef struct {
    void (*write)(void *ctx, const char *str);
    void *ctx;
} LispOutputStream;

LispOutputStream lisp_make_console_stream(EFI_SYSTEM_TABLE *SystemTable);

void lisp_print(LispOutputStream *stream, LispObject obj);

// --- 大脱出機構 (milestone 30) ---
// 呼び出し時のレジスタ状態を保持する。フィールド順とoffsetはsrc/lisp.cの
// アセンブリ実装と一致させる必要がある。このターゲット(x86_64-w64-mingw32-gcc、
// -mabi指定なし)はMS x64呼び出し規約がデフォルトのため、System Vとは異なりrdi/rsi
// も呼び出し先保存レジスタに含まれる（rbx/rbp/rdi/rsi/r12〜r15の8本+rsp+rip）
typedef struct {
    UINT64 rbx;
    UINT64 rbp;
    UINT64 rdi;
    UINT64 rsi;
    UINT64 rsp;
    UINT64 r12;
    UINT64 r13;
    UINT64 r14;
    UINT64 r15;
    UINT64 rip;
} lisp_jmp_buf;

// 呼び出し時のレジスタ状態をbufに保存し、直接呼び出された場合は0を返す。
// lisp_longjmpによってこの呼び出しへ脱出してきた場合はvalを返す
int lisp_setjmp(lisp_jmp_buf *buf) __attribute__((returns_twice));
// bufからレジスタ状態を復元し、対応するlisp_setjmp呼び出しへ脱出する
// (setjmpがvalを返したかのように見える)。通常のC的な意味では戻らない
void lisp_longjmp(lisp_jmp_buf *buf, int val) __attribute__((noreturn));

// --- REPLエラー復旧トラップ (milestone 31) ---
// REPLループが現在アクティブなlisp_jmp_bufを指す。NULLならトラップ未設置
// （まだREPLループに入っていない起動処理中など）を意味し、lisp_panicは
// 従来通りfor(;;){}でハングする
extern lisp_jmp_buf *lisp_active_trap;

// 固定容量資源の枯渇など、REPLに復帰しても安全に継続できない致命的エラー用。
// 常にfor(;;){}でハングし続ける（lisp_panicと違いlongjmpしない）
void lisp_panic_fatal(CHAR16 *message);

// --- per-processスタック領域とコンテキスト保存 (milestone 104) ---
// main.cのEfiMain（milestone88）が起動時に1度だけ行っているAllocatePages+rsp切替
// （呼び出し先が戻ってきたら元のrspへ復帰する「call型」の切替）を汎用化し、
// 「戻ってこない・双方向に何度も切り替えられる」コルーチン型のコンテキストへ拡張したもの。
// lisp_jmp_buf自体（rbx/rbp/rdi/rsi/rsp/r12-r15/rip）を「今どこで止まっているか」の
// 保存領域としてそのまま再利用する。setjmp/longjmpは同一スタック上の巻き戻りにしか
// 使えないが、rspフィールドを未開始プロセス用に手動で偽装した値へ書き換えてから
// longjmpすることで、そのプロセス専用の別スタック上で新規に実行を開始させられる
// （ucontext等が無い環境での一般的なコルーチン実装手法）。
//
// stack_base/stack_pagesはAllocatePagesで確保した領域そのもの（milestone107以降のGCルート
// 拡張・将来のプロセス終了時解放で使う、この段階では記録のみ）。pending_entry/pending_argは
// このコンテキストへの最初のlisp_context_switch呼び出しで一度だけ使われるトランポリン引数。
// startedは既に一度でも実行開始済みかを示す（2回目以降のswitchは偽装rip/rspを使わず、
// 前回lisp_context_switchが保存した本物の再開点を使う）。
//
// vm_stack/vm_sp/active_trapはmilestone105で追加した「CPUレジスタ以外のper-processレジスタ」
// (VMのデータスタック本体・現在使用中の深さ・panic復帰先トラップ)の退避領域。lisp.c側の同名の
// グローバル(vm_stack/vm_sp/lisp_active_trap)は常に「今実際に実行中のプロセス」のものを指す
// 単一の作業領域のままとし、lisp_context_switchがそことの間でコピー/入替を行う（lisp_vm_run
// 等の既存コード自体は無変更。VM_STACK_SIZEはlisp_vm_push等と共有するため.cから.hへ移した）
#define VM_STACK_SIZE 1024

typedef struct LispProcessStack {
    lisp_jmp_buf regs;
    EFI_PHYSICAL_ADDRESS stack_base;
    UINTN stack_pages;
    void (*pending_entry)(void *);
    void *pending_arg;
    int started;
    LispObject vm_stack[VM_STACK_SIZE];
    UINTN vm_sp;
    lisp_jmp_buf *active_trap;
    // milestone112: このコンテキストをlisp_context_switchのtoとして呼び出した側（=resume時に
    // 実行中だったコンテキスト）。process-suspendが「戻る先」を、process-resumeが「finished後に
    // 戻る先」を判断するために使う。lisp_process_stack_create直後はNULL（resume時に設定される）
    struct LispProcessStack *resumer;
} LispProcessStack;

// stack_pages*4KBの新規スタック領域をAllocatePages経由で確保し、outをそのスタック上で
// entry(arg)を開始する「未開始」コンテキストとして初期化する（まだ実行は始まらない。
// 実際に開始するのは最初にこのコンテキストをtoとしてlisp_context_switchを呼んだ時）。
// vm_sp/active_trapは0/NULLで初期化する（空のVMデータスタック・トラップ未設置の状態で開始する）。
// 確保に失敗した場合はlisp_panic_fatalする
void lisp_process_stack_create(LispProcessStack *out, UINTN stack_pages,
                                void (*entry)(void *), void *arg);

// 現在の実行位置をfromへ保存し、toへ切り替える。toが未開始（lisp_process_stack_create
// 直後でまだ一度もlisp_context_switchのtoになっていない）なら、そのスタック上でentry(arg)
// が新規に開始される。既に開始済みなら、toが最後に他のプロセスへlisp_context_switchで
// 制御を渡した地点（このtoをfromとして呼んだ直後）から再開する。
// fromは呼び出し元が保持している既存のLispProcessStack（起動直後の「メイン」コンテキスト
// の場合はstack_base/stack_pagesは未使用のまま、regs等が後続の切り替えで使われる）を指す。
// CPUレジスタ切替(lisp_setjmp/lisp_longjmp)に加え、milestone105でvm_stack[0..vm_sp)・
// active_trapもfrom/to間でコピー/入替する（現在実行中のグローバルvm_stack/vm_sp/
// lisp_active_trapをfromへ退避したうえで、toに保存されていた値をそこへ復元する）
void lisp_context_switch(LispProcessStack *from, LispProcessStack *to);

// mainコンテキストと新規に確保した別スタック上のコンテキストの間を3往復し、両側で
// カウンタが期待どおりに増えることを確認する自己テスト。真なら成功
int lisp_context_switch_selftest(void);

// --- per-process vm_stack/vm_sp/lisp_active_trap分離自己テスト (milestone 105) ---
// mainが積んだ値・設置したトラップが別スタック上のコンテキストの開始直後に一切見えない
// こと、逆にそのコンテキストが自分専用に積んだ値・設置したトラップがmain側の実行によって
// 書き換えられないこと、再開後も自分の状態がそのまま残っていることを確認する。真なら成功
int lisp_process_vm_state_selftest(void);

// --- コルーチンyieldチェック (milestone 106) ---
// lisp_vm_run（コンパイル済みbytecodeディスパッチループ）が毎命令ディスパッチ毎に安価に
// チェックできる「他プロセスからの中断要求」フック。lisp_vm_current_process/
// lisp_vm_yield_targetの両方が非NULLの時のみ「武装」され、武装中はlisp_vm_yield_budgetが
// 1減るごとに0へ達した時点でlisp_vm_current_processからlisp_vm_yield_targetへ
// lisp_context_switchする（milestone104/105のvm_stack/vm_sp/lisp_active_trap退避へ
// そのまま相乗りするため、切替後に呼び出し元がbudgetを再設定してから改めて
// lisp_vm_yield_targetからlisp_vm_current_processへlisp_context_switchすれば、
// pc・オペランドスタック・ローカル変数を含む実行状態を一切壊さず続きから再開できる）。
// デフォルトはどちらもNULL・budgetも実質無制限であり、明示的に武装しない限り既存の
// ブート・REPL・全self-test/test-lisp fixtureの挙動は一切変化しない。
//
// スコープ: lisp_vm_run（コンパイル済みbytecode経路）内のみでチェックする。ツリーウォーク
// 経路（lisp_eval/lisp_apply、defmacro本体・rest-arg形式defunで使われる）はyieldチェック
// 対象外（真のプリエンプションを導入しない限り、この経路上の無限ループはyield不可能なまま
// 残る既知の制約。documents/lisp_os_process.mdマイルストーン106に明記する）
extern LispProcessStack *lisp_vm_current_process;
extern LispProcessStack *lisp_vm_yield_target;
extern UINTN lisp_vm_yield_budget;

// lisp_vm_current_process/lisp_vm_yield_targetを武装したうえで、main（呼び出し元）とは
// 別スタック上のコンテキストでVM bytecode（0からTARGETまで1ずつ数え上げるループ）を実行させ、
// budgetの小さいquantumを使って複数回、命令ディスパッチの「途中」で実際にyield・resumeさせる。
// 1回の切替では完了しないこと（真に複数回中断・再開されたこと）と、最終的にbytecodeが
// 正しい結果まで数え上げを完了すること（pc・オペランドスタック・ローカル変数がyieldを跨いで
// 正しく保持されること）の両方を確認する。真なら成功
int lisp_vm_yield_selftest(void);

// --- 全プロセスGCルート登録 (milestone 107) ---
// lisp_process_stack_create直後のLispProcessStackはまだ「ただの構造体」で、GCからは
// 一切辿れない（vm_stack/vm_spはmilestone105でプロセス毎に分離済みだが、lisp_gc_mark_rootsは
// 「今実際に実行中」のグローバルvm_stack/vm_spしか見ない）。他プロセスへlisp_context_switchで
// 制御を渡し自分が中断される（=自分のvm_stack/vm_spがLispProcessStack構造体側へ退避される）
// と、そのプロセスの操作対象オブジェクトはCローカル変数からもグローバルvm_stackからも
// 到達不能になり、次のGCで回収されてしまう。lisp_gc_extra_root（一時的なCローカル退避先を
// GCルートに加える既存の手動登録パターン）と同じ考え方で、「今後GCルートとして走査してほしい
// LispProcessStack」を明示的に登録・解除できるようにする。
//
// GC発火条件について: 現状GCが実際に発火するのはlisp_heap_low()を見るREPLループ先頭、または
// (gc)組み込み関数の呼び出し時のみであり、いずれも「今実行中の唯一のプロセス」自身の安全地点
// でしか呼ばれない。他の全プロセスは（milestone106のyieldチェックが命令ディスパッチの
// 先頭でのみ発生するため）中断中は常に「命令の境界」で止まっており、そのLispProcessStack.
// vm_stack[0..vm_sp)は常に安全なスナップショットである。したがって「全プロセスが安全点にいる
// 時のみGCを発火する」という要求は、新たな判定コードを追加しなくても現在のyieldチェック設計
// によって既に満たされている（安全点以外で中断する経路が存在しない）
void lisp_process_stack_register(LispProcessStack *ps);
void lisp_process_stack_unregister(LispProcessStack *ps);

// 別スタック上で中断中のプロセスのvm_stackが、lisp_process_stack_registerを通じて
// lisp_gc_mark_rootsのルート集合に正しく含まれているかを検証する自己テスト
// (milestone34のlisp_vm_gc_root_selftestの複数プロセス版)。新規プロセスbを開始し、b専用の
// vm_stackにのみ積んだconsをbからmainへ切り替えて中断させた後、mainからlisp_gc()を実行し、
// さらに複数のconsを確保してフリーリストの再利用を強制する。ルート統合が正しければbの
// vm_stackに残ったconsの内容は上書きされず、誤っていれば再利用時に上書きされ検出できる。
// 真なら成功
int lisp_process_gc_root_selftest(void);

// --- fork時の一意パッケージ生成 (milestone 108) ---
// os:make-process(実体はCビルトイン%make-process)が、プロセス生成と同時に一意名の隔離
// パッケージを作成し、common-lisp-userをuse-packageした上でprocessインスタンスのpackage
// スロットへ格納するようになったことを検証する自己テスト。2回os:make-process相当を実行して
// 得られた2つのプロセスのpackageスロットが(1)いずれも非nilで(2)互いにeqでない別オブジェクトで
// あること、(3)そのpackageがcommon-lisp-userをuse-packageしていること(pkg_usesにeqで含まれる
// こと)、(4)その結果fork先パッケージ内で無修飾に"car"をinternしてもcommon-lisp-user内の
// "car"シンボルと同一オブジェクト(eq)に解決されること(ベースパッケージへの委譲が実際に
// 機能していること)を確認する。真なら成功
int lisp_process_fork_package_selftest(void);

// --- process-suspend/process-resume (milestone 112) ---
// os:processインスタンス（stackframe/statusスロット）とフェーズCのlisp_context_switch機構を
// 結び付け、指定したプロセスの実行を実際に一時停止・再開する。lisp/os.lispのos:process-resume/
// os:process-suspendラッパーから呼ばれるCビルトイン%process-resume/%process-suspendの実体は
// src/lisp.cにある（詳細な設計判断はdocuments/lisp_os_process.mdマイルストーン112参照）。
//
// %process-resumeがos:process未起動（stackframeスロットがnil）のプロセスに対して呼ばれた場合、
// 第2引数（0引数のLisp関数）を新規に確保したper-processコンテキスト上でlisp_apply経由で開始する
// （このコンテキストが最初に呼んだprocess-resumeの呼び出し元へ「resumer」として記録される）。
// 起動済み（stackframeスロットがfixnumのプールindex）のプロセスに対して呼ばれた場合は、直前の
// process-suspendの中断点から再開する。いずれの場合も呼び出し元は自分自身のコンテキストへ
// 戻ってくるまでブロックする（lisp_context_switchが「戻ってくる」まで、という既存の意味論どおり）。
//
// %process-suspendは「今実際に実行中のコンテキスト＝引数のプロセス自身」の場合のみ許可される
// （自分自身をsuspendする、という設計。他プロセスを外部から強制停止する機能ではない。単一
// 実行コンテキストの協調的切替という既存のスコープ外項目と整合させるための意図的な制約）。
// resumerフィールドへ記録済みの「自分をresumeした側」へlisp_context_switchで戻る。
//
// 単一実行コンテキストの協調的切替(スコープ外項目)である以上、あるプロセスが:activeの間は
// 他の誰も実行されていない。従って%process-resumeの呼び出し元がその呼び出しから戻ってきた
// 時点で観測できるstatusは常に:suspendedか:finishedのいずれかであり、:activeを外部から観測する
// ことは原理的にできない(自分自身のstatusを自分の実行中に読む場合を除く)。
//
// 既知の制約: milestone106のyieldチェックはコンパイル済みbytecode経路(lisp_vm_run)のみを
// 対象にしており、ツリーウォーク経路(lisp_eval/lisp_apply)のC呼び出しスタック上に生きている
// LispObjectのC局所変数はmilestone107のvm_stack拡張同様のGCルートとして辿られない。
// process-suspendはツリーウォーク経路の途中（lisp_apply呼び出しの奥）からも呼び出せるため、
// 中断中に他プロセスが(gc)を誘発すると、この未追跡のC局所変数が指すオブジェクトが理論上
// 回収され得るという制約が残る（本マイルストーンではこの解消は行わず、既知のリスクとして
// documents/lisp_os_process.mdに明記するのみとする）。
//
// 自己テスト: mainからos:make-process相当の空プロセスをC内で直接生成し、(1)%process-resumeで
// 0引数の閉包（ダイナミック変数をインクリメントしてから自分自身に対し%process-suspendを1回・
// さらに2回目のインクリメント後に返る、という単純な閉包）を開始し1回目の%process-suspend
// 直後まで実行が進むこと、(2)その時点でprocess-resumeの呼び出し元へ制御が戻ってきており
// statusスロットが:suspendedであること、(3)再度%process-resumeで中断点から再開し2回目の
// インクリメント後に閉包が正常に戻りstatusスロットが:finishedになること、(4)2回のインクリメント
// の結果が期待どおりであることを確認する。真なら成功
int lisp_process_suspend_resume_selftest(void);

// --- process-local-variable自己テスト (milestone 113) ---
// let内側で生成したlambdaクロージャのenvがprocessのenvスロットへ捕捉されること、
// %process-local-variableでそのレキシカル変数の値を外部から読み取れることを確認する。
// 真なら成功
int lisp_process_local_variable_selftest(void);

// --- マーク＆スイープGC (milestone 33) ---
// ヒープのバンプ側残り容量が総量の20%未満なら真を返す。EfiMainのREPLループが
// 毎ループ先頭でこれを見て、真の場合のみlisp_gc()を呼ぶ（評価中には呼ばない——
// Cコールスタック上の一時参照を正確に追跡できないため、安全地点はREPLループ先頭に限る）
int lisp_heap_low(void);

// マーク＆スイープGCを1回実行し、回収したオブジェクト数を返す。安全地点（REPLループ先頭、
// または(gc)組み込み関数からの明示的な呼び出し）以外では呼ばないこと
UINTN lisp_gc(void);

// --- スタックマシン型VM (milestone 34) ---

// vm_stack（VMのデータスタック、lisp.c内のstaticなグローバル配列）がlisp_gc_mark_rootsの
// ルート集合に正しく含まれているかを検証する自己テスト。新規consをvm_stackへ積んだ状態でGCを
// 実行し、その後さらに複数のconsを確保してフリーリストの再利用を強制する。ルート統合が
// 正しければ元のconsの内容は上書きされず、誤っていれば再利用時に上書きされ検出できる。
// 真なら成功
int lisp_vm_gc_root_selftest(void);

// vm_sp（VMのデータスタックポインタ、lisp.c内のstaticなグローバル変数）をゼロへ戻す。
// panicのlongjmpはこのスタックを復元しないため、REPLのpanic復帰点（lisp_setjmpの
// トラップ復帰直後）で必ず呼ぶこと（milestone 48）
void lisp_vm_reset_stack(void);

// --- パッケージシステム再設計 (milestone 68〜) ---

// milestone 74: リーダーの"pkg:sym"/"pkg::sym"修飾子構文を検証する自己テスト。
// export/use-packageのLisp APIがまだ無いため、lisp_make_package/lisp_intern_in_packageの
// ようなC内部APIで専用の使い捨てパッケージとexportリストを直接組み立てて確認する。
// 真なら成功
int lisp_reader_package_qualifier_selftest(void);

// milestone 76: Lisp呼び出し可能なexportビルトインと#74のリーダー修飾子を組み合わせた自己テスト。
// 「exportを評価した後にpkg:symを読む」という順序をtest/lisp/配下のファイル(load経由)では組めない
// ため（milestone72の既知の制約と同根）、C内で直接呼び出し順序を制御して検証する。真なら成功
int lisp_reader_export_selftest(void);

// milestone 77: Lisp呼び出し可能なuse-packageビルトインと、lisp_intern_in_packageの
// use-list探索拡張（自パッケージのローカルシンボルに無ければuseしている各パッケージの
// exportシンボルを探す）を組み合わせた自己テスト。「use-packageを評価した後に無修飾名を
// internして解決する」という順序をtest/lisp/配下のファイル(load経由)では組めないため
// （milestone76と同根の制約）、C内で直接呼び出し順序を制御して検証する。真なら成功
int lisp_reader_use_package_selftest(void);

// milestone 78: Lisp呼び出し可能なintern・in-packageビルトインと、lisp/stdlib.lispの
// defpackageマクロを組み合わせた自己テスト。「in-packageで*package*を切り替えた後に無修飾名を
// internして解決する」という順序をtest/lisp/配下のファイル(load経由)では組めないため
// （milestone76/77と同根の制約）、C内で直接呼び出し順序を制御して検証する。defpackageマクロは
// stdlib.lisp読込済み（lisp_compiler_ready後）が前提のため、main.cでのstdlib.lisp自動load後に
// 呼び出す必要がある。真なら成功
int lisp_reader_defpackage_selftest(void);

// milestone 80: EfiMainの起動順序が「lisp_packages_initが*package*をcommon-lisp-userへ
// seedし終えてからlisp_symbols_initが特殊形式シンボルをinternする」という前提を満たしている
// こと、およびcompiler.lisp/stdlib.lisp読込後もそれらのシンボルがcommon-lisp-userへ帰属し
// 続けていることを確認する自己テスト。main.cでのstdlib.lisp自動load後に呼び出す必要がある。
// 真なら成功
int lisp_bootstrap_package_context_selftest(void);

// milestone 81: OP_GLOBAL_REF/OP_GLOBAL_SETがglobal_envをシンボルのeq同一性で解決する前提が
// *package*導入後も壊れていないことを確認する自己テスト。同一パッケージ内でのdefun前方参照・
// 相互再帰、in-packageを挟んだ同一名再解決のシンボル同一性、*package*が非既定値の最中の
// lisp_gc()安全性を検証する。main.cでのstdlib.lisp自動load後に呼び出す必要がある。真なら成功
int lisp_global_ref_package_identity_selftest(void);

// milestone 100: lisp_symbols_init末尾でcommon-lisp-userからexportした特殊形式トークン
// （defun/if/let等の特殊形式ディスパッチシンボル・tの自己評価トークン・&optional等の
// ラムダリストキーワード）が、*package*を切り替えてuse-package済みの別パッケージからでも
// 無修飾で正しく解決され、実際に特殊形式として機能することを確認する自己テスト。
// main.cでのstdlib.lisp自動load後に呼び出す必要がある。真なら成功
int lisp_reader_special_form_export_selftest(void);

// milestone 101: lisp_builtins_initで登録した全ビルトイン関数（LISP_REGISTER_BUILTIN経由の
// ものと、それを経由しないprint-object・*macroexpand-hook*）が、*package*を切り替えて
// use-package済みの別パッケージからでも無修飾で正しく解決され、実際に呼び出せることを確認する
// 自己テスト。in-package自身が無修飾で呼べる（milestone79の制約解消）ことも合わせて確認する。
// main.cでのstdlib.lisp自動load後に呼び出す必要がある。真なら成功
int lisp_reader_builtin_export_selftest(void);

// --- VMオペコード (milestone 35) ---
// 各命令は1byteのopcode+固定長の即値オペランド（今のところ0または2byte、リトルエンディアン）
// から成る。手動でバイトコード配列を構築する目標1の各マイルストン（35〜39）はこの定義を直接使う
// （milestone60: オペランド幅は元々1byteだったが、defunを既定でコンパイルするようになった結果
// 実際のバイトコード長・定数indexが255を超える関数が現れたため2byteへ拡張した）
#define OP_CONST  0   // 次の2byte（リトルエンディアン）をconstants配列のindexとして解釈し、その定数をpushする
#define OP_ADD    1   // スタック上位2要素をpopして加算し、結果をpushする
#define OP_RETURN 2   // スタック最上位をpopし、それを戻り値としてlisp_vm_execから返る

// --- VM制御フロー/ローカル変数オペコード (milestone 36) ---
// ジャンプ先は関数のbytecode先頭からの絶対バイト位置（2byte、0〜65535）。ローカル変数は
// FP（現在の呼び出しフレームの先頭、milestone37までは実行開始時のspそのもの）相対の
// index（2byte）で指定し、実体はcons（car=値、cdr=NIL固定）をボックスとして再利用する
#define OP_JUMP           3   // 次の2byteを絶対バイト位置として解釈し、そこへ無条件にジャンプする
#define OP_JUMP_IF_FALSE  4   // スタック最上位をpopし、nilなら次の2byteの絶対位置へジャンプする。
                               // nil以外ならジャンプせず次の命令（オペランドの直後）へ進む
#define OP_LOAD_LOCAL     5   // 次の2byteをFP相対indexとして解釈し、car(vm_stack[FP+index])をpushする
#define OP_STORE_LOCAL    6   // スタック最上位をpopし、次の2byteのFP相対indexが指すボックスへ
                               // rplaca相当で書き込む（vm_stack上のスロット自体は書き換えない）
#define OP_MAKE_LOCAL     7   // milestone83/84: 次の2byteをFP相対indexとして解釈する。スタック最上位を
                               // popし、その場でcons(値, NIL)としてボックス化して、呼び出し時に確保済みの
                               // 固定スロットvm_stack[FP+index]へ直接書き込む（pushし直さない。呼び出し
                               // 元がvm_stack[FP..FP+max_locals)を丸ごと確保しておく前提のため、ローカル
                               // 変数領域とその後の一時値用データスタック領域が分離される）

// --- VM関数呼び出しオペコード (milestone 37) ---
// 呼び出し規約: 呼び出し元はargを1個目から順にpushし、最後に呼び出す関数（コンパイル済み
// 関数オブジェクト）をpushしてからOP_CALL <nargs>を発行する。OP_CALLは関数オブジェクトをpopし、
// その下にあるnargs個のスタック位置を「その場でボックス化」して新しいフレームとし、
// その関数のbytecodeを（C再帰で）実行する。戻り値は元のスタック位置に1個pushされる
#define OP_CALL 8   // 次の2byteをnargsとして解釈し、上記の呼び出し規約に従って関数を呼び出す

// --- VMクロージャ生成/upvalueオペコード (milestone 38) ---
// クロージャは「テンプレート」（コンパイル時に作った、bytecode/constants/nargs/upvalue_descsを
// 持つ共有の関数オブジェクト）と「インスタンス」（OP_MAKE_CLOSUREが実行時に作る、upvaluesだけが
// 異なる実体）に分かれる。upvalue_descsは各要素が(kind . index)のconsであるベクタで、kind=0なら
// 「クロージャ生成元フレームのFP+index」のボックスをそのまま捕捉し、kind=1なら「クロージャ生成元
// closure自身のupvalues[index]」をそのままコピーする（2階層以上の捕捉はこれでフラット化され、
// OP_LOAD_UPVALUE/OP_STORE_UPVALUEは常に自分のupvaluesだけを見ればよい）
#define OP_MAKE_CLOSURE   9   // 次の2byteをconstants配列のindexとして解釈し、そこにあるテンプレート
                               // closureから実体を1つ作ってpushする（upvalue_descsを解決してupvaluesを構築する）
#define OP_LOAD_UPVALUE  10   // 次の2byteを自分のupvaluesベクタのindexとして解釈し、
                               // car(upvalues[index])をpushする
#define OP_STORE_UPVALUE 11   // スタック最上位をpopし、次の2byteが指すupvalues[index]のボックスへ
                               // rplaca相当で書き込む

// --- VMプリミティブ最適化命令 (milestone 39) ---
// 頻出する組み込み関数を、関数呼び出し（OP_CALL）を経由せず直接VM命令として実行する。
// 目標1の最終マイルストンであり、他の全オペコードと組み合わせた総合検証を行う
#define OP_CONS 12   // スタック最上位2要素をpopし（先にpushした方をcar、後にpushした方をcdrとする）、
                      // lisp_cons(car, cdr)をpushする
#define OP_CAR  13    // スタック最上位をpopし、car()をpushする
#define OP_CDR  14    // スタック最上位をpopし、cdr()をpushする
#define OP_EQ   15    // スタック最上位2要素をpopし、ポインタ同値ならt、そうでなければnilをpushする

// --- グローバル参照オペコード (milestone 51) ---
// レキシカルスコープ外のシンボル（グローバル変数・グローバル関数）を実行時にglobal_envへ
// 問い合わせる。コンパイル時に静的な位置へは解決しない（defun同士の前方参照・相互再帰を
// 破壊しないため、既存のlisp_env_lookup/lisp_env_setと同じ「毎回global_envを捜査する」
// 方式をそのまま踏襲する）
#define OP_GLOBAL_REF 16   // 次の2byteをconstants配列のindexとして解釈し、そこにあるsymbolを
                            // lisp_env_lookup(global_env, symbol)へ渡した結果をpushする
                            // （束縛が無ければlisp_env_lookupと同じくunbound variableでpanicする）
#define OP_GLOBAL_SET 17   // スタック最上位をpopし、次の2byteが指すconstants配列のsymbolへ
                            // lisp_env_set(global_env, symbol, value)相当で書き込む
                            // （OP_STORE_LOCAL/OP_STORE_UPVALUEと同様、値はpushし直さない）

// --- block/return-from（非局所脱出）オペコード (milestone 55) ---
// if/let/lambdaへの脱糖では表現できないため、既存インタプリタのlisp_return_tag/
// lisp_return_value（milestone19、src/lisp.c）をVMからもそのまま共有して使う。
// blockの本体は（引数無しの）独立したclosureとしてコンパイルし、OP_BLOCKがそれを
// 直接呼び出す（C再帰でネストしたlisp_vm_run呼び出しになる）。OP_CALL側にも、
// ネストした呼び出しの戻り値を見る前にlisp_return_tagが立っていないか確認し、
// 立っていれば（このblockの担当タグでなければ）自分のフレームもそのまま
// 早期returnして上位へ伝播する変更が入っている（lisp_apply経由でツリーウォーク
// インタプリタ側へ委譲したケースも同じチェックで統一的に扱える）
#define OP_BLOCK 18        // 次の2byteをconstants配列のindexとして解釈し、そこにあるsymbolを
                            // このblockのタグとする。スタック最上位（直前のOP_MAKE_CLOSUREが
                            // 積んだ、本体を表す引数無しclosure）をpopして0引数で呼び出す。
                            // 戻ってきた時点でlisp_return_tagがこのタグと一致していれば
                            // シグナルを捕捉し（tagをクリアし）lisp_return_valueをpushする。
                            // 一致しない他タグ宛のシグナルが立っていれば捕捉せずそのまま
                            // このフレームも早期returnして伝播する。シグナルが立っていなければ
                            // 本体の戻り値をそのままpushする
#define OP_RETURN_FROM 19  // スタック最上位をpopし、それを値としてlisp_return_valueへ、
                            // 次の2byteが指すconstants配列のsymbolをlisp_return_tagへセットした上で、
                            // OP_RETURNと同様にこのフレームを早期returnする（対応するOP_BLOCKに
                            // 出会うまで、途中のOP_CALL/OP_BLOCKフレームは戻り値を見る前に
                            // lisp_return_tagを確認し、そのまま自分も早期returnして伝播し続ける）

// --- スタック最上位を捨てる命令 (milestone 78) ---
// (progn e1 e2)のe1のように、値を評価する必要はあるが結果は使わない式のためのオペコード。
// OP_MAKE_LOCAL（let）を代わりに使うとその場でボックス化されスタック上に永続的に残り続ける
// （対応する解放命令が存在しないため）。let/progn/and/or/cond/when/unlessがtail位置にある
// 限りはOP_RETURNがフレーム全体を捨てるので無害だが、関数呼び出しの引数のようなnon-tail位置で
// 使うと後続の計算（OP_CALLのnargsが期待するスタック深さ等）を壊してしまう。OP_POPは値を
// スタックに残さず単純に捨てるため、この問題を起こさない
#define OP_POP 20   // スタック最上位をpopし、捨てる（pushし直さない）

// --- 関数namespace参照オペコード (milestone 94、Lisp-2化) ---
// 呼び出し位置のbare symbolおよび#'foo/(function foo)は、レキシカルスコープ・global_env
// のどちらも経由せずsymbolの関数セル(fn、milestone93)のみを見る（この処理系にflet/labels
// 相当が無いため、関数の局所束縛という概念自体が無い）。書き込み版(OP_GLOBAL_SET相当)は
// 不要（コンパイル済みコードから関数セルへ書き込む経路が存在しないため）
#define OP_GLOBAL_FUNCTION_REF 21  // 次の2byteをconstants配列のindexとして解釈し、そこにある
                            // symbolの関数セル(fn)をpushする（未束縛(LISP_NIL)ならunbound
                            // functionでpanicする）

// bytecode(bytecode_len byte)とconstants(constants_len個のLispObject)を保持するVM
// コンパイル済み関数オブジェクトを作る（LispClosureのescape hatch方式、milestone15/22/26と
// 同じ前例）。どちらも呼び出し元のバッファをヒープへコピーするので、呼び出し後は
// 呼び出し元のバッファを保持し続ける必要はない。nargsは今のところ記録のみ（milestone37で使用）。
// max_locals（milestone83/84）はnargs以上でなければならない（仮引数がスロット0..nargs-1を占め、
// let等が続くスロットを積み増していくため）。呼び出し元はlisp_vm_run前にvm_stack[fp..fp+max_locals)
// を丸ごと確保する（lisp_vm_reserve_frame参照）
LispObject lisp_make_compiled(const unsigned char *bytecode, UINTN bytecode_len,
                               const LispObject *constants, UINTN constants_len, UINTN nargs,
                               UINTN max_locals);

// fn（lisp_make_compiledで作ったコンパイル済み関数）のbytecodeをvm_stack上で実行し、
// OP_RETURNで返された値を返す
LispObject lisp_vm_exec(LispObject fn);

// kinds[i]/indices[i]（kindは0=ローカル捕捉、1=upvalue伝播。OP_MAKE_CLOSUREの説明参照）から
// upvalue_descsベクタを構築する（milestone38）。テンプレートclosureへ
// lisp_compiled_set_upvalue_descsで設定して使う
LispObject lisp_make_upvalue_descs(const UINTN *kinds, const UINTN *indices, UINTN count);

// lisp_make_compiledで作ったテンプレートclosure（fn）にupvalue_descsを後付けで設定する
// （milestone38）。lisp_make_compiledは呼び出し時点ではupvalue_descsを受け取らないため分離している
void lisp_compiled_set_upvalue_descs(LispObject fn, LispObject descs);

// --- コンソール入力拡張 (milestone 116) ---
//
// EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOLをLocateProtocol（システム全体から検索。既存の
// HandleProtocolはEfiMainのImageHandleから辿れるハンドルにしか使えないため不十分）で
// 取得し、g_text_input_exへ格納する。見つからなかった場合はpanicせずg_text_input_exを
// NULLのままにする（ファームウェア実装依存で存在しない可能性があるため、Ctrl検知機能
// （milestone117〜118）を使わない起動経路には影響させない）。g_system_tableが設定済み
// （main.c起動シーケンス中）である必要がある
void lisp_input_ex_init(void);

// 見つかったEFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL（NULLなら未検出）
extern EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *g_text_input_ex;

// key_dataが「修飾キー(Ctrl)単体の押下」を表しているかを判定する。真になる条件は
// (1)KeyState.KeyShiftStateのEFI_SHIFT_STATE_VALIDビットが立っている（ファームウェアが
// 修飾キー状態を報告できている）、(2)EFI_LEFT_CONTROL_PRESSED/EFI_RIGHT_CONTROL_PRESSEDの
// いずれかが立っている、(3)Ctrl以外の修飾ビット（Shift/Alt/Logo/Menu/SysReq）が一切
// 立っていない、(4)Key.ScanCode/Key.UnicodeCharがいずれも0（Ctrlに伴う実際の文字キーが
// 無い、＝Ctrl単体のキーストローク）の4条件すべてを満たす場合のみ。文字キーとの
// 組み合わせ押下（例: Ctrl+A）はScanCode/UnicodeCharが非0になるため対象外
int lisp_key_state_is_lone_ctrl(const EFI_KEY_DATA *key_data);

// lisp_key_state_is_lone_ctrlの判定ロジックを、実機・ファームウェアのキー入力に依存しない
// 手組みのEFI_KEY_DATA値で検証する自己テスト。真なら成功
int lisp_key_state_selftest(void);

// --- Ctrl2回連続押下判定 (milestone 117) ---
//
// lisp_wait_for_double_ctrlのWaitForEventループが「どちらのイベントが発火したか
// (fired_index。鍵イベント側の配列添字がLISP_CTRL_WAIT_KEY_INDEX、タイマー側が
// LISP_CTRL_WAIT_TIMER_INDEX)」と「鍵イベントだった場合その内容」から、タイムアウト
// (2回目が来る前にウィンドウが尽きた)／マッチ(Ctrl単体の2回目)／無視して待ち続ける
// (Ctrl以外の鍵イベント)のいずれかへ分類する。実際のUEFI呼び出しに依存しない部分だけを
// 切り出しており、単独で自己テスト可能
#define LISP_CTRL_WAIT_KEY_INDEX 0
#define LISP_CTRL_WAIT_TIMER_INDEX 1

typedef enum {
    LISP_CTRL_WAIT_OUTCOME_MATCH,
    LISP_CTRL_WAIT_OUTCOME_DISCARD,
    LISP_CTRL_WAIT_OUTCOME_TIMEOUT
} LispCtrlWaitOutcome;

LispCtrlWaitOutcome lisp_ctrl_wait_classify(UINTN fired_index, const EFI_KEY_DATA *key_data);

// lisp_ctrl_wait_classifyを、実機・ファームウェアのキー入力に依存しない手組みの
// (fired_index, EFI_KEY_DATA)組み合わせで検証する自己テスト。真なら成功
int lisp_ctrl_wait_classify_selftest(void);

// g_text_input_exのWaitForKeyExイベントとCreateEvent(EVT_TIMER)/SetTimer(TimerRelative)
// で作った使い捨てタイマーイベントをWaitForEventへ同時に渡し(lisp_builtin_sleep、
// milestone25と同型のCreateEvent/SetTimer/WaitForEvent/CloseEventパターンを複数イベント
// 同時待ちへ拡張したもの)、1回目のCtrl単体押下を無期限に待った後、2回目のCtrl単体押下が
// window_100ns(UEFIネイティブ単位=100ns)以内に来たかを判定する。真(1)なら2回目が
// ウィンドウ内に来た、偽(0)ならタイムアウトした、またはg_text_input_exが未検出(NULL)で
// そもそも判定不能。実機のキー入力配信に依存するため、make testのヘッドレスQEMU環境では
// 呼び出されない(milestone116同様の既知の限界)
int lisp_wait_for_double_ctrl(UINT64 window_100ns);

#endif // OS_BOOT_DEV_LISP_H
