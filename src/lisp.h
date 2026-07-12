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

#endif // OS_BOOT_DEV_LISP_H
