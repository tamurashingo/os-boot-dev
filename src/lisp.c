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

// milestone 32: gc_next/gc_markedは3構造体(LispCons/LispSymbol/LispClosure)共通で
// 先頭2フィールドとして揃える。lisp_alloc_trackedがオフセット0/sizeof(LispObject)を
// 型を問わず直接読み書きするため、この順序・型を崩さないこと
typedef struct {
    LispObject gc_next;  // 確保順の全オブジェクト追跡リストへの次ポインタ
    int gc_marked;        // milestone33のmark&sweep用mark bit（本milestoneでは常に0）
    LispObject car;
    LispObject cdr;
} LispCons;

typedef struct {
    LispObject gc_next;   // milestone 32: LispConsと同じ追跡リスト用（先頭2フィールド揃え必須）
    int gc_marked;
    char name[LISP_SYMBOL_NAME_MAX];
    int is_special;      // milestone 18: defvar/defparameterで真になる動的変数フラグ
    LispObject value;    // is_specialが真の場合の現在の動的値。let/let*が退避・書き換えする
    LispObject package;  // milestone 23: 属するパッケージ。gensymの未interned symbolはLISP_NIL。
                          // milestone 68でLispPackageを廃しLispClosureのescape hatchに統合、
                          // milestone 70でタグ付きLispObjectへ変更した（生ポインタは廃止）
    LispObject fn;        // milestone 93: 関数セル（Lisp-2化の土台）。未束縛はLISP_NIL。
                          // valueと独立した名前空間で、symbol-function/%set-symbol-functionが
                          // 読み書きする。defun等の書き込み先切替はmilestone94で行う。
} LispSymbol;

typedef LispObject (*LispBuiltinFn)(LispObject args);

typedef struct LispClosure {
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
    UINTN max_locals;        // milestone83/84: このフレームがOP_MAKE_LOCALで使う可能性のある
                               // ローカルスロットの総数（仮引数nargs個を含む）。呼び出し直後に
                               // vm_stack[fp..fp+max_locals)を丸ごと確保することで、ローカル
                               // 変数領域とその後の一時値用データスタック領域を分離する
                               // （非compiled/非VM用途のclosureでは常に0）
    LispObject upvalue_descs; // milestone38: テンプレート側。捕捉元の記述子ベクタ（各要素は
                               // (kind . index)のcons。kind=0ならOP_MAKE_CLOSURE実行時の
                               // 呼び出し元フレームのローカルboxをFP+index経由で捕捉、
                               // kind=1なら呼び出し元closure自身のupvalues[index]を
                               // そのまま伝播（多段capture flattening）。非closure/未使用ならNIL
    LispObject upvalues;      // milestone38: インスタンス側。OP_MAKE_CLOSUREが実際に捕捉した
                               // box参照を格納するベクタ（lisp_make_vector互換）。テンプレート
                               // 自身はNILのまま
    char *pkg_name; // milestone 68: NULLならパッケージではない。非NULLならパッケージ名（0終端）
    LispObject pkg_symbols; // パッケージ内にinternされた全シンボルのconsリスト（milestone71統合）
    LispObject pkg_exports; // exportされたシンボルのconsリスト（milestone76まで常にNIL）
    LispObject pkg_uses;    // use-packageしている他パッケージのconsリスト（milestone77まで常にNIL）
    int pkg_is_keyword;     // 真なら自己評価し印字時に":"を前置する特別なパッケージ
    LispObject pkg_nicknames; // milestone91: 別名文字列のconsリスト
    LispObject pkg_shadowing_symbols; // milestone92: shadow/shadowing-importで登録されたローカルシンボルのconsリスト（eq基準）
    int pkg_locked; // milestone110: 真ならlock-package済み。defun/setq等の書込サイトはmilestone111で
                     // このフラグを見てpanicする（読み取り専用操作・shadow/export/use-package等の
                     // メタ操作はロック対象外、CommonLispのpackage lockと同様の切り分け）
    LispObject class_name;         // milestone96: NILならclassではない。非NILならクラス名symbol
    LispObject class_superclass;   // 直接の親クラスオブジェクト（単一継承、無ければNIL）
    LispObject class_direct_slots; // このクラス自身が直接宣言したスロット名symbolのリスト
    LispObject class_all_slots;    // superclassのclass_all_slots ++ direct_slots（defclass時に計算・キャッシュ）
    LispObject inst_class;         // milestone96: NILならinstanceではない。非NILなら生成元クラスオブジェクト
    LispObject inst_slots;         // スロット値を保持する独立したvectorオブジェクト（inst自身のvec_dataとは別）
    LispObject gf_name;            // milestone97: NILならgeneric-functionではない。非NILなら総称関数名symbol
    LispObject gf_methods;         // ((specializer-list . method-closure) ...) のconsリスト
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

// パッケージも同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 68）
static inline int lisp_is_package(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->pkg_name != 0;
}

// class/instanceも同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 96）
static inline int lisp_is_class(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->class_name != LISP_NIL;
}

static inline int lisp_is_instance(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->inst_class != LISP_NIL;
}

// generic-functionも同じくLISP_TAG_CLOSUREを共有するescape hatch（milestone 97）
static inline int lisp_is_generic_function(LispObject obj) {
    return lisp_is_closure(obj) && lisp_closure_cell(obj)->gf_name != LISP_NIL;
}

static inline int lisp_is_number(LispObject obj) {
    return lisp_is_fixnum(obj) || lisp_is_float(obj) || lisp_is_bignum(obj);
}

static UINT64 lisp_heap_ptr;
static UINT64 lisp_heap_end;
static UINT64 lisp_heap_total; // milestone 33: lisp_heap_lowがヒープ使用率を判定するための総量
EFI_SYSTEM_TABLE *g_system_table; // panic時にConOutへ出力するため
EFI_HANDLE g_image_handle; // milestone 16: loadがHandleProtocolでファイルシステムを取得するため
EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *g_text_input_ex; // milestone 116: 未検出ならNULLのまま

// milestone 31: REPLループが設置したトラップ。NULLなら未設置
lisp_jmp_buf *lisp_active_trap = (void *)0;

// エラーメッセージを出力する。トラップが設置されていればそこへlisp_longjmpで
// 復帰し、REPLの次のプロンプトから継続できる。トラップ未設置（起動処理中など）
// の場合は従来通りfor(;;){}でハングする
// milestone 128: パニックメッセージ自体は従来通り直接ConOutへ書くが、その前に
// 画面バッファに溜まっている未flush分(VM命令ディスパッチループへ戻る前にツリー
// ウォーク経路(lisp_eval/lisp_apply)で書かれた内容等)を先にflushする。これにより
// panicメッセージが既存の出力より先に表示されてしまう順序崩れを防ぐ(未初期化なら
// lisp_screen_flush自身がdirty/pending_newlinesとも0のため何もせず即戻る)
void lisp_panic(CHAR16 *message) {
    lisp_screen_flush();
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"Lisp panic: ");
    lisp_screen_track_echoed_wstring(L"Lisp panic: ");
    g_system_table->ConOut->OutputString(g_system_table->ConOut, message);
    lisp_screen_track_echoed_wstring(message);
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"\r\n");
    lisp_screen_track_echoed_wstring(L"\r\n");
    if (lisp_active_trap != (void *)0) {
        lisp_longjmp(lisp_active_trap, 1);
    }
    for (;;) {}
}

// milestone 31: 固定容量資源の枯渇など、REPLに復帰しても安全に継続できない
// 致命的エラー用。トラップの有無に関わらず常にfor(;;){}でハングし続ける
// milestone 128: lisp_panicと同様、直接ConOutへ書く前に未flush分を先に反映する
void lisp_panic_fatal(CHAR16 *message) {
    lisp_screen_flush();
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// VMコンパイル済み関数オブジェクトを作る（milestone 34/35）。LISP_TAG_CLOSUREのescape hatchを
// milestone15/22/26と同様に再利用する。bytecode/constantsはどちらもstr_data/vec_dataと同じく
// 呼び出し元のバッファをヒープへコピーして持つ（呼び出し元の一時バッファを保持し続ける必要はない）。
// max_locals（milestone83/84）はnargs以上でなければならない（仮引数がスロット0..nargs-1を占め、
// let等が続くスロットを積み増していくため）
LispObject lisp_make_compiled(const unsigned char *bytecode, UINTN bytecode_len,
                               const LispObject *constants, UINTN constants_len, UINTN nargs,
                               UINTN max_locals) {
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
    closure->max_locals = max_locals;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
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

static UINTN lisp_cstrlen(const char *s) {
    UINTN len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

// milestone91: string designator(CL用語)の下地。stringはstr_data、symbol(keywordを含む)は
// そのname文字列をそのまま「文字列指定子」として扱う（この処理系にはchar型が無いためCLの
// character designatorは対象外）
static const char *lisp_string_designator_name(LispObject obj) {
    if (lisp_is_string(obj)) {
        return lisp_closure_cell(obj)->str_data;
    }
    if (lisp_is_symbol(obj)) {
        return lisp_symbol_cell(obj)->name;
    }
    lisp_panic(L"expected a string or symbol designator");
    return 0;
}

// milestone91: package designator(package本体/文字列/symbol・keyword)を名前の文字列へ
// 正規化する。lisp_find_package等の既存のconst char*ベースAPIをそのまま使うための下地
static const char *lisp_package_designator_name(LispObject obj) {
    if (lisp_is_package(obj)) {
        return lisp_closure_cell(obj)->pkg_name;
    }
    return lisp_string_designator_name(obj);
}

// milestone 68: パッケージ自体をLISP_TAG_CLOSUREのescape hatchを共有する第一級のGC管理
// オブジェクトにする（文字列/float/bignum/vector/コンパイル済み関数と同じパターン）。
// symbols/exports/usesは固定長配列ではなく最初からconsリストとして持つ（milestone71として
// 計画していたシンボル集合のconsリスト化は、パッケージ自体のヒープオブジェクト化と同時に
// 行うほうが手戻りが無いため本milestoneへ統合済み。LISP_MAX_SYMBOLSの容量上限もこの統合に
// より最初から存在しない）
static LispObject lisp_make_package_object(const char *name, int is_keyword_package) {
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
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    UINTN len = 0;
    while (name[len] != '\0') {
        len++;
    }
    char *buf = (char *)lisp_alloc(len + 1);
    for (UINTN i = 0; i < len; i++) {
        buf[i] = name[i];
    }
    buf[len] = '\0';
    closure->pkg_name = buf;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = is_keyword_package;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// milestone 69: パッケージの集合をconsリスト(global_packages)で管理する。milestone23〜68の
// 固定長配列(LISP_MAX_PACKAGES=8)による上限は撤廃した。common-lisp-user/keywordの2つで
// 始まるが、lisp_make_packageを追加で呼ぶだけで何個でも増やせる
static LispObject global_packages = LISP_NIL;
static LispObject lisp_cl_user_package;
static LispObject lisp_keyword_package;
// milestone96: 最小CLOSサブセットのクラスレジストリ。global_packagesと同型のfile-scope
// consリスト(symbolのeq線形探索)。lisp_gc_mark_rootsより前方で参照されるためここで宣言する
static LispObject global_classes = LISP_NIL;
// milestone 73: 現在のパッケージを指す動的変数*package*のシンボル。lisp_internが読む
// ため、lisp_packages_init内でlisp_intern自身より先にlisp_intern_in_packageで直接
// internし、is_special/valueをdefvarと同じ形で直接セットアップする（*macroexpand-hook*と
// 同じパターン）
static LispObject lisp_sym_package;

// milestone75の`find-package`ビルトインはこれをそのまま呼ぶ
static LispObject lisp_find_package(const char *name) {
    for (LispObject cur = global_packages; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject pkg = lisp_car(cur);
        LispClosure *pkg_cell = lisp_closure_cell(pkg);
        if (lisp_streq(pkg_cell->pkg_name, name)) {
            return pkg;
        }
        // milestone91: package-nicknamesもname同様に照合対象にする
        for (LispObject n = pkg_cell->pkg_nicknames; n != LISP_NIL; n = lisp_cdr(n)) {
            if (lisp_streq(lisp_closure_cell(lisp_car(n))->str_data, name)) {
                return pkg;
            }
        }
    }
    return LISP_NIL;
}

// 名前が重複していれば新規オブジェクトを作らず既存のものを返す（defvarの再load冪等性と同じ
// 考え方。将来defpackage/in-packageを含むファイルをloadで再読込しても状態が壊れないようにする）
static LispObject lisp_make_package(const char *name, int is_keyword_package) {
    LispObject existing = lisp_find_package(name);
    if (existing != LISP_NIL) {
        return existing;
    }
    LispObject pkg = lisp_make_package_object(name, is_keyword_package);
    global_packages = lisp_cons(pkg, global_packages);
    return pkg;
}

// milestone108: lisp_make_packageと違い、同名パッケージが既に存在する場合は黙って既存を
// 返さずpanicする。fork時の隔離パッケージ生成は「必ず新規の別オブジェクトである」ことが
// プロセス分離の前提そのものであり、黙った共有はその前提を静かに破壊してしまうため専用の
// 安全な作成経路を用意する
static LispObject lisp_make_package_strict(const char *name, int is_keyword_package) {
    if (lisp_find_package(name) != LISP_NIL) {
        lisp_panic(L"make-process: fork package name collision");
    }
    LispObject pkg = lisp_make_package_object(name, is_keyword_package);
    global_packages = lisp_cons(pkg, global_packages);
    return pkg;
}

// 同じ名前でintern済みのシンボルがpkg内にあれば同一のLispObjectを返す（eqで比較可能にする）。
// 無ければ新規に確保してpkgのconsリストへ追加する。比較・格納の両方で先にLISP_SYMBOL_NAME_MAX-1文字に
// 切り詰めてから扱うことで、name自体を切り詰めずにlisp_streqへ渡すと「格納済みの切り詰め済み名」
// と「今回渡された切り詰め前のnama」が食い違って毎回別シンボルを生成してしまう
// （呼ぶたびにeqが成立せずunbound variableになる）バグを避ける
// milestone92: lisp_intern_in_packageのphase a（自パッケージのローカル探索）を切り出した
// もの。shadow等、use先を経由せずローカルのみを見たい呼び出し元向け
static LispObject lisp_find_local_symbol(LispClosure *pkg_cell, const char *truncated_name) {
    for (LispObject cur = pkg_cell->pkg_symbols; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject existing = lisp_car(cur);
        LispSymbol *sym = lisp_symbol_cell(existing);
        if (lisp_streq(sym->name, truncated_name)) {
            return existing;
        }
    }
    return LISP_NIL;
}

// milestone92: lisp_intern_in_packageのphase c（新規シンボル作成）を切り出したもの。
// 既存の有無を確認せず常にpkgへ新規シンボルを作成・登録する（既存チェックは呼び出し元の責任）
static LispObject lisp_create_local_symbol(LispObject pkg, const char *truncated_name) {
    LispClosure *pkg_cell = lisp_closure_cell(pkg);
    LispSymbol *sym = (LispSymbol *)lisp_alloc_tracked(sizeof(LispSymbol), LISP_TAG_SYMBOL);
    UINTN i = 0;
    while (truncated_name[i] != '\0') {
        sym->name[i] = truncated_name[i];
        i++;
    }
    sym->name[i] = '\0';
    sym->is_special = 0;
    sym->value = LISP_NIL;
    sym->package = pkg;
    sym->fn = LISP_NIL;

    LispObject obj = ((LispObject)sym) | LISP_TAG_SYMBOL;
    pkg_cell->pkg_symbols = lisp_cons(obj, pkg_cell->pkg_symbols);
    return obj;
}

// nameをLISP_SYMBOL_NAME_MAX-1文字に切り詰めたバッファへ書き込む（比較・格納の両方で
// 先に切り詰めてから扱うことで、格納済みの切り詰め済み名と切り詰め前の名前が食い違って
// 毎回別シンボルを生成してしまうバグを避ける、milestone71由来の既存の制約）
static void lisp_truncate_symbol_name(const char *name, char *out) {
    UINTN len = 0;
    while (name[len] != '\0' && len < LISP_SYMBOL_NAME_MAX - 1) {
        out[len] = name[len];
        len++;
    }
    out[len] = '\0';
}

// milestone92: pkg_shadowing_symbolsにeqで含まれるかを見るだけの薄いヘルパー。
// use-packageの衝突チェック緩和・shadow/shadowing-importの重複追加防止の両方から使う
static int lisp_symbol_is_shadowing(LispClosure *pkg_cell, LispObject sym) {
    for (LispObject s = pkg_cell->pkg_shadowing_symbols; s != LISP_NIL; s = lisp_cdr(s)) {
        if (lisp_car(s) == sym) {
            return 1;
        }
    }
    return 0;
}

LispObject lisp_intern_in_package(LispObject pkg, const char *name) {
    char truncated[LISP_SYMBOL_NAME_MAX];
    lisp_truncate_symbol_name(name, truncated);

    LispClosure *pkg_cell = lisp_closure_cell(pkg);
    LispObject local = lisp_find_local_symbol(pkg_cell, truncated);
    if (local != LISP_NIL) {
        return local;
    }

    // milestone77: 自パッケージのローカルシンボルに無ければ、useしている各パッケージの
    // exportシンボルを探す。use-package側で名前衝突を弾いているため、複数のuse対象に
    // 同名が同時に存在することは無い前提で最初に見つかったものを返す
    for (LispObject u = pkg_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
        LispClosure *used_cell = lisp_closure_cell(lisp_car(u));
        for (LispObject e = used_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            LispObject exported = lisp_car(e);
            if (lisp_streq(lisp_symbol_cell(exported)->name, truncated)) {
                return exported;
            }
        }
    }

    return lisp_create_local_symbol(pkg, truncated);
}

void lisp_packages_init(void) {
    lisp_cl_user_package = lisp_make_package("common-lisp-user", 0);
    lisp_keyword_package = lisp_make_package("keyword", 1);

    // *package*自身はlisp_internではなくlisp_intern_in_packageで直接common-lisp-userへ
    // internする。lisp_internはこの後*package*の値を読むように切り替えるため、
    // ここで先に確立しておかないと*package*を確立するためのintern呼び出しが循環する
    lisp_sym_package = lisp_intern_in_package(lisp_cl_user_package, "*package*");
    LispSymbol *pkg_var_cell = lisp_symbol_cell(lisp_sym_package);
    pkg_var_cell->value = lisp_cl_user_package;
    pkg_var_cell->is_special = 1;
}

LispObject lisp_intern(const char *name) {
    return lisp_intern_in_package(lisp_symbol_cell(lisp_sym_package)->value, name);
}

static LispObject lisp_intern_keyword(const char *name) {
    return lisp_intern_in_package(lisp_keyword_package, name);
}

// milestone 70: LispSymbol.packageがLISP_NIL（uninterned）でないことを確認した上で
// 属するパッケージのpkg_is_keywordを読む。直接->package->pkg_is_keywordと書くと
// LISP_NILに対してlisp_closure_cellを呼んでしまう箇所が複数あったため一箇所に集約する
static int lisp_symbol_package_is_keyword(LispSymbol *sym) {
    return sym->package != LISP_NIL && lisp_closure_cell(sym->package)->pkg_is_keyword;
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
    sym->package = LISP_NIL;
    sym->fn = LISP_NIL;
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
static LispObject lisp_sym_do;
static LispObject lisp_sym_function;
static LispObject lisp_sym_macroexpand_hook;
static LispObject lisp_sym_compile_and_run;
static LispObject lisp_sym_lambda_optional;
static LispObject lisp_sym_lambda_rest;
static LispObject lisp_sym_lambda_key;
static LispObject lisp_sym_lambda_aux;
static LispObject lisp_sym_lambda_allow_other_keys;
static LispObject lisp_sym_print_object;

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
    lisp_sym_do = lisp_intern("do");
    lisp_sym_function = lisp_intern("function");
    lisp_sym_macroexpand_hook = lisp_intern("*macroexpand-hook*");
    // milestone 78 (バグ修正): compile-and-runは他の特殊形式シンボルと同様、*package*が
    // common-lisp-userの間にここで1度だけinternしてキャッシュする。lisp_eval_toplevel内で
    // 都度lisp_internし直すと、milestone73以降lisp_internが*package*依存になっているため、
    // in-packageで*package*を切り替えた後は毎回別オブジェクトが生成され、global_envの
    // 解決に失敗してunbound variableパニックになる(defmacro/rest-arg defun以外の
    // 全トップレベル評価が壊れる重大バグだった)
    lisp_sym_compile_and_run = lisp_intern("compile-and-run");
    // milestone 89: CommonLispラムダリストキーワード。&で始まるがreaderの区切り文字には
    // 含まれない（lisp_reader_is_delim参照）ため、他の特殊形式シンボルと同様に単なる
    // symbolとしてintern・キャッシュするだけでよい
    lisp_sym_lambda_optional = lisp_intern("&optional");
    lisp_sym_lambda_rest = lisp_intern("&rest");
    lisp_sym_lambda_key = lisp_intern("&key");
    lisp_sym_lambda_aux = lisp_intern("&aux");
    lisp_sym_lambda_allow_other_keys = lisp_intern("&allow-other-keys");
    // milestone 98: print-objectもcompile-and-run(m78)と同じ理由で*package*切替後も
    // 同一symbolを指し続けるようここで1度だけinternしてキャッシュする
    lisp_sym_print_object = lisp_intern("print-object");

    // milestone 100: *package*をcommon-lisp-user以外へ切り替えてもuse-package済みなら
    // defun/if/let等の特殊形式トークン（lisp_evalおよびlisp_compileがeq比較でディスパッチ
    // するシンボル群）を無修飾で使えるよう、common-lisp-userからexportしておく。
    // t（self-eval、lisp_eval側でeq比較される）と&optional等のラムダリストキーワード
    // （lambda/defunの引数リスト内でeq比較される）も同じ理由で対象に含める。
    // compile-and-run（トップレベル評価専用の内部トークンでユーザーが直接書くことはない）と
    // *macroexpand-hook*/print-object（特殊形式ではなく通常のグローバル束縛でmilestone101の
    // 対象）は対象外とする
    {
        LispObject special_form_syms[] = {
            lisp_sym_t, lisp_sym_quote, lisp_sym_if, lisp_sym_lambda, lisp_sym_defun,
            lisp_sym_defmacro, lisp_sym_quasiquote, lisp_sym_unquote, lisp_sym_unquote_splicing,
            lisp_sym_progn, lisp_sym_let, lisp_sym_let_star, lisp_sym_setq, lisp_sym_cond,
            lisp_sym_and, lisp_sym_or, lisp_sym_when, lisp_sym_unless, lisp_sym_defvar,
            lisp_sym_defparameter, lisp_sym_block, lisp_sym_return_from, lisp_sym_do,
            lisp_sym_function, lisp_sym_lambda_optional, lisp_sym_lambda_rest,
            lisp_sym_lambda_key, lisp_sym_lambda_aux, lisp_sym_lambda_allow_other_keys,
        };
        LispClosure *cl_user_cell = lisp_closure_cell(lisp_cl_user_package);
        for (UINTN i = 0; i < sizeof(special_form_syms) / sizeof(special_form_syms[0]); i++) {
            cl_user_cell->pkg_exports = lisp_cons(special_form_syms[i], cl_user_cell->pkg_exports);
        }
    }
}


// --- 文字入力 (milestone 6) ---
char input_buffer[LISP_INPUT_BUFFER_MAX];
UINTN input_length;

int lisp_double_ctrl_detected = 0;

// Enterキーまでの1行をキー入力から読み取り、input_bufferにASCII文字列として格納する。
// Backspaceは1文字削除して画面表示も戻す。UnicodeChar==0の制御キー(矢印キー等)は無視する。
// milestone135: g_text_input_ex(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL、milestone116で検出)が
// 見つかっていればReadKeyStrokeEx経由で読み取り、見つからない場合のみ既存のConIn経由へ
// フォールバックする。EFI_KEY_DATA.KeyはEFI_INPUT_KEYそのものなので、以降の文字分類・
// echo・backspace・Enter判定は経路にかかわらず共通のunicode_charに対して行う。
// milestone138: g_text_input_ex経由の場合のみ、KeyStateからCtrl単体押下(milestone116の
// lisp_key_state_is_lone_ctrl)を判定する。時間ウィンドウ方式(milestone136)を廃止し、
// 「1回目の検知でarmed状態に入り期限なく2回目を待つ」状態機械に変更した。armed中に2回目の
// Ctrl単体押下が来たら入力行を破棄してEnterと同様に即座に終了し、ワンショットのグローバル
// フラグlisp_double_ctrl_detectedを立てる(呼び出し元がこれをキャンセル/ダイアログ起動として
// 扱う)。armed中にCtrl単体以外のキーが来た場合はarmedを解除するだけで、そのキー自体は
// 通常どおり処理する(入力行は破棄しない)
void lisp_read_line(EFI_SYSTEM_TABLE *SystemTable) {
    input_length = 0;

    int ctrl_armed = 0;

    for (;;) {
        CHAR16 unicode_char;

        if (g_text_input_ex != (void *)0) {
            EFI_KEY_DATA key_data;
            EFI_STATUS status = g_text_input_ex->ReadKeyStrokeEx(g_text_input_ex, &key_data);
            if (status != 0) {
                continue; // EFI_NOT_READY: まだキー入力がない
            }

            if (lisp_key_state_is_lone_ctrl(&key_data)) {
                if (ctrl_armed) {
                    ctrl_armed = 0;
                    lisp_screen_show_ctrl_indicator(SystemTable, 0);
                    input_length = 0;
                    lisp_double_ctrl_detected = 1;
                    break;
                }
                ctrl_armed = 1;
                lisp_screen_show_ctrl_indicator(SystemTable, 1);
                continue;
            }

            if (ctrl_armed) {
                ctrl_armed = 0;
                lisp_screen_show_ctrl_indicator(SystemTable, 0);
            }

            unicode_char = key_data.Key.UnicodeChar;
        } else {
            EFI_INPUT_KEY key;
            EFI_STATUS status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key);
            if (status != 0) {
                continue; // EFI_NOT_READY: まだキー入力がない
            }
            unicode_char = key.UnicodeChar;
        }

        if (unicode_char == L'\r') {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
            lisp_screen_track_echoed_wstring(L"\r\n");
            break;
        }

        if (unicode_char == 8) { // Backspace
            if (input_length > 0) {
                input_length--;
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\b \b");
                lisp_screen_track_echoed_wstring(L"\b \b");
            }
            continue;
        }

        if (unicode_char == 0) {
            continue;
        }

        if (input_length < LISP_INPUT_BUFFER_MAX - 1) {
            input_buffer[input_length] = (char)unicode_char;
            input_length++;

            CHAR16 echo[2] = { unicode_char, 0 };
            SystemTable->ConOut->OutputString(SystemTable->ConOut, echo);
            lisp_screen_track_echoed_wstring(echo);
        }
    }

    input_buffer[input_length] = '\0';
}

// milestone 24: 出力ストリームのコンソール実装。milestone125で画面ダブルバッファ
// (milestone122〜124)経由に切り替え、1文字ずつlisp_screen_putcへ書き込む形にした。
// この関数の呼び出し単位でlisp_screen_flushする即時flushはmilestone127でVM命令
// ディスパッチループへの1命令ごとflushフックを追加した時点で削除済み(実転送は
// lisp_vm_runの次の命令フェッチ前に自動的に行われる。ツリーウォーク経路(lisp_eval/
// lisp_apply)から呼ばれた場合はこのフックを経由しないため、次にVM命令ループへ戻る
// までflushが遅延する既知の制約がある。境界でのflush統合はmilestone128で行う)。
// 初回呼び出し時にバッファが未初期化ならここで自動的に初期化する(起動シーケンスへの
// 明示的な統合はmilestone128で行う)
static void lisp_console_stream_write(void *ctx, const char *str) {
    (void)ctx;
    if (!lisp_screen_buffer_is_initialized()) {
        lisp_screen_buffer_init();
    }
    UINTN i = 0;
    while (str[i] != '\0') {
        lisp_screen_putc(str[i]);
        i++;
    }
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

// sym_objが対象パッケージ(pkg_cell)のpkg_exportsに含まれるか（eq基準）を調べる
// （milestone76のexport/milestone74のリーダー単一コロン修飾子と同じ線形探索）
static int lisp_symbol_exported_from(LispObject sym_obj, LispClosure *pkg_cell) {
    for (LispObject e = pkg_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
        if (lisp_car(e) == sym_obj) {
            return 1;
        }
    }
    return 0;
}

// sym_objがそのホームパッケージからexportされているかを判定する（milestone79、
// printerの単一コロン/二重コロン切り分けに使う。uninterned(gensym)symbolは
// ホームパッケージを持たないため常に0を返す）
static int lisp_symbol_is_exported(LispObject sym_obj) {
    LispSymbol *sym = lisp_symbol_cell(sym_obj);
    if (sym->package == LISP_NIL) {
        return 0;
    }
    return lisp_symbol_exported_from(sym_obj, lisp_closure_cell(sym->package));
}

// sym_objが現在の*package*から無修飾名で解決できる（＝可視）かを判定する（milestone79）。
// lisp_intern_in_package（milestone77）の探索順序「自パッケージのローカルシンボル→
// useしている各パッケージのexportシンボル」と対称な判定にする（printerがreaderの
// 逆変換になるようにするため）。uninterned(gensym)symbolはそもそもどのパッケージにも
// 属さないため、この判定を経由せず呼び出し側で無条件に無修飾表示する。
// milestone92: importで他パッケージ所属のシンボルをcur_pkgのpkg_symbolsへ追加しても、
// sym->package（home package）はimport元のままなので上のsym->package==cur_pkg比較・
// use先export探索だけでは可視と判定できない。cur_pkgのpkg_symbolsにeqで含まれるかの
// フォールバックを追加し、importしたシンボルも無修飾で印字されるようにする（既存の
// home-package一致による高速パスは変更しないため、import未使用時の動作に回帰は無い）
static int lisp_symbol_visible_in_current_package(LispObject sym_obj) {
    LispObject cur_pkg = lisp_symbol_cell(lisp_sym_package)->value;
    LispSymbol *sym = lisp_symbol_cell(sym_obj);
    if (sym->package == cur_pkg) {
        return 1;
    }
    LispClosure *cur_cell = lisp_closure_cell(cur_pkg);
    for (LispObject u = cur_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
        if (lisp_symbol_exported_from(sym_obj, lisp_closure_cell(lisp_car(u)))) {
            return 1;
        }
    }
    for (LispObject s = cur_cell->pkg_symbols; s != LISP_NIL; s = lisp_cdr(s)) {
        if (lisp_car(s) == sym_obj) {
            return 1;
        }
    }
    return 0;
}

// milestone111: symの帰属パッケージ(sym->package、呼び出し時の*package*ではなくホーム
// パッケージ)がlock-package(milestone110)済みかどうかを判定する。未帰属(uninterned/gensym、
// package==LISP_NIL)は常にfalse
static int lisp_symbol_home_package_locked(LispObject sym_obj) {
    LispSymbol *sym = lisp_symbol_cell(sym_obj);
    if (sym->package == LISP_NIL) {
        return 0;
    }
    return lisp_closure_cell(sym->package)->pkg_locked;
}

// milestone111: 既存の関数定義(fn != LISP_NIL)をロック済みパッケージ内で上書きしようとしたら
// panicする。新規定義(fn == LISP_NIL)は常に許可する("redefinition-only"のロック運用。これにより
// common-lisp-userを起動時にロックしても、既存テストフィクスチャ等がcommon-lisp-user内へ新規
// defunする分には影響しない)
static void lisp_check_function_redefine_allowed(LispObject sym) {
    if (lisp_symbol_cell(sym)->fn != LISP_NIL && lisp_symbol_home_package_locked(sym)) {
        lisp_panic(L"Package is locked");
    }
}

// milestone98: instance印字をprint-object総称関数へ委譲するための前方宣言
// (本来の定義は2965行目付近、m97でlisp_gf_select_method用に追加した前方宣言と同じ理由)
LispObject lisp_apply(LispObject fn, LispObject args);

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

    // milestone96: 既存はpackage種別すら専用分岐が無く#<closure>へ落ちていたが、
    // class/instanceはその抜けを繰り返さず専用に印字する
    if (lisp_is_class(obj)) {
        lisp_print_ascii(stream, "#<STANDARD-CLASS ");
        lisp_print_ascii(stream, lisp_symbol_cell(lisp_closure_cell(obj)->class_name)->name);
        lisp_print_ascii(stream, ">");
        return;
    }

    // milestone98: instanceの印字はprint-object総称関数へ委譲する。defmethod print-objectで
    // ユーザーがオーバーライド可能にするため（class/instance/generic-function自体のような
    // 非拡張の専用分岐ではなくす）。print-objectはlisp_builtins_init内で既定methodを
    // 登録済みのため(stdlib.lisp読込前に完了する)、instanceが存在し得る時点では常にbind済み
    if (lisp_is_instance(obj)) {
        LispObject print_object_gf = lisp_symbol_cell(lisp_sym_print_object)->fn;
        lisp_apply(print_object_gf, lisp_cons(obj, LISP_NIL));
        return;
    }

    // milestone97: generic-functionも同じ理由で専用に印字する（#<closure>へ落とさない）
    if (lisp_is_generic_function(obj)) {
        lisp_print_ascii(stream, "#<GENERIC-FUNCTION ");
        lisp_print_ascii(stream, lisp_symbol_cell(lisp_closure_cell(obj)->gf_name)->name);
        lisp_print_ascii(stream, ">");
        return;
    }

    // milestone99: packageはclass/instance/generic-functionと同じ非拡張の専用分岐で印字する
    // (specializerはユーザー定義クラスのみのためdefmethod print-objectでは上書きできない)
    if (lisp_is_package(obj)) {
        lisp_print_ascii(stream, "#<PACKAGE ");
        lisp_print_ascii(stream, lisp_closure_cell(obj)->pkg_name);
        lisp_print_ascii(stream, ">");
        return;
    }

    // milestone99: compiled-functionは名前情報を持たないため#<builtin>/#<macro>と同様の
    // 無名形式で印字する
    if (lisp_is_compiled(obj)) {
        lisp_print_ascii(stream, "#<COMPILED-FUNCTION>");
        return;
    }

    if (lisp_is_symbol(obj)) {
        LispSymbol *sym = lisp_symbol_cell(obj);
        // milestone 23: keywordパッケージのシンボルは":"を前置して印字する
        if (lisp_symbol_package_is_keyword(sym)) {
            lisp_print_ascii(stream, ":");
            lisp_print_ascii(stream, sym->name);
            return;
        }
        // milestone 79: ホームパッケージを持つ（uninterned/gensymでない）シンボルが
        // 現在の*package*から無修飾名で見えない場合、pkgname:symbol（exportされていれば）
        // またはpkgname::symbol（されていなければ）で修飾して印字する。gensymは常に
        // 無修飾のまま（既存milestone20の挙動を変更しない）
        if (sym->package != LISP_NIL && !lisp_symbol_visible_in_current_package(obj)) {
            lisp_print_ascii(stream, lisp_closure_cell(sym->package)->pkg_name);
            lisp_print_ascii(stream, lisp_symbol_is_exported(obj) ? ":" : "::");
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
    if (c == '#') {
        lisp_reader_pos++;
        // milestone 93: #'fooを(function foo)へ展開する。それ以外の#構文(#\/#(/#x等)は
        // 未実装のため即座にpanicする
        if (*lisp_reader_pos == '\'') {
            lisp_reader_pos++;
            return lisp_cons(lisp_sym_function, lisp_cons(lisp_read(), LISP_NIL));
        }
        lisp_panic(L"unsupported reader macro after #");
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

    // milestone 74: 先頭以外に現れる":"/"::"を"pkg:sym"/"pkg::sym"修飾子として検出する。
    // 文字スキャンループ自体（上のwhile）は無変更で、既に読み終えたtoken内を後から
    // 走査するだけで済む（":"はlisp_reader_is_delimの区切り文字ではないため、
    // "pkg:sym"は最初から1つのtokenとして捕捉されている）
    UINTN colon_pos = 0;
    while (token[colon_pos] != '\0' && token[colon_pos] != ':') {
        colon_pos++;
    }
    if (token[colon_pos] == ':') {
        char pkg_name[LISP_TOKEN_MAX];
        for (UINTN i = 0; i < colon_pos; i++) {
            pkg_name[i] = token[i];
        }
        pkg_name[colon_pos] = '\0';

        UINTN sym_start = colon_pos + 1;
        int internal = (token[sym_start] == ':');
        if (internal) {
            sym_start++;
        }
        const char *sym_name = token + sym_start;
        if (sym_name[0] == '\0') {
            lisp_panic(L"reader: malformed package-qualified symbol");
        }

        LispObject pkg = lisp_find_package(pkg_name);
        if (pkg == LISP_NIL) {
            lisp_panic(L"reader: unknown package in qualified symbol");
        }
        if (internal) {
            // pkg::symはexportされているかどうかを問わずpkg内へ直接intern/検索する
            return lisp_intern_in_package(pkg, sym_name);
        }
        // pkg:sym（単一コロン）はpkgのexportリストに載っているシンボルしか参照できない
        LispClosure *pkg_cell = lisp_closure_cell(pkg);
        for (LispObject cur = pkg_cell->pkg_exports; cur != LISP_NIL; cur = lisp_cdr(cur)) {
            LispObject exported = lisp_car(cur);
            if (lisp_streq(lisp_symbol_cell(exported)->name, sym_name)) {
                return exported;
            }
        }
        lisp_panic(L"reader: symbol not exported from package");
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

// milestone 74: "pkg:sym"/"pkg::sym"修飾子構文の自己テスト。export/use-packageの
// Lisp APIがまだ無いため、専用の使い捨てパッケージを1つ作りC内部APIで直接
// exportリストを組み立てる（既存のcommon-lisp-user/keywordパッケージやシンボル表を
// 汚さないための分離）
int lisp_reader_package_qualifier_selftest(void) {
    LispObject pkg = lisp_make_package("selftest-pkg74", 0);
    LispObject exported_sym = lisp_intern_in_package(pkg, "exported-sym");
    LispObject internal_sym = lisp_intern_in_package(pkg, "internal-sym");
    LispClosure *pkg_cell = lisp_closure_cell(pkg);
    pkg_cell->pkg_exports = lisp_cons(exported_sym, pkg_cell->pkg_exports);

    // "pkg:sym"（単一コロン）はexportされているシンボルと同一オブジェクトに解決される
    if (lisp_read_from_buffer("selftest-pkg74:exported-sym") != exported_sym) {
        return 0;
    }
    // "pkg::sym"（二重コロン）はexportされていないシンボルも同一オブジェクトに解決される
    if (lisp_read_from_buffer("selftest-pkg74::internal-sym") != internal_sym) {
        return 0;
    }
    // "pkg::sym"は未internのシンボルなら新規にpkgへinternする（=lisp_intern_in_packageと同じ）
    LispObject read_new = lisp_read_from_buffer("selftest-pkg74::brand-new-sym");
    if (read_new != lisp_intern_in_package(pkg, "brand-new-sym")) {
        return 0;
    }
    return 1;
}

// 空白を読み飛ばした上でバッファ終端(0終端)に達しているかを調べる。lisp_read自体は
// 単一のS式の読み取りを前提に終端をpanicとして扱うため変更せず、milestone 16のloadが
// 複数のトップレベルS式を読み終える判定にのみこれを使う
static int lisp_reader_at_end(void) {
    lisp_reader_skip_ws();
    return *lisp_reader_pos == '\0';
}


// --- 評価器 (milestone 9) ---

// トップレベルの永続グローバル環境 (milestone 12)。milestone94のLisp-2化により、
// defun/defmacro/組み込み関数の登録先はここではなく各symbolの関数セル(fn、milestone93)へ
// 変更されたため、global_envは以後「関数namespaceとは独立したグローバル変数」専用の
// 値namespace用alistとして残る（引数↔値のバインディング自体はマイルストーン9の
// lisp_env_bind_paramsのままで変更しない）
LispObject global_env = LISP_NIL;

// lisp_vm_run(milestone 51のOP_GLOBAL_REF/OP_GLOBAL_SET、milestone 52のOP_CALLフォールバック)が
// 定義順として先に来るため前方宣言する
LispObject lisp_env_lookup(LispObject env, LispObject sym);
void lisp_env_set(LispObject env, LispObject sym, LispObject value);
LispObject lisp_apply(LispObject fn, LispObject args);
// milestone97: lisp_applyの4番目の分岐(generic-function dispatch)が使うため、定義順として
// 後に来るlisp_gf_select_methodを前方宣言する
static LispObject lisp_gf_select_method(LispObject gf, LispObject args);

// 非局所脱出の進行中シグナル (milestone 19)。setjmp/longjmpが使えないため、
// return-fromが「対象タグ＋値」をここにセットしてから通常のLispObjectとして
// 呼び出し元へ返り、以降のすべての評価経路（lisp_eval_progn/lisp_eval_list/
// lisp_eval各分岐）はlisp_evalの戻り値を使う前にこのタグを確認し、セットされていれば
// 残りの評価をせず即座にそのまま呼び出し元へ伝播する。対応するblockがタグの一致を見て
// LISP_NILに戻すことでシグナルを捕捉する。LISP_NILは「脱出は発生していない」を表すため、
// blockのタグとしてLISP_NILそのもの（nil）を使うことはできない
static LispObject lisp_return_tag = LISP_NIL;
static LispObject lisp_return_value = LISP_NIL;

// milestone87の調査で発見したload時ヒープ枯渇の修正用ルート。lisp_load_eval_buffer
// は評価前にbuf全体を読み切ってconsリストへ積む設計（読み取りと評価を1フォームずつ
// 交互に行うとネストしたloadがbufを上書きして残りのフォームを破壊するため、milestone47
// で意図的に2段階にした）。この「評価待ちの残りフォームリスト」はCローカル変数にしか
// 存在せずlisp_gc_mark_rootsから辿れないため、フォーム間でlisp_gcを呼ぶとまだ評価して
// いない残りのフォームが刈られてしまう。ここに積んでいる間だけ一時的なGCルートとして
// 扱うことでその場を安全にする
static LispObject lisp_gc_extra_root = LISP_NIL;

// --- スタックマシン型VM (milestone 34) ---

// VMのデータスタック。固定長でグローバルに確保する（ヒープ確保ではなくバンプアロケータの
// 外側にある生配列。関数呼び出し前後のスタックフレームやOP_CALLの呼び出し規約もmilestone37以降
// ここに積む想定）。vm_spは次に値を積む位置（スタック上の要素数）を指し、vm_stack[0..vm_sp)が
// 現在有効な要素
// VM_STACK_SIZEはmilestone105でsrc/lisp.hへ移した（LispProcessStackのvm_stackフィールドと
// 同じ定数を共有するため）
static LispObject vm_stack[VM_STACK_SIZE];
static UINTN vm_sp = 0;

// milestone106: コルーチンyieldチェック用フック本体。宣言・意味の詳細はsrc/lisp.h参照。
// デフォルトはどちらもNULL（非武装）なので、既存のlisp_vm_run呼び出し経路は無変更のまま
LispProcessStack *lisp_vm_current_process = (void *)0;
LispProcessStack *lisp_vm_yield_target = (void *)0;
UINTN lisp_vm_yield_budget = (UINTN)-1;

// milestone107: lisp_gc_mark_rootsが追加で走査すべき「中断中の他プロセス」のレジストリ。
// 詳細な設計意図はsrc/lisp.h参照。固定長配列+件数で十分（今回のロードマップの想定プロセス数は
// 少数であり、可変長にする必要はない）
#define LISP_MAX_REGISTERED_PROCESS_STACKS 16
static LispProcessStack *lisp_registered_process_stacks[LISP_MAX_REGISTERED_PROCESS_STACKS];
static UINTN lisp_registered_process_stacks_count = 0;

void lisp_process_stack_register(LispProcessStack *ps) {
    if (lisp_registered_process_stacks_count >= LISP_MAX_REGISTERED_PROCESS_STACKS) {
        lisp_panic_fatal(L"too many registered process stacks");
    }
    lisp_registered_process_stacks[lisp_registered_process_stacks_count] = ps;
    lisp_registered_process_stacks_count++;
}

void lisp_process_stack_unregister(LispProcessStack *ps) {
    for (UINTN i = 0; i < lisp_registered_process_stacks_count; i++) {
        if (lisp_registered_process_stacks[i] == ps) {
            lisp_registered_process_stacks_count--;
            lisp_registered_process_stacks[i] =
                lisp_registered_process_stacks[lisp_registered_process_stacks_count];
            return;
        }
    }
}

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

// milestone83/84: フレーム先頭fpからmax_locals個のスロットを、呼び出し先のbytecodeを実行する
// 前にまとめて確保する（ローカル変数領域とその後の一時値用データスタック領域の分離）。呼び出し元は
// 既にvm_stack[fp..fp+nargs)をボックス化済みのはずなので、この関数はfp+nargsからfp+max_localsまでの
// 残りのスロット（まだletが実行されていないローカル変数用）だけをNILで埋めてvm_spを進める。
// GCのルート走査（vm_stack[0..vm_sp)）がここを辿るため、OP_MAKE_LOCALで実際に書き込まれるまでの間も
// 安全な値（NIL）で埋めておく必要がある
static inline void lisp_vm_reserve_frame(UINTN fp, UINTN max_locals) {
    UINTN target = fp + max_locals;
    if (target > VM_STACK_SIZE) {
        lisp_panic_fatal(L"VM stack overflow");
    }
    for (UINTN i = vm_sp; i < target; i++) {
        vm_stack[i] = LISP_NIL;
    }
    vm_sp = target;
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
        // milestone106: 他プロセスからの中断要求チェック。lisp_vm_current_process/
        // lisp_vm_yield_targetの両方が武装(非NULL)されている場合のみ、budgetが0に達した
        // 時点でlisp_vm_yield_targetへ制御を返す。復帰後はこの直後から通常どおり
        // ディスパッチを継続するため、pc/fp等はCローカル変数としてそのまま保持される
        // （このスコープはlisp_vm_run内のみ。ツリーウォーク経路(lisp_eval/lisp_apply)は
        // 対象外）
        if (lisp_vm_current_process != (void *)0 && lisp_vm_yield_target != (void *)0) {
            if (lisp_vm_yield_budget == 0) {
                lisp_context_switch(lisp_vm_current_process, lisp_vm_yield_target);
            } else {
                lisp_vm_yield_budget--;
            }
        }

        // milestone127: 1命令ごとの画面バッファflushフック。次の命令をフェッチする前に
        // 毎回無条件でlisp_screen_flushを呼ぶ(バッファ未初期化なら即座に戻る、dirtyも
        // pending_newlinesも無ければ何もしない安価な早期returnなので、通常時のオーバーヘッドは
        // 小さい)。これにより、milestone125で暫定的に置いていた「呼び出し単位での即時flush」を
        // 廃止しても、書き込んだ内容は次の命令フェッチより前に必ず実転送される。ツリーウォーク
        // 経路(lisp_eval/lisp_apply)はこのフックを一切通らないため、そちら経由の出力は次に
        // VM命令ループへ戻った時点でまとめてflushされるまで遅延する(milestone106のyieldフックと
        // 同型の既知の制約。境界でのflush統合はmilestone128で行う)
        if (lisp_screen_buffer_is_initialized()) {
            lisp_screen_flush();
        }

        unsigned char op = *pc;
        pc++;
        switch (op) {
            case OP_CONST: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
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
                unsigned int target = pc[0] | (pc[1] << 8);
                pc = cl->bytecode + target;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                unsigned int target = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject test = lisp_vm_pop();
                if (test == LISP_NIL) {
                    pc = cl->bytecode + target;
                }
                break;
            }
            case OP_LOAD_LOCAL: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                lisp_vm_push(lisp_car(vm_stack[fp + idx]));
                break;
            }
            case OP_STORE_LOCAL: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject value = lisp_vm_pop();
                lisp_set_car(vm_stack[fp + idx], value);
                break;
            }
            case OP_MAKE_LOCAL: {
                // milestone83/84: 次の2byteをFP相対indexとして解釈し、popした値をボックス化して
                // 呼び出し時に確保済みの固定スロットvm_stack[fp+idx]へ直接書き込む（スタックに
                // 積み直さない）。これによりOP_MAKE_LOCALの実行は正味でスタックを1つ縮める
                // （値をpopするだけで、対応するpushが無い）ため、letが非tail位置の兄弟式として
                // 使われても、ループの本体として繰り返し実行されても、ローカル変数領域の外側
                // （一時値用データスタック領域）を汚したり、実行を繰り返すごとにスタックが
                // 積み重なったりすることがない
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject value = lisp_vm_pop();
                vm_stack[fp + idx] = lisp_cons(value, LISP_NIL);
                break;
            }
            case OP_POP: {
                lisp_vm_pop();
                break;
            }
            case OP_CALL: {
                unsigned int nargs = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject fn_obj = lisp_vm_pop();
                if (vm_sp < nargs) {
                    lisp_panic(L"VM stack underflow");
                }
                UINTN new_fp = vm_sp - nargs;
                if (lisp_is_compiled(fn_obj)) {
                    LispClosure *callee = lisp_closure_cell(fn_obj);
                    if (callee->nargs != nargs) {
                        lisp_panic(L"VM function called with wrong number of arguments");
                    }
                    for (UINTN i = 0; i < nargs; i++) {
                        vm_stack[new_fp + i] = lisp_cons(vm_stack[new_fp + i], LISP_NIL);
                    }
                    lisp_vm_reserve_frame(new_fp, callee->max_locals);
                    LispObject result = lisp_vm_run(callee, new_fp);
                    vm_sp = new_fp;
                    // milestone 55: ネストしたlisp_vm_run呼び出しの中でreturn-fromが発生し
                    // どのblockにも捕捉されずに戻ってきた場合、自分のフレームもそのまま
                    // 早期returnして呼び出し元へ伝播する（対応するOP_BLOCKに出会うまで続く）
                    if (lisp_return_tag != LISP_NIL) {
                        vm_sp = fp;
                        return result;
                    }
                    lisp_vm_push(result);
                } else {
                    // milestone 52: 呼び出し先がコンパイル済みでなければ、ビルトイン・従来の
                    // インタプリタクロージャいずれもlisp_applyへ委譲する（非関数ならそこでpanicする）。
                    // lisp_applyは評価済み引数の「リスト」を取るため、ボックス化はせずvm_stack上の
                    // 生の値をそのままconsで組み立てる
                    LispObject args = LISP_NIL;
                    for (UINTN i = nargs; i > 0; i--) {
                        args = lisp_cons(vm_stack[new_fp + i - 1], args);
                    }
                    vm_sp = new_fp;
                    LispObject result = lisp_apply(fn_obj, args);
                    // milestone 55: lisp_applyがツリーウォークインタプリタ側のreturn-fromを
                    // 未捕捉のまま返してくる場合も、同じ規約でこのフレームを早期returnして伝播する
                    if (lisp_return_tag != LISP_NIL) {
                        vm_sp = fp;
                        return result;
                    }
                    lisp_vm_push(result);
                }
                break;
            }
            case OP_MAKE_CLOSURE: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
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
                        if ((UINTN)index >= own_upvalues_cell->vec_len) {
                            lisp_panic_fatal(L"VM OOB: OP_MAKE_CLOSURE kind1");
                        }
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
                instance->max_locals = template_cl->max_locals;
                instance->upvalue_descs = template_cl->upvalue_descs;
                instance->upvalues = upvalues;
                instance->pkg_name = 0;
                instance->pkg_symbols = LISP_NIL;
                instance->pkg_exports = LISP_NIL;
                instance->pkg_uses = LISP_NIL;
                instance->pkg_is_keyword = 0;
                instance->pkg_nicknames = LISP_NIL;
                instance->pkg_shadowing_symbols = LISP_NIL;
                instance->pkg_locked = 0;
                instance->class_name = LISP_NIL;
                instance->class_superclass = LISP_NIL;
                instance->class_direct_slots = LISP_NIL;
                instance->class_all_slots = LISP_NIL;
                instance->inst_class = LISP_NIL;
                instance->inst_slots = LISP_NIL;
                instance->gf_name = LISP_NIL;
                instance->gf_methods = LISP_NIL;
                lisp_vm_push(((LispObject)instance) | LISP_TAG_CLOSURE);
                break;
            }
            case OP_LOAD_UPVALUE: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispClosure *upvalues_cell = lisp_closure_cell(cl->upvalues);
                lisp_vm_push(lisp_car(upvalues_cell->vec_data[idx]));
                break;
            }
            case OP_STORE_UPVALUE: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
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
            case OP_GLOBAL_REF: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject sym = cl->constants[idx];
                lisp_vm_push(lisp_env_lookup(global_env, sym));
                break;
            }
            case OP_GLOBAL_SET: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject sym = cl->constants[idx];
                LispObject value = lisp_vm_pop();
                lisp_env_set(global_env, sym, value);
                break;
            }
            case OP_GLOBAL_FUNCTION_REF: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject sym = cl->constants[idx];
                LispObject fn = lisp_symbol_cell(sym)->fn;
                if (fn == LISP_NIL) {
                    lisp_panic(L"unbound function");
                }
                lisp_vm_push(fn);
                break;
            }
            case OP_RETURN: {
                LispObject result = lisp_vm_pop();
                vm_sp = fp;
                return result;
            }
            case OP_RETURN_FROM: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject tag = cl->constants[idx];
                LispObject value = lisp_vm_pop();
                lisp_return_tag = tag;
                lisp_return_value = value;
                vm_sp = fp;
                return value;
            }
            case OP_BLOCK: {
                unsigned int idx = pc[0] | (pc[1] << 8);
                pc += 2;
                LispObject tag = cl->constants[idx];
                LispObject closure_obj = lisp_vm_pop();
                LispClosure *body_cl = lisp_closure_cell(closure_obj);
                UINTN new_fp = vm_sp;
                lisp_vm_reserve_frame(new_fp, body_cl->max_locals);
                LispObject result = lisp_vm_run(body_cl, new_fp);
                vm_sp = new_fp;
                if (lisp_return_tag != LISP_NIL) {
                    if (lisp_return_tag == tag) {
                        lisp_return_tag = LISP_NIL;
                        lisp_vm_push(lisp_return_value);
                    } else {
                        vm_sp = fp;
                        return result;
                    }
                } else {
                    lisp_vm_push(result);
                }
                break;
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
    LispClosure *cl = lisp_closure_cell(fn);
    UINTN base_sp = vm_sp;
    lisp_vm_reserve_frame(base_sp, cl->max_locals);
    LispObject result = lisp_vm_run(cl, base_sp);
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
            lisp_gc_mark(s->fn);
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
        // milestone72: パッケージ（pkg_name != 0のescape hatch）のsymbols/exports/usesは
        // vec_data/constantsと同じ「clに属する生フィールド」なので、closure分岐に組み込んで
        // lisp_gc_mark_roots側の個別ループを不要にする
        if (cl->pkg_name) {
            lisp_gc_mark(cl->pkg_symbols);
            lisp_gc_mark(cl->pkg_exports);
            lisp_gc_mark(cl->pkg_uses);
            lisp_gc_mark(cl->pkg_nicknames);
            lisp_gc_mark(cl->pkg_shadowing_symbols);
        }
        // milestone96: class（class_name != NILのescape hatch）のsuperclass/スロット名リストも
        // clに属する生フィールドなので、closure分岐に組み込んでlisp_gc_mark_roots側の個別ループを
        // 不要にする（global_classes自体をmarkするだけで各クラスオブジェクトへ到達する）
        if (cl->class_name != LISP_NIL) {
            lisp_gc_mark(cl->class_superclass);
            lisp_gc_mark(cl->class_direct_slots);
            lisp_gc_mark(cl->class_all_slots);
        }
        // milestone96: instance（inst_class != NILのescape hatch）のinst_slotsは独立した
        // vectorオブジェクトなので、markするだけで既存のvec_dataマーキングへ自動的に再帰する
        if (cl->inst_class != LISP_NIL) {
            lisp_gc_mark(cl->inst_class);
            lisp_gc_mark(cl->inst_slots);
        }
        // milestone97: generic-function（gf_name != NILのescape hatch）のgf_methodsは
        // ((specializer-list . method-closure) ...)のconsリストなので、markするだけで
        // 各specializerのクラスオブジェクト・method closureまで再帰的に辿れる。専用rootは
        // 不要（generic-function object自体は対象symbolの関数セルfn経由でGCに到達する）
        if (cl->gf_name != LISP_NIL) {
            lisp_gc_mark(cl->gf_methods);
        }
        obj = cl->env;
    }
}

// GCのルート集合: グローバル環境（alist。closureのenvも同じ表現を共有するため、生きている
// closureがマークされた時点でそのenvも連動して辿られる）、全パッケージオブジェクトとそこに
// 登録された全シンボル（t等の特殊シンボルキャッシュもすべてこの中に含まれるため個別マークは
// 不要。milestone69でglobal_packages自体が本物のconsリスト（LispObject）になり、milestone72で
// lisp_gc_markのclosure分岐がpkg_symbols/pkg_exports/pkg_usesも辿るようになったため、
// lisp_gc_mark(global_packages)の1呼び出しだけでスパイン自身の各consセル・各パッケージ
// オブジェクト・その所属シンボル全てが連動して辿られる）、非局所脱出用のシグナル
// （return-fromのタグ不一致panicがこの2つをクリアしない既知の挙動があるため、保守的に
// 常に生きているとみなす）、およびVMデータスタック（milestone34。vm_stack[0..vm_sp)には
// lisp_vm_execが評価中の中間値が生のLispObjectとして置かれるため、Cローカル変数と同様GCの
// 追跡対象外になってしまう。ここでルートに加えないとバンプ確保のたびに回収されてしまう）
static void lisp_gc_mark_roots(void) {
    lisp_gc_mark(global_env);
    lisp_gc_mark(global_packages);
    lisp_gc_mark(global_classes); // milestone96: 各クラスオブジェクトとそのsuperclass/スロット名リストを辿るroot
    lisp_gc_mark(lisp_return_tag);
    lisp_gc_mark(lisp_return_value);
    lisp_gc_mark(lisp_gc_extra_root);
    for (UINTN i = 0; i < vm_sp; i++) {
        lisp_gc_mark(vm_stack[i]);
    }
    // milestone107: lisp_process_stack_registerで登録された、中断中の他プロセスのvm_stackも
    // 同様にルートとして走査する（そのプロセスが今実行中なら、その状態は既に上のグローバル
    // vm_stack/vm_spが指しているので二重に辿るだけで安全。中断中ならLispProcessStack構造体側に
    // 退避されたスナップショットがここでしか辿れない）
    for (UINTN p = 0; p < lisp_registered_process_stacks_count; p++) {
        LispProcessStack *ps = lisp_registered_process_stacks[p];
        for (UINTN i = 0; i < ps->vm_sp; i++) {
            lisp_gc_mark(ps->vm_stack[i]);
        }
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

// --- 全プロセスGCルート登録自己テスト (milestone 107) ---
// mainとは別スタック上のプロセスbを開始し、bだけがpush・かつb専用のvm_stackにのみ積んだ
// consを、bからmainへ切り替えて中断させた（=lisp_context_switchがそのconsをb_ctx.vm_stackへ
// 退避した）直後にmain側からlisp_gc()を実行して、lisp_process_stack_registerによる
// ルート統合が正しく機能しているかを検証する。lisp_vm_gc_root_selftest（単一プロセス版）の
// 複数プロセス拡張
static LispProcessStack *lisp_process_gc_root_selftest_main_ctx = 0;
static LispProcessStack *lisp_process_gc_root_selftest_b_ctx = 0;

static void lisp_process_gc_root_selftest_entry(void *arg) {
    (void)arg;
    LispObject obj = lisp_cons(lisp_make_fixnum(333), lisp_make_fixnum(444));
    vm_stack[0] = obj;
    vm_sp = 1;
    lisp_context_switch(lisp_process_gc_root_selftest_b_ctx, lisp_process_gc_root_selftest_main_ctx);
    for (;;) {}
}

int lisp_process_gc_root_selftest(void) {
    LispProcessStack main_ctx;
    LispProcessStack b_ctx;
    lisp_process_gc_root_selftest_main_ctx = &main_ctx;
    lisp_process_gc_root_selftest_b_ctx = &b_ctx;

    lisp_process_stack_create(&b_ctx, 4, lisp_process_gc_root_selftest_entry, 0);
    lisp_process_stack_register(&b_ctx);

    lisp_context_switch(&main_ctx, &b_ctx); // bが1個consをpushしてすぐmainへ切り替えて中断する

    lisp_gc();

    for (UINTN i = 0; i < 64; i++) {
        lisp_cons(lisp_make_fixnum(0), lisp_make_fixnum(0));
    }

    lisp_process_stack_unregister(&b_ctx); // b_ctxはこの関数を抜けると無効になるため、必ず解除する

    lisp_process_gc_root_selftest_main_ctx = (void *)0;
    lisp_process_gc_root_selftest_b_ctx = (void *)0;

    if (b_ctx.vm_sp != 1) {
        return 0;
    }
    LispObject obj = b_ctx.vm_stack[0];
    if (!lisp_is_cons(obj)) {
        return 0;
    }
    return lisp_car(obj) == lisp_make_fixnum(333) && lisp_cdr(obj) == lisp_make_fixnum(444);
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
    // milestone111: setq自体は「既に確立済みの変数の値を書き換える」通常の実行時操作であり、
    // defvar/defparameterのような変数の"定義"操作ではない(CommonLisp/SBCLのpackage lockも
    // setqそのものは対象にしない)。ここへロックチェックを追加すると、(let ((*dv* ...)) (setq
    // *dv* ...))のような、ロック対象のcommon-lisp-user自身が持つ動的変数への通常の再束縛/
    // 代入(既存のtest-dynamic-vars.lisp等が検証している正常動作)まで塞いでしまうため、
    // 意図的にチェック対象外とする
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

LispObject lisp_eval(LispObject expr, LispObject env);

// milestone 89: &optional/&rest/&key/&aux/&allow-other-keysのいずれか
static int lisp_is_lambda_list_keyword(LispObject sym) {
    return sym == lisp_sym_lambda_optional || sym == lisp_sym_lambda_rest ||
           sym == lisp_sym_lambda_key || sym == lisp_sym_lambda_aux ||
           sym == lisp_sym_lambda_allow_other_keys;
}

// var単体 / (var) / (var default-form) / (var default-form supplied-p-var) の
// いずれかのspecを分解する（&optional/&keyで共通の形）。default-formが省略されたかは
// has_default_out、supplied-p-varが省略されたかはhas_supplied_outに返す
static void lisp_lambda_parse_var_spec(LispObject spec, LispObject *var_out,
                                        LispObject *default_form_out, int *has_default_out,
                                        LispObject *supplied_var_out, int *has_supplied_out) {
    *default_form_out = LISP_NIL;
    *has_default_out = 0;
    *supplied_var_out = LISP_NIL;
    *has_supplied_out = 0;
    if (lisp_is_symbol(spec)) {
        *var_out = spec;
        return;
    }
    if (!lisp_is_cons(spec) || !lisp_is_symbol(lisp_car(spec))) {
        lisp_panic(L"malformed lambda list: expected variable spec");
    }
    *var_out = lisp_car(spec);
    LispObject rest1 = lisp_cdr(spec);
    if (lisp_is_cons(rest1)) {
        *default_form_out = lisp_car(rest1);
        *has_default_out = 1;
        LispObject rest2 = lisp_cdr(rest1);
        if (lisp_is_cons(rest2) && lisp_is_symbol(lisp_car(rest2))) {
            *supplied_var_out = lisp_car(rest2);
            *has_supplied_out = 1;
        }
    }
}

// &optional以降のspec列をargsが尽きるまで/次のキーワードに達するまで束縛する。
// argsは呼び出し側の残り実引数を指すポインタで、消費した分だけ前進させて返す。
// paramsは処理し終えた残り(次のキーワードまたはNIL)をout_paramsへ返す
static LispObject lisp_env_bind_optional(LispObject params, LispObject *args_ptr,
                                          LispObject env, LispObject *out_params) {
    LispObject args = *args_ptr;
    while (lisp_is_cons(params) && !lisp_is_lambda_list_keyword(lisp_car(params))) {
        LispObject var, default_form, supplied_var;
        int has_default, has_supplied;
        lisp_lambda_parse_var_spec(lisp_car(params), &var, &default_form, &has_default,
                                    &supplied_var, &has_supplied);
        if (lisp_is_cons(args)) {
            env = lisp_env_extend(env, var, lisp_car(args));
            if (has_supplied) {
                env = lisp_env_extend(env, supplied_var, lisp_sym_t);
            }
            args = lisp_cdr(args);
        } else {
            LispObject value = has_default ? lisp_eval(default_form, env) : LISP_NIL;
            env = lisp_env_extend(env, var, value);
            if (has_supplied) {
                env = lisp_env_extend(env, supplied_var, LISP_NIL);
            }
        }
        params = lisp_cdr(params);
    }
    *args_ptr = args;
    *out_params = params;
    return env;
}

// &restの直後は単一のvar symbolでなければならない。argsは消費せず、残っている
// 実引数リストをそのままvarへ束縛する（&keyが同じ残り実引数をさらに解釈できるように）
static LispObject lisp_env_bind_rest(LispObject params, LispObject args, LispObject env,
                                      LispObject *out_params) {
    if (!lisp_is_cons(params) || !lisp_is_symbol(lisp_car(params)) ||
        lisp_is_lambda_list_keyword(lisp_car(params))) {
        lisp_panic(L"malformed lambda list: expected a single variable after &rest");
    }
    env = lisp_env_extend(env, lisp_car(params), args);
    *out_params = lisp_cdr(params);
    return env;
}

// &key以降のspec列。argsは&restと同じ残り実引数（未消費）を、":var"キーワードと
// 値の対として解釈する。未知キーワードは&allow-other-keysが無ければpanicする
static LispObject lisp_env_bind_key(LispObject params, LispObject args, LispObject env,
                                     LispObject *out_params) {
    LispObject specs_end = params;
    while (lisp_is_cons(specs_end) && !lisp_is_lambda_list_keyword(lisp_car(specs_end))) {
        specs_end = lisp_cdr(specs_end);
    }
    LispObject after_specs = specs_end;
    int allow_other_keys = 0;
    if (lisp_is_cons(after_specs) && lisp_car(after_specs) == lisp_sym_lambda_allow_other_keys) {
        allow_other_keys = 1;
        after_specs = lisp_cdr(after_specs);
    }

    UINTN pair_count = 0;
    for (LispObject cur = args; lisp_is_cons(cur); cur = lisp_cdr(cur)) {
        pair_count++;
    }
    if (pair_count % 2 != 0) {
        lisp_panic(L"malformed &key arguments: odd number of keyword/value items");
    }

    if (!allow_other_keys) {
        for (LispObject cur = args; lisp_is_cons(cur); cur = lisp_cdr(lisp_cdr(cur))) {
            LispObject keyword = lisp_car(cur);
            int found = 0;
            for (LispObject s = params; s != specs_end; s = lisp_cdr(s)) {
                LispObject spec = lisp_car(s);
                LispObject var = lisp_is_symbol(spec) ? spec : lisp_car(spec);
                if (lisp_intern_keyword(lisp_symbol_cell(var)->name) == keyword) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                lisp_panic(L"unknown keyword argument");
            }
        }
    }

    for (LispObject s = params; s != specs_end; s = lisp_cdr(s)) {
        LispObject var, default_form, supplied_var;
        int has_default, has_supplied;
        lisp_lambda_parse_var_spec(lisp_car(s), &var, &default_form, &has_default,
                                    &supplied_var, &has_supplied);
        LispObject keyword = lisp_intern_keyword(lisp_symbol_cell(var)->name);
        LispObject found_value = LISP_NIL;
        int found = 0;
        for (LispObject cur = args; lisp_is_cons(cur); cur = lisp_cdr(lisp_cdr(cur))) {
            if (lisp_car(cur) == keyword) {
                found_value = lisp_car(lisp_cdr(cur));
                found = 1;
                break;
            }
        }
        if (found) {
            env = lisp_env_extend(env, var, found_value);
            if (has_supplied) {
                env = lisp_env_extend(env, supplied_var, lisp_sym_t);
            }
        } else {
            LispObject value = has_default ? lisp_eval(default_form, env) : LISP_NIL;
            env = lisp_env_extend(env, var, value);
            if (has_supplied) {
                env = lisp_env_extend(env, supplied_var, LISP_NIL);
            }
        }
    }

    *out_params = after_specs;
    return env;
}

// &aux以降は(let*同様に)前のaux変数を後のinit-formから参照できる逐次初期化として
// 束縛する。実引数は関係しない（残っていても消費しない）
static LispObject lisp_env_bind_aux(LispObject params, LispObject env) {
    while (lisp_is_cons(params)) {
        LispObject spec = lisp_car(params);
        LispObject var, init_form;
        if (lisp_is_symbol(spec)) {
            var = spec;
            init_form = LISP_NIL;
        } else if (lisp_is_cons(spec) && lisp_is_symbol(lisp_car(spec))) {
            var = lisp_car(spec);
            LispObject rest = lisp_cdr(spec);
            init_form = lisp_is_cons(rest) ? lisp_car(rest) : LISP_NIL;
        } else {
            lisp_panic(L"malformed &aux spec in lambda list");
        }
        env = lisp_env_extend(env, var, lisp_eval(init_form, env));
        params = lisp_cdr(params);
    }
    return env;
}

// paramsとargsを対応付けてenvに束縛していく。milestone89までは1対1の位置束縛
// （個数不一致はpanic）とmilestone29の「仮引数全体が単一のbare symbol」の可変長引数
// のみだったが、ここからCommonLisp相当の&optional/&rest/&key/&aux/&allow-other-keys
// を解釈する。出現順序は required* [&optional...] [&rest var] [&key... [&allow-other-keys]]
// [&aux...] に固定し、順序違反やキーワードの重複はpanicする
LispObject lisp_env_bind_params(LispObject params, LispObject args, LispObject env) {
    while (lisp_is_cons(params)) {
        LispObject head = lisp_car(params);
        if (lisp_is_lambda_list_keyword(head)) {
            break;
        }
        if (!lisp_is_symbol(head)) {
            lisp_panic(L"malformed lambda list: expected parameter name");
        }
        if (!lisp_is_cons(args)) {
            lisp_panic(L"too few arguments");
        }
        env = lisp_env_extend(env, head, lisp_car(args));
        params = lisp_cdr(params);
        args = lisp_cdr(args);
    }
    // milestone 29: 仮引数リストがsymbol一つだけ（(lambda args ...)/(defun f args ...)）
    // の場合、残りの実引数をリストのままそのsymbolへ束縛する（可変長引数）。
    // (a b . rest)のドット対記法はリーダーが対応していないため、この「仮引数全体が
    // 単一のsymbol」という形だけをサポートする。新キーワードとは併用しない
    if (params != LISP_NIL && lisp_is_symbol(params)) {
        env = lisp_env_extend(env, params, args);
        return env;
    }

    int seen_optional = 0, seen_rest = 0, seen_key = 0, seen_aux = 0;
    int has_rest = 0, has_key = 0;
    while (lisp_is_cons(params)) {
        LispObject head = lisp_car(params);
        if (head == lisp_sym_lambda_optional) {
            if (seen_optional || seen_rest || seen_key || seen_aux) {
                lisp_panic(L"malformed lambda list: unexpected &optional");
            }
            seen_optional = 1;
            env = lisp_env_bind_optional(lisp_cdr(params), &args, env, &params);
        } else if (head == lisp_sym_lambda_rest) {
            if (seen_rest || seen_key || seen_aux) {
                lisp_panic(L"malformed lambda list: unexpected &rest");
            }
            seen_rest = 1;
            has_rest = 1;
            env = lisp_env_bind_rest(lisp_cdr(params), args, env, &params);
        } else if (head == lisp_sym_lambda_key) {
            if (seen_key || seen_aux) {
                lisp_panic(L"malformed lambda list: unexpected &key");
            }
            seen_key = 1;
            has_key = 1;
            env = lisp_env_bind_key(lisp_cdr(params), args, env, &params);
        } else if (head == lisp_sym_lambda_aux) {
            if (seen_aux) {
                lisp_panic(L"malformed lambda list: unexpected &aux");
            }
            seen_aux = 1;
            env = lisp_env_bind_aux(lisp_cdr(params), env);
            params = LISP_NIL;
        } else if (head == lisp_sym_lambda_allow_other_keys) {
            lisp_panic(L"malformed lambda list: &allow-other-keys without &key");
        } else {
            lisp_panic(L"malformed lambda list: expected lambda-list keyword");
        }
    }

    if (!has_rest && !has_key && args != LISP_NIL) {
        lisp_panic(L"too many arguments");
    }
    return env;
}

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
    if (closure->bytecode != 0) {
        // milestone 53: コンパイル済みクロージャはmilestone52のOP_CALLと対になる方向の
        // フォールバックとしてlisp_vm_runへ委譲する。argsを評価済みの値としてvm_stackへ積み、
        // OP_CALLと同じ規約（引数個数分をFP相対でその場でボックス化）に揃えてから呼ぶ
        UINTN fp = vm_sp;
        UINTN nargs = 0;
        LispObject cur = args;
        while (lisp_is_cons(cur)) {
            lisp_vm_push(lisp_car(cur));
            nargs++;
            cur = lisp_cdr(cur);
        }
        if (nargs != closure->nargs) {
            lisp_panic(L"VM function called with wrong number of arguments");
        }
        for (UINTN i = 0; i < nargs; i++) {
            vm_stack[fp + i] = lisp_cons(vm_stack[fp + i], LISP_NIL);
        }
        lisp_vm_reserve_frame(fp, closure->max_locals);
        LispObject result = lisp_vm_run(closure, fp);
        vm_sp = fp;
        return result;
    }
    if (closure->builtin != 0) {
        return closure->builtin(args);
    }
    // milestone97: generic-function objectはlisp_gf_select_methodで選んだmethod closureへ
    // 再度lisp_applyする(OP_CALL側は無変更、非compiled-calleeフォールバックがここへ自動的に
    // 流れてくる)
    if (closure->gf_name != LISP_NIL) {
        LispObject method = lisp_gf_select_method(fn, args);
        return lisp_apply(method, args);
    }
    LispObject call_env = lisp_env_bind_params(closure->params, args, closure->env);
    return lisp_eval(closure->body, call_env);
}

// symがマクロクロージャとして関数セル(fn、milestone93)に束縛されていればそれを返し、
// 無ければpanicせずLISP_NILを返す（milestone94でdefmacroの書き込み先がglobal_envから
// fnへ移った後も、この「未束縛ならpanicしない」契約自体は変わらない。fnは未束縛時に
// LISP_NILがそのまま入っているため、そのまま返せば従来と同じ挙動になる）。
// panicしない理由は、macroexpand-1/macroexpand-all(milestone 40, lisp/stdlib.lisp)がS式を
// 静的に全走査する際、関数呼び出し位置のsymbolが再帰関数呼び出し等のローカル変数
// （関数セルには存在しない）であるケースを日常的に踏むため。lisp_evalの完全なevalで
// 代用すると、そのようなローカル変数はunbound functionとしてpanicしてしまう
// （global_envだった頃のmilestone 50の発見と同根）
static LispObject lisp_lookup_global_macro_candidate(LispObject sym) {
    return lisp_symbol_cell(sym)->fn;
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
    if (!lisp_is_symbol(op)) {
        return expr; // 演算子位置が裸のsymbolでなければ(直接書かれたlambda式等)マクロ呼び出しではない
    }

    LispObject fn = lisp_lookup_global_macro_candidate(op);
    if (!lisp_is_closure(fn) || !lisp_closure_cell(fn)->is_macro) {
        return expr; // マクロ呼び出しではない（未束縛のローカル変数でもここでpanicしない）
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
        if (expr == lisp_sym_t || lisp_symbol_package_is_keyword(cell)) {
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

        // milestone 93: #'foo/(function foo)。bare symbolなら関数セルのみを見る
        // （lexical envは経由しない、この処理系にflet/labels相当が無いため）。
        // 非symbol（lambda式等）はlisp_evalに委ねる（既に閉包を返すためnamespaceの区別が無い）
        if (op == lisp_sym_function) {
            LispObject target = lisp_car(lisp_cdr(expr));
            if (lisp_is_symbol(target)) {
                LispObject fn = lisp_symbol_cell(target)->fn;
                if (fn == LISP_NIL) {
                    lisp_panic(L"unbound function");
                }
                return fn;
            }
            return lisp_eval(target, env);
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

        if (op == lisp_sym_do) {
            // milestone 87: (do ((var init step)...) (end-test result...) body...)。
            // 繰り返し自体をCの再帰(lisp_eval_progn等)ではなくwhile(1)で行うため、
            // イテレーション数に関わらずCコールスタックの深さは一定に保たれる
            // (append/reverse等の非末尾再帰がCスタックを食い尽くす問題への対策)
            LispObject bindings = lisp_car(lisp_cdr(expr));
            LispObject test_and_result = lisp_car(lisp_cdr(lisp_cdr(expr)));
            LispObject body = lisp_cdr(lisp_cdr(lisp_cdr(expr)));
            LispObject end_test = lisp_car(test_and_result);
            LispObject result_forms = lisp_cdr(test_and_result);

            // フェーズ1: letと同じ並行束縛の意味で、各init-formをdoの外側envで評価する
            LispObject values = LISP_NIL; // (sym . value)のリスト
            LispObject cur = bindings;
            while (lisp_is_cons(cur)) {
                LispObject binding = lisp_car(cur);
                LispObject sym = lisp_is_symbol(binding) ? binding : lisp_car(binding);
                lisp_assert_symbol(sym);
                LispObject rest1 = lisp_is_cons(binding) ? lisp_cdr(binding) : LISP_NIL;
                LispObject init_form = lisp_is_cons(rest1) ? lisp_car(rest1) : LISP_NIL;
                LispObject value = lisp_eval(init_form, env);
                if (lisp_return_tag != LISP_NIL) {
                    return value; // milestone19: まだ何も束縛していないのでそのまま伝播
                }
                values = lisp_cons(lisp_cons(sym, value), values);
                cur = lisp_cdr(cur);
            }

            // フェーズ2: 動的変数はvalueを退避してから書き換え、通常変数はenvに積む(letと同型)
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

            // フェーズ3: end-testが真になるまでbody→stepを繰り返す。同じ束縛箱(pair)を
            // lisp_env_setで書き換えるだけなので、letのようにenvを毎回再拡張しない
            LispObject result = LISP_NIL;
            int done = 0;
            while (!done) {
                LispObject test_value = lisp_eval(end_test, new_env);
                if (lisp_return_tag != LISP_NIL) {
                    result = test_value;
                    break;
                }
                if (test_value != LISP_NIL) {
                    result = lisp_eval_progn(result_forms, new_env);
                    break;
                }

                LispObject body_result = lisp_eval_progn(body, new_env);
                if (lisp_return_tag != LISP_NIL) {
                    result = body_result;
                    break;
                }

                // 並行ステップ: 全step-formを更新前のnew_envで先に評価してから
                // まとめて書き込む(letと同じく、他のstep-formが更新後の値を見てはならない)
                LispObject step_values = LISP_NIL; // (sym . new-value)のリスト
                LispObject bcur = bindings;
                int step_aborted = 0;
                while (lisp_is_cons(bcur)) {
                    LispObject binding = lisp_car(bcur);
                    if (lisp_is_cons(binding)) {
                        LispObject rest1 = lisp_cdr(binding);
                        LispObject rest2 = lisp_is_cons(rest1) ? lisp_cdr(rest1) : LISP_NIL;
                        if (lisp_is_cons(rest2)) {
                            LispObject sym = lisp_car(binding);
                            LispObject step_value = lisp_eval(lisp_car(rest2), new_env);
                            if (lisp_return_tag != LISP_NIL) {
                                result = step_value;
                                done = 1;
                                step_aborted = 1;
                                break;
                            }
                            step_values = lisp_cons(lisp_cons(sym, step_value), step_values);
                        }
                    }
                    bcur = lisp_cdr(bcur);
                }
                if (step_aborted) {
                    break;
                }
                bcur = step_values;
                while (lisp_is_cons(bcur)) {
                    LispObject pair = lisp_car(bcur);
                    lisp_env_set(new_env, lisp_car(pair), lisp_cdr(pair));
                    bcur = lisp_cdr(bcur);
                }
            }

            // doを抜ける際、動的変数はdoに入る前の値へ必ず復元する(letと同型)
            cur = saved_specials;
            while (lisp_is_cons(cur)) {
                LispObject pair = lisp_car(cur);
                lisp_symbol_cell(lisp_car(pair))->value = lisp_cdr(pair);
                cur = lisp_cdr(cur);
            }
            return result;
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
            // milestone111: defvarと異なり既存の値があっても常に上書きするため、"redefinition"
            // 判定はis_specialの現在値（この行より前の状態）で行う必要がある
            if (cell->is_special && lisp_symbol_home_package_locked(sym)) {
                lisp_panic(L"Package is locked");
            }
            cell->value = value;
            cell->is_special = 1;
            return sym;
        }

        if (op == lisp_sym_defun) {
            LispObject name = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(name);
            // milestone111: 新規定義は常に許可、既存定義の上書きのみロック済みパッケージで禁止
            lisp_check_function_redefine_allowed(name);
            LispObject params = lisp_car(lisp_cdr(lisp_cdr(expr)));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(expr))));
            LispObject closure = lisp_make_closure(params, body, env);
            // milestone94: 書き込み先はglobal_envではなく関数セル(fn)。同名の再定義は
            // 単純にfnを上書きするだけでよい（alistではないため先頭追加という概念も無い）
            lisp_symbol_cell(name)->fn = closure;
            return name;
        }

        if (op == lisp_sym_defmacro) {
            LispObject name = lisp_car(lisp_cdr(expr));
            lisp_assert_symbol(name);
            LispObject params = lisp_car(lisp_cdr(lisp_cdr(expr)));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(expr))));
            LispObject macro = lisp_make_macro(params, body, env);
            lisp_symbol_cell(name)->fn = macro;
            return name;
        }

        // milestone94: 呼び出し位置のbare symbolはレキシカルenv/global_envを一切見ず
        // 関数セル(fn)のみを見る（この処理系にflet/labels相当が無いため、関数の局所束縛
        // という概念自体が無い）。非symbol(lambda式リテラル等)は従来通りlisp_evalに委ねる
        LispObject fn;
        if (lisp_is_symbol(op)) {
            fn = lisp_symbol_cell(op)->fn;
            if (fn == LISP_NIL) {
                lisp_panic(L"unbound function");
            }
        } else {
            fn = lisp_eval(op, env);
            if (lisp_return_tag != LISP_NIL) {
                return fn; // milestone 19: 呼び出す関数式自体の評価中に脱出した
            }
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

// milestone 58: 統一トップレベル評価ドライバのコンパイラ準備状態フラグ。既定false
// （ツリーウォークのみ）で、lisp/stdlib.lisp自身の読み込みが完了するまではfalseのまま
// 保たれる。stdlib.lispがcompile-expr等の自分自身の定義より前に置かれたdefvar形式の
// opcode定数などをこのフラグがtrueの状態で読もうとすると「まだ存在しないコンパイラで
// コンパイラ自身をロードする」鶏と卵問題が起きるため、stdlib.lispの最後のフォームから
// mark-compiler-readyを呼んでtrueへ切り替える運用とする。当初の計画（documents/
// lisp_vm_integration.md）ではこのフラグの導入はmilestone64（フェーズ3、
// lisp/compiler.lisp分割後の2段階起動）を予定していたが、milestone58の時点で
// 「evalの既定動作をコンパイル+VM実行にする」ことがフラグ無しには成立しない
// （stdlib.lisp自身の起動を壊す）ことが判明したため、前倒しでここに導入する
static int lisp_compiler_ready = 0;

// stdlib.lispの最後のフォームから呼び、以降のトップレベル評価を新経路
// （macroexpand-all→compile-expr→vm-exec）へ切り替える
LispObject lisp_builtin_mark_compiler_ready(LispObject args) {
    (void)args;
    lisp_compiler_ready = 1;
    return lisp_sym_t;
}

// テスト用の照会ビルトイン。stdlib.lispの読み込み完了後は常にtを返す
LispObject lisp_builtin_compiler_ready_p(LispObject args) {
    (void)args;
    return lisp_compiler_ready ? lisp_sym_t : LISP_NIL;
}

// milestone 60: defunをcompile-expr経由でコンパイルした結果（コンパイル済みクロージャ）を、
// ツリーウォークのdefun特殊形式（本ファイル上部のop == lisp_sym_defun分岐）と全く同じ
// 関数セル(fn)へ書き込む（milestone94で書き込み先をglobal_envからfnへ変更）。symを返す
// （defun特殊形式の戻り値と一致させる）
LispObject lisp_builtin_establish_global_function(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    // milestone111: ツリーウォークdefunと同じ"redefinition-only"チェック
    lisp_check_function_redefine_allowed(sym);
    LispObject closure = lisp_car(lisp_cdr(args));
    lisp_symbol_cell(sym)->fn = closure;
    return sym;
}

// milestone 93: 関数セル(LispSymbol.fn)を読み書きする最小API。milestone94でdefun/
// defmacro/組み込み関数すべての書き込み先がここへ切り替わった

// (symbol-function sym): fnを返す。未束縛(LISP_NIL)ならpanicする
LispObject lisp_builtin_symbol_function(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    LispObject fn = lisp_symbol_cell(sym)->fn;
    if (fn == LISP_NIL) {
        lisp_panic(L"unbound function");
    }
    return fn;
}

// (%set-symbol-function sym value): fnへ直接書き込む内部API。milestone94でdefun等の
// 書き込み先として使う
LispObject lisp_builtin_set_symbol_function(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    // milestone111: ツリーウォークdefunと同じ"redefinition-only"チェック
    lisp_check_function_redefine_allowed(sym);
    LispObject value = lisp_car(lisp_cdr(args));
    lisp_symbol_cell(sym)->fn = value;
    return value;
}

// (fboundp sym): fnが束縛済みか
LispObject lisp_builtin_fboundp(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    return lisp_symbol_cell(sym)->fn != LISP_NIL ? lisp_sym_t : LISP_NIL;
}

// (symbol-value sym): 既存の値セル取得(is_specialならsym->value、通常はglobal_env経由)の
// 薄いラッパー。lisp_env_lookupが両方のケースをすでに内包している
LispObject lisp_builtin_symbol_value(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    return lisp_env_lookup(global_env, sym);
}

// milestone 89: すでにコンパイルされている関数の内側に直接書かれた
// (lambda (&optional x) ...)のようなネストしたlambda式は、compile-lambda
// (lisp/compiler.lisp)がツリーウォークへ逃げずにそのままコンパイルしてしまうため、
// &optionalという語がそのまま1個目の仮引数名として扱われる危険な黒魔術（サイレントな
// 誤動作）になる。これを防ぐため、compile-lambdaがparamsに新キーワードシンボルを
// 検出した際にLisp側から呼ぶ専用panicビルトイン（引数は取らない）
LispObject lisp_builtin_panic_compiled_lambda_list_keyword(LispObject args) {
    (void)args;
    lisp_panic(L"lambda-list keywords (&optional/&rest/&key/&aux/&allow-other-keys) "
               L"cannot be used in a lambda nested inside already-compiled code");
    return LISP_NIL; // 到達しない
}

// milestone 60（milestone89で一般化）: defunのparamsがコンパイル済みクロージャの
// 呼び出し規約（OP_CALL/lisp_applyのclosure->nargs厳密一致チェック、milestone37）では
// 表現できない書き方かどうかを判定する。該当するのは
// (a) 「仮引数リスト全体が1つのbare symbol」というrest-arg形式（milestone29、
//     lisp_env_bind_paramsが解釈する可変長引数の書き方）
// (b) &optional/&rest/&key/&aux/&allow-other-keys（milestone89）をどれか1つでも含む場合
// のいずれか。どちらもdocuments/lisp_vm_integration.md・lisp_lambda_list_keywords.mdの
// スコープ外として明記済みで、この形のdefunだけはlisp_eval_toplevelが既存の
// ツリーウォークへフォールバックする判定に使う
static int lisp_defun_params_needs_interpreter(LispObject expr) {
    LispObject params = lisp_car(lisp_cdr(lisp_cdr(expr)));
    if (params != LISP_NIL && lisp_is_symbol(params)) {
        return 1;
    }
    while (lisp_is_cons(params)) {
        if (lisp_is_lambda_list_keyword(lisp_car(params))) {
            return 1;
        }
        params = lisp_cdr(params);
    }
    return 0;
}

// REPLの1行、またはloadの1トップレベル式としてexprをglobal_env上で評価する。
// lisp_compiler_readyが真で、かつexprの先頭がdefun/defmacroでなければ、
// macroexpand-all→compile-expr→vm-execの新経路（lisp/stdlib.lisp）へ委譲する。
// defmacroは恒久的にツリーウォークへフォールバックする（マクロ展開はインタプリタ操作
// そのものであり、コンパイル時に発生する）。defunはmilestone60でコンパイル対応した
// （compile-defun、lisp/stdlib.lisp）が、rest-arg形式のparams（milestone29）や
// &optional/&rest/&key/&aux/&allow-other-keys（milestone89、
// lisp_defun_params_needs_interpreter参照）を含むparamsだけはコンパイル済みクロージャの
// 呼び出し規約がサポートしないため、この形のdefunのみ個別にツリーウォークへ
// フォールバックする。
// return-fromの脱出シグナルが対応するblockに一度も捕捉されずここまで残っている場合、
// タグが指す実行中のblockが存在しないというユーザー側の誤りなのでpanicする。
// これをせずに素通しすると、残ったシグナルが次の入力の評価を最初の一歩で
// 打ち切ってしまい、以降すべての評価が無言で壊れる (milestone 19)。この安全性は
// 新経路（vm-execの戻り値をそのまま返しつつlisp_return_tagは残るcompile-and-runの
// 挙動）でも同様に成り立つため、経路に関わらず共通の末尾チェックとして残す
LispObject lisp_eval_toplevel(LispObject expr) {
    LispObject op = lisp_is_cons(expr) ? lisp_car(expr) : LISP_NIL;
    LispObject result;
    int defun_needs_interpreter = (op == lisp_sym_defun) && lisp_defun_params_needs_interpreter(expr);
    if (lisp_compiler_ready && op != lisp_sym_defmacro && !defun_needs_interpreter) {
        LispObject compile_and_run = lisp_symbol_cell(lisp_sym_compile_and_run)->fn;
        result = lisp_apply(compile_and_run, lisp_cons(expr, LISP_NIL));
    } else {
        result = lisp_eval(expr, global_env);
    }
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

// (funcall fn a1 a2 ...): argsの先頭がfn、残りが評価済みの実引数列そのもの。
// lisp_apply(milestone53、コンパイル済み・インタプリタ・ビルトインいずれのクロージャも
// 一貫して呼び出せる)へそのまま委譲するだけで、呼び出し先の種別による分岐は不要
LispObject lisp_builtin_funcall(LispObject args) {
    return lisp_apply(lisp_car(args), lisp_cdr(args));
}

// (apply fn arg-list): 実引数はargsの先頭(fn)ではなく2番目の要素として渡された
// 「評価済みの実引数のリスト」自体である点がfuncallとの唯一の違い
LispObject lisp_builtin_apply(LispObject args) {
    return lisp_apply(lisp_car(args), lisp_car(lisp_cdr(args)));
}

LispObject lisp_builtin_eq(LispObject args) {
    LispObject a = lisp_car(args);
    LispObject b = lisp_car(lisp_cdr(args));
    return (a == b) ? lisp_sym_t : LISP_NIL;
}

LispObject lisp_builtin_atom(LispObject args) {
    return lisp_is_cons(lisp_car(args)) ? LISP_NIL : lisp_sym_t;
}

// (symbolp obj): objがsymbol(t/keywordを含む。nilはlisp_is_symbolがfalseを返すため対象外)
// ならt、そうでなければnilを返す。milestone51で、compile-variable-refが未解決のatomを
// 「レキシカル外のsymbol(OP_GLOBAL_REFの対象)」と「本来のリテラル(数値・文字列等)」の
// どちらとして扱うか判定するために必要になった
LispObject lisp_builtin_symbolp(LispObject args) {
    return lisp_is_symbol(lisp_car(args)) ? lisp_sym_t : LISP_NIL;
}

// (keywordp obj): objがkeywordパッケージに属するsymbolならt、そうでなければnilを返す。
// milestone51で、tと同様に自己評価するkeyword symbolをcompile-variable-refがOP_GLOBAL_REFの
// 対象(global_envに束縛が無く必ずpanicする)へ誤って回さないために必要になった
LispObject lisp_builtin_keywordp(LispObject args) {
    LispObject obj = lisp_car(args);
    if (!lisp_is_symbol(obj)) {
        return LISP_NIL;
    }
    LispSymbol *cell = lisp_symbol_cell(obj);
    return lisp_symbol_package_is_keyword(cell) ? lisp_sym_t : LISP_NIL;
}

// (make-package name &optional nicknames): 名前がまだ存在しなければ新規のパッケージ
// オブジェクトを作ってglobal_packagesへ登録し、既に存在すれば既存オブジェクトをそのまま
// 返す（milestone69のlisp_make_packageの冪等性そのまま）。milestone75でLispから初めて
// 呼び出せるようになった。milestone91でnameがpackage designator（文字列/symbol・keyword）
// を受け付けるよう拡張し、第2引数nicknames（designatorのリスト）を追加した
LispObject lisp_builtin_make_package(LispObject args) {
    LispObject name_obj = lisp_car(args);
    LispObject pkg = lisp_make_package(lisp_string_designator_name(name_obj), 0);
    LispObject rest = lisp_cdr(args);
    if (lisp_is_cons(rest)) {
        LispClosure *pkg_cell = lisp_closure_cell(pkg);
        for (LispObject cur = lisp_car(rest); cur != LISP_NIL; cur = lisp_cdr(cur)) {
            const char *nickname = lisp_string_designator_name(lisp_car(cur));
            int already = 0;
            for (LispObject n = pkg_cell->pkg_nicknames; n != LISP_NIL; n = lisp_cdr(n)) {
                if (lisp_streq(lisp_closure_cell(lisp_car(n))->str_data, nickname)) {
                    already = 1;
                    break;
                }
            }
            if (!already) {
                pkg_cell->pkg_nicknames = lisp_cons(lisp_make_string(nickname, lisp_cstrlen(nickname)),
                                                     pkg_cell->pkg_nicknames);
            }
        }
    }
    return pkg;
}

// (find-package name): nameはpackage designator（文字列/symbol・keyword、milestone91で拡張）。
// 一致するパッケージオブジェクトを返す。見つからなければnilを返す
LispObject lisp_builtin_find_package(LispObject args) {
    LispObject name_obj = lisp_car(args);
    return lisp_find_package(lisp_string_designator_name(name_obj));
}

// milestone 77: パッケージオブジェクトそのもの、またはパッケージ名の文字列/symbol・keyword
// （milestone91でsymbol/keywordへ拡張）のいずれもパッケージ指定として受け取れるようにする。
// 存在しない名前を渡した場合はpanicする
static LispObject lisp_resolve_package_designator(LispObject obj) {
    if (lisp_is_package(obj)) {
        return obj;
    }
    LispObject pkg = lisp_find_package(lisp_package_designator_name(obj));
    if (pkg == LISP_NIL) {
        lisp_panic(L"unknown package name");
    }
    return pkg;
}

// (export symbols &optional package): symbolsは単一のsymbolまたはsymbolのリスト。
// packageを省略すると*package*（現在のパッケージ）を対象にする。対象パッケージの
// pkg_exportsコンスリストへeq（==によるポインタ比較）基準で重複なく追加する（milestone76）。
// #74のリーダーの単一コロン修飾子（pkg:sym）はこのリストをそのまま参照する。
// milestone91でpackage引数にdesignator解決を通すようにした（従来は素通しで、パッケージ名の
// 文字列/symbolを渡すと`lisp_closure_cell`がパッケージでないオブジェクトを誤ってパッケージ
// として扱ってしまう抜けがあった）
LispObject lisp_builtin_export(LispObject args) {
    LispObject symbols_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject symbols_list = lisp_is_symbol(symbols_arg) ? lisp_cons(symbols_arg, LISP_NIL) : symbols_arg;
    for (LispObject cur = symbols_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject sym = lisp_car(cur);
        lisp_assert_symbol(sym);
        int already_exported = 0;
        for (LispObject e = pkg_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            if (lisp_car(e) == sym) {
                already_exported = 1;
                break;
            }
        }
        if (!already_exported) {
            pkg_cell->pkg_exports = lisp_cons(sym, pkg_cell->pkg_exports);
        }
    }
    return lisp_sym_t;
}

// (use-package packages-to-use &optional package): packages-to-useは単一のパッケージ
// （またはパッケージ名の文字列/symbol・keyword、milestone91で拡張）、またはそれらのリスト。
// packageを省略すると*package*（現在のパッケージ）を対象にする。それぞれのpkg_usesコンス
// リストへeqで重複なく追加する。名前衝突（追加しようとしているパッケージのexportシンボルが、
// 対象パッケージの既存ローカルシンボル、または既にuse済みの別パッケージのexportシンボルと
// 同名だが別オブジェクトである場合）は最小限のガードとしてエラーにする（milestone77。
// shadowingで解決済みの場合の例外はmilestone92で追加）
LispObject lisp_builtin_use_package(LispObject args) {
    LispObject used_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject used_list = (lisp_is_package(used_arg) || lisp_is_string(used_arg) || lisp_is_symbol(used_arg))
                                ? lisp_cons(used_arg, LISP_NIL)
                                : used_arg;
    for (LispObject cur = used_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject used_pkg = lisp_resolve_package_designator(lisp_car(cur));
        LispClosure *used_cell = lisp_closure_cell(used_pkg);

        for (LispObject e = used_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            LispObject exported_sym = lisp_car(e);
            const char *name = lisp_symbol_cell(exported_sym)->name;

            for (LispObject ls = pkg_cell->pkg_symbols; ls != LISP_NIL; ls = lisp_cdr(ls)) {
                if (lisp_car(ls) != exported_sym && lisp_streq(lisp_symbol_cell(lisp_car(ls))->name, name)
                    && !lisp_symbol_is_shadowing(pkg_cell, lisp_car(ls))) {
                    lisp_panic(L"use-package: name conflict with an existing symbol");
                }
            }
            for (LispObject up = pkg_cell->pkg_uses; up != LISP_NIL; up = lisp_cdr(up)) {
                LispClosure *other_cell = lisp_closure_cell(lisp_car(up));
                for (LispObject oe = other_cell->pkg_exports; oe != LISP_NIL; oe = lisp_cdr(oe)) {
                    LispObject other_sym = lisp_car(oe);
                    if (other_sym != exported_sym && lisp_streq(lisp_symbol_cell(other_sym)->name, name)) {
                        LispObject shadow_local = lisp_find_local_symbol(pkg_cell, name);
                        if (shadow_local == LISP_NIL || !lisp_symbol_is_shadowing(pkg_cell, shadow_local)) {
                            lisp_panic(L"use-package: name conflict between used packages");
                        }
                    }
                }
            }
        }

        int already_used = 0;
        for (LispObject u = pkg_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
            if (lisp_car(u) == used_pkg) {
                already_used = 1;
                break;
            }
        }
        if (!already_used) {
            pkg_cell->pkg_uses = lisp_cons(used_pkg, pkg_cell->pkg_uses);
        }
    }
    return lisp_sym_t;
}

// (intern name &optional package): nameはLisp文字列。packageを省略すると*package*
// （現在のパッケージ）を対象にする。lisp_intern_in_packageへそのまま委譲する薄いラッパー
// （milestone78）。#76の`export`はどのパッケージのシンボルでも受け付ける緩い実装のため、
// `defpackage`の`:export`句が対象パッケージへ正しく帰属したシンボルをexportできるように、
// 事前にこの`intern`で対象パッケージへ帰属させておく必要がある。これにより、後で
// `(in-package name)`してから無修飾名で書いても同一オブジェクト（`eq`）に解決される
LispObject lisp_builtin_intern(LispObject args) {
    LispObject name_obj = lisp_car(args);
    lisp_assert_string(name_obj);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    return lisp_intern_in_package(pkg, lisp_closure_cell(name_obj)->str_data);
}

// (in-package name): nameはpackage designator（文字列/symbol・keyword、milestone91で拡張）。
// find-packageで見つからなければpanicし、見つかれば*package*（動的変数）のシンボルセルの
// valueを直接書き換えて対象パッケージへ切り替える（milestone78）
LispObject lisp_builtin_in_package(LispObject args) {
    LispObject name_obj = lisp_car(args);
    LispObject pkg = lisp_find_package(lisp_string_designator_name(name_obj));
    if (pkg == LISP_NIL) {
        lisp_panic(L"in-package: unknown package name");
    }
    lisp_symbol_cell(lisp_sym_package)->value = pkg;
    return pkg;
}

// milestone91: 読み取り系のパッケージ操作ビルトイン群。いずれもpackage designator
// （package本体/文字列/symbol・keyword）を受け付け、省略時は*package*を対象にする

// (package-name package): pkg_nameをLisp文字列として返す
LispObject lisp_builtin_package_name(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    const char *name = lisp_closure_cell(pkg)->pkg_name;
    return lisp_make_string(name, lisp_cstrlen(name));
}

// (package-nicknames package): pkg_nicknames（文字列のconsリスト）をそのまま返す
LispObject lisp_builtin_package_nicknames(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    return lisp_closure_cell(pkg)->pkg_nicknames;
}

// (package-use-list package): pkg_uses（use対象パッケージのconsリスト）をそのまま返す
LispObject lisp_builtin_package_use_list(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    return lisp_closure_cell(pkg)->pkg_uses;
}

// (list-all-packages): global_packages（登録済み全パッケージのconsリスト）をそのまま返す
LispObject lisp_builtin_list_all_packages(LispObject args) {
    (void)args;
    return global_packages;
}

// (%package-symbols package): pkg_symbols（対象パッケージへローカルに帰属する全シンボルの
// consリスト）をそのまま返す内部用ビルトイン。dolistベースのdo-symbols/do-all-symbolsの
// 下地として使う
LispObject lisp_builtin_package_symbols(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    return lisp_closure_cell(pkg)->pkg_symbols;
}

// (%package-exported-symbols package): pkg_exportsをそのまま返す内部用ビルトイン。
// do-external-symbolsの下地として使う
LispObject lisp_builtin_package_exported_symbols(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    return lisp_closure_cell(pkg)->pkg_exports;
}

// (find-symbol name &optional package): nameは文字列限定（symbol designator対応は
// スコープ外）。対象パッケージのローカルシンボル→use先のexportシンボルの順で探索し、
// 見つかれば(cons symbol status)（statusは:internal/:external/:inherited）、
// 見つからなければnilを返す。複数値は存在しないためconsで代用する（CLからの明示的な逸脱）
LispObject lisp_builtin_find_symbol(LispObject args) {
    LispObject name_obj = lisp_car(args);
    lisp_assert_string(name_obj);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    char truncated[LISP_SYMBOL_NAME_MAX];
    lisp_truncate_symbol_name(lisp_closure_cell(name_obj)->str_data, truncated);

    LispObject local = lisp_find_local_symbol(pkg_cell, truncated);
    if (local != LISP_NIL) {
        int exported = 0;
        for (LispObject e = pkg_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            if (lisp_car(e) == local) {
                exported = 1;
                break;
            }
        }
        return lisp_cons(local, exported ? lisp_intern_keyword("external") : lisp_intern_keyword("internal"));
    }

    for (LispObject u = pkg_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
        LispClosure *used_cell = lisp_closure_cell(lisp_car(u));
        for (LispObject e = used_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            LispObject exported_sym = lisp_car(e);
            if (lisp_streq(lisp_symbol_cell(exported_sym)->name, truncated)) {
                return lisp_cons(exported_sym, lisp_intern_keyword("inherited"));
            }
        }
    }

    return LISP_NIL;
}

// (find-all-symbols name): nameは文字列限定。登録済み全パッケージのpkg_symbolsを走査し、
// 名前が一致するシンボル（1パッケージにつき同名ローカルシンボルは最大1つなので重複なし）を
// リストで返す
LispObject lisp_builtin_find_all_symbols(LispObject args) {
    LispObject name_obj = lisp_car(args);
    lisp_assert_string(name_obj);

    char truncated[LISP_SYMBOL_NAME_MAX];
    lisp_truncate_symbol_name(lisp_closure_cell(name_obj)->str_data, truncated);

    LispObject result = LISP_NIL;
    for (LispObject cur = global_packages; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispClosure *pkg_cell = lisp_closure_cell(lisp_car(cur));
        LispObject found = lisp_find_local_symbol(pkg_cell, truncated);
        if (found != LISP_NIL) {
            result = lisp_cons(found, result);
        }
    }
    return result;
}

// (shadow names &optional package): namesは文字列designatorまたはそのリスト。各名前を
// ローカル探索(lisp_find_local_symbol)→無ければその場で新規作成(lisp_create_local_symbol、
// use先のexport探索は経由しない)で解決し、pkg_shadowing_symbolsへeq重複なく追加する
LispObject lisp_builtin_shadow(LispObject args) {
    LispObject names_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject names_list = (lisp_is_string(names_arg) || lisp_is_symbol(names_arg))
                                 ? lisp_cons(names_arg, LISP_NIL)
                                 : names_arg;
    for (LispObject cur = names_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        char truncated[LISP_SYMBOL_NAME_MAX];
        lisp_truncate_symbol_name(lisp_string_designator_name(lisp_car(cur)), truncated);

        LispObject sym = lisp_find_local_symbol(pkg_cell, truncated);
        if (sym == LISP_NIL) {
            sym = lisp_create_local_symbol(pkg, truncated);
        }
        if (!lisp_symbol_is_shadowing(pkg_cell, sym)) {
            pkg_cell->pkg_shadowing_symbols = lisp_cons(sym, pkg_cell->pkg_shadowing_symbols);
        }
    }
    return lisp_sym_t;
}

// (unexport symbols &optional package): symbolsは単一symbolまたはそのリスト（exportと同じ
// 単一/リスト判定）。pkg_exportsからeqで除去する
LispObject lisp_builtin_unexport(LispObject args) {
    LispObject symbols_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject symbols_list = lisp_is_symbol(symbols_arg) ? lisp_cons(symbols_arg, LISP_NIL) : symbols_arg;
    for (LispObject cur = symbols_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject sym = lisp_car(cur);
        LispObject result = LISP_NIL;
        for (LispObject e = pkg_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
            if (lisp_car(e) != sym) {
                result = lisp_cons(lisp_car(e), result);
            }
        }
        pkg_cell->pkg_exports = result;
    }
    return lisp_sym_t;
}

// (unuse-package packages &optional package): packagesは単一のパッケージdesignatorまたは
// そのリスト（use-packageと同じ単一/リスト判定）。pkg_usesからeqで除去する
LispObject lisp_builtin_unuse_package(LispObject args) {
    LispObject used_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject used_list = (lisp_is_package(used_arg) || lisp_is_string(used_arg) || lisp_is_symbol(used_arg))
                                ? lisp_cons(used_arg, LISP_NIL)
                                : used_arg;
    for (LispObject cur = used_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject used_pkg = lisp_resolve_package_designator(lisp_car(cur));
        LispObject result = LISP_NIL;
        for (LispObject u = pkg_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
            if (lisp_car(u) != used_pkg) {
                result = lisp_cons(lisp_car(u), result);
            }
        }
        pkg_cell->pkg_uses = result;
    }
    return lisp_sym_t;
}

// (import symbols &optional package): symbolsは単一symbolまたはそのリスト。対象のpkg_symbols
// に同名だがeqでないシンボルが既にあれば名前衝突としてpanicする。同名同一シンボルなら無処理、
// 無ければそのままpkg_symbolsへ追加する（home package、つまりsym->packageは変更しない）
LispObject lisp_builtin_import(LispObject args) {
    LispObject symbols_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject symbols_list = lisp_is_symbol(symbols_arg) ? lisp_cons(symbols_arg, LISP_NIL) : symbols_arg;
    for (LispObject cur = symbols_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject sym = lisp_car(cur);
        lisp_assert_symbol(sym);
        const char *name = lisp_symbol_cell(sym)->name;

        LispObject existing = lisp_find_local_symbol(pkg_cell, name);
        if (existing != LISP_NIL && existing != sym) {
            lisp_panic(L"import: name conflict with an existing symbol");
        }
        if (existing == LISP_NIL) {
            pkg_cell->pkg_symbols = lisp_cons(sym, pkg_cell->pkg_symbols);
        }
    }
    return lisp_sym_t;
}

// (shadowing-import symbols &optional package): symbolsは単一symbolまたはそのリスト。対象の
// pkg_symbolsに同名の既存シンボルがあれば（eqを問わず）除去してから追加し、pkg_shadowing_symbols
// にもeq重複なく追加する
LispObject lisp_builtin_shadowing_import(LispObject args) {
    LispObject symbols_arg = lisp_car(args);
    LispObject rest = lisp_cdr(args);
    LispObject pkg = lisp_is_cons(rest) ? lisp_resolve_package_designator(lisp_car(rest))
                                         : lisp_symbol_cell(lisp_sym_package)->value;
    LispClosure *pkg_cell = lisp_closure_cell(pkg);

    LispObject symbols_list = lisp_is_symbol(symbols_arg) ? lisp_cons(symbols_arg, LISP_NIL) : symbols_arg;
    for (LispObject cur = symbols_list; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject sym = lisp_car(cur);
        lisp_assert_symbol(sym);
        const char *name = lisp_symbol_cell(sym)->name;

        LispObject result = LISP_NIL;
        for (LispObject ls = pkg_cell->pkg_symbols; ls != LISP_NIL; ls = lisp_cdr(ls)) {
            if (!lisp_streq(lisp_symbol_cell(lisp_car(ls))->name, name)) {
                result = lisp_cons(lisp_car(ls), result);
            }
        }
        pkg_cell->pkg_symbols = lisp_cons(sym, result);

        if (!lisp_symbol_is_shadowing(pkg_cell, sym)) {
            pkg_cell->pkg_shadowing_symbols = lisp_cons(sym, pkg_cell->pkg_shadowing_symbols);
        }
    }
    return lisp_sym_t;
}

// (lock-package designator): pkg_lockedを真にする（milestone110）。milestone111で書込サイト
// (defun/setq/defvar等)がこのフラグを見てpanicするようになる。何度呼んでも冪等
LispObject lisp_builtin_lock_package(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    lisp_closure_cell(pkg)->pkg_locked = 1;
    return lisp_sym_t;
}

// (unlock-package designator): pkg_lockedを偽にする（milestone110）
LispObject lisp_builtin_unlock_package(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    lisp_closure_cell(pkg)->pkg_locked = 0;
    return lisp_sym_t;
}

// (package-locked-p designator): pkg_lockedの現在値を真偽値として返す（milestone110）
LispObject lisp_builtin_package_locked_p(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    return lisp_closure_cell(pkg)->pkg_locked ? lisp_sym_t : LISP_NIL;
}

// milestone111: main.cの起動シーケンス末尾（compiler.lisp/stdlib.lisp/os-package.lisp/os.lisp
// の読み込み・全C自己テストの完了後、lisp_load_init_file直前）から呼ばれる。これより前に
// common-lisp-user上で行われる再定義（milestone81のグローバル参照同一性自己テスト等）は
// ロックの影響を受けず、これより後にloadされるinit.lisp/テストフィクスチャ・REPL入力に対して
// のみ「既存定義の上書きはPackage is locked」が適用される
void lisp_lock_cl_user_package(void) {
    lisp_closure_cell(lisp_cl_user_package)->pkg_locked = 1;
}

// (delete-package designator): global_packagesからeqで除去し、他の全パッケージのpkg_usesから
// もこのパッケージを除去する。対象が現在の*package*自身ならpanicする（CLと同様、カレント
// パッケージは削除できない）
LispObject lisp_builtin_delete_package(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    if (pkg == lisp_symbol_cell(lisp_sym_package)->value) {
        lisp_panic(L"delete-package: cannot delete the current package");
    }

    LispObject result = LISP_NIL;
    for (LispObject cur = global_packages; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        if (lisp_car(cur) != pkg) {
            result = lisp_cons(lisp_car(cur), result);
        }
    }
    global_packages = result;

    for (LispObject cur = global_packages; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispClosure *other_cell = lisp_closure_cell(lisp_car(cur));
        LispObject uses_result = LISP_NIL;
        for (LispObject u = other_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
            if (lisp_car(u) != pkg) {
                uses_result = lisp_cons(lisp_car(u), uses_result);
            }
        }
        other_cell->pkg_uses = uses_result;
    }
    return lisp_sym_t;
}

// (rename-package designator new-name &optional new-nicknames): new-nameがすでに別の既存
// パッケージを指す場合は名前衝突としてpanicする。pkg_nameを新しい文字列バッファへ置き換える
// （lisp_make_package_objectと同じlisp_alloc+手動コピーのパターン、既存バッファの明示的な
// freeは無し）。new-nicknamesが渡された場合のみpkg_nicknamesを置き換える（省略時は既存維持）
LispObject lisp_builtin_rename_package(LispObject args) {
    LispObject pkg = lisp_resolve_package_designator(lisp_car(args));
    LispObject rest = lisp_cdr(args);
    const char *new_name = lisp_string_designator_name(lisp_car(rest));

    LispObject existing = lisp_find_package(new_name);
    if (existing != LISP_NIL && existing != pkg) {
        lisp_panic(L"rename-package: name conflict with an existing package");
    }

    LispClosure *pkg_cell = lisp_closure_cell(pkg);
    UINTN len = lisp_cstrlen(new_name);
    char *buf = (char *)lisp_alloc(len + 1);
    UINTN i = 0;
    while (i < len) {
        buf[i] = new_name[i];
        i++;
    }
    buf[len] = '\0';
    pkg_cell->pkg_name = buf;

    LispObject nicknames_rest = lisp_cdr(rest);
    if (lisp_is_cons(nicknames_rest)) {
        LispObject new_nicknames = LISP_NIL;
        for (LispObject cur = lisp_car(nicknames_rest); cur != LISP_NIL; cur = lisp_cdr(cur)) {
            const char *nickname = lisp_string_designator_name(lisp_car(cur));
            new_nicknames = lisp_cons(lisp_make_string(nickname, lisp_cstrlen(nickname)), new_nicknames);
        }
        pkg_cell->pkg_nicknames = new_nicknames;
    }
    return pkg;
}

// milestone 76: lisp_builtin_export（Lisp呼び出し可能なexport）と#74のリーダー修飾子を
// 組み合わせた自己テスト。「exportを評価した後にpkg:symを読む」という順序は、lisp_load_eval_buffer
// がファイル全体を読み切ってから評価する実装のため test/lisp/ 配下のファイル(load経由)では
// 組めない（milestone72の既知の制約と同根）。C内で直接呼び出し順序を制御することで検証する
int lisp_reader_export_selftest(void) {
    LispObject pkg = lisp_make_package("selftest-pkg76", 0);
    LispObject sym = lisp_intern_in_package(pkg, "exported-sym76");
    LispClosure *pkg_cell = lisp_closure_cell(pkg);
    if (pkg_cell->pkg_exports != LISP_NIL) {
        return 0;
    }

    LispObject args = lisp_cons(sym, lisp_cons(pkg, LISP_NIL));
    if (lisp_builtin_export(args) != lisp_sym_t) {
        return 0;
    }
    // exportした後は単一コロンのpkg:symで同一オブジェクトに解決できる
    if (lisp_read_from_buffer("selftest-pkg76:exported-sym76") != sym) {
        return 0;
    }

    // 同じシンボルをもう一度exportしても結果はtのままで、pkg_exportsに重複追加されない
    if (lisp_builtin_export(args) != lisp_sym_t) {
        return 0;
    }
    UINTN export_count = 0;
    for (LispObject cur = pkg_cell->pkg_exports; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        export_count++;
    }
    if (export_count != 1) {
        return 0;
    }

    // packageを省略した場合は呼び出し時点の*package*(common-lisp-user)が対象になる
    LispObject default_pkg_sym = lisp_intern("export-default-pkg-selftest76");
    LispObject default_args = lisp_cons(default_pkg_sym, LISP_NIL);
    if (lisp_builtin_export(default_args) != lisp_sym_t) {
        return 0;
    }
    if (lisp_read_from_buffer("common-lisp-user:export-default-pkg-selftest76") != default_pkg_sym) {
        return 0;
    }

    return 1;
}

// milestone 77: lisp_builtin_use_package（Lisp呼び出し可能なuse-package）と、
// lisp_intern_in_packageのuse-list探索拡張を組み合わせた自己テスト。「use-packageを評価した後に
// 無修飾名をinternして解決する」という順序はmilestone76と同根の理由でtest/lisp/配下（load経由）
// では組めないため、C内で直接呼び出し順序を制御して検証する
int lisp_reader_use_package_selftest(void) {
    LispObject pkg_a = lisp_make_package("selftest-pkg77a", 0);
    LispObject shared_sym = lisp_intern_in_package(pkg_a, "shared-sym77");
    LispObject export_args = lisp_cons(shared_sym, lisp_cons(pkg_a, LISP_NIL));
    if (lisp_builtin_export(export_args) != lisp_sym_t) {
        return 0;
    }

    LispObject pkg_b = lisp_make_package("selftest-pkg77b", 0);
    LispObject use_args = lisp_cons(pkg_a, lisp_cons(pkg_b, LISP_NIL));
    if (lisp_builtin_use_package(use_args) != lisp_sym_t) {
        return 0;
    }

    // use-packageの効果で、pkg_bで無修飾名"shared-sym77"をinternするとpkg_aの
    // exportシンボルと同一オブジェクト(eq)に解決される（新規シンボルは作られない）
    if (lisp_intern_in_package(pkg_b, "shared-sym77") != shared_sym) {
        return 0;
    }
    LispClosure *pkg_b_cell = lisp_closure_cell(pkg_b);
    if (pkg_b_cell->pkg_symbols != LISP_NIL) {
        return 0;
    }

    // 同じuse-packageを再度呼んでも冪等（pkg_usesに重複追加されない）
    if (lisp_builtin_use_package(use_args) != lisp_sym_t) {
        return 0;
    }
    UINTN use_count = 0;
    for (LispObject cur = pkg_b_cell->pkg_uses; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        use_count++;
    }
    if (use_count != 1) {
        return 0;
    }

    // パッケージ名の文字列でも指定できる（lisp_resolve_package_designator）
    LispObject pkg_c = lisp_make_package("selftest-pkg77c", 0);
    UINTN a_name_len = 0;
    while ("selftest-pkg77a"[a_name_len] != '\0') {
        a_name_len++;
    }
    LispObject pkg_a_name = lisp_make_string("selftest-pkg77a", a_name_len);
    UINTN c_name_len = 0;
    while ("selftest-pkg77c"[c_name_len] != '\0') {
        c_name_len++;
    }
    LispObject pkg_c_name = lisp_make_string("selftest-pkg77c", c_name_len);
    LispObject use_args_by_name = lisp_cons(pkg_a_name, lisp_cons(pkg_c_name, LISP_NIL));
    if (lisp_builtin_use_package(use_args_by_name) != lisp_sym_t) {
        return 0;
    }
    if (lisp_intern_in_package(pkg_c, "shared-sym77") != shared_sym) {
        return 0;
    }

    // packages-to-useはリストでも複数まとめて指定できる
    LispObject pkg_d = lisp_make_package("selftest-pkg77d", 0);
    LispObject other_pkg = lisp_make_package("selftest-pkg77e", 0);
    LispObject other_sym = lisp_intern_in_package(other_pkg, "other-sym77");
    LispObject other_export_args = lisp_cons(other_sym, lisp_cons(other_pkg, LISP_NIL));
    if (lisp_builtin_export(other_export_args) != lisp_sym_t) {
        return 0;
    }
    LispObject use_list_arg = lisp_cons(pkg_a, lisp_cons(other_pkg, LISP_NIL));
    LispObject use_args_list = lisp_cons(use_list_arg, lisp_cons(pkg_d, LISP_NIL));
    if (lisp_builtin_use_package(use_args_list) != lisp_sym_t) {
        return 0;
    }
    if (lisp_intern_in_package(pkg_d, "shared-sym77") != shared_sym) {
        return 0;
    }
    if (lisp_intern_in_package(pkg_d, "other-sym77") != other_sym) {
        return 0;
    }

    // packageを省略した場合は呼び出し時点の*package*(common-lisp-user)が対象になる
    LispObject pkg_f = lisp_make_package("selftest-pkg77f", 0);
    LispObject f_sym = lisp_intern_in_package(pkg_f, "f-sym77");
    LispObject f_export_args = lisp_cons(f_sym, lisp_cons(pkg_f, LISP_NIL));
    if (lisp_builtin_export(f_export_args) != lisp_sym_t) {
        return 0;
    }
    LispObject use_default_args = lisp_cons(pkg_f, LISP_NIL);
    if (lisp_builtin_use_package(use_default_args) != lisp_sym_t) {
        return 0;
    }
    if (lisp_intern("f-sym77") != f_sym) {
        return 0;
    }

    return 1;
}

// milestone 78: intern（Lisp呼び出し可能なintern）・in-package（*package*切替）・defpackage
// マクロ（lisp/stdlib.lisp）を組み合わせた自己テスト。「in-packageで*package*を切り替えた後に
// 無修飾名をinternして解決する」という順序はmilestone76/77と同根の理由でtest/lisp/配下
// (load経由)では組めないため、C内で直接呼び出し順序を制御して検証する。defpackageマクロ自体も
// (defpackage "name" (:export "sym") (:use "pkg"))相当のフォームをlisp_evalで直接評価して
// 展開結果の正しさ（対象パッケージへ実際にintern済みのシンボルをexportしていること）を確認する
int lisp_reader_defpackage_selftest(void) {
    LispObject saved_package = lisp_symbol_cell(lisp_sym_package)->value;

    // internの基本動作: 指定パッケージへ帰属したシンボルを返し、同名を再internすると
    // 同一オブジェクト(eq)を返す。lisp_intern_in_packageの直接呼び出しと一致することも確認する
    LispObject pkg_x = lisp_make_package("selftest-pkg78x", 0);
    LispObject name_x = lisp_make_string("intern-sym78", 12);
    LispObject intern_args_x = lisp_cons(name_x, lisp_cons(pkg_x, LISP_NIL));
    LispObject sym_x1 = lisp_builtin_intern(intern_args_x);
    LispObject sym_x2 = lisp_builtin_intern(intern_args_x);
    if (sym_x1 != sym_x2) {
        return 0;
    }
    if (lisp_intern_in_package(pkg_x, "intern-sym78") != sym_x1) {
        return 0;
    }

    // in-packageは*package*を書き換えて対象パッケージオブジェクトを返す。以後、packageを
    // 省略したinternは*package*(切替後)を対象にする
    LispObject pkg_y = lisp_make_package("selftest-pkg78y", 0);
    LispObject in_package_args = lisp_cons(lisp_make_string("selftest-pkg78y", 15), LISP_NIL);
    if (lisp_builtin_in_package(in_package_args) != pkg_y) {
        return 0;
    }
    if (lisp_symbol_cell(lisp_sym_package)->value != pkg_y) {
        return 0;
    }
    LispObject name_y = lisp_make_string("intern-sym78y", 13);
    LispObject sym_y1 = lisp_builtin_intern(lisp_cons(name_y, LISP_NIL));
    if (lisp_intern_in_package(pkg_y, "intern-sym78y") != sym_y1) {
        return 0;
    }
    lisp_symbol_cell(lisp_sym_package)->value = saved_package;

    // defpackageマクロの展開・評価: (defpackage "selftest-pkg78z" (:export "z-sym78")
    // (:use "selftest-pkg78x"))相当のフォームを直接構築してlisp_evalする
    LispObject pkg_name_z = lisp_make_string("selftest-pkg78z", 15);
    LispObject export_clause = lisp_cons(lisp_intern_keyword("export"),
                                          lisp_cons(lisp_make_string("z-sym78", 7), LISP_NIL));
    LispObject use_clause = lisp_cons(lisp_intern_keyword("use"),
                                       lisp_cons(lisp_make_string("selftest-pkg78x", 15), LISP_NIL));
    LispObject defpackage_form = lisp_cons(lisp_intern("defpackage"),
                                   lisp_cons(pkg_name_z,
                                   lisp_cons(export_clause,
                                   lisp_cons(use_clause, LISP_NIL))));
    LispObject pkg_z = lisp_eval(defpackage_form, global_env);
    if (lisp_return_tag != LISP_NIL) {
        return 0;
    }
    if (!lisp_is_package(pkg_z)) {
        return 0;
    }
    if (lisp_find_package("selftest-pkg78z") != pkg_z) {
        return 0;
    }

    // :export句で渡した"z-sym78"は展開内のinternによりpkg_zへ正しく帰属した上でexportされて
    // いる。in-packageでpkg_zへ切り替えて無修飾名を読んだ場合と同一オブジェクト(eq)であることが、
    // 本マイルストーンが解決すべき核心の識別性保証にあたる
    LispObject z_sym = lisp_intern_in_package(pkg_z, "z-sym78");
    LispClosure *pkg_z_cell = lisp_closure_cell(pkg_z);
    int z_exported = 0;
    for (LispObject e = pkg_z_cell->pkg_exports; e != LISP_NIL; e = lisp_cdr(e)) {
        if (lisp_car(e) == z_sym) {
            z_exported = 1;
            break;
        }
    }
    if (!z_exported) {
        return 0;
    }
    lisp_symbol_cell(lisp_sym_package)->value = pkg_z;
    LispObject z_sym_unqualified = lisp_intern("z-sym78");
    lisp_symbol_cell(lisp_sym_package)->value = saved_package;
    if (z_sym_unqualified != z_sym) {
        return 0;
    }

    // :use句で渡した"selftest-pkg78x"がpkg_zのpkg_usesへ実際に追加されていることも確認する
    int used_x = 0;
    for (LispObject u = pkg_z_cell->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
        if (lisp_car(u) == pkg_x) {
            used_x = 1;
            break;
        }
    }
    if (!used_x) {
        return 0;
    }

    return 1;
}

// milestone 80: EfiMainの起動順序（lisp_heap_init→lisp_packages_init→lisp_symbols_init→
// lisp_builtins_init→compiler.lisp/stdlib.lispのload、という既存の並び自体はmilestone73で
// *package*を導入した時点で既に確定済みで、本マイルストーンで変更は不要だった）が実際に
// 「lisp_packages_initが*package*をcommon-lisp-userへseedし終えてからlisp_symbols_initが
// 特殊形式シンボルをinternする」という前提を満たしていること、およびcompiler.lisp/stdlib.lisp
// が引き続きcommon-lisp-userへinternされ、無修飾で再internした同名シンボルと`eq`であることを
// 確認する自己テスト
int lisp_bootstrap_package_context_selftest(void) {
    // *package*の既定値がcommon-lisp-userであること（lisp_packages_initのseedが
    // lisp_symbols_init以降も保たれていること）
    if (lisp_symbol_cell(lisp_sym_package)->value != lisp_cl_user_package) {
        return 0;
    }

    // lisp_symbols_initでintern済みの特殊形式シンボル（defun）がcommon-lisp-userへ
    // 帰属していること、および無修飾で再internすると同一オブジェクト(eq)を返すこと
    if (lisp_sym_defun == LISP_NIL || lisp_symbol_cell(lisp_sym_defun)->package != lisp_cl_user_package) {
        return 0;
    }
    if (lisp_intern("defun") != lisp_sym_defun) {
        return 0;
    }

    // compiler.lisp/stdlib.lispロード後にdefunで定義された関数（list）のシンボルも
    // common-lisp-userへ帰属し、無修飾で再internすると同一オブジェクト(eq)を返すこと。
    // 関数セル(fn、milestone93/94)で実際に束縛されている（unbound functionでpanicしない）
    // ことも確認する
    LispObject list_sym = lisp_intern("list");
    if (lisp_symbol_cell(list_sym)->package != lisp_cl_user_package) {
        return 0;
    }
    if (!lisp_is_closure(lisp_symbol_cell(list_sym)->fn)) {
        return 0;
    }

    return 1;
}

// milestone 81: OP_GLOBAL_REF/OP_GLOBAL_SET（lisp_vm_integration.mdマイルストーン51）が
// global_envをシンボルのeq同一性で解決する前提が*package*導入後も壊れていないことを確認する
// 自己テスト。(a)同一パッケージ内でのdefun前方参照・相互再帰、(b)in-packageを挟んでも同一
// パッケージ・同一名の再読込みが常に同一シンボルオブジェクトに再解決されること、(c)*package*
// が非既定値を指している最中にlisp_gc()を実行しても*package*自身の値・その最中にinternした
// シンボルが回収されないこと（global_packagesがGCルートのため、"現在の"パッケージかどうかに
// 関わらず全パッケージ・全所属シンボルが常に保護されるという既存設計(milestone72)の直接確認）。
// 特殊形式トークン（defun/if/let等）はcommon-lisp-userからexportされていないため、
// use-packageしていてもcommon-lisp-user以外のパッケージへin-packageすると無修飾では使えなく
// なる（milestone79の対話検証で判明した既知の制約）。本テストはその制約に触れないよう、
// defun等のLisp評価は常に*package*がcommon-lisp-userのままの状態で行い、パッケージ切替自体は
// *package*のシンボルセルを直接書き換えるC内部操作に限定する
int lisp_global_ref_package_identity_selftest(void) {
    LispObject saved_package = lisp_symbol_cell(lisp_sym_package)->value;

    // (a) 同一パッケージ内でのdefun前方参照・相互再帰（既存カバレッジの再確認）
    lisp_eval(lisp_read_from_buffer(
        "(defun m81-even (n) (if (eq n 0) t (m81-odd (- n 1))))"), global_env);
    lisp_eval(lisp_read_from_buffer(
        "(defun m81-odd (n) (if (eq n 0) nil (m81-even (- n 1))))"), global_env);
    if (lisp_eval(lisp_read_from_buffer("(m81-even 10)"), global_env) != lisp_sym_t) {
        return 0;
    }

    // (b) in-packageを挟んでも同一パッケージ・同一名の再読込みが常に同一シンボルオブジェクトに
    // 再解決されること。パッケージ切替自体はlisp_sym_packageのvalueを直接書き換えて行い、
    // defunの評価はcommon-lisp-userへ戻してから行う（上記の特殊形式export制約を踏まないため）
    LispObject other_pkg = lisp_make_package("selftest-pkg81-other", 0);
    LispObject fn_sym_before = lisp_intern("m81-redef-fn");
    lisp_eval(lisp_read_from_buffer("(defun m81-redef-fn (x) (+ x 1))"), global_env);

    lisp_symbol_cell(lisp_sym_package)->value = other_pkg;
    lisp_symbol_cell(lisp_sym_package)->value = lisp_cl_user_package;
    if (lisp_intern("m81-redef-fn") != fn_sym_before) {
        return 0;
    }

    lisp_eval(lisp_read_from_buffer("(defun m81-redef-fn (x) (+ x 2))"), global_env);
    if (lisp_intern("m81-redef-fn") != fn_sym_before) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(m81-redef-fn 10)"), global_env) != lisp_make_fixnum(12)) {
        return 0;
    }

    // (c) *package*が非既定値(other_pkg)を指している最中にlisp_gc()を実行しても、*package*
    // 自身の値、およびそのpkg_symbolsへinternしたシンボルが回収されないこと
    lisp_symbol_cell(lisp_sym_package)->value = other_pkg;
    LispObject probe_sym = lisp_intern_in_package(other_pkg, "m81-probe");
    lisp_gc();
    if (lisp_symbol_cell(lisp_sym_package)->value != other_pkg) {
        return 0;
    }
    if (lisp_intern_in_package(other_pkg, "m81-probe") != probe_sym) {
        return 0;
    }

    lisp_symbol_cell(lisp_sym_package)->value = saved_package;
    return 1;
}

// milestone 100: lisp_symbols_init末尾でcommon-lisp-userからexportした特殊形式トークン
// （defun/if/let等の特殊形式ディスパッチシンボル・tの自己評価トークン・&optional等の
// ラムダリストキーワード）が、*package*を切り替えてuse-package済みの別パッケージからでも
// 無修飾で正しく解決される（同一オブジェクトであること、かつ実際に特殊形式として機能する
// こと）ことを確認する自己テスト。「*package*を切り替えた後に無修飾名を読む」という順序は
// milestone76以降と同根の理由でtest/lisp/配下（load経由）では組めないため、C内で直接
// 呼び出し順序を制御して検証する。真なら成功
int lisp_reader_special_form_export_selftest(void) {
    LispObject saved_package = lisp_symbol_cell(lisp_sym_package)->value;

    LispObject pkg = lisp_make_package("selftest-pkg100", 0);
    LispObject use_args = lisp_cons(lisp_cl_user_package, lisp_cons(pkg, LISP_NIL));
    if (lisp_builtin_use_package(use_args) != lisp_sym_t) {
        return 0;
    }
    lisp_symbol_cell(lisp_sym_package)->value = pkg;

    // (a) exportした全トークンが、use-package先での無修飾名解決で同一オブジェクト(eq)に
    // 解決されること
    struct { const char *name; LispObject expected; } tokens[] = {
        {"t", lisp_sym_t}, {"quote", lisp_sym_quote}, {"if", lisp_sym_if},
        {"lambda", lisp_sym_lambda}, {"defun", lisp_sym_defun}, {"defmacro", lisp_sym_defmacro},
        {"quasiquote", lisp_sym_quasiquote}, {"unquote", lisp_sym_unquote},
        {"unquote-splicing", lisp_sym_unquote_splicing}, {"progn", lisp_sym_progn},
        {"let", lisp_sym_let}, {"let*", lisp_sym_let_star}, {"setq", lisp_sym_setq},
        {"cond", lisp_sym_cond}, {"and", lisp_sym_and}, {"or", lisp_sym_or},
        {"when", lisp_sym_when}, {"unless", lisp_sym_unless}, {"defvar", lisp_sym_defvar},
        {"defparameter", lisp_sym_defparameter}, {"block", lisp_sym_block},
        {"return-from", lisp_sym_return_from}, {"do", lisp_sym_do}, {"function", lisp_sym_function},
        {"&optional", lisp_sym_lambda_optional}, {"&rest", lisp_sym_lambda_rest},
        {"&key", lisp_sym_lambda_key}, {"&aux", lisp_sym_lambda_aux},
        {"&allow-other-keys", lisp_sym_lambda_allow_other_keys},
    };
    for (UINTN i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        if (lisp_intern(tokens[i].name) != tokens[i].expected) {
            return 0;
        }
    }

    // (b) 単なるeq識別性だけでなく、実際にreader+evalの経路で特殊形式として機能すること
    // （exportしなければ本来unbound variableでpanicしていた挙動の回帰確認）
    if (lisp_eval(lisp_read_from_buffer("t"), global_env) != lisp_sym_t) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(if t 1 2)"), global_env) != lisp_make_fixnum(1)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(let ((x 5)) x)"), global_env) != lisp_make_fixnum(5)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(and t t)"), global_env) != lisp_sym_t) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(or nil t)"), global_env) != lisp_sym_t) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(cond (t 42))"), global_env) != lisp_make_fixnum(42)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(block m100-blk (return-from m100-blk 7))"), global_env)
        != lisp_make_fixnum(7)) {
        return 0;
    }
    lisp_eval(lisp_read_from_buffer("(defun m100-opt-fn (x &optional y) (if y y x))"), global_env);
    if (lisp_eval(lisp_read_from_buffer("(m100-opt-fn 1)"), global_env) != lisp_make_fixnum(1)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(m100-opt-fn 1 2)"), global_env) != lisp_make_fixnum(2)) {
        return 0;
    }

    lisp_symbol_cell(lisp_sym_package)->value = saved_package;
    return 1;
}

// milestone 101: lisp_builtins_initで登録した全ビルトイン関数（LISP_REGISTER_BUILTIN経由の
// ものと、それを経由しないprint-object・*macroexpand-hook*の両方）が、*package*を切り替えて
// use-package済みの別パッケージからでも無修飾で正しく解決される（common-lisp-user側の正規
// オブジェクトとeqであり、実際に呼び出せる）ことを確認する自己テスト。milestone100と同じ理由で
// test/lisp/配下（load経由）では組めないため、C内で直接呼び出し順序を制御して検証する。
// また、milestone79で判明した「in-package自身が無修飾で呼べず二重コロン修飾でしか復帰できない」
// という既存の制約がこれで解消されることも合わせて確認する。真なら成功
int lisp_reader_builtin_export_selftest(void) {
    LispObject saved_package = lisp_symbol_cell(lisp_sym_package)->value;

    LispObject pkg = lisp_make_package("selftest-pkg101", 0);
    LispObject use_args = lisp_cons(lisp_cl_user_package, lisp_cons(pkg, LISP_NIL));
    if (lisp_builtin_use_package(use_args) != lisp_sym_t) {
        return 0;
    }
    lisp_symbol_cell(lisp_sym_package)->value = pkg;

    // (a) 代表的なビルトインシンボル（LISP_REGISTER_BUILTIN経由のものと個別exportの2つ）が、
    // use-package先での無修飾名解決でcommon-lisp-user側の正規オブジェクトと同一(eq)であること
    const char *names[] = {
        "car", "cdr", "cons", "eq", "atom", "+", "-", "<", "funcall", "apply",
        "symbolp", "macroexpand-1", "gensym", "in-package",
        "print-object", "*macroexpand-hook*",
    };
    for (UINTN i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (lisp_intern(names[i]) != lisp_intern_in_package(lisp_cl_user_package, names[i])) {
            return 0;
        }
    }

    // (b) 単なるeq識別性だけでなく、実際にreader+evalの経路で呼び出せること
    // （exportしなければ本来unbound variableでpanicしていた挙動の回帰確認）
    if (lisp_eval(lisp_read_from_buffer("(car (cons 1 2))"), global_env) != lisp_make_fixnum(1)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(cdr (cons 1 2))"), global_env) != lisp_make_fixnum(2)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(+ 1 2)"), global_env) != lisp_make_fixnum(3)) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(< 1 2)"), global_env) != lisp_sym_t) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("(atom 5)"), global_env) != lisp_sym_t) {
        return 0;
    }

    // (c) milestone79の既存制約解消確認: *package*がcommon-lisp-user以外の間でも、
    // in-package自身を無修飾（二重コロン修飾なし）で呼んでcommon-lisp-userへ復帰できること
    if (lisp_eval(lisp_read_from_buffer("(in-package \"common-lisp-user\")"), global_env)
        != lisp_cl_user_package) {
        return 0;
    }
    if (lisp_symbol_cell(lisp_sym_package)->value != lisp_cl_user_package) {
        return 0;
    }

    lisp_symbol_cell(lisp_sym_package)->value = saved_package;
    return 1;
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

// DIAGNOSTIC (milestone 60 regression debugging, not a deliverable): 残りヒープバイト数を返す
LispObject lisp_builtin_heap_remaining(LispObject args) {
    return lisp_make_fixnum((long long)(lisp_heap_end - lisp_heap_ptr));
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

// milestone 57: compile-expr側でdefvar/defparameterを脱糖するための最小プリミティブ。
// is_specialはコンパイル時に静的解決できない実行時の可変プロパティ（defvarが実際に
// 実行されるまで真にならない）なので、新opcodeではなく通常のOP_CALL経由で呼べる
// ビルトインとして公開する（milestone56がappendを再利用したのと同じ考え方）。
// lisp_evalのdefvar/defparameter特殊形式（本ファイル上部）と同じis_special/valueの
// 直接操作を行うが、値の評価タイミング（既存is_specialなら評価しない等）は
// compile-defvar側（lisp/stdlib.lisp）でif/prognへ脱糖して制御する
LispObject lisp_builtin_special_variable_p(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    return lisp_symbol_cell(sym)->is_special ? lisp_sym_t : LISP_NIL;
}

// (establish-special sym value): symをvalueで動的変数として確立する（常に上書き）。
// symを返す
LispObject lisp_builtin_establish_special(LispObject args) {
    LispObject sym = lisp_car(args);
    lisp_assert_symbol(sym);
    LispObject value = lisp_car(lisp_cdr(args));
    LispSymbol *cell = lisp_symbol_cell(sym);
    // milestone111: 呼び出し元はcompile-defvar(is_specialがまだ偽の時のみ呼ぶようガード済み、
    // 常にfalse相当なのでチェックは素通り)とcompile-defparameter(無条件に呼ぶ、既存の値の
    // 上書き=redefinition)の2箇所。ツリーウォークのdefparameterと同じ「上書き前のis_special」
    // で判定する
    if (cell->is_special && lisp_symbol_home_package_locked(sym)) {
        lisp_panic(L"Package is locked");
    }
    cell->value = value;
    cell->is_special = 1;
    return sym;
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

// (vm-make-closure nargs max-locals bytecode-list constants-list upvalue-descs-list) -> 実行可能な
// コンパイル済み関数オブジェクト。upvalue-descs-listの各要素は(kind . index)のcons
// max-locals（milestone83/84）はnargs以上でなければならない（仮引数がスロット0..nargs-1を占め、
// let等が続くスロットを積み増していくため）
LispObject lisp_builtin_vm_make_closure(LispObject args) {
    LispObject nargs_obj = lisp_car(args);
    LispObject max_locals_obj = lisp_car(lisp_cdr(args));
    LispObject bytecode_list = lisp_car(lisp_cdr(lisp_cdr(args)));
    LispObject constants_list = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(args))));
    LispObject upvalue_descs_list = lisp_car(lisp_cdr(lisp_cdr(lisp_cdr(lisp_cdr(args)))));

    lisp_assert_fixnum(nargs_obj);
    UINTN nargs = (UINTN)lisp_fixnum_value(nargs_obj);
    lisp_assert_fixnum(max_locals_obj);
    UINTN max_locals = (UINTN)lisp_fixnum_value(max_locals_obj);
    if (max_locals < nargs) {
        lisp_panic(L"vm-make-closure: max-locals must be >= nargs");
    }

    unsigned char bytecode[VM_BRIDGE_MAX_BYTECODE];
    UINTN bytecode_len = 0;
    LispObject cur = bytecode_list;
    while (lisp_is_cons(cur)) {
        if (bytecode_len >= VM_BRIDGE_MAX_BYTECODE) {
            lisp_panic_vm_bridge_limit_exceeded(L"vm-make-closure: bytecode too long", VM_BRIDGE_MAX_BYTECODE);
        }
        LispObject b = lisp_car(cur);
        lisp_assert_fixnum(b);
        long long b_value = lisp_fixnum_value(b);
        if (b_value < 0 || b_value > 255) {
            // milestone 66: low-byte/high-byte(lisp/compiler.lisp)が2byte(0-65535)
            // オペランドをリトルエンディアン分割する際、実際のオペランド値(ジャンプ先
            // オフセット・定数プール/ローカル/upvalue索引)が65535を超えるとhigh-byteが
            // 255を超える値を返し、下のキャストでサイレントに切り捨てられてバイトコードが
            // 静かに破損する。ここで1byteに収まらない値を明示的なpanicで落とす
            lisp_panic_vm_bridge_limit_exceeded(L"vm-make-closure: bytecode byte out of range", 255);
        }
        bytecode[bytecode_len++] = (unsigned char)b_value;
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

    LispObject fn = lisp_make_compiled(bytecode, bytecode_len, constants, constants_len, nargs, max_locals);

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
    // milestone87で発見: main.cのREPLループ同様、トップレベルフォーム間
    // （直前の評価が完全に戻り切っており、tree-walkのcompile-and-runフレームが
    // Cスタックに一切residentしていない安全地点）でのみGCを起動する。loadは
    // 複数フォームを1回のバッファ評価でまとめて処理するため、このチェックが無いと
    // load経由の実行だけがREPLの自動GC機会を一度も得られず、個々には正当な
    // ゴミであっても回収されずヒープを枯渇させる（test-compile-expr.lispの
    // 28関数連続呼び出しで実際に発生した）。評価待ちの残りフォーム(reversed)自体は
    // Cローカル変数にしか存在しないため、その間だけlisp_gc_extra_rootへ退避して
    // GCのルートに含める
    LispObject saved_extra_root = lisp_gc_extra_root;
    while (reversed != LISP_NIL) {
        lisp_gc_extra_root = reversed;
        if (lisp_heap_low()) {
            lisp_gc();
        }
        reversed = lisp_gc_extra_root;
        result = lisp_eval_toplevel(lisp_car(reversed));
        reversed = lisp_cdr(reversed);
    }
    lisp_gc_extra_root = saved_extra_root;
    return result;
}

static EFI_GUID lisp_guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID_VALUE;
static EFI_GUID lisp_guid_simple_file_system = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID_VALUE;
static EFI_GUID lisp_guid_simple_text_input_ex = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID_VALUE;

// milestone 116: EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOLをシステム全体から検索して取得する。
// LocateProtocolはHandleProtocolと異なりハンドルを指定しないため、ConsoleInHandle以外に
// インストールされている場合でも取得できる。見つからない場合はpanicせずg_text_input_exを
// NULLのままにする（ファームウェア実装依存であり、Ctrl検知を使わない起動経路には
// 影響させないため）
void lisp_input_ex_init(void) {
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;
    void *interface = (void *)0;
    if (bs->LocateProtocol(&lisp_guid_simple_text_input_ex, (void *)0, &interface) != 0) {
        g_text_input_ex = (void *)0;
        return;
    }
    g_text_input_ex = (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *)interface;
}

int lisp_key_state_is_lone_ctrl(const EFI_KEY_DATA *key_data) {
    UINT32 shift_state = key_data->KeyState.KeyShiftState;
    if ((shift_state & EFI_SHIFT_STATE_VALID) == 0) {
        return 0;
    }
    UINT32 ctrl_bits = shift_state & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED);
    if (ctrl_bits == 0) {
        return 0;
    }
    UINT32 other_modifier_bits = shift_state & (
        EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED |
        EFI_LEFT_ALT_PRESSED | EFI_RIGHT_ALT_PRESSED |
        EFI_LEFT_LOGO_PRESSED | EFI_RIGHT_LOGO_PRESSED |
        EFI_MENU_KEY_PRESSED | EFI_SYS_REQ_PRESSED
    );
    if (other_modifier_bits != 0) {
        return 0;
    }
    if (key_data->Key.ScanCode != 0 || key_data->Key.UnicodeChar != 0) {
        return 0;
    }
    return 1;
}

int lisp_key_state_selftest(void) {
    EFI_KEY_DATA lone_ctrl;
    lone_ctrl.Key.ScanCode = 0;
    lone_ctrl.Key.UnicodeChar = 0;
    lone_ctrl.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED;
    lone_ctrl.KeyState.KeyToggleState = 0;
    if (!lisp_key_state_is_lone_ctrl(&lone_ctrl)) {
        return 0;
    }

    EFI_KEY_DATA lone_ctrl_right = lone_ctrl;
    lone_ctrl_right.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_RIGHT_CONTROL_PRESSED;
    if (!lisp_key_state_is_lone_ctrl(&lone_ctrl_right)) {
        return 0;
    }

    // ファームウェアが修飾キー状態を報告できない(validビット無し)場合は判定不能→偽
    EFI_KEY_DATA invalid_state = lone_ctrl;
    invalid_state.KeyState.KeyShiftState = EFI_LEFT_CONTROL_PRESSED;
    if (lisp_key_state_is_lone_ctrl(&invalid_state)) {
        return 0;
    }

    // Ctrl+Shiftのような他修飾キーとの組み合わせは対象外
    EFI_KEY_DATA ctrl_shift = lone_ctrl;
    ctrl_shift.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED | EFI_LEFT_SHIFT_PRESSED;
    if (lisp_key_state_is_lone_ctrl(&ctrl_shift)) {
        return 0;
    }

    // Ctrl+A（文字キーとの組み合わせ、UnicodeCharが0x01等の非0値になる）も対象外
    EFI_KEY_DATA ctrl_a = lone_ctrl;
    ctrl_a.Key.UnicodeChar = 1;
    if (lisp_key_state_is_lone_ctrl(&ctrl_a)) {
        return 0;
    }

    // Ctrlが全く押されていない通常のキーストロークも対象外
    EFI_KEY_DATA plain_key;
    plain_key.Key.ScanCode = 0;
    plain_key.Key.UnicodeChar = (CHAR16)'a';
    plain_key.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;
    plain_key.KeyState.KeyToggleState = 0;
    if (lisp_key_state_is_lone_ctrl(&plain_key)) {
        return 0;
    }

    return 1;
}

LispCtrlWaitOutcome lisp_ctrl_wait_classify(UINTN fired_index, const EFI_KEY_DATA *key_data) {
    if (fired_index == LISP_CTRL_WAIT_TIMER_INDEX) {
        return LISP_CTRL_WAIT_OUTCOME_TIMEOUT;
    }
    if (lisp_key_state_is_lone_ctrl(key_data)) {
        return LISP_CTRL_WAIT_OUTCOME_MATCH;
    }
    return LISP_CTRL_WAIT_OUTCOME_DISCARD;
}

int lisp_ctrl_wait_classify_selftest(void) {
    EFI_KEY_DATA dummy;
    dummy.Key.ScanCode = 0;
    dummy.Key.UnicodeChar = 0;
    dummy.KeyState.KeyShiftState = 0;
    dummy.KeyState.KeyToggleState = 0;
    // タイマー側が発火した場合は鍵データの内容に関わらずタイムアウト
    if (lisp_ctrl_wait_classify(LISP_CTRL_WAIT_TIMER_INDEX, &dummy) != LISP_CTRL_WAIT_OUTCOME_TIMEOUT) {
        return 0;
    }

    EFI_KEY_DATA lone_ctrl = dummy;
    lone_ctrl.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED;
    if (lisp_ctrl_wait_classify(LISP_CTRL_WAIT_KEY_INDEX, &lone_ctrl) != LISP_CTRL_WAIT_OUTCOME_MATCH) {
        return 0;
    }

    EFI_KEY_DATA plain_a = dummy;
    plain_a.Key.UnicodeChar = (CHAR16)'a';
    plain_a.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;
    if (lisp_ctrl_wait_classify(LISP_CTRL_WAIT_KEY_INDEX, &plain_a) != LISP_CTRL_WAIT_OUTCOME_DISCARD) {
        return 0;
    }

    return 1;
}

// milestone 117: lisp_builtin_sleep(milestone25)と同じCreateEvent/SetTimer/WaitForEvent/
// CloseEventパターンを、g_text_input_ex->WaitForKeyExとの複数イベント同時待ちへ拡張した
// もの。g_text_input_exが未検出ならそもそも判定不能として0を返す
int lisp_wait_for_double_ctrl(UINT64 window_100ns) {
    if (g_text_input_ex == (void *)0) {
        return 0;
    }
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;

    // 1回目のCtrl単体押下を無期限に待つ(マッチしない鍵イベントは読み捨てて待ち続ける)
    for (;;) {
        UINTN index;
        EFI_EVENT wait_events[1];
        wait_events[LISP_CTRL_WAIT_KEY_INDEX] = g_text_input_ex->WaitForKeyEx;
        if (bs->WaitForEvent(1, wait_events, &index) != 0) {
            lisp_panic(L"double-ctrl: failed to wait for first key event");
        }
        EFI_KEY_DATA key_data;
        if (g_text_input_ex->ReadKeyStrokeEx(g_text_input_ex, &key_data) != 0) {
            continue;
        }
        if (lisp_ctrl_wait_classify(LISP_CTRL_WAIT_KEY_INDEX, &key_data) == LISP_CTRL_WAIT_OUTCOME_MATCH) {
            break;
        }
    }

    // 2回目のCtrl単体押下がwindow_100ns以内に来るかを判定する
    EFI_EVENT timer_event;
    if (bs->CreateEvent(EVT_TIMER, TPL_APPLICATION, (void *)0, (void *)0, &timer_event) != 0) {
        lisp_panic(L"double-ctrl: failed to create timer event");
    }
    if (bs->SetTimer(timer_event, TimerRelative, window_100ns) != 0) {
        lisp_panic(L"double-ctrl: failed to set timer");
    }

    int matched = 0;
    for (;;) {
        UINTN index;
        EFI_EVENT wait_events[2];
        wait_events[LISP_CTRL_WAIT_KEY_INDEX] = g_text_input_ex->WaitForKeyEx;
        wait_events[LISP_CTRL_WAIT_TIMER_INDEX] = timer_event;
        if (bs->WaitForEvent(2, wait_events, &index) != 0) {
            lisp_panic(L"double-ctrl: failed to wait for second key/timer event");
        }
        if (index == LISP_CTRL_WAIT_TIMER_INDEX) {
            matched = 0;
            break;
        }
        EFI_KEY_DATA key_data;
        if (g_text_input_ex->ReadKeyStrokeEx(g_text_input_ex, &key_data) != 0) {
            continue;
        }
        if (lisp_ctrl_wait_classify(index, &key_data) == LISP_CTRL_WAIT_OUTCOME_MATCH) {
            matched = 1;
            break;
        }
        // DISCARD: Ctrl以外の鍵イベントだったので待ち続ける(timer_eventはまだ未消費)
    }
    bs->CloseEvent(timer_event);
    return matched;
}

// milestone 119: 実際にファームウェアのQueryMode/SetCursorPositionを呼び、戻り値が
// EFI_SUCCESSであること・QueryModeが返すCols/Rowsが妥当な範囲であること・
// SetCursorPosition後にConOut->Mode->CursorColumn/CursorRowへ実際に反映されることを
// 確認する
int lisp_console_output_mode_selftest(void) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = g_system_table->ConOut;

    UINTN cols = 0;
    UINTN rows = 0;
    if (out->QueryMode(out, out->Mode->Mode, &cols, &rows) != 0) {
        return 0;
    }
    if (cols == 0 || cols > 1000 || rows == 0 || rows > 1000) {
        return 0;
    }

    if (out->SetCursorPosition(out, 1, 1) != 0) {
        return 0;
    }
    if (out->Mode->CursorColumn != 1 || out->Mode->CursorRow != 1) {
        return 0;
    }

    if (out->SetCursorPosition(out, 0, 0) != 0) {
        return 0;
    }
    if (out->Mode->CursorColumn != 0 || out->Mode->CursorRow != 0) {
        return 0;
    }

    return 1;
}

// milestone 41: lisp/stdlib.lispがコメントを含めて8192byteを超えたため、無警告で
// 末尾が読み捨てられる(truncateされてもlisp_load_eval_buffer側はEOFとして正常終了する
// ため検知できない)事故を防ぐ目的で32768byteへ拡張した。milestone46でstdlib.lispが
// 再び32768byteを超えたため65536byteへ再拡張した(同じ理由の再発)。milestone61で
// funcall/apply/reduce/sortの追加によりstdlib.lispが65536byteを再び超え、末尾の
// (mark-compiler-ready)が無警告で読み捨てられてcompiler-ready-pが恒久的にnilになる
// という形で実際に再発した(3度目)。今後の再発を先回りして防ぐため131072byteへ拡張する
#define LISP_LOAD_BUFFER_MAX 131072
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

// milestone98: write-lineの改行無し版。文字列連結プリミティブが存在しないこの処理系で
// print-objectのmethod本体が出力を組み立てるための基本手段として追加する
LispObject lisp_builtin_write_string(LispObject args) {
    LispObject content_obj = lisp_car(args);
    lisp_assert_string(content_obj);
    LispClosure *content = lisp_closure_cell(content_obj);

    LispOutputStream stream = lisp_make_console_stream(g_system_table);
    lisp_print_ascii(&stream, content->str_data);

    return lisp_sym_t;
}

// milestone98: 任意のLispObjectを既存のlisp_print経由でコンソールへ改行無しで書き出す
// (CLのprincと同様、引数自身を返す)。print-objectのmethod本体からスロット値等の
// 非文字列オブジェクトを書き出すための手段
LispObject lisp_builtin_princ(LispObject args) {
    LispObject obj = lisp_car(args);
    LispOutputStream stream = lisp_make_console_stream(g_system_table);
    lisp_print(&stream, obj);
    return obj;
}

// milestone118: Lispコード(os:switch-processのプロセス選択メニュー等)がコンソールから
// 1式読み込むための薄いビルトイン。プロンプト文字列を表示した後、REPL本体
// (main.cのEfiMainImpl、milestone6/8)と全く同じlisp_read_line/lisp_read_from_bufferを
// そのまま再利用する。milestone135でlisp_read_line自体がg_text_input_ex経由の
// ReadKeyStrokeExへ一本化されたため、以前のように「ConIn以外に一切触れない」わけでは
// 無くなったが、読み取り経路自体がREPL本体と完全に同じ単一のlisp_read_lineへ統一された
// ことで、複数の異なる経路が同じキーイベントキューを競合して読むリスクは構造的に排除
// されている(milestone118で懸念されていたリスクの解消そのもの)。空行が入力された場合は
// プロンプトを再表示して読み直す(REPL本体の「空行ならcontinue」とは異なり、こちらは必ず
// 1式返す必要があるため)。milestone136: lisp_read_lineがCtrl2回押下を検知して打ち切った
// (lisp_double_ctrl_detected)場合は、これも「1式返す必要がある」呼び出し規約には乗せられ
// ないため、単純にnilを返してキャンセル扱いにする(再帰的にダイアログを起動する処理は
// main REPLループのみに限定する設計方針、milestone137の対象)
LispObject lisp_builtin_read_console_expr(LispObject args) {
    LispObject prompt_obj = lisp_car(args);
    lisp_assert_string(prompt_obj);
    LispClosure *prompt = lisp_closure_cell(prompt_obj);

    LispOutputStream stream = lisp_make_console_stream(g_system_table);
    for (;;) {
        lisp_print_ascii(&stream, prompt->str_data);
        lisp_read_line(g_system_table);
        if (lisp_double_ctrl_detected) {
            lisp_double_ctrl_detected = 0;
            return LISP_NIL;
        }
        if (input_length == 0) {
            continue;
        }
        return lisp_read_from_buffer(input_buffer);
    }
}

// --- 画面ダブルバッファリング (milestone 122〜) ---
// documents/lisp_console_buffer.mdフェーズK。単一プロセスのみが実行中というコルーチン方式
// (milestone100〜118)の前提により、プロセス毎ではなく単一の共有グローバルとする。
// UEFIの標準テキストコンソールで現実的に出現するモード(80x25〜128x40程度)を十分に
// 上回る固定上限で静的に確保する(CLAUDE.mdの方針通り、この段階ではヒープ確保をしない)。
// LispScreenBuffer型自体(LISP_SCREEN_*_MAX/STATUS_ROWSの#defineも含む)はmilestone133で
// LispProcessStackへ埋め込むためsrc/lisp.hへ移した(このグローバルインスタンス自体は不変)
static LispScreenBuffer lisp_screen_buffer;

// milestone133: documents/lisp_process_screen_switch.mdフェーズN。実ハードウェア
// (QueryMode/ClearScreen)に依存しない、back/front初期化ロジックだけを切り出したもの。
// 本番の起動時初期化(下記lisp_screen_buffer_init)はQueryMode/ClearScreen成功後にこれを
// 呼ぶ。lisp_context_switchが新規プロセスの画面バッファを初回使用時に初期化する際は、
// 実ハードウェアへは一切触れず(既にアクティブな側が実画面を表示中のため)この関数のみを呼ぶ
static void lisp_screen_buffer_init_blank(LispScreenBuffer *buf, UINTN cols, UINTN rows) {
    buf->cols = cols;
    buf->rows = rows;
    for (UINTN r = 0; r < rows; r++) {
        for (UINTN c = 0; c < cols; c++) {
            buf->back[r][c] = L' ';
            buf->front[r][c] = L' ';
        }
    }
    buf->cursor_col = 0;
    buf->cursor_row = LISP_SCREEN_STATUS_ROWS;
    buf->pending_newlines = 0;
    buf->dirty = 0;
    buf->force_full_redraw = 0;
    for (UINTN r = 0; r < LISP_SCREEN_ROWS_MAX; r++) {
        buf->row_touched[r] = 0;
    }
    buf->initialized = 1;
}

// milestone133: LispScreenBuffer丸ごとの構造体代入(dst = src)はサイズが大きいため、
// クロスコンパイラが暗黙にmemcpy呼び出しへ展開してしまう(-nostdlibでlibcが無いため
// リンクエラーになる、CLAUDE.mdの「標準ヘッダ・libcは使用できない」方針の実例)。
// 1byteずつの明示的なループで代替する
static void lisp_screen_buffer_copy(LispScreenBuffer *dst, const LispScreenBuffer *src) {
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;
    for (UINTN i = 0; i < sizeof(LispScreenBuffer); i++) {
        d[i] = s[i];
    }
}

// QueryModeで実際のcols/rowsを確定し(上限を超えていればpanic)、ClearScreenで実画面を
// クリアしてから、back/front両方をスペースで埋めカーソル/pending_newlines/dirtyを0に
// 戻す。milestone122の時点では既存の出力経路(lisp_console_stream_write等)からは
// 一切呼ばれず、単体で状態を保持するだけの段階
void lisp_screen_buffer_init(void) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = g_system_table->ConOut;

    UINTN cols = 0;
    UINTN rows = 0;
    if (out->QueryMode(out, out->Mode->Mode, &cols, &rows) != 0) {
        lisp_panic(L"lisp_screen_buffer_init: QueryMode failed");
    }
    if (cols == 0 || cols > LISP_SCREEN_COLS_MAX || rows == 0 || rows > LISP_SCREEN_ROWS_MAX) {
        lisp_panic(L"lisp_screen_buffer_init: screen size out of supported range");
    }
    // milestone130: 先頭LISP_SCREEN_STATUS_ROWS行をOS予約行として確保するため、
    // 通常の文字書き込み用領域が最低1行残ることを保証する
    if (rows <= LISP_SCREEN_STATUS_ROWS) {
        lisp_panic(L"lisp_screen_buffer_init: screen too small for status row");
    }
    // milestone133: lisp_screen_flush_set_cursor_position/lisp_screen_flush_output_stringと
    // 同じ理由(下記lisp_screen_flushのコメント参照)で、ClearScreenも一時的な失敗を即座に
    // panicとせず、短いStallを挟んで数回リトライする
    {
        EFI_BOOT_SERVICES *bs = g_system_table->BootServices;
        int cleared = 0;
        for (UINTN attempt = 0; attempt < LISP_SCREEN_FLUSH_RETRY_MAX; attempt++) {
            if (out->ClearScreen(out) == 0) {
                cleared = 1;
                break;
            }
            bs->Stall(LISP_SCREEN_FLUSH_RETRY_STALL_US);
        }
        if (!cleared) {
            lisp_panic(L"lisp_screen_buffer_init: ClearScreen failed");
        }
    }

    lisp_screen_buffer_init_blank(&lisp_screen_buffer, cols, rows);
}

// milestone125: lisp_console_stream_writeが未初期化時に自動初期化するかどうかを
// 判断するためのアクセサ。lisp_screen_bufferはこのファイル内のstaticなので、
// より前方(1380行付近)にあるlisp_console_stream_writeからも呼べるようにする
int lisp_screen_buffer_is_initialized(void) {
    return lisp_screen_buffer.initialized;
}

// milestone122: lisp_screen_buffer_initを実際に呼び、QueryModeが返した実際のcols/rowsが
// 妥当な範囲であること・初期化直後のカーソル/pending_newlines/dirtyが全て0であること・
// back/front両方の全セルが実際にスペースで埋まっていることを確認する
int lisp_screen_buffer_selftest(void) {
    lisp_screen_buffer_init();

    if (lisp_screen_buffer.cols == 0 || lisp_screen_buffer.cols > LISP_SCREEN_COLS_MAX) {
        return 0;
    }
    if (lisp_screen_buffer.rows == 0 || lisp_screen_buffer.rows > LISP_SCREEN_ROWS_MAX) {
        return 0;
    }
    if (lisp_screen_buffer.initialized != 1) {
        return 0;
    }
    if (lisp_screen_buffer.cursor_col != 0 || lisp_screen_buffer.cursor_row != LISP_SCREEN_STATUS_ROWS) {
        return 0;
    }
    if (lisp_screen_buffer.pending_newlines != 0 || lisp_screen_buffer.dirty != 0) {
        return 0;
    }

    for (UINTN r = 0; r < lisp_screen_buffer.rows; r++) {
        for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
            if (lisp_screen_buffer.back[r][c] != L' ' || lisp_screen_buffer.front[r][c] != L' ') {
                return 0;
            }
        }
    }

    return 1;
}

// milestone125: 指定行の指定列が実際に書き込まれたことを記録する(バグ修正、詳細は
// lisp_screen_flushのコメント参照)。1回目のtouchでmin/maxをそのcolに初期化し、
// 以降は範囲を広げるだけ
static void lisp_screen_touch_cell(UINTN row, UINTN col) {
    if (!lisp_screen_buffer.row_touched[row]) {
        lisp_screen_buffer.row_touched[row] = 1;
        lisp_screen_buffer.touched_min[row] = col;
        lisp_screen_buffer.touched_max[row] = col;
        return;
    }
    if (col < lisp_screen_buffer.touched_min[row]) {
        lisp_screen_buffer.touched_min[row] = col;
    }
    if (col > lisp_screen_buffer.touched_max[row]) {
        lisp_screen_buffer.touched_max[row] = col;
    }
}

// milestone123: back bufferへの1文字書き込み・改行・スクロールのみを行う純粋なバッファ
// 操作関数(UEFI呼び出しを一切含まない)。改行('\n')はpending_newlinesを加算して
// カーソルを次行の先頭へ移すのみで、実際の"\r\n"送出はlisp_screen_flush(milestone124)の
// 責務とする。行末に達した通常文字は次行の先頭へ折り返り、画面最下行を超えた場合は
// 1行分shiftしてスクロールし、最下行をスペースで初期化する
void lisp_screen_putc(char ch) {
    if (!lisp_screen_buffer.initialized) {
        lisp_panic(L"lisp_screen_putc: screen buffer not initialized");
    }

    if (ch == '\n') {
        lisp_screen_buffer.pending_newlines++;
        lisp_screen_buffer.cursor_col = 0;
        lisp_screen_buffer.cursor_row++;
    } else {
        UINTN row = lisp_screen_buffer.cursor_row;
        UINTN col = lisp_screen_buffer.cursor_col;
        lisp_screen_buffer.back[row][col] = (CHAR16)ch;
        lisp_screen_touch_cell(row, col);
        lisp_screen_buffer.dirty = 1;
        lisp_screen_buffer.cursor_col++;
        if (lisp_screen_buffer.cursor_col >= lisp_screen_buffer.cols) {
            lisp_screen_buffer.cursor_col = 0;
            lisp_screen_buffer.cursor_row++;
        }
    }

    if (lisp_screen_buffer.cursor_row >= lisp_screen_buffer.rows) {
        // milestone130: 先頭LISP_SCREEN_STATUS_ROWS行はOS予約行のため、shift元・shift先の
        // どちらにもこの範囲を含めない(行0は常にスクロールの影響を受けない)
        for (UINTN r = LISP_SCREEN_STATUS_ROWS + 1; r < lisp_screen_buffer.rows; r++) {
            for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
                lisp_screen_buffer.back[r - 1][c] = lisp_screen_buffer.back[r][c];
            }
        }
        for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
            lisp_screen_buffer.back[lisp_screen_buffer.rows - 1][c] = L' ';
        }
        lisp_screen_buffer.cursor_row = lisp_screen_buffer.rows - 1;
        lisp_screen_buffer.dirty = 1;
        // スクロールで全行の内容が動くため、範囲を絞らず全行を丸ごとtouchする
        // (ただしOS予約行(行0)はスクロールで変化しないためtouch対象から除外する)
        for (UINTN r = LISP_SCREEN_STATUS_ROWS; r < lisp_screen_buffer.rows; r++) {
            lisp_screen_buffer.row_touched[r] = 1;
            lisp_screen_buffer.touched_min[r] = 0;
            lisp_screen_buffer.touched_max[r] = lisp_screen_buffer.cols - 1;
        }
    }
}

// milestone123: lisp_screen_putcの基本文字書き込み・改行・行末折り返し・スクロールを、
// 都度lisp_screen_buffer_initで初期状態へリセットしてからそれぞれ独立に検証する
int lisp_screen_putc_selftest(void) {
    // (1) 基本の1文字書き込み: 指定位置へ反映され、カーソルが1つ進みdirtyが立つ
    // (milestone130: 書き込みは常にLISP_SCREEN_STATUS_ROWS行目以降で行われる)
    lisp_screen_buffer_init();
    lisp_screen_putc('A');
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][0] != L'A') {
        return 0;
    }
    if (lisp_screen_buffer.cursor_row != LISP_SCREEN_STATUS_ROWS || lisp_screen_buffer.cursor_col != 1) {
        return 0;
    }
    if (lisp_screen_buffer.dirty != 1) {
        return 0;
    }
    // milestone130: 先頭行(OS予約行)は通常の書き込みでは一切変化しない
    if (lisp_screen_buffer.row_touched[0] != 0) {
        return 0;
    }

    // (2) 改行: pending_newlinesが増え、カーソルが次行の先頭に移動する。セル内容は不変
    UINTN pending_before = lisp_screen_buffer.pending_newlines;
    lisp_screen_putc('\n');
    if (lisp_screen_buffer.pending_newlines != pending_before + 1) {
        return 0;
    }
    if (lisp_screen_buffer.cursor_col != 0 || lisp_screen_buffer.cursor_row != LISP_SCREEN_STATUS_ROWS + 1) {
        return 0;
    }
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][0] != L'A') {
        return 0;
    }

    // (3) 行末での折り返し: cols文字書き込むとカーソルが次行の先頭へ折り返る
    lisp_screen_buffer_init();
    UINTN cols = lisp_screen_buffer.cols;
    for (UINTN i = 0; i < cols; i++) {
        lisp_screen_putc('x');
    }
    if (lisp_screen_buffer.cursor_row != LISP_SCREEN_STATUS_ROWS + 1 || lisp_screen_buffer.cursor_col != 0) {
        return 0;
    }
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][cols - 1] != L'x') {
        return 0;
    }

    // (4) スクロール: 改行のみでカーソルを最下行まで進め、目印を置いた行がさらに
    // 1行shiftされることを確認する(lisp_console_output_mode_selftest(milestone119)で
    // 実機のQueryModeがrows>=1の妥当な値を返すことは確認済みだが、OS予約行を除いた
    // 内容領域が3行未満では、このスクロール検証自体が成立しないため、その場合のみ
    // 明示的に失敗とする)
    lisp_screen_buffer_init();
    UINTN rows = lisp_screen_buffer.rows;
    if (rows - LISP_SCREEN_STATUS_ROWS < 3) {
        return 0;
    }
    lisp_screen_putc('\n'); // 内容領域の先頭行 -> 次行
    lisp_screen_putc('M');  // 目印
    for (UINTN r = LISP_SCREEN_STATUS_ROWS + 1; r < rows - 1; r++) {
        lisp_screen_putc('\n'); // row(rows-1)まで進める
    }
    lisp_screen_putc('N'); // row(rows-1)のcol0に目印
    lisp_screen_putc('\n'); // スクロール発生

    if (lisp_screen_buffer.cursor_row != rows - 1 || lisp_screen_buffer.cursor_col != 0) {
        return 0;
    }
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][0] != L'M') {
        return 0;
    }
    if (lisp_screen_buffer.back[rows - 2][0] != L'N') {
        return 0;
    }
    for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
        if (lisp_screen_buffer.back[rows - 1][c] != L' ') {
            return 0;
        }
    }

    // (5) milestone130の中心的な検証項目: (1)〜(4)の書き込み・スクロールを経ても
    // 先頭行(OS予約行、行0)の内容・touched状態は一切変化していない
    for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
        if (lisp_screen_buffer.back[0][c] != L' ') {
            return 0;
        }
    }
    if (lisp_screen_buffer.row_touched[0] != 0) {
        return 0;
    }

    return 1;
}

// milestone124: 実際に書き込まれた(touchされた)セルのみを実際のConOutへ反映する。
// 1行につきtouched_min[r]〜touched_max[r]の区間を1回のSetCursorPosition+OutputString
// にまとめ、画面全体を毎回送り直すことを避ける。pending_newlines分の実"\r\n"はセル差分
// とは独立に送出する(milestone123で改行はセル差分として表現しないことにした設計上の
// 帰結。scripts/run_test.pyが行区切り検出に実際の改行バイトを要求するため、内容が
// 変化していなくても改行だけは送出する必要がある)。最後にハードウェアカーソルを
// 論理カーソル位置(cursor_col, cursor_row)へ合わせる。
//
// バグ修正(milestone125で発覚): 当初はback[r][c]!=front[r][c]の値比較でrunを決めて
// いたが、これは「同じ文字が既に表示されている」場合に実際のOutputString呼び出し自体を
// スキップしてしまう。実機の画面(VT100端末等)であれば見た目上は正しくても、
// scripts/run_test.pyはシリアル上の生バイト列を素朴に行区切りで読むだけで、カーソル
// 位置を伴う端末状態を再現しているわけではない。そのため、値が偶然一致するセルへの
// 書き込みを黙って送出しないと、実際に送られるバイト列から文字(典型的には連続する
// スペース)が欠落し、テスト用の出力行が破損する(例: "RESULT while PASS"が
// "RESULTwhilePASS"になる)。この修正により、"書き込まれたかどうか"(touched_min/max、
// lisp_screen_putc側で記録)を基準にrunを決めるようにし、値が変化していないセルも
// touchされていれば必ず送出する。これにより見た目の再描画範囲は変わらない
// (書き込まれていないセルは依然として送出しない)まま、実際に書いた内容が必ず
// バイト列として現れることを保証する。
//
// C自己テストからUEFI呼び出し回数を検証できるよう、実ファームウェア呼び出し自体は
// 差し替えず、本関数がSetCursorPosition/OutputStringを呼ぶと"決定"した回数を
// 静的カウンタに記録する
static UINTN lisp_screen_flush_cell_output_count = 0;
static UINTN lisp_screen_flush_set_cursor_count = 0;
static UINTN lisp_screen_flush_newline_output_count = 0;
static CHAR16 lisp_screen_flush_run_buffer[LISP_SCREEN_COLS_MAX + 1];

// milestone133: force_full_redraw時は最大で画面全行分のSetCursorPosition/OutputStringを
// 連続で発行するバーストになる(milestone130以前にはこの規模の連続呼び出しは存在しなかった)。
// 実機/QEMUどちらでも、この規模のバーストの最中に限ってSetCursorPosition/OutputStringが
// 原因不明の一時的なエラーを返すことがあるのを確認した(make test全体を繰り返すと数回に
// 1回程度の頻度で再現、個別の1テストだけを実行すると再現しない=バースト量に依存する非決定的な
// 事象と判断)。1回の失敗を即座に致命的panicとせず、短いStall(EFI_BOOT_SERVICES.Stall)を
// 挟んで数回リトライしてから初めてpanicする(実機のシリアル/コンソールドライバのFIFOが
// 一時的に詰まっているだけなら、短い待機で解消することを期待する)。定数自体はlisp.hで定義
// (lisp_screen_buffer_initがこの関数より前で同じ定数を使うため)

static void lisp_screen_flush_set_cursor_position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINTN col, UINTN row) {
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;
    for (UINTN attempt = 0; attempt < LISP_SCREEN_FLUSH_RETRY_MAX; attempt++) {
        if (out->SetCursorPosition(out, col, row) == 0) {
            return;
        }
        bs->Stall(LISP_SCREEN_FLUSH_RETRY_STALL_US);
    }
    lisp_panic(L"lisp_screen_flush: SetCursorPosition failed");
}

static void lisp_screen_flush_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, CHAR16 *str) {
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;
    for (UINTN attempt = 0; attempt < LISP_SCREEN_FLUSH_RETRY_MAX; attempt++) {
        if (out->OutputString(out, str) == 0) {
            return;
        }
        bs->Stall(LISP_SCREEN_FLUSH_RETRY_STALL_US);
    }
    lisp_panic(L"lisp_screen_flush: OutputString failed");
}

void lisp_screen_flush(void) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = g_system_table->ConOut;

    // milestone132: force_full_redrawが立っていれば、以降は「全rows(status行含む)が
    // touchされた」ものとして扱う。プロセス切替でバッファ自体が入れ替わった直後は
    // dirty/pending_newlinesの値も入れ替わった側(=退避されていた過去の値)のままなので、
    // dirtyが0であっても後続の早期returnをすり抜けさせるためdirtyもここで立てる
    if (lisp_screen_buffer.force_full_redraw) {
        for (UINTN r = 0; r < lisp_screen_buffer.rows; r++) {
            lisp_screen_buffer.row_touched[r] = 1;
            lisp_screen_buffer.touched_min[r] = 0;
            lisp_screen_buffer.touched_max[r] = lisp_screen_buffer.cols - 1;
        }
        lisp_screen_buffer.dirty = 1;
        lisp_screen_buffer.force_full_redraw = 0;
    }

    if (!lisp_screen_buffer.dirty && lisp_screen_buffer.pending_newlines == 0) {
        return;
    }

    for (UINTN r = 0; r < lisp_screen_buffer.rows; r++) {
        if (!lisp_screen_buffer.row_touched[r]) {
            continue;
        }
        UINTN start = lisp_screen_buffer.touched_min[r];
        UINTN end = lisp_screen_buffer.touched_max[r];
        UINTN run_len = 0;
        for (UINTN c = start; c <= end; c++) {
            lisp_screen_flush_run_buffer[run_len] = lisp_screen_buffer.back[r][c];
            lisp_screen_buffer.front[r][c] = lisp_screen_buffer.back[r][c];
            run_len++;
        }
        lisp_screen_flush_run_buffer[run_len] = 0;

        lisp_screen_flush_set_cursor_position(out, start, r);
        lisp_screen_flush_set_cursor_count++;
        lisp_screen_flush_output_string(out, lisp_screen_flush_run_buffer);
        lisp_screen_flush_cell_output_count++;
        lisp_screen_buffer.row_touched[r] = 0;
    }

    for (UINTN i = 0; i < lisp_screen_buffer.pending_newlines; i++) {
        lisp_screen_flush_output_string(out, L"\r\n");
        lisp_screen_flush_newline_output_count++;
    }
    lisp_screen_buffer.pending_newlines = 0;

    lisp_screen_flush_set_cursor_position(out, lisp_screen_buffer.cursor_col, lisp_screen_buffer.cursor_row);
    lisp_screen_flush_set_cursor_count++;

    lisp_screen_buffer.dirty = 0;
}

// lisp_read_lineのキー入力エコー(milestone125〜128の方針により意図的にバッファ非経由、
// 直接ConOutへ出力)は論理カーソル(cursor_col/cursor_row)を一切更新しない。そのため
// 入力行の途中や改行後にlisp_screen_flushを呼ぶと、実ハードウェアカーソルが
// 古い論理カーソル位置(プロンプト表示直後の位置)へ戻されてしまい、次のプロンプトが
// 入力後の実際の行ではなく同じ行に描画される。
//
// バグ修正(第2報): 当初はConOut->Mode->CursorColumn/CursorRowを読み戻して論理カーソルへ
// 書き戻す方式で対処したが、これはSetCursorPositionの明示呼び出し以外ではMode
// が更新されないコンソールドライバ実装(実機での対話操作時に踏んだ)では機能せず、
// 常に直前のSetCursorPosition位置(プロンプト直後の位置)を読み戻すだけになっていた。
// これにより結果がプロンプト直後の位置に上書きされる・空入力でのEnterが同じ行の
// 右側にしか進まないという症状が再現する。ファームウェアの状態へ問い合わせず、
// 直接ConOutへ出力した文字列そのもの(内容はこちらが把握している)から論理カーソル
// 位置を計算して直接更新する方式に置き換える。back/front/dirty/touchedは一切
// 変更しない(実際に描画された内容はConOutへ直接出力済みであり、バッファ経由の
// 再描画対象ではないため)
static void lisp_screen_track_echoed_char(char ch) {
    if (!lisp_screen_buffer.initialized) {
        return;
    }
    if (ch == '\r') {
        return; // "\r\n"の\r自体は列を変えない。改行effectは'\n'側でのみ処理する
    }
    if (ch == '\n') {
        lisp_screen_buffer.cursor_col = 0;
        lisp_screen_buffer.cursor_row++;
    } else if (ch == 8) { // Backspace
        if (lisp_screen_buffer.cursor_col > 0) {
            lisp_screen_buffer.cursor_col--;
        }
    } else {
        lisp_screen_buffer.cursor_col++;
        if (lisp_screen_buffer.cursor_col >= lisp_screen_buffer.cols) {
            lisp_screen_buffer.cursor_col = 0;
            lisp_screen_buffer.cursor_row++;
        }
    }
    if (lisp_screen_buffer.cursor_row >= lisp_screen_buffer.rows) {
        // 実画面はスクロールして最下行に留まるはずだが、backへは反映していないため
        // 内容の追従はできない。カーソル位置だけは最下行へ留める
        lisp_screen_buffer.cursor_row = lisp_screen_buffer.rows - 1;
    }
}

void lisp_screen_track_echoed_wstring(CHAR16 *str) {
    for (UINTN i = 0; str[i] != 0; i++) {
        lisp_screen_track_echoed_char((char)str[i]);
    }
}

// milestone124: lisp_screen_flushが実際にSetCursorPosition/OutputStringを呼ぶと
// 決定した回数を、既知のtouchパターン(milestone125でback/front値比較からtouch
// 範囲ベースへ修正)に対して検証する
int lisp_screen_flush_selftest(void) {
    // (1) 何も変化していなければ何も送出しない
    lisp_screen_buffer_init();
    UINTN cell0 = lisp_screen_flush_cell_output_count;
    UINTN cursor0 = lisp_screen_flush_set_cursor_count;
    UINTN nl0 = lisp_screen_flush_newline_output_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0) return 0;
    if (lisp_screen_flush_newline_output_count != nl0) return 0;

    // (2) 1文字書き込み: run1個(1文字分のOutputString)+最終カーソル合わせで、
    // cell_output+1、set_cursor+2
    lisp_screen_buffer_init();
    lisp_screen_putc('A');
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    nl0 = lisp_screen_flush_newline_output_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0 + 1) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0 + 2) return 0;
    if (lisp_screen_flush_newline_output_count != nl0) return 0;
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][0] != L'A') return 0;
    if (lisp_screen_buffer.dirty != 0) return 0;

    // front/backが同期済みの直後にもう一度flushしても追加送出は発生しない
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0) return 0;

    // (3) 隣接する2文字は1つのrunにまとまる: cell_output+1、set_cursor+2
    lisp_screen_buffer_init();
    lisp_screen_putc('A');
    lisp_screen_putc('B');
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0 + 1) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0 + 2) return 0;
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][0] != L'A' || lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][1] != L'B') return 0;

    // (4) 同じ行でtouchされた4文字(間の2文字はスペースでfrontの初期値と偶然一致)は
    // touched_min/maxベースでは1つのrunにまとまる: cell_output+1、set_cursor+2。
    // 値が変化していないセル(間のスペース)もtouchされていれば必ず送出されることを、
    // 実際のfront内容(A/スペース/スペース/B)で確認する
    lisp_screen_buffer_init();
    lisp_screen_putc('A');
    lisp_screen_putc(' ');
    lisp_screen_putc(' ');
    lisp_screen_putc('B');
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0 + 1) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0 + 2) return 0;
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][0] != L'A' || lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][1] != L' ') return 0;
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][2] != L' ' || lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][3] != L'B') return 0;

    // (5) 改行のみ(セル内容は不変、dirtyも立たない)でもpending_newlines分の実"\r\n"は
    // 送出され、最終カーソル合わせの1回だけset_cursorが増える
    lisp_screen_buffer_init();
    lisp_screen_putc('\n');
    if (lisp_screen_buffer.dirty != 0) return 0;
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    nl0 = lisp_screen_flush_newline_output_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0) return 0;
    if (lisp_screen_flush_newline_output_count != nl0 + 1) return 0;
    if (lisp_screen_flush_set_cursor_count != cursor0 + 1) return 0;
    if (lisp_screen_buffer.pending_newlines != 0) return 0;

    return 1;
}

// milestone132: documents/lisp_process_screen_switch.mdフェーズN。force_full_redrawフラグが
// 立っている場合、次のlisp_screen_flushで全行(status行含む)が実際に送出され、フラグ自身も
// クリアされることを確認する。単一グローバルバッファのままフラグを直接操作して検証する
// (実際にプロセス毎バッファを入れ替える経路はmilestone133で追加する)
int lisp_screen_force_full_redraw_selftest(void) {
    lisp_screen_buffer_init();
    lisp_screen_putc('A');
    lisp_screen_flush();
    if (lisp_screen_buffer.dirty != 0) return 0;
    if (lisp_screen_buffer.force_full_redraw != 0) return 0;

    // 何も新たに書き込んでいない(dirty=0、pending_newlines=0)状態でフラグだけを立てる。
    // 通常ならlisp_screen_flush冒頭の早期returnで何も送出されないはずだが、
    // force_full_redrawにより全rows分がtouchされ、実際に送出される。
    //
    // ロジック自体はrowsの実際の値に依存しないため(force_full_redrawは常に
    // lisp_screen_buffer.rows件を全てtouch化するだけ)、他の画面バッファ系自己テストと
    // 同程度の少ない実I/O量に抑えるため、検証中のみrowsを一時的に小さい値へ差し替える。
    // 実際のQueryMode由来の行数(数十行)のままSetCursorPosition+OutputStringを
    // 連続実行すると、ヘッドレスQEMUのConOutエミュレーションが稀に一時的な失敗
    // (OutputString failed)を返すことがあり、make test全体が非決定的に
    // TIMEOUT/FAILする原因になっていた(実装当初に発覚、既知のOVMFファームウェア
    // 揺れの一種と判断した)
    UINTN saved_rows = lisp_screen_buffer.rows;
    lisp_screen_buffer.rows = LISP_SCREEN_STATUS_ROWS + 1;

    lisp_screen_buffer.force_full_redraw = 1;
    UINTN cell0 = lisp_screen_flush_cell_output_count;
    UINTN cursor0 = lisp_screen_flush_set_cursor_count;
    UINTN rows = lisp_screen_buffer.rows;
    lisp_screen_flush();
    int ok = 1;
    if (lisp_screen_flush_cell_output_count != cell0 + rows) ok = 0;
    if (lisp_screen_flush_set_cursor_count != cursor0 + rows + 1) ok = 0;
    if (lisp_screen_buffer.force_full_redraw != 0) ok = 0;
    if (lisp_screen_buffer.dirty != 0) ok = 0;
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][0] != L'A') ok = 0;

    // フラグは既にクリア済みかつ内容も不変のため、直後にもう一度flushしても追加送出は
    // 発生しない
    cell0 = lisp_screen_flush_cell_output_count;
    cursor0 = lisp_screen_flush_set_cursor_count;
    lisp_screen_flush();
    if (lisp_screen_flush_cell_output_count != cell0) ok = 0;
    if (lisp_screen_flush_set_cursor_count != cursor0) ok = 0;

    lisp_screen_buffer.rows = saved_rows;
    return ok;
}

// milestone126: %clear-screenのバッファ経由実装。QueryModeは起動時に1回だけ呼ぶという
// 設計方針(documents/lisp_console_buffer.mdのスコープ外事項)を保つため、cols/rowsは
// 再取得しない。未初期化ならlisp_screen_buffer_init(実ClearScreenを含む)に委ねて終わり、
// 初期化済みなら実ClearScreenを呼んだ上でback/front/カーソル/touch状態を初期化直後と
// 同じ状態(全面スペース・カーソル(0,0)・pending_newlines無し)に戻す
void lisp_screen_clear(void) {
    if (!lisp_screen_buffer.initialized) {
        lisp_screen_buffer_init();
        return;
    }

    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = g_system_table->ConOut;
    if (out->ClearScreen(out) != 0) {
        lisp_panic(L"lisp_screen_clear: ClearScreen failed");
    }
    for (UINTN r = 0; r < lisp_screen_buffer.rows; r++) {
        for (UINTN c = 0; c < lisp_screen_buffer.cols; c++) {
            lisp_screen_buffer.back[r][c] = L' ';
            lisp_screen_buffer.front[r][c] = L' ';
        }
        lisp_screen_buffer.row_touched[r] = 0;
    }
    lisp_screen_buffer.cursor_col = 0;
    // milestone130: 先頭行はOS予約行のため、通常書き込み用カーソルの初期位置は
    // lisp_screen_buffer_initと同様にLISP_SCREEN_STATUS_ROWS行目にする
    lisp_screen_buffer.cursor_row = LISP_SCREEN_STATUS_ROWS;
    lisp_screen_buffer.pending_newlines = 0;
    lisp_screen_buffer.dirty = 0;
}

// milestone126: %set-cursor-positionのバッファ経由実装。カーソル移動はlisp_screen_flushの
// dirty/pending_newlinesゲート(何も描画内容が変わっていなければ何もしない)とは無関係に、
// 呼ばれたら必ず即座にハードウェアカーソルへ反映する必要があるため、専用の実SetCursorPosition
// 発行経路として分離する(flush本体の既存の呼び出し回数の自己テストには影響しない)
void lisp_screen_move_cursor(UINTN col, UINTN row) {
    if (!lisp_screen_buffer.initialized) {
        lisp_screen_buffer_init();
    }
    if (col >= lisp_screen_buffer.cols || row >= lisp_screen_buffer.rows) {
        lisp_panic(L"lisp_screen_move_cursor: position out of range");
    }
    lisp_screen_buffer.cursor_col = col;
    lisp_screen_buffer.cursor_row = row;

    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = g_system_table->ConOut;
    if (out->SetCursorPosition(out, col, row) != 0) {
        lisp_panic(L"lisp_screen_move_cursor: SetCursorPosition failed");
    }
    lisp_screen_flush_set_cursor_count++;
}

// milestone126: %get-screen-sizeのバッファ経由実装。cols/rowsは起動時のlisp_screen_buffer_init
// (QueryMode)で確定済みの値をそのまま返す(実行時の画面サイズ変更は非対応、スコープ外)
void lisp_screen_get_size(UINTN *cols, UINTN *rows) {
    if (!lisp_screen_buffer.initialized) {
        lisp_screen_buffer_init();
    }
    *cols = lisp_screen_buffer.cols;
    *rows = lisp_screen_buffer.rows;
}

// milestone131: documents/lisp_process_screen_switch.mdフェーズM。先頭行(OS予約行、行0)へ
// text(len文字)をpadding/truncate込みで直接書き込む専用経路。lisp_screen_putcの通常書き込み
// (カーソル追従・折り返し・スクロール)とは完全に独立しており、cursor_col/cursor_rowは
// 一切変更しない。cols幅に対してtextが短ければ残りをスペースで埋め、長ければcols文字目
// より後ろを切り捨てる。milestone138: 列0〜LISP_SCREEN_STATUS_RESERVED_COLS-1はシステム
// 予約領域(Ctrl待機インジケータ用)なので、この関数はそれより後ろの列にのみ書き込む。
// touchもlisp_screen_touch_cellで行い、予約領域側のtouched範囲を無条件に上書きしない
void lisp_screen_set_status_line(const char *text, UINTN len) {
    if (!lisp_screen_buffer.initialized) {
        lisp_screen_buffer_init();
    }
    UINTN cols = lisp_screen_buffer.cols;
    for (UINTN c = LISP_SCREEN_STATUS_RESERVED_COLS; c < cols; c++) {
        UINTN i = c - LISP_SCREEN_STATUS_RESERVED_COLS;
        lisp_screen_buffer.back[0][c] = (i < len) ? (CHAR16)text[i] : L' ';
        lisp_screen_touch_cell(0, c);
    }
    lisp_screen_buffer.dirty = 1;
}

// milestone138: Ctrl単体押下1回目の待機中(armed)であることを示す'C'を、状態行左端(列0、
// システム予約LISP_SCREEN_STATUS_RESERVED_COLS列のうち先頭)へ表示/消去する。
// lisp_read_lineがキー入力を待ってブロックしている間はlisp_screen_flushが一度も呼ばれ
// ないため(milestone125以降のキーechoと同じ理由)、back bufferへ書くだけでは画面に反映
// されない。そのためキーechoと同じ方式で、ConOutへ直接描画する。入力行編集中の実際の
// カーソル位置(lisp_screen_buffer.cursor_col/cursor_row、キーechoが追従管理)を書き込み
// 前後で保つため、SetCursorPositionで一時的に(0,0)へ移動してから書き込み後に元へ戻す
void lisp_screen_show_ctrl_indicator(EFI_SYSTEM_TABLE *SystemTable, int armed) {
    if (!lisp_screen_buffer.initialized) {
        return;
    }
    SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, 0, 0);
    CHAR16 ch[2] = { armed ? L'C' : L' ', 0 };
    SystemTable->ConOut->OutputString(SystemTable->ConOut, ch);
    SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, lisp_screen_buffer.cursor_col, lisp_screen_buffer.cursor_row);
}

// milestone120で暫定実装、milestone126でバッファ経由(lisp_screen_clear)へ切り替え
LispObject lisp_builtin_clear_screen(LispObject args) {
    (void)args;
    lisp_screen_clear();
    return lisp_sym_t;
}

// (%set-cursor-position col row): col/rowはfixnum必須(0始まり、UEFI仕様と同じ)。
// milestone120で暫定実装、milestone126でバッファ経由(lisp_screen_move_cursor)へ切り替え
LispObject lisp_builtin_set_cursor_position(LispObject args) {
    LispObject col_obj = lisp_car(args);
    LispObject row_obj = lisp_car(lisp_cdr(args));
    lisp_assert_fixnum(col_obj);
    lisp_assert_fixnum(row_obj);
    lisp_screen_move_cursor((UINTN)lisp_fixnum_value(col_obj), (UINTN)lisp_fixnum_value(row_obj));
    return lisp_sym_t;
}

// milestone121で暫定実装、milestone126でバッファ経由(lisp_screen_get_size)へ切り替え。
// (%get-screen-size) -> (cons cols rows)
LispObject lisp_builtin_get_screen_size(LispObject args) {
    (void)args;
    UINTN cols = 0;
    UINTN rows = 0;
    lisp_screen_get_size(&cols, &rows);
    return lisp_cons(lisp_make_fixnum((long long)cols), lisp_make_fixnum((long long)rows));
}

// (%set-status-line "text"): 先頭行(OS予約行)へtextを直接書き込む(milestone131)。
// textはstring必須。行0の内容そのもの(表示するプロセス名等)はLisp側の関心事とし、
// このビルトインは「行0へpadding/truncate込みで書き込む」という機構のみを提供する
LispObject lisp_builtin_set_status_line(LispObject args) {
    LispObject text_obj = lisp_car(args);
    lisp_assert_string(text_obj);
    LispClosure *text = lisp_closure_cell(text_obj);
    lisp_screen_set_status_line(text->str_data, text->str_len);
    return lisp_sym_t;
}

// --- VM命令ディスパッチループの1命令ごとflushフック自己テスト (milestone 127) ---
//
// 手作りbytecodeで「画面バッファへ1文字書き込むだけのCビルトイン」を呼び出し、その直後に
// OP_RETURNするだけの関数をlisp_vm_execで実行する。lisp_console_stream_write経由の即時flush
// (milestone125、本milestoneで削除済み)を一切経由せずに、lisp_screen_buffer.frontへ実際に
// 反映されていること(=lisp_vm_run内の1命令ごとflushフックが自動的に動いたこと)を確認する
static LispObject lisp_vm_flush_hook_selftest_write_x(LispObject args) {
    (void)args;
    lisp_screen_putc('X');
    return lisp_sym_t;
}

static const unsigned char lisp_vm_flush_hook_selftest_bytecode[] = {
    OP_CONST, 0, 0,  // 0: push constants[0] (=書き込みビルトイン)
    OP_CALL,  0, 0,  // 3: nargs=0で呼ぶ(戻り値tがpushされる)
    OP_RETURN,       // 6
};

int lisp_vm_flush_hook_selftest(void) {
    lisp_screen_buffer_init();
    if (lisp_screen_buffer.dirty != 0) return 0;
    if (lisp_screen_buffer.row_touched[LISP_SCREEN_STATUS_ROWS] != 0) return 0;

    LispObject constants[1];
    constants[0] = lisp_make_builtin(lisp_vm_flush_hook_selftest_write_x);
    LispObject fn = lisp_make_compiled(lisp_vm_flush_hook_selftest_bytecode,
                                        sizeof(lisp_vm_flush_hook_selftest_bytecode),
                                        constants, 1, 0, 0);

    LispObject result = lisp_vm_exec(fn);
    if (result != lisp_sym_t) return 0;

    // lisp_vm_run自身が(OP_RETURNをフェッチする直前に)flushしたはずなので、
    // 呼び出し元でlisp_screen_flushを一切呼んでいないにもかかわらずfrontへ反映されている
    if (lisp_screen_buffer.front[LISP_SCREEN_STATUS_ROWS][0] != L'X') return 0;
    if (lisp_screen_buffer.dirty != 0) return 0;
    if (lisp_screen_buffer.row_touched[LISP_SCREEN_STATUS_ROWS] != 0) return 0;

    // 検証用に書き込んだ'X'をこの自己テスト内で後始末する。lisp_screen_buffer_initは
    // ソフトウェア側のback/front/touched状態のみを初期化するため、既に実画面へflush済みの
    // 'X'自体は直接ConOutへ空白を書き込んで消す必要がある(lisp_screen_show_ctrl_indicatorと
    // 同じ直接書き込み手法、リトライ付きヘルパーはこのファイル内で前方定義済みのものを再利用)。
    // これを怠るとfront側の記録だけが空白に戻り、実画面には'X'が残ったままREPLに到達してしまう
    // (以降の起動処理はどこもこのセルを上書きしない)
    lisp_screen_flush_set_cursor_position(g_system_table->ConOut, 0, LISP_SCREEN_STATUS_ROWS);
    lisp_screen_flush_output_string(g_system_table->ConOut, L" ");
    lisp_screen_buffer_init();

    return 1;
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

// --- 最小CLOSサブセット: class/instance/単一継承、ディスパッチ無し (milestone 96) ---
// global_classesはlisp_gc_mark_rootsより前方(global_packagesの隣)で宣言済み

// symbolのeq線形探索でクラスを探す。見つからなければNILを返す（内部ヘルパー自体は
// find-packageと同じnil-on-miss方針。panicはlisp_builtin_find_class側の責務にする）
static LispObject lisp_find_class(LispObject name) {
    for (LispObject cur = global_classes; cur != LISP_NIL; cur = lisp_cdr(cur)) {
        LispObject cls = lisp_car(cur);
        if (lisp_closure_cell(cls)->class_name == name) {
            return cls;
        }
    }
    return LISP_NIL;
}

// consリストの長さを返す（既存には無かった汎用ヘルパー。inst_slotsの確保に使う）
static UINTN lisp_list_length(LispObject list) {
    UINTN len = 0;
    while (lisp_is_cons(list)) {
        len++;
        list = lisp_cdr(list);
    }
    return len;
}

static inline void lisp_assert_instance(LispObject obj) {
    if (!lisp_is_instance(obj)) {
        lisp_panic(L"expected a CLOS instance but got something else");
    }
}

// (name superclass-or-nil direct-slots-list)からクラスを作る。名前がglobal_classesに
// 既に存在すれば同一オブジェクトのフィールドを書き換えて返す（defvarの再load冪等性と同じ
// 考え方で、eq同一性を保つ）。無ければ新規に確保してglobal_classesへconsする
static LispObject lisp_make_class(LispObject name, LispObject superclass, LispObject direct_slots) {
    LispObject all_slots = superclass == LISP_NIL
        ? direct_slots
        : lisp_append(lisp_closure_cell(superclass)->class_all_slots, direct_slots);
    LispObject existing = lisp_find_class(name);
    if (existing != LISP_NIL) {
        LispClosure *cell = lisp_closure_cell(existing);
        cell->class_superclass = superclass;
        cell->class_direct_slots = direct_slots;
        cell->class_all_slots = all_slots;
        return existing;
    }
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
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = name;
    closure->class_superclass = superclass;
    closure->class_direct_slots = direct_slots;
    closure->class_all_slots = all_slots;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
    LispObject cls = ((LispObject)closure) | LISP_TAG_CLOSURE;
    global_classes = lisp_cons(cls, global_classes);
    return cls;
}

// clsのinstanceを1つ確保する。全スロットはnilで初期化する（:initarg/:initformは非対応）
static LispObject lisp_make_instance(LispObject cls) {
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
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = cls;
    closure->inst_slots = lisp_make_vector(lisp_list_length(lisp_closure_cell(cls)->class_all_slots), LISP_NIL);
    closure->gf_name = LISP_NIL;
    closure->gf_methods = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// slot-value/set-slot-valueが共有するインデックス探索。class_all_slots内でslot_nameとeqな
// 最初の位置を返す（親のスロット名をサブクラスが再宣言した場合の重複は未定義動作として
// スコープ外、先頭=親側のoccurrenceが見つかる）。見つからなければpanicする
static UINTN lisp_instance_slot_index(LispObject instance, LispObject slot_name) {
    LispObject slots = lisp_closure_cell(lisp_closure_cell(instance)->inst_class)->class_all_slots;
    UINTN index = 0;
    for (LispObject cur = slots; lisp_is_cons(cur); cur = lisp_cdr(cur), index++) {
        if (lisp_car(cur) == slot_name) {
            return index;
        }
    }
    lisp_panic(L"slot-value: no such slot");
    return 0; // 到達しない（lisp_panicはlongjmpする）。コンパイラの戻り値警告を避けるためのみ
}

// (%make-class name superclass-or-nil direct-slots-list)
LispObject lisp_builtin_make_class(LispObject args) {
    LispObject name = lisp_car(args);
    lisp_assert_symbol(name);
    LispObject superclass = lisp_car(lisp_cdr(args));
    LispObject direct_slots = lisp_car(lisp_cdr(lisp_cdr(args)));
    return lisp_make_class(name, superclass, direct_slots);
}

// (find-class name): 見つからなければpanicする（find-packageのnil-on-miss方針とは意図的に
// 異なる。specializer解決でnilと「無指定」を区別できなくなるためfail-fastにする）
LispObject lisp_builtin_find_class(LispObject args) {
    LispObject name = lisp_car(args);
    lisp_assert_symbol(name);
    LispObject cls = lisp_find_class(name);
    if (cls == LISP_NIL) {
        lisp_panic(L"find-class: no such class");
    }
    return cls;
}

// (make-instance class-or-name): symbolならまずfind-classで解決する
LispObject lisp_builtin_make_instance(LispObject args) {
    LispObject designator = lisp_car(args);
    LispObject cls = lisp_is_symbol(designator) ? lisp_builtin_find_class(args) : designator;
    if (!lisp_is_class(cls)) {
        lisp_panic(L"make-instance: expected a class");
    }
    return lisp_make_instance(cls);
}

// (slot-value instance slot-name)
LispObject lisp_builtin_slot_value(LispObject args) {
    LispObject instance = lisp_car(args);
    lisp_assert_instance(instance);
    LispObject slot_name = lisp_car(lisp_cdr(args));
    UINTN index = lisp_instance_slot_index(instance, slot_name);
    return lisp_closure_cell(lisp_closure_cell(instance)->inst_slots)->vec_data[index];
}

// (set-slot-value instance slot-name value): valueを返す（svsetと同じ規約、setfは存在しない）
LispObject lisp_builtin_set_slot_value(LispObject args) {
    LispObject instance = lisp_car(args);
    lisp_assert_instance(instance);
    LispObject slot_name = lisp_car(lisp_cdr(args));
    LispObject value = lisp_car(lisp_cdr(lisp_cdr(args)));
    UINTN index = lisp_instance_slot_index(instance, slot_name);
    lisp_closure_cell(lisp_closure_cell(instance)->inst_slots)->vec_data[index] = value;
    return value;
}

// (class-of obj): lisp_is_instance必須（ビルトイン型のクラス階層は無いためそれ以外はpanic）
LispObject lisp_builtin_class_of(LispObject args) {
    LispObject obj = lisp_car(args);
    lisp_assert_instance(obj);
    return lisp_closure_cell(obj)->inst_class;
}

// --- os:make-process (milestone 103) ---
//
// この段階ではos:processインスタンスの生成・name slotの設定・os:*all-processes*への
// 登録のみを行う。fork実行・パッケージ分離・スタック確保は一切行わない(documents/
// lisp_os_process.mdフェーズCの対象)。
//
// 文字列の内容比較(lisp_streq)・一意名生成の両方に、Lisp側に公開されていないstr_data直接
// アクセスが必要なため、%make-classと同様にCビルトインとして実装し、lisp/os.lispの
// os:make-processラッパー(&optional引数展開のみ担当)から呼ぶ

// os:*all-processes*内にnameと内容が一致するプロセス名を持つインスタンスが既にあればtrue
static int lisp_process_name_taken(LispObject os_pkg, const char *name) {
    LispObject all_processes_sym = lisp_intern_in_package(os_pkg, "*all-processes*");
    LispObject name_slot_sym = lisp_intern("name");
    for (LispObject cur = lisp_symbol_cell(all_processes_sym)->value; lisp_is_cons(cur); cur = lisp_cdr(cur)) {
        LispObject proc_name = lisp_closure_cell(
            lisp_closure_cell(lisp_car(cur))->inst_slots)->vec_data[lisp_instance_slot_index(lisp_car(cur), name_slot_sym)];
        if (lisp_is_string(proc_name) && lisp_streq(lisp_closure_cell(proc_name)->str_data, name)) {
            return 1;
        }
    }
    return 0;
}

// gensymと同じカウンタ方式で"PROCESS-<N>"という名前を生成する。文字列を返す点のみ異なる
// (gensymは非intern済みシンボルを返す)。カウンタが生成した名前が(ユーザー指定名との衝突により
// 既に使われていた場合はカウンタを進めて再試行する
static UINTN lisp_process_name_counter = 0;

static LispObject lisp_generate_process_name(LispObject os_pkg) {
    for (;;) {
        char name[LISP_SYMBOL_NAME_MAX];
        UINTN i = 0;
        const char *prefix = "PROCESS-";
        while (prefix[i] != '\0') {
            name[i] = prefix[i];
            i++;
        }
        char digits[24];
        UINTN dcount = 0;
        UINTN n = lisp_process_name_counter;
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
        lisp_process_name_counter++;
        if (!lisp_process_name_taken(os_pkg, name)) {
            return lisp_make_string(name, lisp_cstrlen(name));
        }
    }
}

// milestone108: fork時に生成する隔離パッケージの一意名を"FORK-PKG-<N>"の形式で生成する
// (gensym/lisp_generate_process_nameと同じカウンタ方式)。lisp_process_name_counterとは
// 独立した別カウンタ・別接頭辞にすることで、プロセス名の一意性チェックと衝突する余地を
// 最初から無くしている
static UINTN lisp_fork_package_name_counter = 0;

static LispObject lisp_generate_fork_package_name(void) {
    for (;;) {
        char name[LISP_SYMBOL_NAME_MAX];
        UINTN i = 0;
        const char *prefix = "FORK-PKG-";
        while (prefix[i] != '\0') {
            name[i] = prefix[i];
            i++;
        }
        char digits[24];
        UINTN dcount = 0;
        UINTN n = lisp_fork_package_name_counter;
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
        lisp_fork_package_name_counter++;
        if (lisp_find_package(name) == LISP_NIL) {
            return lisp_make_string(name, lisp_cstrlen(name));
        }
    }
}

// (%make-process name-or-nil): nameがnilなら自動生成した一意名を使う。文字列が指定されて
// いれば、既存のos:*all-processes*内のいずれかのプロセス名と内容が一致する場合はpanicする
// (make-instanceのみでは名前の一意性を保証できないため、この専用ビルトインが必要)。
// milestone108: プロセス生成と同時に、一意名を持つ隔離パッケージをlisp_make_package_strict
// (衝突時に黙って共有せずpanicする安全な作成経路)で新規作成し、ベースパッケージ
// (common-lisp-user)をuse-packageした上でprocessインスタンスのpackageスロットへ格納する。
// fork側が独自関数を定義したい場合、ベースパッケージのシンボルには一切触れず、この隔離
// パッケージ内にshadowで新規シンボルを作ってdefunすればよい(milestone109)
LispObject lisp_builtin_make_process(LispObject args) {
    LispObject os_pkg = lisp_find_package("os");
    LispObject name_arg = lisp_is_cons(args) ? lisp_car(args) : LISP_NIL;

    LispObject name_obj;
    if (name_arg == LISP_NIL) {
        name_obj = lisp_generate_process_name(os_pkg);
    } else {
        lisp_assert_string(name_arg);
        if (lisp_process_name_taken(os_pkg, lisp_closure_cell(name_arg)->str_data)) {
            lisp_panic(L"make-process: duplicate process name");
        }
        name_obj = name_arg;
    }

    LispObject fork_pkg_name = lisp_generate_fork_package_name();
    LispObject fork_pkg = lisp_make_package_strict(lisp_closure_cell(fork_pkg_name)->str_data, 0);
    lisp_builtin_use_package(lisp_cons(lisp_cl_user_package, lisp_cons(fork_pkg, LISP_NIL)));

    LispObject cls = lisp_find_class(lisp_intern_in_package(os_pkg, "process"));
    LispObject instance = lisp_make_instance(cls);
    UINTN name_index = lisp_instance_slot_index(instance, lisp_intern("name"));
    lisp_closure_cell(lisp_closure_cell(instance)->inst_slots)->vec_data[name_index] = name_obj;
    UINTN package_index = lisp_instance_slot_index(instance, lisp_intern("package"));
    lisp_closure_cell(lisp_closure_cell(instance)->inst_slots)->vec_data[package_index] = fork_pkg;

    LispObject all_processes_sym = lisp_intern_in_package(os_pkg, "*all-processes*");
    LispSymbol *sym_cell = lisp_symbol_cell(all_processes_sym);
    sym_cell->value = lisp_cons(instance, sym_cell->value);

    return instance;
}

// --- fork時の一意パッケージ生成自己テスト (milestone 108) ---
// lisp_builtin_make_processを2回呼び、生成された2つのprocessインスタンスのpackageスロットが
// (1)いずれも非nilで(2)互いにeqでない別オブジェクトであること、(3)いずれもcommon-lisp-userを
// use-packageしていること(pkg_usesにeqで含まれること)、(4)fork先パッケージ内で無修飾に"car"を
// internしてもcommon-lisp-user内の"car"シンボルと同一オブジェクトに解決されること
// (ベースパッケージへの委譲が実際に機能していること)を確認する
int lisp_process_fork_package_selftest(void) {
    LispObject package_sym = lisp_intern("package");

    LispObject p1 = lisp_builtin_make_process(LISP_NIL);
    LispObject p2 = lisp_builtin_make_process(LISP_NIL);

    UINTN package_index = lisp_instance_slot_index(p1, package_sym);
    LispObject pkg1 = lisp_closure_cell(lisp_closure_cell(p1)->inst_slots)->vec_data[package_index];
    LispObject pkg2 = lisp_closure_cell(lisp_closure_cell(p2)->inst_slots)->vec_data[package_index];

    if (pkg1 == LISP_NIL || pkg2 == LISP_NIL) {
        return 0;
    }
    if (pkg1 == pkg2) {
        return 0;
    }
    if (!lisp_is_package(pkg1) || !lisp_is_package(pkg2)) {
        return 0;
    }

    int pkg1_uses_base = 0;
    for (LispObject u = lisp_closure_cell(pkg1)->pkg_uses; u != LISP_NIL; u = lisp_cdr(u)) {
        if (lisp_car(u) == lisp_cl_user_package) {
            pkg1_uses_base = 1;
        }
    }
    if (!pkg1_uses_base) {
        return 0;
    }

    LispObject base_car = lisp_intern_in_package(lisp_cl_user_package, "car");
    LispObject fork_car = lisp_intern_in_package(pkg1, "car");
    if (base_car != fork_car) {
        return 0;
    }

    return 1;
}

// --- process-suspend/process-resume (milestone 112) ---
// os:processインスタンスに実際の実行機構を与える。詳細な設計判断はsrc/lisp.h・
// documents/lisp_os_process.md参照。stackframeスロットには「lisp_process_context_pool内の
// どのスロットを使っているか」をfixnumで格納する(未起動ならnil)。プールは固定長配列のみ
// (本処理系には汎用ヒープアロケータが無いため)
#define LISP_MAX_PROCESS_CONTEXTS 16
#define LISP_PROCESS_STACK_PAGES 256 // 1プロセスあたり1MiB

static LispProcessStack lisp_process_context_pool[LISP_MAX_PROCESS_CONTEXTS];
static int lisp_process_context_pool_used[LISP_MAX_PROCESS_CONTEXTS];

static UINTN lisp_process_context_pool_alloc(void) {
    for (UINTN i = 0; i < LISP_MAX_PROCESS_CONTEXTS; i++) {
        if (!lisp_process_context_pool_used[i]) {
            lisp_process_context_pool_used[i] = 1;
            return i;
        }
    }
    lisp_panic(L"process-resume: too many started processes");
    return 0; // 到達しない
}

// 起動直後のブート処理そのもの(EfiMain経由のREPLループ)を表す「メイン」コンテキスト。
// プールとは別に静的に確保する(mainは他のプロセスと同じ一時的な確保・回収対象ではないため)
static LispProcessStack lisp_process_main_context;
static LispProcessStack *lisp_current_process_stack = &lisp_process_main_context;

// lisp_process_thunk_entryがlisp_apply完了後にセットし、%process-resume側がlisp_context_switch
// から戻った直後にチェックしてstatusスロットを:finishedへ更新するために使う一時フラグ
static int lisp_process_thunk_finished = 0;

static void lisp_process_thunk_entry(void *arg) {
    LispObject closure = (LispObject)(UINT64)(UINTN)arg;
    lisp_apply(closure, LISP_NIL);
    lisp_process_thunk_finished = 1;
    lisp_context_switch(lisp_current_process_stack, lisp_current_process_stack->resumer);
    // entryが戻ってきた場合と同様、プロセス終了後の破棄はスコープ外なので安全のためhangする
    for (;;) {}
}

// (%process-resume process &optional thunk): processのstackframeスロットがnilなら
// thunk(0引数のLisp関数)を新規コンテキストで開始し、fixnumのプールindexならそのコンテキストを
// 直前のprocess-suspendの中断点から再開する。いずれの場合も呼び出し元のコンテキストへ
// lisp_context_switchが戻ってくるまでブロックする。
// statusを:activeに設定するのはlisp_context_switchで実際に制御を渡す直前であり、単一実行
// コンテキストの協調的切替である以上、この呼び出しが戻ってきた時点で外部から:activeを
// 観測することは原理的にできない(戻ってきた時点のstatusは常に:suspendedか:finished)
LispObject lisp_builtin_process_resume(LispObject args) {
    LispObject p = lisp_car(args);
    LispObject thunk = lisp_is_cons(lisp_cdr(args)) ? lisp_car(lisp_cdr(args)) : LISP_NIL;
    lisp_assert_instance(p);

    UINTN stackframe_index = lisp_instance_slot_index(p, lisp_intern("stackframe"));
    UINTN status_index = lisp_instance_slot_index(p, lisp_intern("status"));
    UINTN env_index = lisp_instance_slot_index(p, lisp_intern("env"));
    LispObject *slots = lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data;

    LispProcessStack *target;
    if (slots[stackframe_index] == LISP_NIL) {
        if (thunk == LISP_NIL) {
            lisp_panic(L"process-resume: process has not started and no thunk was given");
        }
        UINTN pool_index = lisp_process_context_pool_alloc();
        target = &lisp_process_context_pool[pool_index];
        lisp_process_stack_create(target, LISP_PROCESS_STACK_PAGES, lisp_process_thunk_entry,
            (void *)(UINTN)(UINT64)thunk);
        slots[stackframe_index] = lisp_make_fixnum((long long)pool_index);
        // milestone113: process-local-variableが後から覗けるように、thunkクロージャ自身が
        // 捕捉しているレキシカル環境(生成時点のenv、lambda/defunのクロージャが持つenvフィールド)を
        // envスロットへコピーする。make-process時点ではなくthunk定義時点の環境である点に注意
        // (%process-resumeにthunkを渡せるのはmake-process呼び出しより後なので、両者は一致しない
        // ことがある。詳細はdocuments/lisp_os_process.md参照)
        slots[env_index] = lisp_closure_cell(thunk)->env;
    } else {
        lisp_assert_fixnum(slots[stackframe_index]);
        UINTN pool_index = (UINTN)lisp_fixnum_value(slots[stackframe_index]);
        target = &lisp_process_context_pool[pool_index];
        lisp_process_stack_unregister(target); // milestone107のGCルート登録から外す(自走再開)
    }

    target->resumer = lisp_current_process_stack;
    LispProcessStack *caller = lisp_current_process_stack;
    lisp_current_process_stack = target;
    slots[status_index] = lisp_intern_keyword("active");

    // milestone133: 画面バッファの退避/復元はここ(実際に「表示中のプロセス」が変わる場所)で
    // 行う。lisp_context_switch自体には置かない(共有の低レベル関数であり、register/vm_stack
    // 分離だけを検証する既存の多数の自己テストも同じ関数を直接呼ぶため、そちら側に置くと
    // 無関係な自己テストのたびにforce_full_redrawが立ち、次のVM命令フック契機の全画面再送出が
    // テストハーネスの改行前提の出力解析を壊す実害があった)
    lisp_screen_buffer_copy(&caller->screen, &lisp_screen_buffer);
    int screen_fresh_start = !target->screen.initialized; // milestone134: このresumeで新規に
        // screenが初期化されるかどうか(状態行にプロセス名を書くタイミングの判定に使う)
    if (screen_fresh_start) {
        lisp_screen_buffer_init_blank(&target->screen, lisp_screen_buffer.cols, lisp_screen_buffer.rows);
    }
    lisp_screen_buffer_copy(&lisp_screen_buffer, &target->screen);
    lisp_screen_buffer.force_full_redraw = 1;

    // milestone134: 新規プロセスが初めてresumeされた瞬間(screen_fresh_start)にだけ、対象p自身の
    // nameスロットを状態行(行0)へ書き込む。以降はsuspend/resumeの度にscreen構造体全体がそのまま
    // 退避/復元される(milestone133)ため、行0の内容もそのまま持ち越されて明示的な再設定は不要。
    // 計画では「os:process-resume/os:process-suspend(lisp/os.lisp)側で切替直前にLisp側から
    // %set-status-lineを呼ぶ」という設計だったが、実装時に次の技術的な矛盾を発見した:
    // %process-resumeはthunk自身のクロージャのenvフィールドをそのままprocess-local-variable
    // (milestone113)/os:inspect-process(milestone114)の参照先として使う実装であり、
    // os:process-resume側でthunkを別の(lambda () (%set-status-line ...) (funcall thunk))で
    // ラップすると、C側が捕捉するenvが元のthunkの定義時レキシカル環境ではなくラッパー自身の
    // (p/thunk引数のみを束縛する)envに変わってしまい、既存のprocess-local-variable/
    // os:inspect-processが壊れる。この矛盾はLisp側では解消できない(ラッパーに元のthunkと同じ
    // envを後付けする手段がLispコードには公開されていない)ため、ユーザーの判断を待たず、
    // 既にp自身のスロットを直接読んでいるこの関数内でnameスロットも同様に読んで書き込む方式へ
    // 変更した(C側がプロセスの内部データを一切見ないという計画の理想からは外れるが、
    // 既存機能を壊さない実装可能な方式はこちらのみだった)
    if (screen_fresh_start) {
        UINTN name_index = lisp_instance_slot_index(p, lisp_intern("name"));
        LispClosure *name = lisp_closure_cell(slots[name_index]);
        lisp_screen_set_status_line(name->str_data, name->str_len);
    }

    lisp_process_thunk_finished = 0;
    lisp_context_switch(caller, target);

    // milestone133: ここへ戻ってくる経路は2つある。(1)targetが%process-suspendを呼んで
    // 自発的に中断した場合(suspend側で既にlisp_screen_bufferをcallerの画面へ戻し済み)、
    // (2)thunkがsuspendを一度も呼ばずに最後まで実行され自然終了した場合(この場合はsuspend側の
    // 復元コードを一切経由しない)。(2)を素通りさせるとlisp_screen_bufferがtargetの画面の
    // ままになり、次のflushでcols/rowsが実画面と食い違ったままの状態を引き回す実害があった
    // (test-while他で不定期にSetCursorPosition失敗パニックを確認)。(1)(2)どちらでも
    // callerの画面へ戻すのはこの1回のコピーだけで十分なので、suspend側の復元処理と重複しても
    // 無条件にここで実行する。
    //
    // (2)の場合、この時点ではlisp_process_thunk_finishedが1で、lisp_screen_bufferはまだ
    // target自身が最後に書いた内容(自然終了直前の画面、suspend側の復元処理を経由していない)を
    // 保持している。これをcallerの画面で上書きする前にtarget->screenへ保存しておく
    // (target->screenが直前のsuspend時点の内容のまま古くなり、後から誰かがtarget(既に
    // :finished)の画面をインスペクトした場合に不整合な内容を見せてしまう非対称バグがあったため)。
    // (1)の場合はここに来た時点でlisp_screen_bufferは既にsuspend側がcallerの画面へ入替済み
    // (targetの内容ではない)なので、ここで保存すると逆にtarget->screenをcallerの内容で
    // 破壊してしまう。そのためこのtarget->screenへの保存はthunk_finishedの場合のみに限定する
    if (lisp_process_thunk_finished) {
        lisp_screen_buffer_copy(&target->screen, &lisp_screen_buffer);
    }
    lisp_screen_buffer_copy(&lisp_screen_buffer, &caller->screen);
    lisp_screen_buffer.force_full_redraw = 1;

    lisp_current_process_stack = caller;
    if (lisp_process_thunk_finished) {
        lisp_process_thunk_finished = 0;
        slots[status_index] = lisp_intern_keyword("finished");
    }
    return p;
}

// (%process-suspend process): processが「今実際に実行中のコンテキスト自身」である場合のみ
// 許可される(自分自身をsuspendする)。resumerフィールドに記録済みの、自分をresumeした側へ
// lisp_context_switchで戻る。他プロセスのvm_stackをGCルートとして登録してから中断するため、
// suspend中もそのプロセスのVMデータスタック上のオブジェクトは回収されない(milestone107)。
// ただしツリーウォーク経路(lisp_eval/lisp_apply)のC局所変数はこのルート集合の対象外であり、
// 中断中に他プロセスが(gc)を誘発した場合の既知の未解消リスクがある(src/lisp.h参照)
LispObject lisp_builtin_process_suspend(LispObject args) {
    LispObject p = lisp_car(args);
    lisp_assert_instance(p);

    UINTN stackframe_index = lisp_instance_slot_index(p, lisp_intern("stackframe"));
    UINTN status_index = lisp_instance_slot_index(p, lisp_intern("status"));
    LispObject *slots = lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data;

    if (slots[stackframe_index] == LISP_NIL) {
        lisp_panic(L"process-suspend: process has not started");
    }
    lisp_assert_fixnum(slots[stackframe_index]);
    UINTN pool_index = (UINTN)lisp_fixnum_value(slots[stackframe_index]);
    LispProcessStack *target = &lisp_process_context_pool[pool_index];

    if (target != lisp_current_process_stack) {
        lisp_panic(L"process-suspend: process is not currently running");
    }

    slots[status_index] = lisp_intern_keyword("suspended");
    lisp_process_stack_register(target); // milestone107: 中断中はGCルートとして走査対象にする
    LispProcessStack *resumer = target->resumer;
    lisp_current_process_stack = resumer;

    // milestone133: resume側と対称。lisp_context_switch自体には置かない理由も同様
    // (lisp_builtin_process_resumeのコメント参照)
    lisp_screen_buffer_copy(&target->screen, &lisp_screen_buffer);
    lisp_screen_buffer_copy(&lisp_screen_buffer, &resumer->screen);
    lisp_screen_buffer.force_full_redraw = 1;

    lisp_context_switch(target, resumer);

    // ここに戻ってくるのは、後で誰かが%process-resumeでこのプロセスを再開した時
    return p;
}

// (%process-local-variable process symbol): processのenvスロット(milestone112の%process-resumeが
// 初回起動時にthunkクロージャ自身のenvを捕捉したもの)からsymbolの値をlisp_env_lookup経由で
// 読み取る。stackframeスロットがnil(未起動)ならpanicする。
//
// lisp_env_lookupは動的/special変数ならシンボル自身のvalue(全プロセス共有、プロセス毎の分離は
// 無い既知の制約)を返し、レキシカル変数ならenvチェーン→見つからなければglobal_envを探して
// 無ければpanicする(通常のシンボル解決と同じ規則)。
//
// ここで見えるのはthunk定義時点(make-processではなくlambda/defun評価時点)で既にレキシカル
// スコープにあった変数のみである。thunk本体の実行中に新たに導入されたlet束縛は、中断中でも
// ツリーウォーク経路(lisp_eval/lisp_apply)のC呼び出しスタック上にのみ存在し外部から見えない。
//
// 重要な制約: envフィールド(alist)はツリーウォーク経路(lisp_eval)が作るクロージャのみが持つ。
// lisp_eval_toplevel(milestone60〜)はdefmacro・ラムダリストキーワード入りdefun以外の
// トップレベル式をデフォルトでcompile-and-run(macroexpand-all→compile-expr→vm-exec)へ委譲し、
// コンパイル済みclosureはレキシカル変数をenvアリストではなく位置ベース(kind/index)の
// upvalue_descs/upvalues(milestone38)で捕捉するため、変数名からの逆引きができない。したがって
// thunkがコンパイル経路で作られたclosureの場合、envスロットは常にLISP_NILのままであり、
// process-local-variableは(dynamic変数を除き)何も見つけられずunbound variableでpanicする。
// 現状process-local-variableが機能するのは、thunkの生成自体がツリーウォークへフォールバックする
// 経路(&optional/&rest等のラムダリストキーワードを持つdefunの本体全体、またはCから直接
// lisp_evalを呼ぶ場合)に限られる(documents/lisp_os_process.mdマイルストーン113参照)
LispObject lisp_builtin_process_local_variable(LispObject args) {
    LispObject p = lisp_car(args);
    LispObject sym = lisp_car(lisp_cdr(args));
    lisp_assert_instance(p);
    lisp_assert_symbol(sym);

    UINTN stackframe_index = lisp_instance_slot_index(p, lisp_intern("stackframe"));
    UINTN env_index = lisp_instance_slot_index(p, lisp_intern("env"));
    LispObject *slots = lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data;

    if (slots[stackframe_index] == LISP_NIL) {
        lisp_panic(L"process-local-variable: process has not started");
    }
    return lisp_env_lookup(slots[env_index], sym);
}

// --- process-suspend/process-resume自己テスト (milestone 112) ---
// 0引数の閉包(ダイナミック変数m112-counterをインクリメント→自分自身に対しprocess-suspend→
// 再度インクリメント)を新規プロセスに対して開始し、(1)1回目のインクリメント後、suspend直後で
// 実行が呼び出し元(このセルフテスト自身)へ返ってくること・statusが:suspendedであること
// (単一実行コンテキストの協調的切替である以上、呼び出し元が制御を取り戻した時点でこの
// プロセス自身が:activeであることはあり得ない)、(2)再度resumeすると2回目のインクリメント後に
// 閉包が正常に戻り、statusが:finishedになること、(3)2回のインクリメントの結果が2であることを
// 確認する
int lisp_process_suspend_resume_selftest(void) {
    LispObject os_pkg = lisp_find_package("os");
    LispObject cls = lisp_find_class(lisp_intern_in_package(os_pkg, "process"));
    LispObject p = lisp_make_instance(cls);

    lisp_eval(lisp_read_from_buffer("(defparameter m112-counter 0)"), global_env);
    // defun/lambdaの本体は単一formのみ(milestone21で確認済みのprogn gotcha)なので、
    // 複数formを実行するには明示的にprognで束ねる必要がある
    lisp_eval(lisp_read_from_buffer(
        "(defun m112-thunk () (progn (setq m112-counter (+ m112-counter 1)) "
        "(%process-suspend m112-proc) (setq m112-counter (+ m112-counter 1))))"),
        global_env);

    LispObject proc_sym = lisp_intern("m112-proc");
    LispSymbol *proc_cell = lisp_symbol_cell(proc_sym);
    proc_cell->is_special = 1;
    proc_cell->value = p;

    LispObject thunk = lisp_symbol_cell(lisp_intern("m112-thunk"))->fn;

    lisp_builtin_process_resume(lisp_cons(p, lisp_cons(thunk, LISP_NIL)));

    UINTN status_index = lisp_instance_slot_index(p, lisp_intern("status"));
    LispObject status_after_suspend =
        lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data[status_index];
    if (status_after_suspend != lisp_intern_keyword("suspended")) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("m112-counter"), global_env) != lisp_make_fixnum(1)) {
        return 0;
    }

    lisp_builtin_process_resume(lisp_cons(p, LISP_NIL));

    LispObject status_after_finish =
        lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data[status_index];
    if (status_after_finish != lisp_intern_keyword("finished")) {
        return 0;
    }
    if (lisp_eval(lisp_read_from_buffer("m112-counter"), global_env) != lisp_make_fixnum(2)) {
        return 0;
    }

    return 1;
}

// --- process-local-variable自己テスト (milestone 113) ---
// letでレキシカル変数m113-xを束縛した内側で(lambda () nil)を生成し、そのクロージャ自身の
// envフィールドがm113-xの束縛を含んでいることを利用する。thunkをプロセスとして起動
// (%process-suspendを呼ばないので即座に:finishedになる。ここでは実行完了そのものは検証対象
// 外で、%process-resumeがthunkのenvをprocessのenvスロットへ捕捉することのみを確認する)し、
// %process-local-variableでm113-xの値(42)を外部から読み取れることを確認する
int lisp_process_local_variable_selftest(void) {
    LispObject os_pkg = lisp_find_package("os");
    LispObject cls = lisp_find_class(lisp_intern_in_package(os_pkg, "process"));
    LispObject p = lisp_make_instance(cls);

    lisp_eval(lisp_read_from_buffer(
        "(let ((m113-x 42)) (defparameter m113-thunk (lambda () nil)))"),
        global_env);

    LispObject thunk = lisp_symbol_cell(lisp_intern("m113-thunk"))->value;

    lisp_builtin_process_resume(lisp_cons(p, lisp_cons(thunk, LISP_NIL)));

    LispObject x_val = lisp_builtin_process_local_variable(
        lisp_cons(p, lisp_cons(lisp_intern("m113-x"), LISP_NIL)));
    if (x_val != lisp_make_fixnum(42)) {
        return 0;
    }

    return 1;
}

// --- プロセス毎画面バッファ分離自己テスト (milestone 133) ---
//
// documents/lisp_process_screen_switch.mdフェーズN。実際の%process-resume/%process-suspend
// (lisp_builtin_process_resume/lisp_builtin_process_suspend)経由でのみ画面バッファの退避/復元・
// force_full_redrawが発生する設計(lisp_context_switch自体には置いていない、上記コメント参照)
// なので、この2つのビルトインを直接呼ぶ形で検証する。lisp_screen_buffer_selftest等と異なり
// generic-thunkを介さずCの関数そのものをプロセスのトランポリン先(thunk)として使い、
// %process-suspend呼び出し前後で同じCスタックフレーム内の実行を継続させる(既存の
// lisp_context_switch_selftest_entryと同じコルーチン的パターン)
#define LISP_PROCESS_SCREEN_SELFTEST_COL 5

static LispObject lisp_process_screen_selftest_p = LISP_NIL;
static int lisp_process_screen_selftest_caller_marker_absent_ok = 0;

static LispObject lisp_process_screen_selftest_thunk(LispObject args) {
    (void)args;

    // 開始直後、現在の画面バッファ(=このプロセス自身の、新規に空初期化されたバッファ)には
    // 呼び出し元(main)が書き込んだ'A'マーカーが一切見えない(空白のまま)ことを確認する
    lisp_process_screen_selftest_caller_marker_absent_ok =
        (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] == L' ');

    lisp_screen_move_cursor(LISP_PROCESS_SCREEN_SELFTEST_COL, LISP_SCREEN_STATUS_ROWS);
    lisp_screen_putc('B');

    lisp_builtin_process_suspend(lisp_cons(lisp_process_screen_selftest_p, LISP_NIL));

    // 再開後、'B'を書いた位置がそのまま残っている(suspend中に他プロセスに書き換えられて
    // いない)ことを確認してから、finish直前の内容として'C'に書き換える
    int b_preserved_across_suspend =
        (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] == L'B');
    lisp_screen_move_cursor(LISP_PROCESS_SCREEN_SELFTEST_COL, LISP_SCREEN_STATUS_ROWS);
    lisp_screen_putc(b_preserved_across_suspend ? 'C' : 'x');

    return lisp_sym_t;
}

int lisp_process_screen_separation_selftest(void) {
    LispObject os_pkg = lisp_find_package("os");
    LispObject cls = lisp_find_class(lisp_intern_in_package(os_pkg, "process"));
    LispObject p = lisp_make_instance(cls);
    lisp_process_screen_selftest_p = p;
    lisp_process_screen_selftest_caller_marker_absent_ok = 0;

    // 呼び出し元(main)自身の画面バッファへ'A'マーカーを書き込む
    lisp_screen_move_cursor(LISP_PROCESS_SCREEN_SELFTEST_COL, LISP_SCREEN_STATUS_ROWS);
    lisp_screen_putc('A');

    LispObject thunk = lisp_make_builtin(lisp_process_screen_selftest_thunk);

    lisp_builtin_process_resume(lisp_cons(p, lisp_cons(thunk, LISP_NIL)));

    // suspend直後、呼び出し元へ復元された画面バッファに'A'マーカーがそのまま残っている
    // (プロセスBが書いた'B'に上書きされていない)ことを確認する
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] != L'A') {
        return 0;
    }
    if (!lisp_process_screen_selftest_caller_marker_absent_ok) {
        return 0;
    }

    // プロセスB自身の(退避済みの)バッファには、呼び出し元の画面に一切影響せず'B'が
    // 保存されていることを直接back内容比較で確認する
    UINTN stackframe_index = lisp_instance_slot_index(p, lisp_intern("stackframe"));
    LispObject *slots = lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data;
    UINTN pool_index = (UINTN)lisp_fixnum_value(slots[stackframe_index]);
    LispProcessStack *target = &lisp_process_context_pool[pool_index];
    if (target->screen.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] != L'B') {
        return 0;
    }

    lisp_builtin_process_resume(lisp_cons(p, LISP_NIL));

    // 自然終了後も、呼び出し元の画面は'A'のまま(=正しく復元され続けている)ことを確認する
    if (lisp_screen_buffer.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] != L'A') {
        return 0;
    }
    // プロセスB自身のバッファにはsuspend再開後に書いた'C'が残っている
    if (target->screen.back[LISP_SCREEN_STATUS_ROWS][LISP_PROCESS_SCREEN_SELFTEST_COL] != L'C') {
        return 0;
    }

    return 1;
}

// --- 状態行とプロセス切替の連動自己テスト (milestone 134) ---
//
// documents/lisp_process_screen_switch.mdフェーズN最終マイルストーン。プロセスが初めて
// resumeされた瞬間に、lisp_builtin_process_resume内(上記screen_fresh_start分岐)が対象
// プロセス自身のnameスロットを状態行(行0)へ書き込むこと、自然終了後もそのラベルがtarget自身の
// screenへそのまま保存され続けること(milestone133の「自然終了時の退避」修正がここでも
// 正しく効いていることの再確認にもなる)を、back内容の直接比較で検証する
static LispObject lisp_process_status_line_selftest_thunk(LispObject args) {
    (void)args;
    return lisp_sym_t; // %process-suspendを呼ばず即座に自然終了する
}

int lisp_process_status_line_selftest(void) {
    // milestone102のprocessクラスを裸のlisp_make_instanceで作ると、%make-processが本来行う
    // name自動生成(lisp_generate_process_name)を経由しないためnameスロットがnilのままになる。
    // 状態行への書き込みはnameスロットが実際の文字列である前提(%make-process経由の生成物)
    // なので、ここでも%make-process(lisp_builtin_make_process)を使って生成する
    LispObject p = lisp_builtin_make_process(LISP_NIL);

    UINTN name_index = lisp_instance_slot_index(p, lisp_intern("name"));
    UINTN stackframe_index = lisp_instance_slot_index(p, lisp_intern("stackframe"));
    LispObject *slots = lisp_closure_cell(lisp_closure_cell(p)->inst_slots)->vec_data;
    LispClosure *name = lisp_closure_cell(slots[name_index]);

    LispObject thunk = lisp_make_builtin(lisp_process_status_line_selftest_thunk);
    lisp_builtin_process_resume(lisp_cons(p, lisp_cons(thunk, LISP_NIL)));

    UINTN pool_index = (UINTN)lisp_fixnum_value(slots[stackframe_index]);
    LispProcessStack *target = &lisp_process_context_pool[pool_index];

    for (UINTN i = 0; i < name->str_len; i++) {
        if (target->screen.back[0][LISP_SCREEN_STATUS_RESERVED_COLS + i] != (CHAR16)name->str_data[i]) {
            return 0;
        }
    }

    return 1;
}

// milestone98: print-objectの既定method。m96/97時点の#<name instance>相当の表示を、
// defmethodでオーバーライド可能な総称関数の1メソッドとして提供する。lisp_method_applicable
// は無指定specializerを引数の型を問わず適用可能と判定するため、instance以外が渡された
// 誤用はここのlisp_assert_instanceで止める
LispObject lisp_builtin_default_print_object(LispObject args) {
    LispObject obj = lisp_car(args);
    lisp_assert_instance(obj);
    LispOutputStream stream = lisp_make_console_stream(g_system_table);
    lisp_print_ascii(&stream, "#<");
    lisp_print_ascii(&stream,
        lisp_symbol_cell(lisp_closure_cell(lisp_closure_cell(obj)->inst_class)->class_name)->name);
    lisp_print_ascii(&stream, " instance>");
    return obj;
}

// --- defmethod・総称関数・多重ディスパッチ (milestone 97) ---

// name用の新しいgeneric-function objectを作る（gf_methodsは空リストから開始する。
// %ensure-generic-functionの新規作成分岐からのみ呼ばれる）
static LispObject lisp_make_generic_function(LispObject name) {
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
    closure->bytecode = 0;
    closure->bytecode_len = 0;
    closure->constants = 0;
    closure->constants_len = 0;
    closure->nargs = 0;
    closure->max_locals = 0;
    closure->upvalue_descs = LISP_NIL;
    closure->upvalues = LISP_NIL;
    closure->pkg_name = 0;
    closure->pkg_symbols = LISP_NIL;
    closure->pkg_exports = LISP_NIL;
    closure->pkg_uses = LISP_NIL;
    closure->pkg_is_keyword = 0;
    closure->pkg_nicknames = LISP_NIL;
    closure->pkg_shadowing_symbols = LISP_NIL;
    closure->pkg_locked = 0;
    closure->class_name = LISP_NIL;
    closure->class_superclass = LISP_NIL;
    closure->class_direct_slots = LISP_NIL;
    closure->class_all_slots = LISP_NIL;
    closure->inst_class = LISP_NIL;
    closure->inst_slots = LISP_NIL;
    closure->gf_name = name;
    closure->gf_methods = LISP_NIL;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// clsからancestorまでclass_superclassを辿ったホップ数を返す（cls自身なら0、辿り着けなければ-1）
static long long lisp_class_hops_to_ancestor(LispObject cls, LispObject ancestor) {
    long long hops = 0;
    for (LispObject cur = cls; cur != LISP_NIL; cur = lisp_closure_cell(cur)->class_superclass) {
        if (cur == ancestor) {
            return hops;
        }
        hops++;
    }
    return -1;
}

// specializers(specializer-listのconsリスト、各要素はclassオブジェクトまたは無指定のNIL)が
// args(評価済みリスト)に適用可能か判定する。各位置について、specializerがNILなら常に適用可、
// 非NILなら対応する実引数がinstanceであり、そのinst_classからspecializerまでのホップ数が
// 求まる(>=0)ことが必要
static int lisp_method_applicable(LispObject specializers, LispObject args) {
    LispObject spec_cur = specializers;
    LispObject arg_cur = args;
    while (lisp_is_cons(spec_cur)) {
        if (!lisp_is_cons(arg_cur)) {
            return 0; // アリティ不足（%add-methodのチェックにより通常は起こらない）
        }
        LispObject spec = lisp_car(spec_cur);
        LispObject arg = lisp_car(arg_cur);
        if (spec != LISP_NIL) {
            if (!lisp_is_instance(arg) || lisp_class_hops_to_ancestor(lisp_closure_cell(arg)->inst_class, spec) < 0) {
                return 0;
            }
        }
        spec_cur = lisp_cdr(spec_cur);
        arg_cur = lisp_cdr(arg_cur);
    }
    return 1;
}

// specs_a/specs_bをargs(適用可能であること前提、ホップ数解決に使う)のもとで比較する。
// 「無指定より指定ありが詳細」「指定ありどうしはホップ数が少ない方が詳細」を各位置で判定し、
// 全位置を通して一方だけが勝っていた場合のみ非0を返す(部分順序としての比較)。+1ならspecs_aが
// より詳細、-1ならspecs_bがより詳細、0なら比較不能（両方が異なる位置で勝った、または
// どちらも勝たなかった）
static int lisp_compare_method_specificity(LispObject specs_a, LispObject specs_b, LispObject args) {
    int a_wins = 0;
    int b_wins = 0;
    LispObject cur_a = specs_a;
    LispObject cur_b = specs_b;
    LispObject cur_arg = args;
    while (lisp_is_cons(cur_a)) {
        LispObject spec_a = lisp_car(cur_a);
        LispObject spec_b = lisp_car(cur_b);
        if (spec_a == LISP_NIL && spec_b != LISP_NIL) {
            b_wins = 1;
        } else if (spec_a != LISP_NIL && spec_b == LISP_NIL) {
            a_wins = 1;
        } else if (spec_a != LISP_NIL && spec_b != LISP_NIL && spec_a != spec_b) {
            LispObject arg_cls = lisp_closure_cell(lisp_car(cur_arg))->inst_class;
            long long hops_a = lisp_class_hops_to_ancestor(arg_cls, spec_a);
            long long hops_b = lisp_class_hops_to_ancestor(arg_cls, spec_b);
            if (hops_a < hops_b) {
                a_wins = 1;
            } else if (hops_b < hops_a) {
                b_wins = 1;
            }
        }
        cur_a = lisp_cdr(cur_a);
        cur_b = lisp_cdr(cur_b);
        cur_arg = lisp_cdr(cur_arg);
    }
    if (a_wins && !b_wins) {
        return 1;
    }
    if (b_wins && !a_wins) {
        return -1;
    }
    return 0;
}

// gfのgf_methodsからargs(評価済みリスト)に適用可能かつ他のどのmethodにも劣後しなかった
// method closureを選ぶ。適用可能なmethodが無ければ"no applicable method"、非劣後なものが
// 2つ以上残れば"ambiguous method call"でpanicする（部分順序は非空有限集合なら必ず非劣後な
// 要素を持つため、この判定は健全）
static LispObject lisp_gf_select_method(LispObject gf, LispObject args) {
    LispObject applicable = LISP_NIL;
    for (LispObject cur = lisp_closure_cell(gf)->gf_methods; lisp_is_cons(cur); cur = lisp_cdr(cur)) {
        LispObject entry = lisp_car(cur);
        if (lisp_method_applicable(lisp_car(entry), args)) {
            applicable = lisp_cons(entry, applicable);
        }
    }
    if (applicable == LISP_NIL) {
        lisp_panic(L"no applicable method");
    }
    LispObject survivors = LISP_NIL;
    for (LispObject cur = applicable; lisp_is_cons(cur); cur = lisp_cdr(cur)) {
        LispObject candidate = lisp_car(cur);
        int dominated = 0;
        for (LispObject other_cur = applicable; lisp_is_cons(other_cur); other_cur = lisp_cdr(other_cur)) {
            LispObject other = lisp_car(other_cur);
            if (other != candidate &&
                lisp_compare_method_specificity(lisp_car(other), lisp_car(candidate), args) > 0) {
                dominated = 1;
                break;
            }
        }
        if (!dominated) {
            survivors = lisp_cons(candidate, survivors);
        }
    }
    if (lisp_cdr(survivors) != LISP_NIL) {
        lisp_panic(L"ambiguous method call");
    }
    return lisp_cdr(lisp_car(survivors));
}

// (%ensure-generic-function name): fboundpかつ既存のsymbol-functionがgeneric-functionで
// あればそれを返す。無ければ新規作成してnameの関数セルへbindする（defunの同名再定義と同じ
// 「shadowで上書き」規約）
LispObject lisp_builtin_ensure_generic_function(LispObject args) {
    LispObject name = lisp_car(args);
    lisp_assert_symbol(name);
    LispObject existing = lisp_symbol_cell(name)->fn;
    if (existing != LISP_NIL && lisp_is_generic_function(existing)) {
        return existing;
    }
    LispObject gf = lisp_make_generic_function(name);
    lisp_symbol_cell(name)->fn = gf;
    return gf;
}

// (%add-method name specializer-list method-closure): nameの関数セルがgeneric-function
// であることを確認する。既存entryとspecializer-listが(要素ごとにeq)一致すれば置き換え、
// 無ければ新規追加する。既存entryとアリティが異なれば"incongruent lambda list"でpanicする
LispObject lisp_builtin_add_method(LispObject args) {
    LispObject name = lisp_car(args);
    lisp_assert_symbol(name);
    LispObject specializers = lisp_car(lisp_cdr(args));
    LispObject method_closure = lisp_car(lisp_cdr(lisp_cdr(args)));

    LispObject gf = lisp_symbol_cell(name)->fn;
    if (gf == LISP_NIL || !lisp_is_generic_function(gf)) {
        lisp_panic(L"%add-method: expected a generic function");
    }
    LispClosure *gfc = lisp_closure_cell(gf);
    UINTN new_arity = lisp_list_length(specializers);

    // 既存method群は互いに同じアリティのはず(この不変条件自体をここで維持する)なので、
    // 先頭entryのアリティとだけ比較すれば十分
    if (lisp_is_cons(gfc->gf_methods) &&
        lisp_list_length(lisp_car(lisp_car(gfc->gf_methods))) != new_arity) {
        lisp_panic(L"%add-method: incongruent lambda list");
    }

    for (LispObject cur = gfc->gf_methods; lisp_is_cons(cur); cur = lisp_cdr(cur)) {
        LispObject entry = lisp_car(cur);
        LispObject a = lisp_car(entry);
        LispObject b = specializers;
        int same = 1;
        while (lisp_is_cons(a) && lisp_is_cons(b)) {
            if (lisp_car(a) != lisp_car(b)) {
                same = 0;
                break;
            }
            a = lisp_cdr(a);
            b = lisp_cdr(b);
        }
        if (same) {
            lisp_set_cdr(entry, method_closure);
            return name;
        }
    }
    gfc->gf_methods = lisp_cons(lisp_cons(specializers, method_closure), gfc->gf_methods);
    return name;
}

// milestone 101: LISP_REGISTER_BUILTINで登録する全ビルトイン関数のシンボルを、milestone100の
// 特殊形式exportと同じ理由でcommon-lisp-userからexportする。マクロの展開先をこの1箇所に
// 集約することで、以後LISP_REGISTER_BUILTINで追加される新規ビルトインも自動的にexport対象になる
static void lisp_register_and_export_builtin(const char *name, LispBuiltinFn func) {
    LispObject sym = lisp_intern(name);
    lisp_symbol_cell(sym)->fn = lisp_make_builtin(func);
    LispClosure *cl_user_cell = lisp_closure_cell(lisp_cl_user_package);
    cl_user_cell->pkg_exports = lisp_cons(sym, cl_user_cell->pkg_exports);
}

// car/cdr/cons/eq/atom/+/-/load をグローバル環境に束縛して返す
// milestone94: 組み込み関数の登録先をglobal_env(alist)から各symbolの関数セル(fn)への
// 直接書き込みへ変更した。戻り値のenvは不要になったためvoidにした(呼び出し元main.cの
// 代入も削除する)
#define LISP_REGISTER_BUILTIN(name, func) lisp_register_and_export_builtin(name, func)

void lisp_builtins_init(void) {
    LISP_REGISTER_BUILTIN("car", lisp_builtin_car);
    LISP_REGISTER_BUILTIN("cdr", lisp_builtin_cdr);
    LISP_REGISTER_BUILTIN("cons", lisp_builtin_cons);
    LISP_REGISTER_BUILTIN("funcall", lisp_builtin_funcall);
    LISP_REGISTER_BUILTIN("apply", lisp_builtin_apply);
    LISP_REGISTER_BUILTIN("eq", lisp_builtin_eq);
    LISP_REGISTER_BUILTIN("atom", lisp_builtin_atom);
    LISP_REGISTER_BUILTIN("symbolp", lisp_builtin_symbolp);
    LISP_REGISTER_BUILTIN("keywordp", lisp_builtin_keywordp);
    LISP_REGISTER_BUILTIN("make-package", lisp_builtin_make_package);
    LISP_REGISTER_BUILTIN("find-package", lisp_builtin_find_package);
    LISP_REGISTER_BUILTIN("export", lisp_builtin_export);
    LISP_REGISTER_BUILTIN("use-package", lisp_builtin_use_package);
    LISP_REGISTER_BUILTIN("intern", lisp_builtin_intern);
    LISP_REGISTER_BUILTIN("in-package", lisp_builtin_in_package);
    LISP_REGISTER_BUILTIN("package-name", lisp_builtin_package_name);
    LISP_REGISTER_BUILTIN("package-nicknames", lisp_builtin_package_nicknames);
    LISP_REGISTER_BUILTIN("package-use-list", lisp_builtin_package_use_list);
    LISP_REGISTER_BUILTIN("list-all-packages", lisp_builtin_list_all_packages);
    LISP_REGISTER_BUILTIN("%package-symbols", lisp_builtin_package_symbols);
    LISP_REGISTER_BUILTIN("%package-exported-symbols", lisp_builtin_package_exported_symbols);
    LISP_REGISTER_BUILTIN("find-symbol", lisp_builtin_find_symbol);
    LISP_REGISTER_BUILTIN("find-all-symbols", lisp_builtin_find_all_symbols);
    LISP_REGISTER_BUILTIN("shadow", lisp_builtin_shadow);
    LISP_REGISTER_BUILTIN("unexport", lisp_builtin_unexport);
    LISP_REGISTER_BUILTIN("unuse-package", lisp_builtin_unuse_package);
    LISP_REGISTER_BUILTIN("import", lisp_builtin_import);
    LISP_REGISTER_BUILTIN("shadowing-import", lisp_builtin_shadowing_import);
    LISP_REGISTER_BUILTIN("delete-package", lisp_builtin_delete_package);
    LISP_REGISTER_BUILTIN("rename-package", lisp_builtin_rename_package);
    LISP_REGISTER_BUILTIN("lock-package", lisp_builtin_lock_package);
    LISP_REGISTER_BUILTIN("unlock-package", lisp_builtin_unlock_package);
    LISP_REGISTER_BUILTIN("package-locked-p", lisp_builtin_package_locked_p);
    LISP_REGISTER_BUILTIN("rplaca", lisp_builtin_rplaca);
    LISP_REGISTER_BUILTIN("rplacd", lisp_builtin_rplacd);
    LISP_REGISTER_BUILTIN("hash-code", lisp_builtin_hash_code);
    LISP_REGISTER_BUILTIN("+", lisp_builtin_add);
    LISP_REGISTER_BUILTIN("-", lisp_builtin_sub);
    LISP_REGISTER_BUILTIN("<", lisp_builtin_lt);
    LISP_REGISTER_BUILTIN("load", lisp_builtin_load);
    LISP_REGISTER_BUILTIN("write-file", lisp_builtin_write_file);
    LISP_REGISTER_BUILTIN("write-line", lisp_builtin_write_line);
    // milestone98: print-objectのmethod本体が出力を組み立てるための基本手段
    LISP_REGISTER_BUILTIN("write-string", lisp_builtin_write_string);
    LISP_REGISTER_BUILTIN("princ", lisp_builtin_princ);
    LISP_REGISTER_BUILTIN("%read-console-expr", lisp_builtin_read_console_expr);
    // milestone120: カーソル制御ビルトイン(暫定実装、直接ConOut)
    LISP_REGISTER_BUILTIN("%clear-screen", lisp_builtin_clear_screen);
    LISP_REGISTER_BUILTIN("%set-cursor-position", lisp_builtin_set_cursor_position);
    LISP_REGISTER_BUILTIN("%get-screen-size", lisp_builtin_get_screen_size);
    LISP_REGISTER_BUILTIN("%set-status-line", lisp_builtin_set_status_line);
    LISP_REGISTER_BUILTIN("sleep", lisp_builtin_sleep);
    LISP_REGISTER_BUILTIN("gensym", lisp_builtin_gensym);
    LISP_REGISTER_BUILTIN("gc", lisp_builtin_gc);
    LISP_REGISTER_BUILTIN("heap-remaining", lisp_builtin_heap_remaining);
    LISP_REGISTER_BUILTIN("make-vector", lisp_builtin_make_vector);
    LISP_REGISTER_BUILTIN("svref", lisp_builtin_svref);
    LISP_REGISTER_BUILTIN("svset", lisp_builtin_svset);
    LISP_REGISTER_BUILTIN("macroexpand-1", lisp_builtin_macroexpand_1);
    LISP_REGISTER_BUILTIN("vm-make-closure", lisp_builtin_vm_make_closure);
    LISP_REGISTER_BUILTIN("vm-exec", lisp_builtin_vm_exec);
    LISP_REGISTER_BUILTIN("special-variable-p", lisp_builtin_special_variable_p);
    LISP_REGISTER_BUILTIN("establish-special", lisp_builtin_establish_special);
    LISP_REGISTER_BUILTIN("mark-compiler-ready", lisp_builtin_mark_compiler_ready);
    LISP_REGISTER_BUILTIN("compiler-ready-p", lisp_builtin_compiler_ready_p);
    LISP_REGISTER_BUILTIN("establish-global-function", lisp_builtin_establish_global_function);
    LISP_REGISTER_BUILTIN("%panic-compiled-lambda-list-keyword", lisp_builtin_panic_compiled_lambda_list_keyword);
    // milestone 93で追加した関数セル(fn)のfuncall系API自身も、milestone94で他の組み込み関数と
    // 同じ関数セル直接登録方式に統一する
    LISP_REGISTER_BUILTIN("symbol-function", lisp_builtin_symbol_function);
    LISP_REGISTER_BUILTIN("%set-symbol-function", lisp_builtin_set_symbol_function);
    LISP_REGISTER_BUILTIN("fboundp", lisp_builtin_fboundp);
    LISP_REGISTER_BUILTIN("symbol-value", lisp_builtin_symbol_value);

    // milestone96: 最小CLOSサブセット(class/instance/単一継承、ディスパッチ無し)
    LISP_REGISTER_BUILTIN("%make-class", lisp_builtin_make_class);
    LISP_REGISTER_BUILTIN("find-class", lisp_builtin_find_class);
    LISP_REGISTER_BUILTIN("make-instance", lisp_builtin_make_instance);
    LISP_REGISTER_BUILTIN("slot-value", lisp_builtin_slot_value);
    LISP_REGISTER_BUILTIN("set-slot-value", lisp_builtin_set_slot_value);
    LISP_REGISTER_BUILTIN("class-of", lisp_builtin_class_of);
    LISP_REGISTER_BUILTIN("%make-process", lisp_builtin_make_process);
    LISP_REGISTER_BUILTIN("%process-resume", lisp_builtin_process_resume);
    LISP_REGISTER_BUILTIN("%process-suspend", lisp_builtin_process_suspend);
    LISP_REGISTER_BUILTIN("%process-local-variable", lisp_builtin_process_local_variable);

    // milestone97: defmethod・総称関数・多重ディスパッチ
    LISP_REGISTER_BUILTIN("%ensure-generic-function", lisp_builtin_ensure_generic_function);
    LISP_REGISTER_BUILTIN("%add-method", lisp_builtin_add_method);

    // milestone98: print-objectの既定methodをstdlib.lisp読込前(defmethodマクロが使えない
    // 時点)にC側から直接登録する。instanceが存在し得るのはstdlib.lisp読込完了後のみなので、
    // この時点で常にbind済みにしておけば未束縛フォールバックは不要
    lisp_builtin_ensure_generic_function(lisp_cons(lisp_sym_print_object, LISP_NIL));
    {
        LispObject print_object_default_specializers = lisp_cons(LISP_NIL, LISP_NIL);
        lisp_builtin_add_method(lisp_cons(lisp_sym_print_object,
            lisp_cons(print_object_default_specializers,
            lisp_cons(lisp_make_builtin(lisp_builtin_default_print_object), LISP_NIL))));
    }

    // *macroexpand-hook*をdefvarと同じ形（is_special=1 + 初期値）で直接セットアップする
    // (milestone 21)。動的変数はenvチェーンに束縛を積まないため、global_envへの
    // 登録は不要（milestone94以降も、これは値namespace専用のis_special機構なので無変更）
    LispSymbol *hook_cell = lisp_symbol_cell(lisp_sym_macroexpand_hook);
    hook_cell->value = lisp_make_builtin(lisp_default_macroexpand_hook);
    hook_cell->is_special = 1;

    // milestone 101: print-object（総称関数、LISP_REGISTER_BUILTIN経由ではなくlisp_sym_print_object
    // を直接使って登録される）と*macroexpand-hook*（動的変数、global_envを経由しない）は
    // 上のLISP_REGISTER_BUILTIN自動export網の対象外なので、ここで個別にexportする
    {
        LispClosure *cl_user_cell = lisp_closure_cell(lisp_cl_user_package);
        cl_user_cell->pkg_exports = lisp_cons(lisp_sym_print_object, cl_user_cell->pkg_exports);
        cl_user_cell->pkg_exports = lisp_cons(lisp_sym_macroexpand_hook, cl_user_cell->pkg_exports);
    }
}

#undef LISP_REGISTER_BUILTIN

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

// --- per-processスタック領域とコンテキスト保存 (milestone 104) ---
// トランポリン(lisp_process_trampoline)は「未開始」コンテキストへの最初のlisp_context_switch
// でのみ実行される。単一実行コンテキストの協調的切替(スコープ外項目に明記済み: 真のプリエンプション
// 無し)なので、実行開始待ちのentry/argを保持するグローバル変数は同時に1つで足りる
static void (*lisp_pending_trampoline_entry)(void *) = 0;
static void *lisp_pending_trampoline_arg = 0;

static void lisp_process_trampoline(void) {
    void (*entry)(void *) = lisp_pending_trampoline_entry;
    void *arg = lisp_pending_trampoline_arg;
    entry(arg);
    // entryが戻ってきた場合(プロセスの終了・破棄はスコープ外のため未対応、documents/
    // lisp_os_process.md参照)は安全のためhangする
    for (;;) {}
}

void lisp_process_stack_create(LispProcessStack *out, UINTN stack_pages, void (*entry)(void *), void *arg) {
    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;
    if (bs->AllocatePages(AllocateAnyPages, EfiLoaderData, stack_pages, &base) != 0) {
        lisp_panic_fatal(L"lisp_process_stack_create: AllocatePages failed");
    }

    // milestone88のEfiMainと同じく16byte境界に揃えたうえで、call命令が積む戻り先アドレス
    // 8byte分をあらかじめ差し引いておく(jmpq経由でlisp_process_trampolineへ直接飛び込む際、
    // callされた直後であるかのようにrspを整えておく必要があるため。トランポリンは戻らない
    // 前提なので、この位置に置く「戻り先」の値自体は使われない)
    UINT64 top = (base + (UINT64)stack_pages * 4096) & ~(UINT64)0xF;
    UINT64 *sp = (UINT64 *)top;
    sp -= 1;
    *sp = 0;

    out->stack_base = base;
    out->stack_pages = stack_pages;
    out->pending_entry = entry;
    out->pending_arg = arg;
    out->started = 0;
    out->vm_sp = 0; // milestone105: 空のVMデータスタックで開始する
    out->active_trap = (void *)0; // milestone105: トラップ未設置で開始する
    out->resumer = (void *)0; // milestone112: resume時に設定される
    // milestone133: 未使用(初回%process-resumeで実際にtargetとして使われるまで)であることを
    // 示すマーカー。lisp_builtin_process_resumeはこれが0の間、現在のグローバル画面バッファと
    // 同じcols/rowsでこの場を初めて空白初期化する(lisp_screen_buffer_init_blank)
    out->screen.initialized = 0;
    out->regs.rbx = 0;
    out->regs.rbp = 0;
    out->regs.rdi = 0;
    out->regs.rsi = 0;
    out->regs.rsp = (UINT64)sp;
    out->regs.r12 = 0;
    out->regs.r13 = 0;
    out->regs.r14 = 0;
    out->regs.r15 = 0;
    out->regs.rip = (UINT64)lisp_process_trampoline;
}

// milestone105: グローバルなvm_stack/vm_sp/lisp_active_trapは常に「今実際に実行中の
// プロセス」のものを指す単一の作業領域のまま(lisp_vm_run等の既存コードは無変更)とし、
// ここでfrom/to間でコピー/入替する。CPUレジスタ(regs)の切替と同じ「今の状態をfromへ
// 退避してからtoの状態を復元する」順序を守る
void lisp_context_switch(LispProcessStack *from, LispProcessStack *to) {
    for (UINTN i = 0; i < vm_sp; i++) {
        from->vm_stack[i] = vm_stack[i];
    }
    from->vm_sp = vm_sp;
    from->active_trap = lisp_active_trap;

    for (UINTN i = 0; i < to->vm_sp; i++) {
        vm_stack[i] = to->vm_stack[i];
    }
    vm_sp = to->vm_sp;
    lisp_active_trap = to->active_trap;

    // milestone133: documents/lisp_process_screen_switch.mdフェーズN。画面バッファの退避/
    // 復元はここではなく%process-resume/%process-suspend側(lisp_builtin_process_resume/
    // lisp_builtin_process_suspend)で行う。既存のvm_stack/vm_sp/active_trap入替と同じ場所に
    // 置く案を最初に試したが、lisp_context_switch自体は本物のプロセス切替以外にも(register/
    // vm_stack分離を検証する多数の自己テストが)raw stack-local変数を使って直接呼んでおり、
    // それら全てで無条件にforce_full_redrawが立つと、どの自己テストとも無関係な後続の
    // どこかの時点(次にVM命令ディスパッチフックがflushする瞬間)で画面全体の再送出が
    // 割り込み、テストハーネスの改行前提のシリアル出力解析を壊す実害を確認した
    // (make testで全ファイルがTIMEOUTする形で発覚)。実際に「表示中のプロセスが切り替わる」
    // ことを意味するのは%process-resume/%process-suspendの2箇所のみなので、画面バッファの
    // 退避/復元とforce_full_redrawの設定はそちら側にのみ置く

    if (!to->started) {
        to->started = 1;
        lisp_pending_trampoline_entry = to->pending_entry;
        lisp_pending_trampoline_arg = to->pending_arg;
    }
    if (lisp_setjmp(&from->regs) == 0) {
        lisp_longjmp(&to->regs, 1);
    }
    // toが(あるいは別の誰かがtoを経由して)いつかlisp_context_switch(to, from)を呼ぶまでここで停止する
}

// mainコンテキスト<->別スタック上のコンテキストを3往復し、双方のカウンタが期待どおり
// 増えることを確認する自己テストのentry。3回yieldしたあとは戻らない(プロセス終了は
// スコープ外)ためhangするが、テスト側は3往復分の切り替えしか行わないので到達しない
static UINTN lisp_context_switch_selftest_counter = 0;
static LispProcessStack *lisp_context_switch_selftest_main_ctx = 0;
static LispProcessStack *lisp_context_switch_selftest_b_ctx = 0;

static void lisp_context_switch_selftest_entry(void *arg) {
    (void)arg;
    for (UINTN i = 0; i < 3; i++) {
        lisp_context_switch_selftest_counter++;
        lisp_context_switch(lisp_context_switch_selftest_b_ctx, lisp_context_switch_selftest_main_ctx);
    }
    for (;;) {}
}

int lisp_context_switch_selftest(void) {
    LispProcessStack main_ctx;
    LispProcessStack b_ctx;
    lisp_context_switch_selftest_main_ctx = &main_ctx;
    lisp_context_switch_selftest_b_ctx = &b_ctx;
    lisp_context_switch_selftest_counter = 0;

    lisp_process_stack_create(&b_ctx, 4, lisp_context_switch_selftest_entry, 0);

    for (UINTN i = 0; i < 3; i++) {
        lisp_context_switch(&main_ctx, &b_ctx);
        if (lisp_context_switch_selftest_counter != i + 1) {
            return 0;
        }
    }
    return 1;
}

// --- per-process vm_stack/vm_sp/lisp_active_trap分離自己テスト (milestone 105) ---
//
// bのentryは2段階に分けて検証する。1段目(開始直後)はmainが積んだ値・設置したトラップが
// 一切見えていない(空のVMデータスタック・トラップ未設置のまま開始する)ことを確認し、その後
// b専用の値をVMデータスタックへ積み・b専用のトラップを設置してmainへ戻る。2段目(mainからの
// 再開後)はその「b専用の値・トラップ」が退避先(main側での実行)に一切書き換えられずそのまま
// 残っていることを確認する。mainは各回の切替後、自分自身が積んだ値・設置したトラップが
// bの実行によって書き換えられていないことを確認する
static int lisp_process_vm_state_selftest_b_clean_start_ok = 0;
static int lisp_process_vm_state_selftest_b_resumed_ok = 0;
static lisp_jmp_buf lisp_process_vm_state_selftest_b_trap;
static LispProcessStack *lisp_process_vm_state_selftest_main_ctx = 0;
static LispProcessStack *lisp_process_vm_state_selftest_b_ctx = 0;

static void lisp_process_vm_state_selftest_entry(void *arg) {
    (void)arg;

    lisp_process_vm_state_selftest_b_clean_start_ok =
        (vm_sp == 0 && lisp_active_trap == (void *)0);

    lisp_vm_push(lisp_make_fixnum(777));
    lisp_setjmp(&lisp_process_vm_state_selftest_b_trap); // 戻り値は使わない: 実際にlongjmpされる
                                                          // ことは無く、トラップとして設置するだけ
    lisp_active_trap = &lisp_process_vm_state_selftest_b_trap;

    lisp_context_switch(lisp_process_vm_state_selftest_b_ctx, lisp_process_vm_state_selftest_main_ctx);

    lisp_process_vm_state_selftest_b_resumed_ok =
        (vm_sp == 1 && vm_stack[0] == lisp_make_fixnum(777) &&
         lisp_active_trap == &lisp_process_vm_state_selftest_b_trap);

    // mainは2回bへ切替える(初期状態確認用・再開状態確認用)ので、bも2回mainへ切り戻す
    // 必要がある(切替回数が対称でないと、mainの2回目の切替が戻ってこず永久にハングする)
    lisp_context_switch(lisp_process_vm_state_selftest_b_ctx, lisp_process_vm_state_selftest_main_ctx);

    for (;;) {}
}

int lisp_process_vm_state_selftest(void) {
    LispProcessStack main_ctx;
    LispProcessStack b_ctx;
    lisp_process_vm_state_selftest_main_ctx = &main_ctx;
    lisp_process_vm_state_selftest_b_ctx = &b_ctx;
    lisp_process_vm_state_selftest_b_clean_start_ok = 0;
    lisp_process_vm_state_selftest_b_resumed_ok = 0;

    lisp_process_stack_create(&b_ctx, 4, lisp_process_vm_state_selftest_entry, 0);

    // このセルフテストはlisp_active_trapがまだ一度も設置されていない(main.cがREPLループの
    // トラップを設置するより前)起動シーケンス中に呼ばれる前提で、終了時にvm_sp==0・
    // lisp_active_trap==NULLへ戻すだけでよい(呼び出し前の実際の値を汎用的に退避・復元する
    // 必要は無い)
    lisp_jmp_buf main_trap;

    lisp_vm_reset_stack();
    lisp_vm_push(lisp_make_fixnum(111));
    lisp_vm_push(lisp_make_fixnum(222));
    lisp_setjmp(&main_trap); // 同上、トラップとして設置するだけ
    lisp_active_trap = &main_trap;

    // 1回目の切替: bは開始直後の「空の状態」であることを確認したのち、自分専用の状態を
    // 作ってmainへ戻る
    lisp_context_switch(&main_ctx, &b_ctx);

    int ok = lisp_process_vm_state_selftest_b_clean_start_ok;
    ok = ok && (vm_sp == 2 && vm_stack[0] == lisp_make_fixnum(111) &&
                vm_stack[1] == lisp_make_fixnum(222) && lisp_active_trap == &main_trap);

    // 2回目の切替: bを再開させ、1回目でbが作った状態がそのまま残っていたことを確認させる
    lisp_context_switch(&main_ctx, &b_ctx);

    ok = ok && lisp_process_vm_state_selftest_b_resumed_ok;
    ok = ok && (vm_sp == 2 && vm_stack[0] == lisp_make_fixnum(111) &&
                vm_stack[1] == lisp_make_fixnum(222) && lisp_active_trap == &main_trap);

    lisp_vm_reset_stack();
    lisp_active_trap = (void *)0;

    return ok;
}

// --- コルーチンyieldチェック自己テスト (milestone 106) ---
//
// lisp_vm_current_process/lisp_vm_yield_targetを武装したうえで、b専用スタック上でVM
// bytecode（0からTARGETまで1ずつ数え上げるループ）を実行させる。budgetを小さいquantumに
// 設定してから1回ずつlisp_context_switchでbへ制御を渡す、というのをbがdoneフラグを立てる
// まで繰り返す。これにより、(1)1回の切替では完了せず、命令ディスパッチの「途中」で
// 実際に複数回yield・resumeされること、(2)最終的にbytecodeが正しい結果(TARGET)まで
// 数え上げを完了すること（pc・オペランドスタック・ローカル変数がyieldを跨いで正しく
// 保持されること）の両方を確認できる。driverの`while (!done)`ループがyieldの回数に応じて
// 動的に切替回数を合わせるため、milestone105の固定回数往復とは違い、切替回数の対称性を
// 手動で数え合わせる必要が無い（bのentryは完了時に1回だけ明示的にmainへ戻ればよい）
#define LISP_VM_YIELD_SELFTEST_TARGET 12
#define LISP_VM_YIELD_SELFTEST_QUANTUM 3

// 0からTARGETまで数え上げてTARGETを返すbytecode(手書き。局所変数slot0を数え上げカウンタに
// 使う。定数0=初期値、定数1=増分、定数2=TARGET)
static const unsigned char lisp_vm_yield_selftest_bytecode[] = {
    OP_CONST,          0, 0,  // 0:  push constants[0] (=0)
    OP_MAKE_LOCAL,     0, 0,  // 3:  local[0] := pop() (counter := 0)
    OP_LOAD_LOCAL,     0, 0,  // 6:  (LOOP) push local[0]
    OP_CONST,          1, 0,  // 9:  push constants[1] (=1)
    OP_ADD,                   // 12: counter+1
    OP_STORE_LOCAL,    0, 0,  // 13: local[0] := pop()
    OP_LOAD_LOCAL,     0, 0,  // 16: push local[0]
    OP_CONST,          2, 0,  // 19: push constants[2] (=TARGET)
    OP_EQ,                    // 22: push (eq counter target)
    OP_JUMP_IF_FALSE,  6, 0,  // 23: 未到達ならLOOPへ
    OP_LOAD_LOCAL,     0, 0,  // 26: push local[0] (戻り値)
    OP_RETURN,                // 29
};

static LispProcessStack *lisp_vm_yield_selftest_main_ctx = 0;
static LispProcessStack *lisp_vm_yield_selftest_b_ctx = 0;
static int lisp_vm_yield_selftest_done = 0;
static LispObject lisp_vm_yield_selftest_result = LISP_NIL;

static void lisp_vm_yield_selftest_entry(void *arg) {
    (void)arg;

    LispObject constants[3];
    constants[0] = lisp_make_fixnum(0);
    constants[1] = lisp_make_fixnum(1);
    constants[2] = lisp_make_fixnum(LISP_VM_YIELD_SELFTEST_TARGET);
    LispObject fn = lisp_make_compiled(lisp_vm_yield_selftest_bytecode,
                                        sizeof(lisp_vm_yield_selftest_bytecode),
                                        constants, 3, 0, 1);

    LispObject result = lisp_vm_exec(fn); // この呼び出しの最中、lisp_vm_run内のyieldチェックが
                                           // 複数回mainへ制御を返し、mainからの再開でここへ戻る

    lisp_vm_yield_selftest_result = result;
    lisp_vm_yield_selftest_done = 1;
    lisp_context_switch(lisp_vm_yield_selftest_b_ctx, lisp_vm_yield_selftest_main_ctx);

    for (;;) {}
}

int lisp_vm_yield_selftest(void) {
    LispProcessStack main_ctx;
    LispProcessStack b_ctx;
    lisp_vm_yield_selftest_main_ctx = &main_ctx;
    lisp_vm_yield_selftest_b_ctx = &b_ctx;
    lisp_vm_yield_selftest_done = 0;
    lisp_vm_yield_selftest_result = LISP_NIL;

    lisp_process_stack_create(&b_ctx, 4, lisp_vm_yield_selftest_entry, 0);

    lisp_vm_current_process = &b_ctx;
    lisp_vm_yield_target = &main_ctx;

    UINTN switch_count = 0;
    while (!lisp_vm_yield_selftest_done && switch_count < 1000) {
        lisp_vm_yield_budget = LISP_VM_YIELD_SELFTEST_QUANTUM;
        lisp_context_switch(&main_ctx, &b_ctx);
        switch_count++;
    }

    // 武装解除: main_ctx/b_ctxはこの関数のスタックローカル変数であり、この関数を抜けると
    // 無効になる。以降のVM実行(この直後に続くcompiler.lisp/stdlib.lispのロード等)が
    // ダングリングポインタへlisp_context_switchしてしまわないよう、成功・失敗どちらの
    // 経路でも必ず武装解除してから返る
    lisp_vm_current_process = (void *)0;
    lisp_vm_yield_target = (void *)0;
    lisp_vm_yield_budget = (UINTN)-1;

    int ok = lisp_vm_yield_selftest_done;
    ok = ok && (switch_count > 1); // 1回の切替では終わらず、実際に複数回yield・resumeしたこと
    ok = ok && (lisp_vm_yield_selftest_result == lisp_make_fixnum(LISP_VM_YIELD_SELFTEST_TARGET));

    return ok;
}
