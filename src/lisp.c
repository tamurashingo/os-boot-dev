#include "lisp.h"

// --- Lisp Object System ---

#define LISP_TAG_MASK    0x3ULL
#define LISP_TAG_CONS    0x0ULL   // ポインタ（cons cellへの16byte境界アドレス、下位2bit=00）
#define LISP_TAG_FIXNUM  0x1ULL   // 整数（上位62bitに値、下位2bit=01）
#define LISP_TAG_SYMBOL  0x2ULL   // ポインタ（LispSymbolへの16byte境界アドレス、下位2bit=10）
#define LISP_TAG_CLOSURE 0x3ULL   // ポインタ（LispClosureへの16byte境界アドレス、下位2bit=11）

// 予約アドレス0。ヒープはPhysicalStart==0を使わないため衝突しない。
// nilはシンボルとしてintern済みテーブルに載せず、この専用の即値のまま扱う
// （空リストの終端とfalse相当を1つの値で表す、という一般的なLisp実装の簡略化）
#define LISP_NIL ((LispObject)0)

#define LISP_SYMBOL_NAME_MAX 64
#define LISP_PACKAGE_NAME_MAX 32
#define LISP_MAX_PACKAGES 8
#define LISP_MAX_SYMBOLS 256

// milestone 32: gc_next/gc_markedは3構造体(LispCons/LispSymbol/LispClosure)共通で
// 先頭2フィールドとして揃える。lisp_alloc_trackedがオフセット0/sizeof(LispObject)を
// 型を問わず直接読み書きするため、この順序・型を崩さないこと
typedef struct {
    LispObject gc_next;  // 確保順の全オブジェクト追跡リストへの次ポインタ
    int gc_marked;        // milestone33のmark&sweep用mark bit（本milestoneでは常に0）
    LispObject car;
    LispObject cdr;
} LispCons;

// milestone 23: パッケージ（common-lisp-user/keyword等）ごとに独立したシンボルテーブルを持つ
typedef struct {
    char name[LISP_PACKAGE_NAME_MAX];
    LispObject symbols[LISP_MAX_SYMBOLS];
    UINTN symbol_count;
    int is_keyword_package; // 真なら自己評価し印字時に":"を前置する特別なパッケージ
} LispPackage;

typedef struct {
    LispObject gc_next;   // milestone 32: LispConsと同じ追跡リスト用（先頭2フィールド揃え必須）
    int gc_marked;
    char name[LISP_SYMBOL_NAME_MAX];
    int is_special;      // milestone 18: defvar/defparameterで真になる動的変数フラグ
    LispObject value;    // is_specialが真の場合の現在の動的値。let/let*が退避・書き換えする
    LispPackage *package; // milestone 23: 属するパッケージ。gensymの未interned symbolはNULL
} LispSymbol;

typedef LispObject (*LispBuiltinFn)(LispObject args);

typedef struct {
    LispObject gc_next;   // milestone 32: LispConsと同じ追跡リスト用（先頭2フィールド揃え必須）
    int gc_marked;
    LispObject params; // 仮引数シンボルのリスト（builtinの場合は未使用）
    LispObject body;   // 本文（単一式、builtinの場合は未使用）
    LispObject env;    // 生成時の環境（レキシカルスコープ用、builtinの場合は未使用）
    LispBuiltinFn builtin; // NULLならlambda由来、非NULLならC実装の組み込み関数
    int is_macro; // 真ならdefmacro由来のマクロ（呼び出し時に引数を評価しない）
    char *str_data; // NULLなら文字列ではない。非NULLなら文字列データ（0終端）へのポインタ
    UINTN str_len;  // str_dataが指す文字列の長さ（終端の'\0'を含まない）
    int is_float;       // milestone 22: 真ならfloat_valueが有効なfloatオブジェクト
    double float_value;
    UINT32 *big_digits; // milestone 22: 非NULLならbignum。2^32進の桁配列（[0]が最下位、先頭ゼロ桁はtrim済み）
    UINTN big_len;       // big_digitsの有効桁数
    int big_negative;    // 真なら負数（0は常にfixnumで表すため、bignumが0を表すことはない）
    LispObject *vec_data; // milestone 26: 非NULLならvector。要素配列（vec_len個）へのポインタ
    UINTN vec_len;         // vec_dataが指す要素数
    unsigned char *bytecode; // milestone 34/35: 非NULLならVMコンパイル済み関数。生バイト列へのポインタ
    UINTN bytecode_len;       // bytecodeの長さ（バイト数）
    LispObject *constants;   // OP_CONSTが参照する定数配列（LispObjectのベクタ、vec_dataと同じ形）
    UINTN constants_len;      // constantsの要素数
    UINTN nargs;              // 呼び出し時に期待される引数の個数（milestone37で使用）
    LispObject upvalue_descs; // milestone38: テンプレート側。捕捉元の記述子ベクタ（各要素は
                               // (kind . index)のcons。kind=0ならOP_MAKE_CLOSURE実行時の
                               // 呼び出し元フレームのローカルboxをFP+index経由で捕捉、
                               // kind=1なら呼び出し元closure自身のupvalues[index]を
                               // そのまま伝播（多段capture flattening）。非closure/未使用ならNIL
    LispObject upvalues;      // milestone38: インスタンス側。OP_MAKE_CLOSUREが実際に捕捉した
                               // box参照を格納するベクタ（lisp_make_vector互換）。テンプレート
                               // 自身はNILのまま
} LispClosure;

static inline LispObject lisp_make_fixnum(long long value) {
    return ((UINT64)value << 2) | LISP_TAG_FIXNUM;
}

static inline long long lisp_fixnum_value(LispObject obj) {
    return ((long long)obj) >> 2; // 算術シフトで符号を保持
}

static inline int lisp_is_fixnum(LispObject obj) {
    return (obj & LISP_TAG_MASK) == LISP_TAG_FIXNUM;
}

static inline int lisp_is_cons(LispObject obj) {
    return obj != LISP_NIL && (obj & LISP_TAG_MASK) == LISP_TAG_CONS;
}

static inline int lisp_is_symbol(LispObject obj) {
    return (obj & LISP_TAG_MASK) == LISP_TAG_SYMBOL;
}

static inline int lisp_is_closure(LispObject obj) {
    return (obj & LISP_TAG_MASK) == LISP_TAG_CLOSURE;
}

static inline LispCons *lisp_cons_cell(LispObject obj) {
    return (LispCons *)(obj & ~LISP_TAG_MASK);
}

static inline LispSymbol *lisp_symbol_cell(LispObject obj) {
    return (LispSymbol *)(obj & ~LISP_TAG_MASK);
}

static inline LispClosure *lisp_closure_cell(LispObject obj) {
    return (LispClosure *)(obj & ~LISP_TAG_MASK);
}

// 文字列はLISP_TAG_CLOSUREを共有し、str_dataフィールドの有無で判別する
// （マイルストーン10のbuiltinフィールドと同じescape hatch。新しい2bitタグは増やさない）
static inline int lisp_is_string(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->str_data != 0;
}

// float/bignumも同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 22）
static inline int lisp_is_float(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->is_float;
}

static inline int lisp_is_bignum(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->big_digits != 0;
}

// vectorも同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 26、22と同じ方針）
static inline int lisp_is_vector(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->vec_data != 0;
}

// VMコンパイル済み関数も同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 34/35）
static inline int lisp_is_compiled(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->bytecode != 0;
}

static inline int lisp_is_number(LispObject obj) {
    return lisp_is_fixnum(obj) || lisp_is_float(obj) || lisp_is_bignum(obj);
}

static UINT64 lisp_heap_ptr;
static UINT64 lisp_heap_end;
static UINT64 lisp_heap_total; // milestone 33: lisp_heap_lowがヒープ使用率を判定するための総量
EFI_SYSTEM_TABLE *g_system_table; // panic時にConOutへ出力するため
EFI_HANDLE g_image_handle; // milestone 16: loadがHandleProtocolでファイルシステムを取得するため

// milestone 31: REPLループが設置したトラップ。NULLなら未設置
lisp_jmp_buf *lisp_active_trap = (void *)0;

// エラーメッセージを出力する。トラップが設置されていればそこへlisp_longjmpで
// 復帰し、REPLの次のプロンプトから継続できる。トラップ未設置（起動処理中など）
// の場合は従来通りfor(;;){}でハングする
void lisp_panic(CHAR16 *message) {
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"Lisp panic: ");
    g_system_table->ConOut->OutputString(g_system_table->ConOut, message);
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"\r\n");
    if (lisp_active_trap != (void *)0) {
        lisp_longjmp(lisp_active_trap, 1);
    }
    for (;;) {}
}

// milestone 31: 固定容量資源の枯渇など、REPLに復帰しても安全に継続できない
// 致命的エラー用。トラップの有無に関わらず常にfor(;;){}でハングし続ける
void lisp_panic_fatal(CHAR16 *message) {
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"Lisp fatal panic: ");
    g_system_table->ConOut->OutputString(g_system_table->ConOut, message);
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"\r\n");
    for (;;) {}
}

static inline void lisp_assert_cons(LispObject obj) {
    if (!lisp_is_cons(obj)) {
        lisp_panic(L"expected a cons cell but got something else");
    }
}

static inline void lisp_assert_symbol(LispObject obj) {
    if (!lisp_is_symbol(obj)) {
        lisp_panic(L"expected a symbol but got something else");
    }
}

static inline void lisp_assert_string(LispObject obj) {
    if (!lisp_is_string(obj)) {
        lisp_panic(L"expected a string but got something else");
    }
}

static inline void lisp_assert_number(LispObject obj) {
    if (!lisp_is_number(obj)) {
        lisp_panic(L"expected a number but got something else");
    }
}

static inline void lisp_assert_vector(LispObject obj) {
    if (!lisp_is_vector(obj)) {
        lisp_panic(L"expected a vector but got something else");
    }
}

// milestone 26: make-vectorの長さ・svref/svsetのindexはfixnumでなければならない
// （float/bignumのタグ付きポインタをlisp_fixnum_valueで解釈すると値が破損するため、
// lisp_assert_numberでは不十分）
static inline void lisp_assert_fixnum(LispObject obj) {
    if (!lisp_is_fixnum(obj)) {
        lisp_panic(L"expected a fixnum but got something else");
    }
}

void lisp_heap_init(UINT64 start, UINT64 size) {
    lisp_heap_ptr = (start + 15) & ~15ULL; // 16byte境界に切り上げ（タグ用に下位ビットを空ける）
    lisp_heap_end = start + size;
    lisp_heap_total = size;
}

// milestone 33: バンプ側の残り容量が総量の20%未満ならGCを検討すべきタイミングとみなす
// （フリーリストに再利用可能なスロットがあっても見ないため近似だが、GCはバンプ枯渇を防ぐ
// ための仕組みなのでバンプ残量を指標にするのは妥当）
int lisp_heap_low(void) {
    return (lisp_heap_end - lisp_heap_ptr) < (lisp_heap_total / 5);
}

// ヒープからsizeバイトを切り出す（解放・GCなしのバンプアロケータ）。
// 戻り値は常に16byte境界なので、下位2bitをそのままタグとして使える
void *lisp_alloc(UINTN size) {
    UINT64 aligned_size = (size + 15) & ~15ULL;
    if (lisp_heap_ptr + aligned_size > lisp_heap_end) {
        lisp_panic_fatal(L"heap exhausted");
    }
    void *ptr = (void *)lisp_heap_ptr;
    lisp_heap_ptr += aligned_size;
    return ptr;
}

// milestone 32: 確保済み全オブジェクト（cons/symbol/closure）を確保順に連結した
// 追跡リストの先頭。milestone33のGCスイープフェーズがここから全走査する
static LispObject lisp_all_objects_head = LISP_NIL;

// milestone 33: タグ別フリーリストの先頭。sweepで未マークだったオブジェクトをここへ
// 戻し、lisp_alloc_trackedがバンプ確保の前にここから再利用する。gc_next（offset 0）を
// 「追跡リストの次」と「フリーリストの次」の両方の用途で再利用する（生死は排他なので
// フィールドが競合することはない）
static LispObject lisp_free_cons = LISP_NIL;
static LispObject lisp_free_symbol = LISP_NIL;
static LispObject lisp_free_closure = LISP_NIL;

static LispObject *lisp_free_list_for_tag(UINT64 tag) {
    switch (tag) {
        case LISP_TAG_CONS:   return &lisp_free_cons;
        case LISP_TAG_SYMBOL: return &lisp_free_symbol;
        default:              return &lisp_free_closure; // LISP_TAG_CLOSURE
    }
}

// 対応するフリーリストにスロットがあればそれを再利用し、無ければlisp_allocでバンプ確保する。
// 確保後は先頭フィールド(gc_next, offset 0)でlisp_all_objects_headへ連結してからtag付き
// LispObjectとして返す生ポインタを返す。gc_next/gc_markedの初期化もここで行う（呼び出し側が
// 個別に初期化する必要はない）。3構造体で先頭2フィールドの型・順序が揃っていることが前提
// （LispCons/LispSymbol/LispClosureの定義コメント参照）
static void *lisp_alloc_tracked(UINTN size, UINT64 tag) {
    LispObject *free_head = lisp_free_list_for_tag(tag);
    void *ptr;
    if (*free_head != LISP_NIL) {
        ptr = (void *)(*free_head & ~LISP_TAG_MASK);
        *free_head = *(LispObject *)ptr; // gc_nextを辿って次のフリーノードへ
    } else {
        ptr = lisp_alloc(size);
    }
    LispObject tagged = ((LispObject)ptr) | tag;
    *(LispObject *)ptr = lisp_all_objects_head;       // gc_next (offset 0)
    *(int *)((char *)ptr + sizeof(LispObject)) = 0;   // gc_marked (offset 8) = 未マーク
    lisp_all_objects_head = tagged;
    return ptr;
}

LispObject alloc_cons(void) {
    return (LispObject)lisp_alloc_tracked(sizeof(LispCons), LISP_TAG_CONS); // 下位2bit=00=LISP_TAG_CONS、タグ付け不要
}

LispObject lisp_cons(LispObject car, LispObject cdr) {
    LispObject obj = alloc_cons();
    LispCons *cell = lisp_cons_cell(obj);
    cell->car = car;
    cell->cdr = cdr;
    return obj;
}

LispObject lisp_car(LispObject obj) {
    lisp_assert_cons(obj);
    return lisp_cons_cell(obj)->car;
}

LispObject lisp_cdr(LispObject obj) {
    lisp_assert_cons(obj);
    return lisp_cons_cell(obj)->cdr;
}

void lisp_set_car(LispObject obj, LispObject value) {
    lisp_assert_cons(obj);
    lisp_cons_cell(obj)->car = value;
}

void lisp_set_cdr(LispObject obj, LispObject value) {
    lisp_assert_cons(obj);
    lisp_cons_cell(obj)->cdr = value;
}

LispObject lisp_make_closure(LispObject params, LispObject body, LispObject env) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = params;
    closure->body = body;
    closure->env = env;
    closure->builtin = 0;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

LispObject lisp_make_builtin(LispBuiltinFn fn) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = fn;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// defmacro由来のマクロを作る。lambda由来のクロージャと同じ構造だが、
// 呼び出し時にlisp_evalが引数を評価せず展開処理に回すためis_macroを立てる
LispObject lisp_make_macro(LispObject params, LispObject body, LispObject env) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = params;
    closure->body = body;
    closure->env = env;
    closure->builtin = 0;
    closure->is_macro = 1;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// 文字列オブジェクトを作る（milestone 15）。charsのlen文字分をヒープに
// コピーして0終端したバッファをstr_dataに持たせる（呼び出し元のバッファを
// 保持し続けるわけではないので、charsはこの呼び出し中だけ有効なスタック上の
// 一時バッファ等で構わない）
LispObject lisp_make_string(const char *chars, UINTN len) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = 0;
    closure->is_macro = 0;
    char *buf = (char *)lisp_alloc(len + 1);
    for (UINTN i = 0; i < len; i++) {
        buf[i] = chars[i];
    }
    buf[len] = '\0';
    closure->str_data = buf;
    closure->str_len = len;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

#define LISP_BIGNUM_MAX_LIMBS 64

// floatオブジェクトを作る（milestone 22）。LISP_TAG_CLOSUREのescape hatchを再利用する
LispObject lisp_make_float(double value) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = 0;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 1;
    closure->float_value = value;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// bignumオブジェクトを作る（milestone 22）。digitsはlen個の有効桁（先頭ゼロ桁は
// trim済みでlen>=1であること）をヒープにコピーする。fixnum範囲に収まるかどうかの
// 判定は行わない生のコンストラクタなので、通常はlisp_make_number_from_magnitude経由で
// 呼ぶこと（正規化を保証するため）
LispObject lisp_make_bignum(const UINT32 *digits, UINTN len, int negative) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = 0;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    UINT32 *buf = (UINT32 *)lisp_alloc(len * sizeof(UINT32));
    for (UINTN i = 0; i < len; i++) {
        buf[i] = digits[i];
    }
    closure->big_digits = buf;
    closure->big_len = len;
    closure->big_negative = negative;
    closure->vec_data = 0;
    closure->vec_len = 0;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// vectorオブジェクトを作る（milestone 26）。LISP_TAG_CLOSUREのescape hatchを
// milestone15/22と同様に再利用し、新しいタグは追加しない。要素配列はLispClosure本体とは
// 別にlisp_allocでヒープ上へ確保し、全要素をfillで初期化する。len==0の場合、lisp_allocは
// 次の割り当てと同じアドレスを返すことがあるが、vec_lenが0のためvec_dataを実際に
// 読み書きするコードは存在せず問題ない
LispObject lisp_make_vector(UINTN len, LispObject fill) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = 0;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    LispObject *buf = (LispObject *)lisp_alloc(len * sizeof(LispObject));
    for (UINTN i = 0; i < len; i++) {
        buf[i] = fill;
    }
    closure->vec_data = buf;
    closure->vec_len = len;
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// VMコンパイル済み関数オブジェクトを作る（milestone 34/35）。LISP_TAG_CLOSUREのescape hatchを
// milestone15/22/26と同様に再利用する。bytecode/constantsはどちらもstr_data/vec_dataと同じく
// 呼び出し元のバッファをヒープへコピーして持つ（呼び出し元の一時バッファを保持し続ける必要はない）
LispObject lisp_make_compiled(const unsigned char *bytecode, UINTN bytecode_len,
                               const LispObject *constants, UINTN constants_len, UINTN nargs) {
    LispClosure *closure = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = 0;
    closure->is_macro = 0;
    closure->str_data = 0;
    closure->str_len = 0;
    closure->is_float = 0;
    closure->float_value = 0.0;
    closure->big_digits = 0;
    closure->big_len = 0;
    closure->big_negative = 0;
    closure->vec_data = 0;
    closure->vec_len = 0;
    unsigned char *code_buf = (unsigned char *)lisp_alloc(bytecode_len);
    for (UINTN i = 0; i < bytecode_len; i++) {
        code_buf[i] = bytecode[i];
    }
    closure->bytecode = code_buf;
    closure->bytecode_len = bytecode_len;
    LispObject *const_buf = (LispObject *)lisp_alloc(constants_len * sizeof(LispObject));
    for (UINTN i = 0; i < constants_len; i++) {
        const_buf[i] = constants[i];
    }
    closure->constants = const_buf;
    closure->constants_len = constants_len;
    closure->nargs = nargs;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// kinds[i]/indices[i]から(kind . index)のconsをcount個持つベクタを構築する（milestone38）。
// lisp_make_compiledのテンプレートclosureにlisp_compiled_set_upvalue_descsで設定して使う
LispObject lisp_make_upvalue_descs(const UINTN *kinds, const UINTN *indices, UINTN count) {
    LispObject descs = lisp_make_vector(count, LISP_NIL);
    LispClosure *descs_cell = lisp_closure_cell(descs);
    for (UINTN i = 0; i < count; i++) {
        descs_cell->vec_data[i] = lisp_cons(lisp_make_fixnum((long long)kinds[i]),
                                             lisp_make_fixnum((long long)indices[i]));
    }
    return descs;
}

// テンプレートclosure fnにupvalue_descsを後付けで設定する（milestone38）
void lisp_compiled_set_upvalue_descs(LispObject fn, LispObject descs) {
    lisp_closure_cell(fn)->upvalue_descs = descs;
}

// 桁配列(digits, len)と符号(negative)がfixnum表現範囲に収まるならfixnumを、
// 収まらなければbignumをboxして返す。bignum生成の正規化された唯一の入り口とする
// （「小さい結果は必ずfixnum」という不変条件を保ち、eq比較を成立させるため）
LispObject lisp_make_number_from_magnitude(const UINT32 *digits, UINTN len, int negative) {
    while (len > 0 && digits[len - 1] == 0) {
        len--;
    }
    if (len == 0) {
        return lisp_make_fixnum(0); // 0は常にfixnum（符号は無視する）
    }
    if (len <= 2) {
        UINT64 mag = digits[0];
        if (len == 2) {
            mag |= ((UINT64)digits[1]) << 32;
        }
        // fixnumは(value << 2)が64bitに収まる範囲、すなわち[-2^61, 2^61-1]まで表現できる
        UINT64 max_mag = negative ? ((UINT64)1 << 61) : (((UINT64)1 << 61) - 1);
        if (mag <= max_mag) {
            long long value = negative ? -(long long)mag : (long long)mag;
            return lisp_make_fixnum(value);
        }
    }
    return lisp_make_bignum(digits, len, negative);
}

// 絶対値の加算(schoolbook algorithm、2^32進)。out capacityは
// max(alen,blen)+1以上であること。戻り値は書き込んだ桁数
static UINTN lisp_bignum_add_mag(const UINT32 *a, UINTN alen, const UINT32 *b, UINTN blen, UINT32 *out) {
    UINTN n = (alen > blen) ? alen : blen;
    UINT64 carry = 0;
    UINTN i;
    for (i = 0; i < n; i++) {
        UINT64 av = (i < alen) ? a[i] : 0;
        UINT64 bv = (i < blen) ? b[i] : 0;
        UINT64 sum = av + bv + carry;
        out[i] = (UINT32)sum;
        carry = sum >> 32;
    }
    if (carry != 0) {
        out[i] = (UINT32)carry;
        i++;
    }
    return i;
}

// 絶対値の減算。|a|>=|b|であることが前提（呼び出し側でcompareして保証する）。
// out capacityはalen以上であること。戻り値は先頭ゼロ桁をtrimした後の桁数
static UINTN lisp_bignum_sub_mag(const UINT32 *a, UINTN alen, const UINT32 *b, UINTN blen, UINT32 *out) {
    long long borrow = 0;
    for (UINTN i = 0; i < alen; i++) {
        long long av = a[i];
        long long bv = (i < blen) ? b[i] : 0;
        long long diff = av - bv - borrow;
        if (diff < 0) {
            diff += ((long long)1 << 32);
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[i] = (UINT32)diff;
    }
    UINTN len = alen;
    while (len > 0 && out[len - 1] == 0) {
        len--;
    }
    return len;
}

// 絶対値の比較。1: |a|>|b|, 0: 等しい, -1: |a|<|b|
static int lisp_bignum_compare_mag(const UINT32 *a, UINTN alen, const UINT32 *b, UINTN blen) {
    if (alen != blen) {
        return (alen > blen) ? 1 : -1;
    }
    for (UINTN idx = alen; idx > 0; idx--) {
        UINTN i = idx - 1;
        if (a[i] != b[i]) {
            return (a[i] > b[i]) ? 1 : -1;
        }
    }
    return 0;
}

// objの絶対値の桁配列と符号を得る。fixnumならtmp(要素数2以上)に展開して返し、
// bignumなら内部の桁配列をそのまま返す（floatは呼び出し禁止、呼び出し前に
// lisp_is_floatで分岐しておくこと）
static const UINT32 *lisp_number_magnitude(LispObject obj, UINT32 *tmp, UINTN *out_len, int *out_negative) {
    if (lisp_is_fixnum(obj)) {
        long long value = lisp_fixnum_value(obj);
        *out_negative = value < 0;
        UINT64 mag = (value < 0) ? (UINT64)(-value) : (UINT64)value;
        tmp[0] = (UINT32)(mag & 0xFFFFFFFFULL);
        tmp[1] = (UINT32)(mag >> 32);
        *out_len = (tmp[1] != 0) ? 2 : ((tmp[0] != 0) ? 1 : 0);
        return tmp;
    }
    LispClosure *cell = lisp_closure_cell(obj);
    *out_negative = cell->big_negative;
    *out_len = cell->big_len;
    return cell->big_digits;
}

// obj(fixnum/bignum/float)をdoubleに変換する。bignumは上位桁から順に
// doubleへ積み上げるため、doubleの仮数部(53bit)を超える桁は丸め誤差が生じる
// （float混在時の昇格専用の変換であり、既知の精度限界として許容する）
static double lisp_number_to_double(LispObject obj) {
    if (lisp_is_float(obj)) {
        return lisp_closure_cell(obj)->float_value;
    }
    if (lisp_is_fixnum(obj)) {
        return (double)lisp_fixnum_value(obj);
    }
    LispClosure *cell = lisp_closure_cell(obj);
    double result = 0.0;
    for (UINTN idx = cell->big_len; idx > 0; idx--) {
        result = result * 4294967296.0 + (double)cell->big_digits[idx - 1];
    }
    return cell->big_negative ? -result : result;
}

// 汎用の数値加算。fixnum/bignum/floatのどの組み合わせでも受け付ける（milestone 22の
// 型混在昇格規則の中核）: どちらかがfloatならdoubleで計算してfloatを返す。それ以外は
// 絶対値配列の加減算(符号が同じなら加算、異なれば大きい方から小さい方を引く)を行い、
// 結果をlisp_make_number_from_magnitudeで正規化する（fixnum範囲に収まればfixnumへ戻す）
LispObject lisp_num_add(LispObject a, LispObject b) {
    if (lisp_is_float(a) || lisp_is_float(b)) {
        return lisp_make_float(lisp_number_to_double(a) + lisp_number_to_double(b));
    }

    UINT32 ta[2], tb[2];
    UINTN alen, blen;
    int aneg, bneg;
    const UINT32 *ad = lisp_number_magnitude(a, ta, &alen, &aneg);
    const UINT32 *bd = lisp_number_magnitude(b, tb, &blen, &bneg);

    UINTN cap = (alen > blen ? alen : blen) + 1;
    if (cap > LISP_BIGNUM_MAX_LIMBS) {
        lisp_panic(L"bignum overflow: magnitude too large");
    }

    UINT32 out[LISP_BIGNUM_MAX_LIMBS];
    UINTN outlen;
    int outneg;
    if (aneg == bneg) {
        outlen = lisp_bignum_add_mag(ad, alen, bd, blen, out);
        outneg = aneg;
    } else {
        int cmp = lisp_bignum_compare_mag(ad, alen, bd, blen);
        if (cmp >= 0) {
            outlen = lisp_bignum_sub_mag(ad, alen, bd, blen, out);
            outneg = aneg;
        } else {
            outlen = lisp_bignum_sub_mag(bd, blen, ad, alen, out);
            outneg = bneg;
        }
    }
    return lisp_make_number_from_magnitude(out, outlen, outneg);
}

// 符号反転（unary -）。fixnum/bignumは絶対値をそのまま符号だけ反転して正規化し直す
// （fixnumのmin/max境界の非対称性はlisp_make_number_from_magnitudeが吸収する）
LispObject lisp_num_negate(LispObject a) {
    if (lisp_is_float(a)) {
        return lisp_make_float(-lisp_closure_cell(a)->float_value);
    }
    UINT32 ta[2];
    UINTN alen;
    int aneg;
    const UINT32 *ad = lisp_number_magnitude(a, ta, &alen, &aneg);
    return lisp_make_number_from_magnitude(ad, alen, !aneg);
}

LispObject lisp_num_sub(LispObject a, LispObject b) {
    return lisp_num_add(a, lisp_num_negate(b));
}

static int lisp_streq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

// milestone 23: パッケージの集合。common-lisp-user/keywordの2つで始まるが、将来
// lisp_make_packageを追加で呼ぶだけで3つ目以降のパッケージを増やせる（LISP_MAX_PACKAGESの範囲内）
static LispPackage lisp_packages[LISP_MAX_PACKAGES];
static UINTN lisp_package_count = 0;
static LispPackage *lisp_cl_user_package;
static LispPackage *lisp_keyword_package;

static LispPackage *lisp_make_package(const char *name, int is_keyword_package) {
    if (lisp_package_count >= LISP_MAX_PACKAGES) {
        lisp_panic_fatal(L"package table exhausted");
    }
    LispPackage *pkg = &lisp_packages[lisp_package_count];
    UINTN i = 0;
    while (name[i] != '\0' && i < LISP_PACKAGE_NAME_MAX - 1) {
        pkg->name[i] = name[i];
        i++;
    }
    pkg->name[i] = '\0';
    pkg->symbol_count = 0;
    pkg->is_keyword_package = is_keyword_package;
    lisp_package_count++;
    return pkg;
}

// 現時点ではLispからは使わない内部APIだが、将来の`find-package`相当の土台として用意しておく
static LispPackage *lisp_find_package(const char *name) {
    for (UINTN i = 0; i < lisp_package_count; i++) {
        if (lisp_streq(lisp_packages[i].name, name)) {
            return &lisp_packages[i];
        }
    }
    return 0;
}

void lisp_packages_init(void) {
    lisp_cl_user_package = lisp_make_package("common-lisp-user", 0);
    lisp_keyword_package = lisp_make_package("keyword", 1);
}

// 同じ名前でintern済みのシンボルがpkg内にあれば同一のLispObjectを返す（eqで比較可能にする）。
// 無ければ新規に確保してpkgのテーブルに登録する。比較・格納の両方で先にLISP_SYMBOL_NAME_MAX-1文字に
// 切り詰めてから扱うことで、name自体を切り詰めずにlisp_streqへ渡すと「格納済みの切り詰め済み名」
// と「今回渡された切り詰め前のnama」が食い違って毎回別シンボルを生成してしまう
// （呼ぶたびにeqが成立せずunbound variableになる）バグを避ける
LispObject lisp_intern_in_package(LispPackage *pkg, const char *name) {
    char truncated[LISP_SYMBOL_NAME_MAX];
    UINTN len = 0;
    while (name[len] != '\0' && len < LISP_SYMBOL_NAME_MAX - 1) {
        truncated[len] = name[len];
        len++;
    }
    truncated[len] = '\0';

    for (UINTN i = 0; i < pkg->symbol_count; i++) {
        LispSymbol *sym = lisp_symbol_cell(pkg->symbols[i]);
        if (lisp_streq(sym->name, truncated)) {
            return pkg->symbols[i];
        }
    }

    if (pkg->symbol_count >= LISP_MAX_SYMBOLS) {
        lisp_panic_fatal(L"symbol table exhausted");
    }

    LispSymbol *sym = (LispSymbol *)lisp_alloc_tracked(sizeof(LispSymbol), LISP_TAG_SYMBOL);
    UINTN i = 0;
    while (truncated[i] != '\0') {
        sym->name[i] = truncated[i];
        i++;
    }
    sym->name[i] = '\0';
    sym->is_special = 0;
    sym->value = LISP_NIL;
    sym->package = pkg;

    LispObject obj = ((LispObject)sym) | LISP_TAG_SYMBOL;
    pkg->symbols[pkg->symbol_count] = obj;
    pkg->symbol_count++;
    return obj;
}

LispObject lisp_intern(const char *name) {
    return lisp_intern_in_package(lisp_cl_user_package, name);
}

static LispObject lisp_intern_keyword(const char *name) {
    return lisp_intern_in_package(lisp_keyword_package, name);
}

// gensym専用 (milestone 20): どのパッケージのテーブルにも登録せず新規のLispSymbolを確保するだけの
// シンボルを作る。eqはオブジェクトの同一性そのものであり、lisp_intern_in_packageの一致判定は
// パッケージのテーブルに載っているシンボルしか見つけられないため、テーブルに載せないことで
// 「名前が何であっても、reader/internを経由する限り絶対にeqにならない」ユニークな
// シンボルになる。CLのuninterned symbol（どのパッケージにも属さない）に相当するためpackageはNULL
static LispObject lisp_make_uninterned_symbol(const char *name) {
    LispSymbol *sym = (LispSymbol *)lisp_alloc_tracked(sizeof(LispSymbol), LISP_TAG_SYMBOL);
    UINTN i = 0;
    while (name[i] != '\0' && i < LISP_SYMBOL_NAME_MAX - 1) {
        sym->name[i] = name[i];
        i++;
    }
    sym->name[i] = '\0';
    sym->is_special = 0;
    sym->value = LISP_NIL;
    sym->package = 0;
    return ((LispObject)sym) | LISP_TAG_SYMBOL;
}

// よく使う特別なシンボル（nilはLISP_NILの即値のまま。それ以外はここでintern）
static LispObject lisp_sym_t;
static LispObject lisp_sym_quote;
static LispObject lisp_sym_if;
static LispObject lisp_sym_lambda;
static LispObject lisp_sym_defun;
static LispObject lisp_sym_defmacro;
static LispObject lisp_sym_quasiquote;
static LispObject lisp_sym_unquote;
static LispObject lisp_sym_unquote_splicing;
static LispObject lisp_sym_progn;
static LispObject lisp_sym_let;
static LispObject lisp_sym_let_star;
static LispObject lisp_sym_setq;
static LispObject lisp_sym_cond;
static LispObject lisp_sym_and;
static LispObject lisp_sym_or;
static LispObject lisp_sym_when;
static LispObject lisp_sym_unless;
static LispObject lisp_sym_defvar;
static LispObject lisp_sym_defparameter;
static LispObject lisp_sym_block;
static LispObject lisp_sym_return_from;
static LispObject lisp_sym_macroexpand_hook;

void lisp_symbols_init(void) {
    lisp_sym_t = lisp_intern("t");
    lisp_sym_quote = lisp_intern("quote");
    lisp_sym_if = lisp_intern("if");
    lisp_sym_lambda = lisp_intern("lambda");
    lisp_sym_defun = lisp_intern("defun");
    lisp_sym_defmacro = lisp_intern("defmacro");
    lisp_sym_quasiquote = lisp_intern("quasiquote");
    lisp_sym_unquote = lisp_intern("unquote");
    lisp_sym_unquote_splicing = lisp_intern("unquote-splicing");
    lisp_sym_progn = lisp_intern("progn");
    lisp_sym_let = lisp_intern("let");
    lisp_sym_let_star = lisp_intern("let*");
    lisp_sym_setq = lisp_intern("setq");
    lisp_sym_cond = lisp_intern("cond");
    lisp_sym_and = lisp_intern("and");
    lisp_sym_or = lisp_intern("or");
    lisp_sym_when = lisp_intern("when");
    lisp_sym_unless = lisp_intern("unless");
    lisp_sym_defvar = lisp_intern("defvar");
    lisp_sym_defparameter = lisp_intern("defparameter");
    lisp_sym_block = lisp_intern("block");
    lisp_sym_return_from = lisp_intern("return-from");
    lisp_sym_macroexpand_hook = lisp_intern("*macroexpand-hook*");
}


// --- 文字入力 (milestone 6) ---
char input_buffer[LISP_INPUT_BUFFER_MAX];
UINTN input_length;

// Enterキーまでの1行をキー入力から読み取り、input_bufferにASCII文字列として格納する。
// Backspaceは1文字削除して画面表示も戻す。UnicodeChar==0の制御キー(矢印キー等)は無視する
void lisp_read_line(EFI_SYSTEM_TABLE *SystemTable) {
    input_length = 0;

    for (;;) {
        EFI_INPUT_KEY key;
        EFI_STATUS status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key);
        if (status != 0) {
            continue; // EFI_NOT_READY: まだキー入力がない
        }

        if (key.UnicodeChar == L'\r') {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
            break;
        }

        if (key.UnicodeChar == 8) { // Backspace
            if (input_length > 0) {
                input_length--;
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\b \b");
            }
            continue;
        }

        if (key.UnicodeChar == 0) {
            continue;
        }

        if (input_length < LISP_INPUT_BUFFER_MAX - 1) {
            input_buffer[input_length] = (char)key.UnicodeChar;
            input_length++;

            CHAR16 echo[2] = { key.UnicodeChar, 0 };
            SystemTable->ConOut->OutputString(SystemTable->ConOut, echo);
        }
    }

    input_buffer[input_length] = '\0';
}

// milestone 24: 出力ストリームのコンソール実装。ASCII文字列(8bit char)をCHAR16に
// 変換してコンソールへ出力する
static void lisp_console_stream_write(void *ctx, const char *str) {
    EFI_SYSTEM_TABLE *SystemTable = (EFI_SYSTEM_TABLE *)ctx;
    CHAR16 buf[LISP_INPUT_BUFFER_MAX];
    UINTN i = 0;
    while (str[i] != '\0' && i < LISP_INPUT_BUFFER_MAX - 1) {
        buf[i] = (CHAR16)str[i];
        i++;
    }
    buf[i] = 0;
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buf);
}

LispOutputStream lisp_make_console_stream(EFI_SYSTEM_TABLE *SystemTable) {
    LispOutputStream stream = { lisp_console_stream_write, (void *)SystemTable };
    return stream;
}

// ASCII文字列(8bit char)をストリームへ書き込む
void lisp_print_ascii(LispOutputStream *stream, const char *str) {
    stream->write(stream->ctx, str);
}


// --- プリンター (milestone 7) ---

// 10進数を表示する（符号付き。libcのitoa相当が無いため自前実装）
void lisp_print_fixnum(LispOutputStream *stream, long long value) {
    char digits[24];
    UINTN i = 0;
    int negative = value < 0;
    unsigned long long uval = negative ? (unsigned long long)(-value) : (unsigned long long)value;

    do {
        digits[i] = '0' + (char)(uval % 10);
        i++;
        uval /= 10;
    } while (uval > 0);

    if (negative) {
        lisp_print_ascii(stream, "-");
    }

    // digitsには下位桁から積んでいるので、後ろから出力する
    while (i > 0) {
        i--;
        char ch[2] = { digits[i], 0 };
        lisp_print_ascii(stream, ch);
    }
}

// digits(len個、2^32進)をdivisor(32bit以下の小さい数)で割り、商をdigitsに
// 上書きする(最上位桁からの長除法)。戻り値は余り
static UINT32 lisp_bignum_divmod_small(UINT32 *digits, UINTN len, UINT32 divisor) {
    UINT64 remainder = 0;
    for (UINTN idx = len; idx > 0; idx--) {
        UINTN i = idx - 1;
        UINT64 cur = (remainder << 32) | digits[i];
        digits[i] = (UINT32)(cur / divisor);
        remainder = cur % divisor;
    }
    return (UINT32)remainder;
}

// bignumを10進文字列として表示する（libcの多倍長→10進変換相当が無いため自前実装。
// 桁配列を10で繰り返し割って余りを集めることで、下位桁から順に10進の1桁を取り出す）
void lisp_print_bignum(LispOutputStream *stream, LispClosure *big) {
    UINT32 scratch[LISP_BIGNUM_MAX_LIMBS];
    UINTN len = big->big_len;
    for (UINTN i = 0; i < len; i++) {
        scratch[i] = big->big_digits[i];
    }

    char decimal[LISP_BIGNUM_MAX_LIMBS * 10 + 2]; // 64桁*32bit ≒ 617桁+符号分の余裕
    UINTN dcount = 0;
    while (len > 0) {
        UINT32 rem = lisp_bignum_divmod_small(scratch, len, 10);
        decimal[dcount] = '0' + (char)rem;
        dcount++;
        while (len > 0 && scratch[len - 1] == 0) {
            len--;
        }
    }

    if (big->big_negative) {
        lisp_print_ascii(stream, "-");
    }
    while (dcount > 0) {
        dcount--;
        char ch[2] = { decimal[dcount], 0 };
        lisp_print_ascii(stream, ch);
    }
}

// floatを固定小数点形式で表示する（指数表記は扱わない簡略化）。整数部はlisp_print_fixnumを
// 再利用し、小数部は10進6桁を手計算で求め、末尾の'0'は1桁残るまでtrimする
// （libcのsprintf/%f相当が無いため自前実装。値がlong longで表せない極端な大きさのfloatは
// 想定していない）
void lisp_print_float(LispOutputStream *stream, double value) {
    int negative = value < 0.0;
    double v = negative ? -value : value;
    long long int_part = (long long)v;
    double frac = v - (double)int_part;

    if (negative) {
        lisp_print_ascii(stream, "-");
    }
    lisp_print_fixnum(stream, int_part);
    lisp_print_ascii(stream, ".");

    char frac_digits[6];
    for (UINTN i = 0; i < 6; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d > 9) {
            d = 9; // 浮動小数点誤差の防御
        }
        frac_digits[i] = '0' + (char)d;
        frac -= d;
    }

    UINTN show = 6;
    while (show > 1 && frac_digits[show - 1] == '0') {
        show--;
    }
    for (UINTN i = 0; i < show; i++) {
        char ch[2] = { frac_digits[i], 0 };
        lisp_print_ascii(stream, ch);
    }
}

// vectorを#(elem1 elem2 ...)形式で表示する（milestone 26）。各要素はlisp_printを
// 再帰呼び出しして表示する（consのリスト表示と同じ再帰構造）
static void lisp_print_vector(LispOutputStream *stream, LispClosure *vec) {
    lisp_print_ascii(stream, "#(");
    for (UINTN i = 0; i < vec->vec_len; i++) {
        if (i != 0) {
            lisp_print_ascii(stream, " ");
        }
        lisp_print(stream, vec->vec_data[i]);
    }
    lisp_print_ascii(stream, ")");
}

// LispObjectを人間が読める形式でコンソールに表示する。
// fixnumは10進、symbolは名前、consは(a b c)または(a . b)形式、nilはnilと表示する
void lisp_print(LispOutputStream *stream, LispObject obj) {
    if (obj == LISP_NIL) {
        lisp_print_ascii(stream, "nil");
        return;
    }

    if (lisp_is_fixnum(obj)) {
        lisp_print_fixnum(stream, lisp_fixnum_value(obj));
        return;
    }

    if (lisp_is_float(obj)) {
        lisp_print_float(stream, lisp_closure_cell(obj)->float_value);
        return;
    }

    if (lisp_is_bignum(obj)) {
        lisp_print_bignum(stream, lisp_closure_cell(obj));
        return;
    }

    if (lisp_is_vector(obj)) {
        lisp_print_vector(stream, lisp_closure_cell(obj));
        return;
    }

    if (lisp_is_symbol(obj)) {
        LispSymbol *sym = lisp_symbol_cell(obj);
        // milestone 23: keywordパッケージのシンボルは":"を前置して印字する
        if (sym->package != 0 && sym->package->is_keyword_package) {
            lisp_print_ascii(stream, ":");
        }
        lisp_print_ascii(stream, sym->name);
        return;
    }

    if (lisp_is_cons(obj)) {
        lisp_print_ascii(stream, "(");

        LispObject cur = obj;
        int first = 1;
        while (lisp_is_cons(cur)) {
            if (!first) {
                lisp_print_ascii(stream, " ");
            }
            first = 0;
            lisp_print(stream, lisp_car(cur));
            cur = lisp_cdr(cur);
        }

        if (cur != LISP_NIL) {
            lisp_print_ascii(stream, " . ");
            lisp_print(stream, cur);
        }

        lisp_print_ascii(stream, ")");
        return;
    }

    if (lisp_is_closure(obj)) {
        if (lisp_closure_cell(obj)->str_data != 0) {
            lisp_print_ascii(stream, "\"");
            lisp_print_ascii(stream, lisp_closure_cell(obj)->str_data);
            lisp_print_ascii(stream, "\"");
        } else if (lisp_closure_cell(obj)->builtin != 0) {
            lisp_print_ascii(stream, "#<builtin>");
        } else if (lisp_closure_cell(obj)->is_macro) {
            lisp_print_ascii(stream, "#<macro>");
        } else {
            lisp_print_ascii(stream, "#<closure>");
        }
        return;
    }

    lisp_panic(L"unknown lisp object type in printer");
}


// --- リーダー (milestone 8) ---
#define LISP_TOKEN_MAX 64

// 読み取り中の入力文字列上の現在位置。lisp_read_from_bufferで先頭に設定される
static const char *lisp_reader_pos;

static inline int lisp_reader_is_delim(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')' ||
           c == '\'' || c == '`' || c == ',' || c == '"';
}

#define LISP_STRING_READ_MAX 128

// 空白と";"から行末(または入力終端)までのコメントを読み飛ばす。コメントの後に空白が
// 続く場合もあるため、両方無くなるまでループする
static void lisp_reader_skip_ws(void) {
    for (;;) {
        while (*lisp_reader_pos == ' ' || *lisp_reader_pos == '\t' ||
               *lisp_reader_pos == '\r' || *lisp_reader_pos == '\n') {
            lisp_reader_pos++;
        }
        if (*lisp_reader_pos != ';') {
            return;
        }
        while (*lisp_reader_pos != '\0' && *lisp_reader_pos != '\n') {
            lisp_reader_pos++;
        }
    }
}

// トークンが（先頭の"-"を許した）10進整数リテラルかどうかを判定する。"-"単体は記号として扱う
static int lisp_token_is_fixnum(const char *token) {
    UINTN i = (token[0] == '-') ? 1 : 0;
    if (token[i] == '\0') {
        return 0;
    }
    for (; token[i] != '\0'; i++) {
        if (token[i] < '0' || token[i] > '9') {
            return 0;
        }
    }
    return 1;
}

// トークンを数値オブジェクトへ変換する。桁を読みながら直接桁配列へ積む
// （mag = mag*10+digit）ため、long longの範囲を超える長い整数リテラルも
// 正しくbignumとして読める（milestone 22より前は`long long value = value*10+digit`で
// 一旦組み立ててからlisp_make_fixnumに渡していたため、fixnum表現範囲を超える
// リテラルが符号ビットの位置に食い込んで値が化ける不具合があった）。
// 結果はlisp_make_number_from_magnitudeで正規化するので、小さい値は自動的にfixnumになる
static LispObject lisp_token_to_number(const char *token) {
    int negative = token[0] == '-';
    UINTN i = negative ? 1 : 0;
    UINT32 mag[LISP_BIGNUM_MAX_LIMBS];
    UINTN len = 0;
    for (; token[i] != '\0'; i++) {
        UINT32 digit = (UINT32)(token[i] - '0');
        UINT64 carry = digit;
        for (UINTN j = 0; j < len; j++) {
            UINT64 v = (UINT64)mag[j] * 10 + carry;
            mag[j] = (UINT32)v;
            carry = v >> 32;
        }
        if (carry != 0) {
            if (len >= LISP_BIGNUM_MAX_LIMBS) {
                lisp_panic(L"number literal overflow: too many digits");
            }
            mag[len] = (UINT32)carry;
            len++;
        }
    }
    return lisp_make_number_from_magnitude(mag, len, negative);
}

// トークンが（先頭の"-"を許した）"digit+.digit+"形式のfloatリテラルかどうかを判定する
// (milestone 22)。".5"や"5."（整数部・小数部いずれかの省略）や指数表記("1e10")は
// 扱わない簡略化
static int lisp_token_is_float(const char *token) {
    UINTN i = (token[0] == '-') ? 1 : 0;
    UINTN int_digits = 0;
    while (token[i] >= '0' && token[i] <= '9') {
        i++;
        int_digits++;
    }
    if (int_digits == 0 || token[i] != '.') {
        return 0;
    }
    i++;
    UINTN frac_digits = 0;
    while (token[i] >= '0' && token[i] <= '9') {
        i++;
        frac_digits++;
    }
    return frac_digits > 0 && token[i] == '\0';
}

// libcのatof/strtod相当が無いため自前実装。整数部→小数部の順に手で桁を積む
static double lisp_token_to_float(const char *token) {
    int negative = token[0] == '-';
    UINTN i = negative ? 1 : 0;
    double int_part = 0.0;
    while (token[i] >= '0' && token[i] <= '9') {
        int_part = int_part * 10.0 + (double)(token[i] - '0');
        i++;
    }
    i++; // '.'を読み飛ばす
    double frac_part = 0.0;
    double scale = 0.1;
    while (token[i] >= '0' && token[i] <= '9') {
        frac_part += (double)(token[i] - '0') * scale;
        scale *= 0.1;
        i++;
    }
    double value = int_part + frac_part;
    return negative ? -value : value;
}

LispObject lisp_read(void);

// "("の直後から呼ばれ、")"までの要素をconsのリストとして読み取る
LispObject lisp_read_list(void) {
    lisp_reader_skip_ws();

    if (*lisp_reader_pos == ')') {
        lisp_reader_pos++;
        return LISP_NIL;
    }

    if (*lisp_reader_pos == '\0') {
        lisp_panic(L"unexpected end of input in list");
    }

    LispObject head = lisp_read();
    LispObject tail = lisp_read_list();
    return lisp_cons(head, tail);
}

// 現在位置から1つのS式を読み取り、LispObjectを返す。
// "("なら再帰的にリストを読み取り、"'"/"`"/","/",@"ならquote/quasiquote/unquote/
// unquote-splicingへの糖衣構文として展開し、それ以外は整数リテラルまたはシンボルの
// トークンとして読む
LispObject lisp_read(void) {
    lisp_reader_skip_ws();

    char c = *lisp_reader_pos;
    if (c == '\0') {
        lisp_panic(L"unexpected end of input");
    }
    if (c == ')') {
        lisp_panic(L"unexpected )");
    }
    if (c == '(') {
        lisp_reader_pos++;
        return lisp_read_list();
    }
    if (c == '\'') {
        lisp_reader_pos++;
        return lisp_cons(lisp_sym_quote, lisp_cons(lisp_read(), LISP_NIL));
    }
    if (c == '`') {
        lisp_reader_pos++;
        return lisp_cons(lisp_sym_quasiquote, lisp_cons(lisp_read(), LISP_NIL));
    }
    if (c == ',') {
        lisp_reader_pos++;
        if (*lisp_reader_pos == '@') {
            lisp_reader_pos++;
            return lisp_cons(lisp_sym_unquote_splicing, lisp_cons(lisp_read(), LISP_NIL));
        }
        return lisp_cons(lisp_sym_unquote, lisp_cons(lisp_read(), LISP_NIL));
    }
    if (c == '"') {
        lisp_reader_pos++;
        char str_buf[LISP_STRING_READ_MAX];
        UINTN str_len = 0;
        while (*lisp_reader_pos != '"') {
            if (*lisp_reader_pos == '\0') {
                lisp_panic(L"unterminated string literal");
            }
            if (str_len < LISP_STRING_READ_MAX - 1) {
                str_buf[str_len] = *lisp_reader_pos;
                str_len++;
            }
            lisp_reader_pos++;
        }
        lisp_reader_pos++; // 閉じの"を読み飛ばす
        return lisp_make_string(str_buf, str_len);
    }

    char token[LISP_TOKEN_MAX];
    UINTN len = 0;
    while (!lisp_reader_is_delim(*lisp_reader_pos)) {
        if (len < LISP_TOKEN_MAX - 1) {
            token[len] = *lisp_reader_pos;
            len++;
        }
        lisp_reader_pos++;
    }
    token[len] = '\0';

    if (token[0] == ':') {
        // milestone 23: ":foo"はkeywordパッケージへintern（先頭の":"はsymbol-nameに含めない）
        return lisp_intern_keyword(token + 1);
    }
    if (lisp_token_is_fixnum(token)) {
        return lisp_token_to_number(token);
    }
    if (lisp_token_is_float(token)) {
        return lisp_make_float(lisp_token_to_float(token));
    }
    if (lisp_streq(token, "nil")) {
        return LISP_NIL; // nilはintern済みシンボルではなく即値LISP_NILそのものを返す
    }
    return lisp_intern(token);
}

// ASCII文字列(0終端)の先頭から1つのS式を読み取る
LispObject lisp_read_from_buffer(const char *str) {
    lisp_reader_pos = str;
    return lisp_read();
}

// 空白を読み飛ばした上でバッファ終端(0終端)に達しているかを調べる。lisp_read自体は
// 単一のS式の読み取りを前提に終端をpanicとして扱うため変更せず、milestone 16のloadが
// 複数のトップレベルS式を読み終える判定にのみこれを使う
static int lisp_reader_at_end(void) {
    lisp_reader_skip_ws();
    return *lisp_reader_pos == '\0';
}


// --- 評価器 (milestone 9) ---

// トップレベルの永続グローバル環境 (milestone 12)。EfiMainのREPLループが起動時に
// lisp_builtins_init()の結果で初期化する。ファイルスコープのグローバル変数にすることで、
// defun/loadなど今後の特殊形式がここを直接書き換えて新しい束縛を追加すれば、その後の
// すべてのREPL入力から見えるようになる（引数↔値のバインディング自体は
// マイルストーン9のlisp_env_bind_paramsのままで変更しない）
LispObject global_env = LISP_NIL;

// 非局所脱出の進行中シグナル (milestone 19)。setjmp/longjmpが使えないため、
// return-fromが「対象タグ＋値」をここにセットしてから通常のLispObjectとして
// 呼び出し元へ返り、以降のすべての評価経路（lisp_eval_progn/lisp_eval_list/
// lisp_eval各分岐）はlisp_evalの戻り値を使う前にこのタグを確認し、セットされていれば
// 残りの評価をせず即座にそのまま呼び出し元へ伝播する。対応するblockがタグの一致を見て
// LISP_NILに戻すことでシグナルを捕捉する。LISP_NILは「脱出は発生していない」を表すため、
// blockのタグとしてLISP_NILそのもの（nil）を使うことはできない
static LispObject lisp_return_tag = LISP_NIL;
static LispObject lisp_return_value = LISP_NIL;

// --- スタックマシン型VM (milestone 34) ---

// VMのデータスタック。固定長でグローバルに確保する（ヒープ確保ではなくバンプアロケータの
// 外側にある生配列。関数呼び出し前後のスタックフレームやOP_CALLの呼び出し規約もmilestone37以降
// ここに積む想定）。vm_spは次に値を積む位置（スタック上の要素数）を指し、vm_stack[0..vm_sp)が
// 現在有効な要素
#define VM_STACK_SIZE 1024
static LispObject vm_stack[VM_STACK_SIZE];
static UINTN vm_sp = 0;

// スタックへの積み下ろし。固定容量資源のため、溢れは(gc)等では解決できずlisp_panic_fatalとする
// （ヒープ・シンボルテーブル等の他の固定容量資源の枯渇と同じ扱い）
static inline void lisp_vm_push(LispObject value) {
    if (vm_sp >= VM_STACK_SIZE) {
        lisp_panic_fatal(L"VM stack overflow");
    }
    vm_stack[vm_sp] = value;
    vm_sp++;
}

static inline LispObject lisp_vm_pop(void) {
    if (vm_sp == 0) {
        lisp_panic(L"VM stack underflow");
    }
    vm_sp--;
    return vm_stack[vm_sp];
}

// panicのlongjmpはvm_sp/vm_stackを復元しない（C関数呼び出しの巻き戻しと違い、longjmp先には
// このスタックを元の深さへ戻す手立てがない）。REPLのpanic復帰点（lisp_setjmpのトラップ復帰
// 直後）で必ず呼び、vm_spをゼロへ戻してGCルート汚染・VMスタックオーバーフローの累積を防ぐ
// (milestone 48)
void lisp_vm_reset_stack(void) {
    vm_sp = 0;
}

// clのbytecodeをフレーム先頭fpで実行する（milestone35/36/37共通の実行ループ本体）。
// fp==vm_sp（呼び出し時点のスタック最上位）で呼ばれれば引数無しの実行（lisp_vm_execからの
// 直接呼び出し）、fp<vm_spで呼ばれればOP_CALLが既に呼び出し元のフレームからnargs個の生の
// 引数をvm_stack[fp..fp+nargs)へ積み、その場でボックス化した状態で呼ばれる（milestone37）。
// OP_LOAD_LOCAL/OP_STORE_LOCALはこのfp相対でボックスを参照する。C関数呼び出しの再帰を使って
// VM呼び出しの入れ子を表現するため、Cコールスタック自体がVMの呼び出しスタックを兼ねる
static LispObject lisp_vm_run(LispClosure *cl, UINTN fp) {
    unsigned char *pc = cl->bytecode;

    for (;;) {
        unsigned char op = *pc;
        pc++;
        switch (op) {
            case OP_CONST: {
                unsigned char idx = *pc;
                pc++;
                lisp_vm_push(cl->constants[idx]);
                break;
            }
            case OP_ADD: {
                LispObject b = lisp_vm_pop();
                LispObject a = lisp_vm_pop();
                lisp_vm_push(lisp_num_add(a, b));
                break;
            }
            case OP_JUMP: {
                unsigned char target = *pc;
                pc = cl->bytecode + target;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                unsigned char target = *pc;
                pc++;
                LispObject test = lisp_vm_pop();
                if (test == LISP_NIL) {
                    pc = cl->bytecode + target;
                }
                break;
            }
            case OP_LOAD_LOCAL: {
                unsigned char idx = *pc;
                pc++;
                lisp_vm_push(lisp_car(vm_stack[fp + idx]));
                break;
            }
            case OP_STORE_LOCAL: {
                unsigned char idx = *pc;
                pc++;
                LispObject value = lisp_vm_pop();
                lisp_set_car(vm_stack[fp + idx], value);
                break;
            }
            case OP_MAKE_LOCAL: {
                LispObject value = lisp_vm_pop();
                lisp_vm_push(lisp_cons(value, LISP_NIL));
                break;
            }
            case OP_CALL: {
                unsigned char nargs = *pc;
                pc++;
                LispObject fn_obj = lisp_vm_pop();
                if (!lisp_is_compiled(fn_obj)) {
                    lisp_panic(L"attempt to call a non-compiled function on the VM");
                }
                LispClosure *callee = lisp_closure_cell(fn_obj);
                if (callee->nargs != nargs) {
                    lisp_panic(L"VM function called with wrong number of arguments");
                }
                if (vm_sp < nargs) {
                    lisp_panic(L"VM stack underflow");
                }
                UINTN new_fp = vm_sp - nargs;
                for (UINTN i = 0; i < nargs; i++) {
                    vm_stack[new_fp + i] = lisp_cons(vm_stack[new_fp + i], LISP_NIL);
                }
                LispObject result = lisp_vm_run(callee, new_fp);
                vm_sp = new_fp;
                lisp_vm_push(result);
                break;
            }
            case OP_MAKE_CLOSURE: {
                unsigned char idx = *pc;
                pc++;
                LispClosure *template_cl = lisp_closure_cell(cl->constants[idx]);
                LispObject descs = template_cl->upvalue_descs;
                UINTN n = 0;
                LispObject *descs_data = 0;
                if (descs != LISP_NIL) {
                    LispClosure *descs_cell = lisp_closure_cell(descs);
                    n = descs_cell->vec_len;
                    descs_data = descs_cell->vec_data;
                }
                LispObject upvalues = lisp_make_vector(n, LISP_NIL);
                LispClosure *upvalues_cell = lisp_closure_cell(upvalues);
                for (UINTN i = 0; i < n; i++) {
                    LispObject desc = descs_data[i];
                    long long kind = lisp_fixnum_value(lisp_car(desc));
                    long long index = lisp_fixnum_value(lisp_cdr(desc));
                    if (kind == 0) {
                        upvalues_cell->vec_data[i] = vm_stack[fp + index];
                    } else {
                        LispClosure *own_upvalues_cell = lisp_closure_cell(cl->upvalues);
                        upvalues_cell->vec_data[i] = own_upvalues_cell->vec_data[index];
                    }
                }
                LispClosure *instance = (LispClosure *)lisp_alloc_tracked(sizeof(LispClosure), LISP_TAG_CLOSURE);
                instance->params = LISP_NIL;
                instance->body = LISP_NIL;
                instance->env = LISP_NIL;
                instance->builtin = 0;
                instance->is_macro = 0;
                instance->str_data = 0;
                instance->str_len = 0;
                instance->is_float = 0;
                instance->float_value = 0.0;
                instance->big_digits = 0;
                instance->big_len = 0;
                instance->big_negative = 0;
                instance->vec_data = 0;
                instance->vec_len = 0;
                instance->bytecode = template_cl->bytecode;
                instance->bytecode_len = template_cl->bytecode_len;
                instance->constants = template_cl->constants;
                instance->constants_len = template_cl->constants_len;
                instance->nargs = template_cl->nargs;
                instance->upvalue_descs = template_cl->upvalue_descs;
                instance->upvalues = upvalues;
                lisp_vm_push(((LispObject)instance) | LISP_TAG_CLOSURE);
                break;
            }
            case OP_LOAD_UPVALUE: {
                unsigned char idx = *pc;
                pc++;
                LispClosure *upvalues_cell = lisp_closure_cell(cl->upvalues);
                lisp_vm_push(lisp_car(upvalues_cell->vec_data[idx]));
                break;
            }
            case OP_STORE_UPVALUE: {
                unsigned char idx = *pc;
                pc++;
                LispObject value = lisp_vm_pop();
                LispClosure *upvalues_cell = lisp_closure_cell(cl->upvalues);
                lisp_set_car(upvalues_cell->vec_data[idx], value);
                break;
            }
            case OP_CONS: {
                LispObject cdr_val = lisp_vm_pop();
                LispObject car_val = lisp_vm_pop();
                lisp_vm_push(lisp_cons(car_val, cdr_val));
                break;
            }
            case OP_CAR: {
                LispObject x = lisp_vm_pop();
                lisp_vm_push(lisp_car(x));
                break;
            }
            case OP_CDR: {
                LispObject x = lisp_vm_pop();
                lisp_vm_push(lisp_cdr(x));
                break;
            }
            case OP_EQ: {
                LispObject b = lisp_vm_pop();
                LispObject a = lisp_vm_pop();
                lisp_vm_push((a == b) ? lisp_sym_t : LISP_NIL);
                break;
            }
            case OP_RETURN: {
                LispObject result = lisp_vm_pop();
                vm_sp = fp;
                return result;
            }
            default:
                lisp_panic(L"unknown VM opcode");
        }
    }
}

// fn（lisp_make_compiledで作ったコンパイル済み関数）を、引数無しの新規フレームとして実行する。
// 呼び出し前後でvm_spを保存・復元し、他の呼び出しのスタック内容を汚さないようにする
LispObject lisp_vm_exec(LispObject fn) {
    if (!lisp_is_compiled(fn)) {
        lisp_panic(L"attempt to execute a non-compiled function on the VM");
    }
    UINTN base_sp = vm_sp;
    LispObject result = lisp_vm_run(lisp_closure_cell(fn), base_sp);
    vm_sp = base_sp;
    return result;
}

// --- マーク＆スイープGC (milestone 33) ---
// (lisp_vm_gc_root_selftestはlisp_gc定義後、本セクション末尾に置く)

// objから到達可能な全オブジェクトのgc_markedを立てる。既にマーク済みなら即座に戻ることで
// rplaca/rplacdで作れる循環参照でも無限再帰・無限ループしない。長いリストのcdr鎖は確保数に
// 比例してCスタックを消費してしまうため、cdr方向（symbolのvalue、closureのenv方向も同様）は
// 再帰せずループで辿り、car/params/body/vec_dataの各要素方向だけ再帰する
// （幅方向は現実的なプログラムでは深くならない）
static void lisp_gc_mark(LispObject obj) {
    while (1) {
        if (lisp_is_fixnum(obj) || obj == LISP_NIL) {
            return;
        }
        if (lisp_is_cons(obj)) {
            LispCons *c = lisp_cons_cell(obj);
            if (c->gc_marked) {
                return;
            }
            c->gc_marked = 1;
            lisp_gc_mark(c->car);
            obj = c->cdr;
            continue;
        }
        if (lisp_is_symbol(obj)) {
            LispSymbol *s = lisp_symbol_cell(obj);
            if (s->gc_marked) {
                return;
            }
            s->gc_marked = 1;
            obj = s->value;
            continue;
        }
        // LISP_TAG_CLOSURE（文字列/float/bignum/vector/関数、すべて共通のescape hatch）
        LispClosure *cl = lisp_closure_cell(obj);
        if (cl->gc_marked) {
            return;
        }
        cl->gc_marked = 1;
        lisp_gc_mark(cl->params);
        lisp_gc_mark(cl->body);
        // vec_data/constantsだけが実際のLispObject配列（str_data/big_digits/bytecodeは
        // 生バイト列でありLispObjectではないため辿らない。所有するclosureが生きていれば
        // 不透明バッファとしてそのまま保持される）
        if (cl->vec_data) {
            for (UINTN i = 0; i < cl->vec_len; i++) {
                lisp_gc_mark(cl->vec_data[i]);
            }
        }
        if (cl->constants) {
            for (UINTN i = 0; i < cl->constants_len; i++) {
                lisp_gc_mark(cl->constants[i]);
            }
        }
        // upvalue_descs/upvaluesはconstants等と違い生配列ではなく、それ自体がLispObject
        // （milestone38: 通常のcons/vectorオブジェクト）なのでparams/bodyと同じ1回のmarkで足りる
        lisp_gc_mark(cl->upvalue_descs);
        lisp_gc_mark(cl->upvalues);
        obj = cl->env;
    }
}

// GCのルート集合: グローバル環境（alist。closureのenvも同じ表現を共有するため、生きている
// closureがマークされた時点でそのenvも連動して辿られる）、全パッケージに登録された全シンボル
// （t等の特殊シンボルキャッシュもすべてこの中に含まれるため個別マークは不要）、非局所脱出用の
// シグナル（return-fromのタグ不一致panicがこの2つをクリアしない既知の挙動があるため、保守的に
// 常に生きているとみなす）、およびVMデータスタック（milestone34。vm_stack[0..vm_sp)には
// lisp_vm_execが評価中の中間値が生のLispObjectとして置かれるため、Cローカル変数と同様GCの
// 追跡対象外になってしまう。ここでルートに加えないとバンプ確保のたびに回収されてしまう）
static void lisp_gc_mark_roots(void) {
    lisp_gc_mark(global_env);
    for (UINTN i = 0; i < lisp_package_count; i++) {
        LispPackage *pkg = &lisp_packages[i];
        for (UINTN j = 0; j < pkg->symbol_count; j++) {
            lisp_gc_mark(pkg->symbols[j]);
        }
    }
    lisp_gc_mark(lisp_return_tag);
    lisp_gc_mark(lisp_return_value);
    for (UINTN i = 0; i < vm_sp; i++) {
        lisp_gc_mark(vm_stack[i]);
    }
}

// lisp_all_objects_headを1回走査し、マーク済みは生存リストへ戻し（マークビットは次回に向けて
// リセット）、未マークはタグに応じたフリーリストへ戻す。回収したオブジェクト数を返す。
// 回収したLispClosureがstr_data/big_digits/vec_dataを持っていた場合、それらの2次バッファは
// 解放されない（milestone32から追跡対象外のまま、コンパクション非対応の方針に合わせて
// 恒久的なリークとして受け入れる）
static UINTN lisp_gc_sweep(void) {
    LispObject obj = lisp_all_objects_head;
    LispObject survivors = LISP_NIL;
    UINTN freed = 0;
    while (obj != LISP_NIL) {
        UINT64 tag = obj & LISP_TAG_MASK;
        void *ptr = (void *)(obj & ~LISP_TAG_MASK);
        LispObject next = *(LispObject *)ptr; // gc_next
        int *marked = (int *)((char *)ptr + sizeof(LispObject));
        if (*marked) {
            *marked = 0;
            *(LispObject *)ptr = survivors;
            survivors = obj;
        } else {
            LispObject *free_head = lisp_free_list_for_tag(tag);
            *(LispObject *)ptr = *free_head;
            *free_head = obj;
            freed++;
        }
        obj = next;
    }
    lisp_all_objects_head = survivors;
    return freed;
}

// マーク＆スイープGCを1回実行し、回収したオブジェクト数を返す。評価中の一時的な中間値を
// 正確に追跡できないため（`documents/lisp_robustness.md`のスコープ外項目）、これはCの
// ローカル変数だけが指す中間オブジェクトが存在しない安全地点（REPLループの先頭、または
// 明示的な(gc)呼び出し）でのみ呼び出すこと
UINTN lisp_gc(void) {
    lisp_gc_mark_roots();
    return lisp_gc_sweep();
}

// milestone 34: vm_stackがlisp_gc_mark_rootsのルート集合に正しく含まれているかを検証する。
// vm_stack以外からは全く参照されない新規consを積んでGCを実行し、その後さらにconsをいくつも
// 確保してフリーリストの再利用を強制する。ルート統合が正しければobjのcar/cdrはGCの前後で
// 変化せず、誤っていればobjのセルがフリーリストへ戻り後続の確保で上書きされて検出できる
int lisp_vm_gc_root_selftest(void) {
    LispObject obj = lisp_cons(lisp_make_fixnum(111), lisp_make_fixnum(222));
    vm_stack[0] = obj;
    vm_sp = 1;
    lisp_gc();
    vm_sp = 0; // 検証専用の一時的な積み込みなので、確認前に戻しておく

    for (UINTN i = 0; i < 64; i++) {
        lisp_cons(lisp_make_fixnum(0), lisp_make_fixnum(0));
    }

    if (!lisp_is_cons(obj)) {
        return 0;
    }
    return lisp_car(obj) == lisp_make_fixnum(111) && lisp_cdr(obj) == lisp_make_fixnum(222);
}

// symをvalueに束縛したペアをenvの先頭に追加した新しい環境を返す
LispObject lisp_env_extend(LispObject env, LispObject sym, LispObject value) {
    return lisp_cons(lisp_cons(sym, value), env);
}

// alist envをsymで線形探索し、見つかった値を返す。無ければlisp_panic。
// envの中に見つからなければ、現在のglobal_envも探す。defunで作られたクロージャは
// 自分自身の束縛が追加される前のenvスナップショットを保持しているため、これが無いと
// 再帰呼び出しや、後から定義された他のdefun関数の呼び出しがすべてunbound variableに
// なってしまう
LispObject lisp_env_lookup(LispObject env, LispObject sym) {
    // 動的変数(milestone 18)はenvチェーンに束縛を積まず、シンボル自身が持つvalueが
    // 常に「今のダイナミックエクステントでの値」を表すため、alist探索より先に見る
    if (lisp_symbol_cell(sym)->is_special) {
        return lisp_symbol_cell(sym)->value;
    }

    LispObject cur = env;
    while (lisp_is_cons(cur)) {
        LispObject pair = lisp_car(cur);
        if (lisp_car(pair) == sym) {
            return lisp_cdr(pair);
        }
        cur = lisp_cdr(cur);
    }
    if (env != global_env) {
        cur = global_env;
        while (lisp_is_cons(cur)) {
            LispObject pair = lisp_car(cur);
            if (lisp_car(pair) == sym) {
                return lisp_cdr(pair);
            }
            cur = lisp_cdr(cur);
        }
    }
    lisp_panic(L"unbound variable");
}

// symの既存の束縛を破壊的に書き換える（milestone 17のsetq用）。envチェーンを先に探し、
// 見つからなければlisp_env_lookupと同じ考え方でglobal_envも探す。どちらにも束縛が
// 無ければlisp_env_lookupと同じくunbound variableとしてpanicする（新規変数の暗黙定義はしない）
void lisp_env_set(LispObject env, LispObject sym, LispObject value) {
    // 動的変数はlisp_env_lookupと同様、alistを経由せずシンボル自身のvalueを直接書き換える
    if (lisp_symbol_cell(sym)->is_special) {
        lisp_symbol_cell(sym)->value = value;
        return;
    }

    LispObject cur = env;
    while (lisp_is_cons(cur)) {
        LispObject pair = lisp_car(cur);
        if (lisp_car(pair) == sym) {
            lisp_set_cdr(pair, value);
            return;
        }
        cur = lisp_cdr(cur);
    }
    if (env != global_env) {
        cur = global_env;
        while (lisp_is_cons(cur)) {
            LispObject pair = lisp_car(cur);
            if (lisp_car(pair) == sym) {
                lisp_set_cdr(pair, value);
                return;
            }
            cur = lisp_cdr(cur);
        }
    }
    lisp_panic(L"unbound variable");
}

// paramsとargsを1対1で対応付けてenvに順に束縛していく（個数不一致はpanic）
LispObject lisp_env_bind_params(LispObject params, LispObject args, LispObject env) {
    while (lisp_is_cons(params)) {
        if (!lisp_is_cons(args)) {
            lisp_panic(L"too few arguments");
        }
        env = lisp_env_extend(env, lisp_car(params), lisp_car(args));
        params = lisp_cdr(params);
        args = lisp_cdr(args);
    }
    // milestone 29: 仮引数リストがsymbol一つだけ（(lambda args ...)/(defun f args ...)）
    // の場合、残りの実引数をリストのままそのsymbolへ束縛する（可変長引数）。
    // (a b . rest)のドット対記法はリーダーが対応していないため、この「仮引数全体が
    // 単一のsymbol」という形だけをサポートする
    if (params != LISP_NIL && lisp_is_symbol(params)) {
        env = lisp_env_extend(env, params, args);
        return env;
    }
    if (args != LISP_NIL) {
        lisp_panic(L"too many arguments");
    }
    return env;
}

LispObject lisp_eval(LispObject expr, LispObject env);

// リストの各要素をenvで評価し、結果のリストを返す（関数呼び出しの引数評価に使う）
LispObject lisp_eval_list(LispObject list, LispObject env) {
    if (!lisp_is_cons(list)) {
        return LISP_NIL;
    }
    LispObject head = lisp_eval(lisp_car(list), env);
    if (lisp_return_tag != LISP_NIL) {
        return head; // milestone 19: 非局所脱出中は残りの引数を評価せずそのまま伝播する
    }
    LispObject tail = lisp_eval_list(lisp_cdr(list), env);
    return lisp_cons(head, tail);
}

// フォーム列を順にenvで評価し、最後の値を返す（formsが無ければnilを返す）。
// milestone 17のprogn本体、およびlet/let*/when/unless/condの各本体評価に共通で使う
LispObject lisp_eval_progn(LispObject forms, LispObject env) {
    LispObject result = LISP_NIL;
    while (lisp_is_cons(forms)) {
        result = lisp_eval(lisp_car(forms), env);
        if (lisp_return_tag != LISP_NIL) {
            return result; // milestone 19: 非局所脱出中は残りのformを評価せずそのまま伝播する
        }
        forms = lisp_cdr(forms);
    }
    return result;
}

// クロージャfnをargs(評価済みリスト)に適用する。builtinならCの実装関数を直接呼ぶ
LispObject lisp_apply(LispObject fn, LispObject args) {
    if (!lisp_is_closure(fn)) {
        lisp_panic(L"attempt to call a non-function");
    }
    LispClosure *closure = lisp_closure_cell(fn);
    if (closure->builtin != 0) {
        return closure->builtin(args);
    }
    LispObject call_env = lisp_env_bind_params(closure->params, args, closure->env);
    return lisp_eval(closure->body, call_env);
}

// (macroexpand-1 form): マクロ呼び出しの外側の呼び出し1回分だけを展開する (milestone 21)。
// これまでlisp_evalのマクロ呼び出し分岐に直接埋め込まれていた「呼び出し式を評価してマクロ
// クロージャか確認し、本体を評価して展開結果を得る」処理を独立した関数として切り出したもので、
// lisp_eval自身と組み込みmacroexpand-1の両方がこれを呼ぶ。マクロ呼び出しでなければexprを
// そのまま（同じオブジェクトとして、eqが真になる形で）返す。多値が無いこのLispでは
// CLの2番目の戻り値`expanded-p`を返せないため、代わりに`(eq form (macroexpand-1 form))`で
// 「展開されなかった」ことを判定できる、という設計上の約束にしている。
// 実際の展開処理そのものは*macroexpand-hook*（デフォルトはlisp_default_macroexpand_hook）に
// 委譲する
LispObject lisp_macroexpand_1(LispObject expr, LispObject env) {
    if (!lisp_is_cons(expr)) {
        return expr;
    }

    LispObject op = lisp_car(expr);
    LispObject fn = lisp_eval(op, env);
    if (lisp_return_tag != LISP_NIL) {
        return fn; // milestone 19: 呼び出し式自体の評価中に脱出した
    }

    if (!lisp_is_closure(fn) || !lisp_closure_cell(fn)->is_macro) {
        return expr; // マクロ呼び出しではない
    }

    LispObject hook = lisp_symbol_cell(lisp_sym_macroexpand_hook)->value;
    LispObject hook_args = lisp_cons(fn, lisp_cons(expr, lisp_cons(env, LISP_NIL)));
    return lisp_apply(hook, hook_args);
}

// リストaの末尾にリストbを連結した新しいリストを返す（aは破壊しない）。
// quasiquoteの",@"展開でのみ使う内部ヘルパー
LispObject lisp_append(LispObject a, LispObject b) {
    if (!lisp_is_cons(a)) {
        return b;
    }
    return lisp_cons(lisp_car(a), lisp_append(lisp_cdr(a), b));
}

// quasiquoteのテンプレートformを再帰的に展開する。ネストしたquasiquote自体は
// 特別扱いしない（1段のbackquoteのみを想定した単純化。内側にquasiquoteが
// 現れる場合、その中のunquote/unquote-splicingも同じ深さのまま展開されるため、
// 標準的なLisp処理系のネスト深度追跡とは異なる挙動になる点に注意）。
// - formが(unquote x)そのものならenvでxを評価した結果を返す
// - リストの要素が(unquote-splicing x)なら、評価結果（リスト）を周囲に継ぎ足す
// - それ以外のconsは car/cdrを再帰的に展開して再構築する
// - cons以外（nil・fixnum・symbol・closure）はそのまま返す（自己クオート）
LispObject lisp_qq_expand(LispObject form, LispObject env) {
    if (!lisp_is_cons(form)) {
        return form;
    }

    LispObject head = lisp_car(form);

    if (head == lisp_sym_unquote) {
        return lisp_eval(lisp_car(lisp_cdr(form)), env);
    }

    if (lisp_is_cons(head) && lisp_car(head) == lisp_sym_unquote_splicing) {
        LispObject spliced = lisp_eval(lisp_car(lisp_cdr(head)), env);
        if (lisp_return_tag != LISP_NIL) {
            return spliced; // milestone 19: ,@内でのreturn-fromをそのまま伝播する
        }
        LispObject rest = lisp_qq_expand(lisp_cdr(form), env);
        return lisp_append(spliced, rest);
    }

    LispObject expanded_head = lisp_qq_expand(head, env);
    if (lisp_return_tag != LISP_NIL) {
        return expanded_head; // milestone 19
    }
    LispObject expanded_tail = lisp_qq_expand(lisp_cdr(form), env);
    return lisp_cons(expanded_head, expanded_tail);
}

// exprをenv上で評価する。fixnum/nil/tは自己評価、symbolは変数参照、
// consはquote/if/progn/let/let*/setq/cond/and/or/when/unless/defvar/defparameter/
// lambdaなどの特殊形式または関数呼び出しとして扱う
LispObject lisp_eval(LispObject expr, LispObject env) {
    if (lisp_is_fixnum(expr) || expr == LISP_NIL) {
        return expr;
    }

    if (lisp_is_symbol(expr)) {
        LispSymbol *cell = lisp_symbol_cell(expr);
        // milestone 23: keywordパッケージのシンボルはtと同様に自己評価する
        if (expr == lisp_sym_t || (cell->package != 0 && cell->package->is_keyword_package)) {
            return expr;
        }
        return lisp_env_lookup(env, expr);
    }

    if (lisp_is_cons(expr)) {
        LispObject op = lisp_car(expr);

        if (op == lisp_sym_quote) {
            return lisp_car(lisp_cdr(expr));
        }

        if (op == lisp_sym_quasiquote) {
            return lisp_qq_expand(lisp_car(lisp_cdr(expr)), env);
        }

        if (op == lisp_sym_if) {
            LispObject test = lisp_eval(lisp_car(lisp_cdr(expr)), env);
            if (lisp_return_tag != LISP_NIL) {
                return test; // milestone 19
            }
            LispObject rest = lisp_cdr(lisp_cdr(expr));
            if (test != LISP_NIL) {
                return lisp_eval(lisp_car(rest), env);
            }
            LispObject else_rest = lisp_cdr(rest);
            if (else_rest == LISP_NIL) {
                return LISP_NIL;
            }
            return lisp_eval(lisp_car(else_rest), env);
        }

        if (op == lisp_sym_progn) {
            return lisp_eval_progn(lisp_cdr(expr), env);
        }

        if (op == lisp_sym_let) {
            LispObject bindings = lisp_car(lisp_cdr(expr));
            LispObject body = lisp_cdr(lisp_cdr(expr));
            // フェーズ1: 各初期値式をletの外側のenvで評価する（束縛済みの他の変数を
            // 参照できない、let*との違い）。動的変数の値もこの時点ではまだ書き換えない
            // ため、他のbindingの初期値式が古い値を見るという並列束縛の意味を保つ
            LispObject values = LISP_NIL; // (sym . value)のリスト（順序はどうでもよい）
            LispObject cur = bindings;
            while (lisp_is_cons(cur)) {
                LispObject binding = lisp_car(cur);
                LispObject value = lisp_eval(lisp_car(lisp_cdr(binding)), env);
                if (lisp_return_tag != LISP_NIL) {
                    // milestone 19: まだ何も束縛・書き換えていないのでそのまま伝播してよい
                    return value;
                }
                values = lisp_cons(lisp_cons(lisp_car(binding), value), values);
                cur = lisp_cdr(cur);
            }
            // フェーズ2: 動的変数はシンボル自身のvalueを退避してから書き換え、
            // 通常の変数はenvに積む
            LispObject new_env = env;
            LispObject saved_specials = LISP_NIL; // (sym . 退避した旧値)のリスト
            cur = values;
            while (lisp_is_cons(cur)) {
                LispObject pair = lisp_car(cur);
                LispObject sym = lisp_car(pair);
                LispObject value = lisp_cdr(pair);
                if (lisp_symbol_cell(sym)->is_special) {
                    saved_specials = lisp_cons(lisp_cons(sym, lisp_symbol_cell(sym)->value), saved_specials);
                    lisp_symbol_cell(sym)->value = value;
                } else {
                    new_env = lisp_env_extend(new_env, sym, value);
                }
                cur = lisp_cdr(cur);
            }
            LispObject result = lisp_eval_progn(body, new_env);
            // letを抜ける際、動的変数はletに入る前の値へ必ず復元する
            cur = saved_specials;
            while (lisp_is_cons(cur)) {
                LispObject pair = lisp_car(cur);
                lisp_symbol_cell(lisp_car(pair))->value = lisp_cdr(pair);
                cur = lisp_cdr(cur);
            }
            return result;
        }

        if (op == lisp_sym_let_star) {
            LispObject bindings = lisp_car(lisp_cdr(expr));
            LispObject body = lisp_cdr(lisp_cdr(expr));
            LispObject new_env = env;
            LispObject saved_specials = LISP_NIL; // (sym . 退避した旧値)のリスト
            LispObject cur = bindings;
            LispObject result = LISP_NIL;
            int aborted = 0; // milestone 19: 初期値式の評価中に非局所脱出が起きたか
            // let*は各初期値式をそれまでに束縛した変数（動的変数を含む）が見える環境で
            // 評価する。動的変数は書き換えが即座に反映されるため、new_envに積まなくても
            // 以降の初期値式や本体から自然に見える
            while (lisp_is_cons(cur)) {
                LispObject binding = lisp_car(cur);
                LispObject sym = lisp_car(binding);
                LispObject value = lisp_eval(lisp_car(lisp_cdr(binding)), new_env);
                if (lisp_return_tag != LISP_NIL) {
                    // すでに書き換えた動的変数がある可能性があるため、ループを抜けて
                    // 下のsaved_specials復元処理を必ず通してから伝播する
                    result = value;
                    aborted = 1;
                    break;
                }
                if (lisp_symbol_cell(sym)->is_special) {
                    saved_specials = lisp_cons(lisp_cons(sym, lisp_symbol_cell(sym)->value), saved_specials);
                    lisp_symbol_cell(sym)->value = value;
                } else {
                    new_env = lisp_env_extend(new_env, sym, value);
                }
                cur = lisp_cdr(cur);
            }
            if (!aborted) {
                result = lisp_eval_progn(body, new_env);
            }
            cur = saved_specials;
            while (lisp_is_cons(cur)) {
                LispObject pair = lisp_car(cur);
                lisp_symbol_cell(lisp_car(pair))->value = lisp_cdr(pair);
                cur = lisp_cdr(cur);
            }
            return result;
        }

        if (op == lisp_sym_setq) {
            LispObject sym = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(sym);
            LispObject value = lisp_eval(lisp_car(lisp_cdr(lisp_cdr(expr))), env);
            if (lisp_return_tag != LISP_NIL) {
                return value; // milestone 19: 代入前に脱出。書き換えは行わない
            }
            lisp_env_set(env, sym, value);
            return value;
        }

        if (op == lisp_sym_cond) {
            LispObject clauses = lisp_cdr(expr);
            while (lisp_is_cons(clauses)) {
                LispObject clause = lisp_car(clauses);
                LispObject test = lisp_eval(lisp_car(clause), env);
                if (lisp_return_tag != LISP_NIL) {
                    return test; // milestone 19
                }
                if (test != LISP_NIL) {
                    LispObject body = lisp_cdr(clause);
                    if (body == LISP_NIL) {
                        return test; // 本体が無いクローズはtestの値そのものを返す
                    }
                    return lisp_eval_progn(body, env);
                }
                clauses = lisp_cdr(clauses);
            }
            return LISP_NIL; // どのclauseもマッチしなかった
        }

        if (op == lisp_sym_and) {
            LispObject forms = lisp_cdr(expr);
            LispObject result = lisp_sym_t; // (and)はtを返す
            while (lisp_is_cons(forms)) {
                result = lisp_eval(lisp_car(forms), env);
                if (lisp_return_tag != LISP_NIL) {
                    return result; // milestone 19
                }
                if (result == LISP_NIL) {
                    return LISP_NIL; // 短絡評価: 以降のformは評価しない
                }
                forms = lisp_cdr(forms);
            }
            return result;
        }

        if (op == lisp_sym_or) {
            LispObject forms = lisp_cdr(expr);
            while (lisp_is_cons(forms)) {
                LispObject result = lisp_eval(lisp_car(forms), env);
                if (lisp_return_tag != LISP_NIL) {
                    return result; // milestone 19
                }
                if (result != LISP_NIL) {
                    return result; // 短絡評価: 最初にnilでない値が出た時点で返す
                }
                forms = lisp_cdr(forms);
            }
            return LISP_NIL; // (or)はnilを返す
        }

        if (op == lisp_sym_when) {
            LispObject test = lisp_eval(lisp_car(lisp_cdr(expr)), env);
            if (lisp_return_tag != LISP_NIL) {
                return test; // milestone 19
            }
            if (test == LISP_NIL) {
                return LISP_NIL;
            }
            return lisp_eval_progn(lisp_cdr(lisp_cdr(expr)), env);
        }

        if (op == lisp_sym_unless) {
            LispObject test = lisp_eval(lisp_car(lisp_cdr(expr)), env);
            if (lisp_return_tag != LISP_NIL) {
                return test; // milestone 19
            }
            if (test != LISP_NIL) {
                return LISP_NIL;
            }
            return lisp_eval_progn(lisp_cdr(lisp_cdr(expr)), env);
        }

        if (op == lisp_sym_block) {
            LispObject tag = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(tag);
            LispObject body = lisp_cdr(lisp_cdr(expr));
            LispObject result = lisp_eval_progn(body, env);
            if (lisp_return_tag == tag) {
                // 自分宛のシグナルを捕捉する。他のタグ宛（外側のblock用）ならreturn_tagを
                // 残したままresultをそのまま返し、呼び出し元へ伝播させる
                lisp_return_tag = LISP_NIL;
                return lisp_return_value;
            }
            return result;
        }

        if (op == lisp_sym_return_from) {
            LispObject tag = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(tag);
            LispObject value_forms = lisp_cdr(lisp_cdr(expr));
            LispObject value = (value_forms == LISP_NIL) ? LISP_NIL : lisp_eval(lisp_car(value_forms), env);
            if (lisp_return_tag != LISP_NIL) {
                // value式の評価中に別のreturn-fromが先に発生した場合は、そちらを優先して
                // そのまま伝播する（このreturn-fromの本体は実行されなかったことになる）
                return value;
            }
            lisp_return_tag = tag;
            lisp_return_value = value;
            return value;
        }

        if (op == lisp_sym_lambda) {
            LispObject params = lisp_car(lisp_cdr(expr));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(expr)));
            return lisp_make_closure(params, body, env);
        }

        if (op == lisp_sym_defvar) {
            LispObject sym = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(sym);
            LispSymbol *cell = lisp_symbol_cell(sym);
            // すでに動的変数として値を持つ場合は上書きしない（defvarをファイルの再loadで
            // 再度実行しても状態が保たれる、というCommon Lispのdefvarと同じ挙動）
            if (!cell->is_special) {
                LispObject value = lisp_eval(lisp_car(lisp_cdr(lisp_cdr(expr))), env);
                if (lisp_return_tag != LISP_NIL) {
                    return value; // milestone 19: 代入前に脱出。is_specialを立てない
                }
                cell->value = value;
                cell->is_special = 1;
            }
            return sym;
        }

        if (op == lisp_sym_defparameter) {
            LispObject sym = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(sym);
            LispObject value = lisp_eval(lisp_car(lisp_cdr(lisp_cdr(expr))), env);
            if (lisp_return_tag != LISP_NIL) {
                return value; // milestone 19
            }
            LispSymbol *cell = lisp_symbol_cell(sym);
            // defvarと異なり既存の値があっても常に上書きする
            cell->value = value;
            cell->is_special = 1;
            return sym;
        }

        if (op == lisp_sym_defun) {
            LispObject name = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(name);
            LispObject params = lisp_car(lisp_cdr(lisp_cdr(expr)));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(expr))));
            LispObject closure = lisp_make_closure(params, body, env);
            // 同名の再定義でも既存の束縛を消さず先頭に追加するだけでよい。
            // lisp_env_lookupは先頭から線形探索するため新しい束縛が優先される
            global_env = lisp_env_extend(global_env, name, closure);
            return name;
        }

        if (op == lisp_sym_defmacro) {
            LispObject name = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(name);
            LispObject params = lisp_car(lisp_cdr(lisp_cdr(expr)));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(expr))));
            LispObject macro = lisp_make_macro(params, body, env);
            global_env = lisp_env_extend(global_env, name, macro);
            return name;
        }

        LispObject fn = lisp_eval(op, env);
        if (lisp_return_tag != LISP_NIL) {
            return fn; // milestone 19: 呼び出す関数式自体の評価中に脱出した
        }

        // マクロ呼び出しは引数を評価せず、未評価の式のまま仮引数に束縛して
        // マクロ本文を評価する（マクロ展開、milestone 21で切り出したlisp_macroexpand_1に
        // 委譲）。展開結果は呼び出し元のenvで通常のevalにかける（2段階評価）
        if (lisp_is_closure(fn) && lisp_closure_cell(fn)->is_macro) {
            LispObject expansion = lisp_macroexpand_1(expr, env);
            if (lisp_return_tag != LISP_NIL) {
                return expansion; // milestone 19: マクロ展開自体の評価中に脱出した
            }
            return lisp_eval(expansion, env);
        }

        LispObject args = lisp_eval_list(lisp_cdr(expr), env);
        if (lisp_return_tag != LISP_NIL) {
            return args; // milestone 19: 引数評価中に脱出。呼び出しは行わない
        }
        return lisp_apply(fn, args);
    }

    if (lisp_is_closure(expr)) {
        return expr;
    }

    lisp_panic(L"cannot evaluate this object");
}

// REPLの1行、またはloadの1トップレベル式としてexprをglobal_env上で評価する。
// return-fromの脱出シグナルが対応するblockに一度も捕捉されずここまで残っている場合、
// タグが指す実行中のblockが存在しないというユーザー側の誤りなのでpanicする。
// これをせずに素通しすると、残ったシグナルが次の入力の評価を最初の一歩で
// 打ち切ってしまい、以降すべての評価が無言で壊れる (milestone 19)
LispObject lisp_eval_toplevel(LispObject expr) {
    LispObject result = lisp_eval(expr, global_env);
    if (lisp_return_tag != LISP_NIL) {
        lisp_panic(L"return-from: no enclosing block for this tag");
    }
    return result;
}


// --- 組み込みプリミティブ (milestone 10) ---

LispObject lisp_builtin_car(LispObject args) {
    return lisp_car(lisp_car(args));
}

LispObject lisp_builtin_cdr(LispObject args) {
    return lisp_cdr(lisp_car(args));
}

LispObject lisp_builtin_cons(LispObject args) {
    return lisp_cons(lisp_car(args), lisp_car(lisp_cdr(args)));
}

LispObject lisp_builtin_eq(LispObject args) {
    LispObject a = lisp_car(args);
    LispObject b = lisp_car(lisp_cdr(args));
    return (a == b) ? lisp_sym_t : LISP_NIL;
}

LispObject lisp_builtin_atom(LispObject args) {
    return lisp_is_cons(lisp_car(args)) ? LISP_NIL : lisp_sym_t;
}

// (rplaca cons-cell new-car): cons-cellのcarをnew-carへ破壊的に書き換え、cons-cell自身を
// 返す（milestone 27。CommonLispのrplacaと同じ「書き換えたコンスセル自身を返す」仕様で、
// svsetがvalueを返すのとは異なる）。既存のlisp_set_carがlisp_assert_consを内包しているため
// 追加の型検証は不要
LispObject lisp_builtin_rplaca(LispObject args) {
    LispObject cell = lisp_car(args);
    LispObject value = lisp_car(lisp_cdr(args));
    lisp_set_car(cell, value);
    return cell;
}

// (rplacd cons-cell new-cdr): cons-cellのcdrをnew-cdrへ破壊的に書き換え、cons-cell自身を返す
LispObject lisp_builtin_rplacd(LispObject args) {
    LispObject cell = lisp_car(args);
    LispObject value = lisp_car(lisp_cdr(args));
    lisp_set_cdr(cell, value);
    return cell;
}

// hash-codeが返す値がlisp_make_fixnumで常に有効な非負fixnumになるようにするための
// マスク（下位61bit。62bit目以降を捨てることで符号ビットの復元を保証する。milestone28）
#define LISP_HASH_CODE_MASK 0x1FFFFFFFFFFFFFFFULL

// (hash-code object): objectのタグ付きポインタ表現（fixnumなら値そのもの、ヒープ
// オブジェクトならアドレス）の下位61bitを取り出しfixnumとして返す、アイデンティティ
// ベースのハッシュ関数（milestone 28）。eqは`==`によるビット比較そのものなので、
// 「eqなら同じhash-code」という不変条件は型ごとの分岐を書かずに自動的に満たされる。
// マスクによる情報落ちで別オブジェクトが同じ値になる（衝突する）ことはハッシュ関数
// として許容範囲であり、一意性は保証しない
LispObject lisp_builtin_hash_code(LispObject args) {
    LispObject obj = lisp_car(args);
    long long h = (long long)(((UINT64)obj) & LISP_HASH_CODE_MASK);
    return lisp_make_fixnum(h);
}

// milestone 22: fixnum/bignum/floatの型混在に対応（昇格規則はlisp_num_add参照）
LispObject lisp_builtin_add(LispObject args) {
    LispObject acc = lisp_make_fixnum(0);
    LispObject cur = args;
    while (lisp_is_cons(cur)) {
        LispObject v = lisp_car(cur);
        lisp_assert_number(v);
        acc = lisp_num_add(acc, v);
        cur = lisp_cdr(cur);
    }
    return acc;
}

LispObject lisp_builtin_sub(LispObject args) {
    if (!lisp_is_cons(args)) {
        lisp_panic(L"- requires at least 1 argument");
    }
    LispObject first = lisp_car(args);
    lisp_assert_number(first);
    LispObject cur = lisp_cdr(args);
    if (!lisp_is_cons(cur)) {
        return lisp_num_negate(first); // 単項: 符号反転
    }
    LispObject acc = first;
    while (lisp_is_cons(cur)) {
        LispObject v = lisp_car(cur);
        lisp_assert_number(v);
        acc = lisp_num_sub(acc, v);
        cur = lisp_cdr(cur);
    }
    return acc;
}

// aが負数かどうかを判定する（fixnum/float/bignumの3表現に対応）。milestone22の
// lisp_num_addなどと同じ「is_float→is_bignum→fixnum」の判定順に合わせる
static int lisp_number_is_negative(LispObject a) {
    if (lisp_is_float(a)) {
        return lisp_closure_cell(a)->float_value < 0.0;
    }
    if (lisp_is_bignum(a)) {
        return lisp_closure_cell(a)->big_negative;
    }
    return lisp_fixnum_value(a) < 0;
}

// (< a b c ...): 隣接するペアがすべてa<bを満たせばt、そうでなければnilを返す
// （CLの多引数<と同じ「単調増加」判定）。差の符号をlisp_number_is_negativeで見るだけで
// fixnum/bignum/floatの型混在をすべてlisp_num_subの既存の昇格規則に委譲できる。milestone
// 29でこれ以外の比較演算子（>/=/<=/>=/zerop等）はstdlib.lisp側でこの<から導出する
LispObject lisp_builtin_lt(LispObject args) {
    LispObject cur = args;
    lisp_assert_number(lisp_car(cur));
    while (lisp_is_cons(lisp_cdr(cur))) {
        LispObject a = lisp_car(cur);
        LispObject b = lisp_car(lisp_cdr(cur));
        lisp_assert_number(b);
        LispObject diff = lisp_num_sub(a, b);
        if (!lisp_number_is_negative(diff)) {
            return LISP_NIL;
        }
        cur = lisp_cdr(cur);
    }
    return lisp_sym_t;
}

// (gc): マーク＆スイープGCを手動発火する。検証・デバッグ用（milestone33の自動発火は
// main.cのREPLループ先頭でヒープ使用率がしきい値を超えた時のみ走る）。引数は無視し、
// 回収したオブジェクト数をfixnumで返す
LispObject lisp_builtin_gc(LispObject args) {
    return lisp_make_fixnum((long long)lisp_gc());
}

// (gensym)または(gensym "prefix"): 呼ぶたびに一意な非intern済みシンボルを返す。
// 名前は「prefix（省略時は"G"）+ カウンタの10進数字」で、マクロ展開時の変数捕捉回避用の
// 名前として読みやすくする以外の意味は持たない（一意性そのものはlisp_make_uninterned_symbolが
// テーブルに登録しないことで保証しており、この名前文字列自体に依存しない）
static UINTN lisp_gensym_counter = 0;

LispObject lisp_builtin_gensym(LispObject args) {
    const char *prefix = "G";
    if (lisp_is_cons(args)) {
        LispObject prefix_obj = lisp_car(args);
        lisp_assert_string(prefix_obj);
        prefix = lisp_closure_cell(prefix_obj)->str_data;
    }

    char name[LISP_SYMBOL_NAME_MAX];
    UINTN i = 0;
    while (prefix[i] != '\0' && i < LISP_SYMBOL_NAME_MAX - 1) {
        name[i] = prefix[i];
        i++;
    }

    // カウンタを10進数字に変換する（lisp_print_fixnumと同様、下位桁から積んで逆順に書き出す）
    char digits[24];
    UINTN dcount = 0;
    UINTN n = lisp_gensym_counter;
    do {
        digits[dcount] = '0' + (char)(n % 10);
        dcount++;
        n /= 10;
    } while (n > 0);
    while (dcount > 0 && i < LISP_SYMBOL_NAME_MAX - 1) {
        dcount--;
        name[i] = digits[dcount];
        i++;
    }
    name[i] = '\0';

    lisp_gensym_counter++;
    return lisp_make_uninterned_symbol(name);
}

// (make-vector length) または (make-vector length fill): lengthの長さの新しいvectorを
// 作る（milestone 26）。fill省略時は各要素をnilで初期化する
LispObject lisp_builtin_make_vector(LispObject args) {
    LispObject len_obj = lisp_car(args);
    lisp_assert_fixnum(len_obj);
    long long len = lisp_fixnum_value(len_obj);
    if (len < 0) {
        lisp_panic(L"make-vector: length must not be negative");
    }

    LispObject fill = LISP_NIL;
    LispObject rest = lisp_cdr(args);
    if (lisp_is_cons(rest)) {
        fill = lisp_car(rest);
    }
    return lisp_make_vector((UINTN)len, fill);
}

// (svref vector index): index番目の要素を返す。範囲外は即座にpanicする
// （condition systemが無く、範囲外を素通しさせるとヒープ上の隣接オブジェクトを
// 誤って読むことになるため、他のlisp_assert_*・heap exhausted等と同じ
// 「不変条件違反は即panic」方針に合わせる）
LispObject lisp_builtin_svref(LispObject args) {
    LispObject vec = lisp_car(args);
    lisp_assert_vector(vec);
    LispObject idx_obj = lisp_car(lisp_cdr(args));
    lisp_assert_fixnum(idx_obj);
    long long idx = lisp_fixnum_value(idx_obj);

    LispClosure *cell = lisp_closure_cell(vec);
    if (idx < 0 || (UINTN)idx >= cell->vec_len) {
        lisp_panic(L"svref: index out of range");
    }
    return cell->vec_data[idx];
}

// (svset vector index value): index番目の要素をvalueに破壊的に書き換え、
// valueを返す（CommonLispの(setf (svref vector index) value)相当）
LispObject lisp_builtin_svset(LispObject args) {
    LispObject vec = lisp_car(args);
    lisp_assert_vector(vec);
    LispObject idx_obj = lisp_car(lisp_cdr(args));
    lisp_assert_fixnum(idx_obj);
    LispObject value = lisp_car(lisp_cdr(lisp_cdr(args)));
    long long idx = lisp_fixnum_value(idx_obj);

    LispClosure *cell = lisp_closure_cell(vec);
    if (idx < 0 || (UINTN)idx >= cell->vec_len) {
        lisp_panic(L"svset: index out of range");
    }
    cell->vec_data[idx] = value;
    return value;
}

// milestone 46: compile-expr(Lisp側)が生成したbytecode/constants/upvalue-descsの
// リストから実際のVMコンパイル済み関数を組み立てる検証用ブリッジ。固定長のCローカル配列に
// 一旦積んでからlisp_make_compiled等へ渡す（load時のlisp_load_bufferと同じ「静的/固定長の
// スクラッチ領域を使う」方針）。constants引数の各要素は呼び出し元(lisp/stdlib.lispの
// vm-materialize-constants)が事前に再帰展開済みである前提で、ここではネストしたlambdaの
// テンプレートリストをそれ以上辿らない。
// milestone 48時点の値（256/64/32）はmilestone35〜39の手動バイトコード検証にしか耐えず、
// フェーズ2で実stdlib.lisp関数をコンパイルするようになると容易に超過するため、
// milestone 49でこの値まで拡張した（各呼び出しのCローカル配列なので、拡張分は再帰的な
// ネストしたクロージャのコンパイル数だけスタック使用量が増える点に注意）
#define VM_BRIDGE_MAX_BYTECODE 2048
#define VM_BRIDGE_MAX_CONSTANTS 256
#define VM_BRIDGE_MAX_UPVALUES 128

// vm-make-closureの各固定長バッファが超過した際、上限値も含めて診断しやすいpanicメッセージを
// 組み立てる（milestone 49）。lisp_panicは固定のCHAR16*しか受け取れないため、ここでラベルと
// 上限値(16進)を1つのバッファへ連結してから渡す
static CHAR16 *lisp_char16_append(CHAR16 *dst, const CHAR16 *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = 0;
    return dst;
}

static void lisp_panic_vm_bridge_limit_exceeded(CHAR16 *what, UINTN limit) {
    CHAR16 hex[20];
    UINT64ToHexStr((UINT64)limit, hex);
    CHAR16 msg[96];
    CHAR16 *p = msg;
    p = lisp_char16_append(p, what);
    p = lisp_char16_append(p, L" (limit ");
    p = lisp_char16_append(p, hex);
    lisp_char16_append(p, L")");
    lisp_panic(msg);
}

// (vm-make-closure nargs bytecode-list constants-list upvalue-descs-list) -> 実行可能な
// コンパイル済み関数オブジェクト。upvalue-descs-listの各要素は(kind . index)のcons
LispObject lisp_builtin_vm_make_closure(LispObject args) {
    LispObject nargs_obj = lisp_car(args);
    LispObject bytecode_list = lisp_car(lisp_cdr(args));
    LispObject constants_list = lisp_car(lisp_cdr(lisp_cdr(args)));
    LispObject upvalue_descs_list = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(args))));

    lisp_assert_fixnum(nargs_obj);
    UINTN nargs = (UINTN)lisp_fixnum_value(nargs_obj);

    unsigned char bytecode[VM_BRIDGE_MAX_BYTECODE];
    UINTN bytecode_len = 0;
    LispObject cur = bytecode_list;
    while (lisp_is_cons(cur)) {
        if (bytecode_len >= VM_BRIDGE_MAX_BYTECODE) {
            lisp_panic_vm_bridge_limit_exceeded(L"vm-make-closure: bytecode too long", VM_BRIDGE_MAX_BYTECODE);
        }
        LispObject b = lisp_car(cur);
        lisp_assert_fixnum(b);
        bytecode[bytecode_len++] = (unsigned char)lisp_fixnum_value(b);
        cur = lisp_cdr(cur);
    }

    LispObject constants[VM_BRIDGE_MAX_CONSTANTS];
    UINTN constants_len = 0;
    cur = constants_list;
    while (lisp_is_cons(cur)) {
        if (constants_len >= VM_BRIDGE_MAX_CONSTANTS) {
            lisp_panic_vm_bridge_limit_exceeded(L"vm-make-closure: too many constants", VM_BRIDGE_MAX_CONSTANTS);
        }
        constants[constants_len++] = lisp_car(cur);
        cur = lisp_cdr(cur);
    }

    LispObject fn = lisp_make_compiled(bytecode, bytecode_len, constants, constants_len, nargs);

    UINTN kinds[VM_BRIDGE_MAX_UPVALUES];
    UINTN indices[VM_BRIDGE_MAX_UPVALUES];
    UINTN upvalue_count = 0;
    cur = upvalue_descs_list;
    while (lisp_is_cons(cur)) {
        if (upvalue_count >= VM_BRIDGE_MAX_UPVALUES) {
            lisp_panic_vm_bridge_limit_exceeded(L"vm-make-closure: too many upvalue descriptors", VM_BRIDGE_MAX_UPVALUES);
        }
        LispObject desc = lisp_car(cur);
        LispObject kind_obj = lisp_car(desc);
        LispObject index_obj = lisp_cdr(desc);
        lisp_assert_fixnum(kind_obj);
        lisp_assert_fixnum(index_obj);
        kinds[upvalue_count] = (UINTN)lisp_fixnum_value(kind_obj);
        indices[upvalue_count] = (UINTN)lisp_fixnum_value(index_obj);
        upvalue_count++;
        cur = lisp_cdr(cur);
    }
    if (upvalue_count > 0) {
        lisp_compiled_set_upvalue_descs(fn, lisp_make_upvalue_descs(kinds, indices, upvalue_count));
    }

    return fn;
}

// (vm-exec fn): vm-make-closureが返したコンパイル済み関数を引数無しの新規フレームとして
// 実行し、結果を返す（lisp_vm_execの薄いラッパー）
LispObject lisp_builtin_vm_exec(LispObject args) {
    return lisp_vm_exec(lisp_car(args));
}

// *macroexpand-hook*のデフォルト値 (milestone 21)。実際の展開処理（マクロの仮引数への
// 束縛→本文評価）はここに閉じている。呼び出し規約はCLの*macroexpand-hook*
// （展開器・フォーム・環境の3引数）に合わせたが、このLispのマクロはCLの「関数」とは異なり
// ユーザー定義マクロのLispClosureそのものを1番目の引数（展開器）として直接渡す。
// 3番目のenv引数はCLとの見た目を合わせるためだけに受け取り実際には使わない
// （macroletが無く、マクロは常に自分の定義時の環境macro->envに閉じているため、
// 呼び出し元の環境は展開結果に影響しない）
LispObject lisp_default_macroexpand_hook(LispObject args) {
    LispObject macro_obj = lisp_car(args);
    LispObject form = lisp_car(lisp_cdr(args));
    LispClosure *macro = lisp_closure_cell(macro_obj);
    LispObject expand_env = lisp_env_bind_params(macro->params, lisp_cdr(form), macro->env);
    return lisp_eval(macro->body, expand_env);
}

// (macroexpand-1 form): formがマクロ呼び出しならlisp_macroexpand_1で1段展開した結果を返す。
// マクロ呼び出しでなければformをそのまま返す。展開結果を実行せずに確認できるため、
// マクロのデバッグ用途に使う。CLのmacroexpand-1と異なり、このLispにはmacroletが無く
// レキシカルなマクロ環境という概念自体が存在しないため、2番目のenv引数は受け取らず
// 常にglobal_envで展開する
LispObject lisp_builtin_macroexpand_1(LispObject args) {
    LispObject form = lisp_car(args);
    return lisp_macroexpand_1(form, global_env);
}

// milestone 16: (load "filename") がFAT32のESPから読み込んだファイル内容を
// バッファ終端までトップレベルS式として順にglobal_envで評価する。最後に評価した
// 値（ファイルが空ならlisp_sym_t）を返す
// bufの全フォームを先に読み切ってconsのリストへ積んでから評価する。読み取り中の
// evalが（load組み込みなどを介して）bufと同じ静的スクラッチバッファへ再度書き込む
// ことがあるため、読み取りと評価を1フォームずつ交互に行うとbuf自体が上書きされ
// 残りのフォームが破壊される（milestone47のinit.lispがネストしたloadを呼ぶ際に発覚）
static LispObject lisp_load_eval_buffer(const char *buf) {
    lisp_reader_pos = buf;
    LispObject forms = LISP_NIL;
    while (!lisp_reader_at_end()) {
        LispObject form = lisp_read();
        forms = lisp_cons(form, forms);
    }

    LispObject result = lisp_sym_t;
    LispObject reversed = LISP_NIL;
    while (forms != LISP_NIL) {
        reversed = lisp_cons(lisp_car(forms), reversed);
        forms = lisp_cdr(forms);
    }
    while (reversed != LISP_NIL) {
        result = lisp_eval_toplevel(lisp_car(reversed));
        reversed = lisp_cdr(reversed);
    }
    return result;
}

static EFI_GUID lisp_guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID_VALUE;
static EFI_GUID lisp_guid_simple_file_system = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID_VALUE;

// milestone 41: lisp/stdlib.lispがコメントを含めて8192byteを超えたため、無警告で
// 末尾が読み捨てられる(truncateされてもlisp_load_eval_buffer側はEOFとして正常終了する
// ため検知できない)事故を防ぐ目的で32768byteへ拡張した。milestone46でstdlib.lispが
// 再び32768byteを超えたため65536byteへ再拡張した(同じ理由の再発)
#define LISP_LOAD_BUFFER_MAX 65536
static char lisp_load_buffer[LISP_LOAD_BUFFER_MAX];

// (load)と(write-file)共通: EfiMainのImageHandle→LoadedImage→DeviceHandle→
// SimpleFileSystemの順にHandleProtocolでたどり、ESPのルートディレクトリを開く
static EFI_FILE_PROTOCOL *lisp_open_esp_root(void) {
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    if (bs->HandleProtocol(g_image_handle, &lisp_guid_loaded_image, (void **)&loaded_image) != 0) {
        lisp_panic(L"failed to get loaded image protocol");
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    if (bs->HandleProtocol(loaded_image->DeviceHandle, &lisp_guid_simple_file_system, (void **)&fs) != 0) {
        lisp_panic(L"failed to get simple file system protocol");
    }

    EFI_FILE_PROTOCOL *root;
    if (fs->OpenVolume(fs, &root) != 0) {
        lisp_panic(L"failed to open volume");
    }
    return root;
}

// (load "filename"): ESPのルートディレクトリからfilenameを読み込んで内容を評価する。
// GetInfoでファイルサイズを問い合わせず、1回のReadで静的バッファに収まる分だけ読み取る
// （CLAUDE.mdの静的バッファ方針に従う）
LispObject lisp_builtin_load(LispObject args) {
    LispObject filename_obj = lisp_car(args);
    lisp_assert_string(filename_obj);
    LispClosure *filename = lisp_closure_cell(filename_obj);

    CHAR16 wpath[LISP_STRING_READ_MAX];
    UINTN i = 0;
    for (; i < filename->str_len && i < LISP_STRING_READ_MAX - 1; i++) {
        wpath[i] = (CHAR16)filename->str_data[i];
    }
    wpath[i] = 0;

    EFI_FILE_PROTOCOL *root = lisp_open_esp_root();

    EFI_FILE_PROTOCOL *file;
    if (root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0) != 0) {
        lisp_panic(L"load: failed to open file");
    }
    root->Close(root);

    UINTN size = LISP_LOAD_BUFFER_MAX - 1;
    if (file->Read(file, &size, lisp_load_buffer) != 0) {
        lisp_panic(L"load: failed to read file");
    }
    lisp_load_buffer[size] = '\0';
    file->Close(file);

    return lisp_load_eval_buffer(lisp_load_buffer);
}

// (write-file "filename" content): ESPのルートディレクトリにfilenameを新規作成
// （既存ならEFI_FILE_MODE_CREATEにより開いて上書き）し、Lisp文字列contentの
// str_data/str_lenをそのままEFI_FILE_WRITEへ渡す。テスト結果をファイルへ書き出す
// ことで、ホスト側はesp_dirを監視するだけでテストの終了・成否（PASS/FAIL等）を
// 確認できるようになる（シリアル出力のパースが不要になる）
LispObject lisp_builtin_write_file(LispObject args) {
    LispObject filename_obj = lisp_car(args);
    LispObject content_obj = lisp_car(lisp_cdr(args));
    lisp_assert_string(filename_obj);
    lisp_assert_string(content_obj);
    LispClosure *filename = lisp_closure_cell(filename_obj);
    LispClosure *content = lisp_closure_cell(content_obj);

    CHAR16 wpath[LISP_STRING_READ_MAX];
    UINTN i = 0;
    for (; i < filename->str_len && i < LISP_STRING_READ_MAX - 1; i++) {
        wpath[i] = (CHAR16)filename->str_data[i];
    }
    wpath[i] = 0;

    EFI_FILE_PROTOCOL *root = lisp_open_esp_root();

    EFI_FILE_PROTOCOL *file;
    if (root->Open(root, &file, wpath,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) != 0) {
        lisp_panic(L"write-file: failed to open file");
    }
    root->Close(root);

    UINTN size = content->str_len;
    if (file->Write(file, &size, content->str_data) != 0) {
        lisp_panic(L"write-file: failed to write file");
    }
    file->Close(file);

    return lisp_sym_t;
}

// (write-line "text"): 文字列をそのままシリアルコンソールへ改行付きで出力する。
// write-fileはQEMUのvvfat(fat:rw:)が書き込みをホストへコミットする際に内容を
// 破損させる問題があり自動テストの結果通知には使えないため、代わりにこちらを使う。
// REPLの評価結果の自動echoとは別に、テスト側が明示的にPASS/FAIL等の区切りを
// 出力できるようにする
LispObject lisp_builtin_write_line(LispObject args) {
    LispObject content_obj = lisp_car(args);
    lisp_assert_string(content_obj);
    LispClosure *content = lisp_closure_cell(content_obj);

    LispOutputStream stream = lisp_make_console_stream(g_system_table);
    lisp_print_ascii(&stream, content->str_data);
    lisp_print_ascii(&stream, "\r\n");

    return lisp_sym_t;
}

// milestone 29: EfiMainが起動時に標準ライブラリファイルを読み込むための入口。
// lisp_builtin_loadはLisp文字列オブジェクトの引数リストを要求するので、
// Cの文字列リテラルからそれを組み立てるだけの薄いラッパー
void lisp_load_boot_file(const char *filename) {
    UINTN len = 0;
    while (filename[len] != '\0') {
        len++;
    }
    LispObject args = lisp_cons(lisp_make_string(filename, len), LISP_NIL);
    lisp_builtin_load(args);
}

// milestone47: REPL開始直前に、BOOTX64.EFIと同じディレクトリ(EFI/BOOT/)にある
// init.lispをユーザ配置ファイルとして読み込む。pythonでシリアルを操作せずに
// write-lineでテスト結果を出力する自動テストを起動時に実行できるようにする目的。
// lisp_builtin_loadと違い、ファイルが存在しない場合はpanicせず何もしない
// （init.lispの配置は任意なため）
void lisp_load_init_file(void) {
    EFI_FILE_PROTOCOL *root = lisp_open_esp_root();

    EFI_FILE_PROTOCOL *file;
    if (root->Open(root, &file, L"EFI\\BOOT\\init.lisp", EFI_FILE_MODE_READ, 0) != 0) {
        root->Close(root);
        return;
    }
    root->Close(root);

    UINTN size = LISP_LOAD_BUFFER_MAX - 1;
    if (file->Read(file, &size, lisp_load_buffer) != 0) {
        lisp_panic(L"init.lisp: failed to read file");
    }
    lisp_load_buffer[size] = '\0';
    file->Close(file);

    lisp_load_eval_buffer(lisp_load_buffer);
}

// (sleep seconds): CreateEvent(EVT_TIMER)で使い捨てのタイマーイベントを作り、
// SetTimer(TimerRelative)でseconds秒後(UEFIネイティブ単位=100ns)に発火するよう
// セットし、WaitForEventでブロックして待つ。secondsはfixnum/bignum/floatいずれも
// 受け付ける(milestone22の数値タワーのlisp_number_to_doubleを再利用、CLのsleepと
// 同じ「秒単位・小数可」の意味を保つ)
LispObject lisp_builtin_sleep(LispObject args) {
    LispObject seconds_obj = lisp_car(args);
    lisp_assert_number(seconds_obj);
    double seconds = lisp_number_to_double(seconds_obj);
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    UINT64 trigger_time = (UINT64)(seconds * 10000000.0); // 100ns単位

    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;

    EFI_EVENT timer_event;
    if (bs->CreateEvent(EVT_TIMER, TPL_APPLICATION, (void *)0, (void *)0, &timer_event) != 0) {
        lisp_panic(L"sleep: failed to create timer event");
    }
    if (bs->SetTimer(timer_event, TimerRelative, trigger_time) != 0) {
        lisp_panic(L"sleep: failed to set timer");
    }
    UINTN index;
    if (bs->WaitForEvent(1, &timer_event, &index) != 0) {
        lisp_panic(L"sleep: failed to wait for event");
    }
    bs->CloseEvent(timer_event);

    return LISP_NIL;
}

// car/cdr/cons/eq/atom/+/-/load をグローバル環境に束縛して返す
LispObject lisp_builtins_init(void) {
    LispObject env = LISP_NIL;
    env = lisp_env_extend(env, lisp_intern("car"), lisp_make_builtin(lisp_builtin_car));
    env = lisp_env_extend(env, lisp_intern("cdr"), lisp_make_builtin(lisp_builtin_cdr));
    env = lisp_env_extend(env, lisp_intern("cons"), lisp_make_builtin(lisp_builtin_cons));
    env = lisp_env_extend(env, lisp_intern("eq"), lisp_make_builtin(lisp_builtin_eq));
    env = lisp_env_extend(env, lisp_intern("atom"), lisp_make_builtin(lisp_builtin_atom));
    env = lisp_env_extend(env, lisp_intern("rplaca"), lisp_make_builtin(lisp_builtin_rplaca));
    env = lisp_env_extend(env, lisp_intern("rplacd"), lisp_make_builtin(lisp_builtin_rplacd));
    env = lisp_env_extend(env, lisp_intern("hash-code"), lisp_make_builtin(lisp_builtin_hash_code));
    env = lisp_env_extend(env, lisp_intern("+"), lisp_make_builtin(lisp_builtin_add));
    env = lisp_env_extend(env, lisp_intern("-"), lisp_make_builtin(lisp_builtin_sub));
    env = lisp_env_extend(env, lisp_intern("<"), lisp_make_builtin(lisp_builtin_lt));
    env = lisp_env_extend(env, lisp_intern("load"), lisp_make_builtin(lisp_builtin_load));
    env = lisp_env_extend(env, lisp_intern("write-file"), lisp_make_builtin(lisp_builtin_write_file));
    env = lisp_env_extend(env, lisp_intern("write-line"), lisp_make_builtin(lisp_builtin_write_line));
    env = lisp_env_extend(env, lisp_intern("sleep"), lisp_make_builtin(lisp_builtin_sleep));
    env = lisp_env_extend(env, lisp_intern("gensym"), lisp_make_builtin(lisp_builtin_gensym));
    env = lisp_env_extend(env, lisp_intern("gc"), lisp_make_builtin(lisp_builtin_gc));
    env = lisp_env_extend(env, lisp_intern("make-vector"), lisp_make_builtin(lisp_builtin_make_vector));
    env = lisp_env_extend(env, lisp_intern("svref"), lisp_make_builtin(lisp_builtin_svref));
    env = lisp_env_extend(env, lisp_intern("svset"), lisp_make_builtin(lisp_builtin_svset));
    env = lisp_env_extend(env, lisp_intern("macroexpand-1"), lisp_make_builtin(lisp_builtin_macroexpand_1));
    env = lisp_env_extend(env, lisp_intern("vm-make-closure"), lisp_make_builtin(lisp_builtin_vm_make_closure));
    env = lisp_env_extend(env, lisp_intern("vm-exec"), lisp_make_builtin(lisp_builtin_vm_exec));

    // *macroexpand-hook*をdefvarと同じ形（is_special=1 + 初期値）で直接セットアップする
    // (milestone 21)。動的変数はenvチェーンに束縛を積まないため、global_envへの
    // lisp_env_extendは不要
    LispSymbol *hook_cell = lisp_symbol_cell(lisp_sym_macroexpand_hook);
    hook_cell->value = lisp_make_builtin(lisp_default_macroexpand_hook);
    hook_cell->is_special = 1;

    return env;
}

// --- 大脱出機構 (milestone 30) ---
// 通常のC関数だとGCCが自動でプロローグ(push rbp; mov rsp,rbp等)を生成し、
// 呼び出し元のレジスタ値がasm実行前に書き換わってしまう。x86_64のGCCには
// 信頼できるnaked属性が無いため、ファイルスコープの生アセンブリで関数全体を
// 手書きし、プロローグ生成を完全に回避する(musl等のlibcと同じ手法)。
// このターゲット(x86_64-w64-mingw32-gcc、-mabi指定なし)はMS x64呼び出し
// 規約がデフォルトのため、第1引数はrcx、第2引数はrdxに渡る(System Vのrdi/rsi
// ではない)。lisp_jmp_bufのフィールドoffset: rbx=0, rbp=8, rdi=16, rsi=24,
// rsp=32, r12=40, r13=48, r14=56, r15=64, rip=72 (計80byte、src/lisp.hと一致させる)
__asm__(
    ".global lisp_setjmp\n"
    "lisp_setjmp:\n"
    "    movq %rbx, 0(%rcx)\n"
    "    movq %rbp, 8(%rcx)\n"
    "    movq %rdi, 16(%rcx)\n"
    "    movq %rsi, 24(%rcx)\n"
    "    leaq 8(%rsp), %rax\n"      // callへの`ret`で戻った直後のrsp
    "    movq %rax, 32(%rcx)\n"
    "    movq %r12, 40(%rcx)\n"
    "    movq %r13, 48(%rcx)\n"
    "    movq %r14, 56(%rcx)\n"
    "    movq %r15, 64(%rcx)\n"
    "    movq (%rsp), %rax\n"       // callが積んだ戻り先アドレス
    "    movq %rax, 72(%rcx)\n"
    "    xorl %eax, %eax\n"         // 初回は0を返す
    "    ret\n"
    ".global lisp_longjmp\n"
    "lisp_longjmp:\n"
    "    movq 72(%rcx), %r8\n"      // 戻り先rip (スクラッチ)
    "    movq 32(%rcx), %r9\n"      // 復元後のrsp (スクラッチ)
    "    movq 0(%rcx), %rbx\n"
    "    movq 8(%rcx), %rbp\n"
    "    movq 16(%rcx), %rdi\n"
    "    movq 24(%rcx), %rsi\n"
    "    movq 40(%rcx), %r12\n"
    "    movq 48(%rcx), %r13\n"
    "    movq 56(%rcx), %r14\n"
    "    movq 64(%rcx), %r15\n"
    "    movl %edx, %eax\n"         // 2回目の"戻り値" (valを渡す第2引数edx→eax)
    "    movq %r9, %rsp\n"          // スタックポインタを復元(この後はcall/ret前提のスタック操作は不可)
    "    jmpq *%r8\n"               // setjmp呼び出し箇所へ直接ジャンプ(retではなくjmp)
);
