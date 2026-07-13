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

// トップレベルの永続グローバル環境 (milestone 12)。EfiMainが起動時に
// lisp_builtins_init()の結果で初期化する。defun/loadなど今後の特殊形式が
// lisp_eval内部からここを直接書き換えて新しい束縛を追加すれば、その後の
// すべてのREPL入力から見えるようになる
extern LispObject global_env;

// --- 文字入力 (milestone 6) ---
extern char input_buffer[LISP_INPUT_BUFFER_MAX];
extern UINTN input_length;

void lisp_heap_init(UINT64 start, UINT64 size);
void lisp_packages_init(void);
void lisp_symbols_init(void);
LispObject lisp_builtins_init(void);
void lisp_load_boot_file(const char *filename); // milestone 29
void lisp_load_init_file(void); // milestone 47

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

// --- VMオペコード (milestone 35) ---
// 各命令は1byteのopcode+固定長の即値オペランド（今のところ0または1byte）から成る。
// 手動でバイトコード配列を構築する目標1の各マイルストン（35〜39）はこの定義を直接使う
#define OP_CONST  0   // 次の1byteをconstants配列のindexとして解釈し、その定数をpushする
#define OP_ADD    1   // スタック上位2要素をpopして加算し、結果をpushする
#define OP_RETURN 2   // スタック最上位をpopし、それを戻り値としてlisp_vm_execから返る

// --- VM制御フロー/ローカル変数オペコード (milestone 36) ---
// ジャンプ先は関数のbytecode先頭からの絶対バイト位置（1byte、0〜255）。ローカル変数は
// FP（現在の呼び出しフレームの先頭、milestone37までは実行開始時のspそのもの）相対の
// index（1byte）で指定し、実体はcons（car=値、cdr=NIL固定）をボックスとして再利用する
#define OP_JUMP           3   // 次の1byteを絶対バイト位置として解釈し、そこへ無条件にジャンプする
#define OP_JUMP_IF_FALSE  4   // スタック最上位をpopし、nilなら次の1byteの絶対位置へジャンプする。
                               // nil以外ならジャンプせず次の命令（オペランドの直後）へ進む
#define OP_LOAD_LOCAL     5   // 次の1byteをFP相対indexとして解釈し、car(vm_stack[FP+index])をpushする
#define OP_STORE_LOCAL    6   // スタック最上位をpopし、次の1byteのFP相対indexが指すボックスへ
                               // rplaca相当で書き込む（vm_stack上のスロット自体は書き換えない）
#define OP_MAKE_LOCAL     7   // スタック最上位をpopし、その場でcons(値, NIL)としてボックス化しpushし直す

// --- VM関数呼び出しオペコード (milestone 37) ---
// 呼び出し規約: 呼び出し元はargを1個目から順にpushし、最後に呼び出す関数（コンパイル済み
// 関数オブジェクト）をpushしてからOP_CALL <nargs>を発行する。OP_CALLは関数オブジェクトをpopし、
// その下にあるnargs個のスタック位置を「その場でボックス化」して新しいフレームとし、
// その関数のbytecodeを（C再帰で）実行する。戻り値は元のスタック位置に1個pushされる
#define OP_CALL 8   // 次の1byteをnargsとして解釈し、上記の呼び出し規約に従って関数を呼び出す

// --- VMクロージャ生成/upvalueオペコード (milestone 38) ---
// クロージャは「テンプレート」（コンパイル時に作った、bytecode/constants/nargs/upvalue_descsを
// 持つ共有の関数オブジェクト）と「インスタンス」（OP_MAKE_CLOSUREが実行時に作る、upvaluesだけが
// 異なる実体）に分かれる。upvalue_descsは各要素が(kind . index)のconsであるベクタで、kind=0なら
// 「クロージャ生成元フレームのFP+index」のボックスをそのまま捕捉し、kind=1なら「クロージャ生成元
// closure自身のupvalues[index]」をそのままコピーする（2階層以上の捕捉はこれでフラット化され、
// OP_LOAD_UPVALUE/OP_STORE_UPVALUEは常に自分のupvaluesだけを見ればよい）
#define OP_MAKE_CLOSURE   9   // 次の1byteをconstants配列のindexとして解釈し、そこにあるテンプレート
                               // closureから実体を1つ作ってpushする（upvalue_descsを解決してupvaluesを構築する）
#define OP_LOAD_UPVALUE  10   // 次の1byteを自分のupvaluesベクタのindexとして解釈し、
                               // car(upvalues[index])をpushする
#define OP_STORE_UPVALUE 11   // スタック最上位をpopし、次の1byteが指すupvalues[index]のボックスへ
                               // rplaca相当で書き込む

// --- VMプリミティブ最適化命令 (milestone 39) ---
// 頻出する組み込み関数を、関数呼び出し（OP_CALL）を経由せず直接VM命令として実行する。
// 目標1の最終マイルストンであり、他の全オペコードと組み合わせた総合検証を行う
#define OP_CONS 12   // スタック最上位2要素をpopし（先にpushした方をcar、後にpushした方をcdrとする）、
                      // lisp_cons(car, cdr)をpushする
#define OP_CAR  13    // スタック最上位をpopし、car()をpushする
#define OP_CDR  14    // スタック最上位をpopし、cdr()をpushする
#define OP_EQ   15    // スタック最上位2要素をpopし、ポインタ同値ならt、そうでなければnilをpushする

// bytecode(bytecode_len byte)とconstants(constants_len個のLispObject)を保持するVM
// コンパイル済み関数オブジェクトを作る（LispClosureのescape hatch方式、milestone15/22/26と
// 同じ前例）。どちらも呼び出し元のバッファをヒープへコピーするので、呼び出し後は
// 呼び出し元のバッファを保持し続ける必要はない。nargsは今のところ記録のみ（milestone37で使用）
LispObject lisp_make_compiled(const unsigned char *bytecode, UINTN bytecode_len,
                               const LispObject *constants, UINTN constants_len, UINTN nargs);

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

#endif // OS_BOOT_DEV_LISP_H
