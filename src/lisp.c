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

typedef struct {
    LispObject car;
    LispObject cdr;
} LispCons;

typedef struct {
    char name[LISP_SYMBOL_NAME_MAX];
    int is_special;   // milestone 18: defvar/defparameterで真になる動的変数フラグ
    LispObject value; // is_specialが真の場合の現在の動的値。let/let*が退避・書き換えする
} LispSymbol;

typedef LispObject (*LispBuiltinFn)(LispObject args);

typedef struct {
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

static inline int lisp_is_number(LispObject obj) {
    return lisp_is_fixnum(obj) || lisp_is_float(obj) || lisp_is_bignum(obj);
}

static UINT64 lisp_heap_ptr;
static UINT64 lisp_heap_end;
EFI_SYSTEM_TABLE *g_system_table; // panic時にConOutへ出力するため
EFI_HANDLE g_image_handle; // milestone 16: loadがHandleProtocolでファイルシステムを取得するため

// エラーメッセージを出力して停止する。呼び出し元には戻らない
void lisp_panic(CHAR16 *message) {
    g_system_table->ConOut->OutputString(g_system_table->ConOut, L"Lisp panic: ");
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

void lisp_heap_init(UINT64 start, UINT64 size) {
    lisp_heap_ptr = (start + 15) & ~15ULL; // 16byte境界に切り上げ（タグ用に下位ビットを空ける）
    lisp_heap_end = start + size;
}

// ヒープからsizeバイトを切り出す（解放・GCなしのバンプアロケータ）。
// 戻り値は常に16byte境界なので、下位2bitをそのままタグとして使える
void *lisp_alloc(UINTN size) {
    UINT64 aligned_size = (size + 15) & ~15ULL;
    if (lisp_heap_ptr + aligned_size > lisp_heap_end) {
        lisp_panic(L"heap exhausted");
    }
    void *ptr = (void *)lisp_heap_ptr;
    lisp_heap_ptr += aligned_size;
    return ptr;
}

LispObject alloc_cons(void) {
    return (LispObject)lisp_alloc(sizeof(LispCons)); // 下位2bit=00=LISP_TAG_CONS、タグ付け不要
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
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

LispObject lisp_make_builtin(LispBuiltinFn fn) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// defmacro由来のマクロを作る。lambda由来のクロージャと同じ構造だが、
// 呼び出し時にlisp_evalが引数を評価せず展開処理に回すためis_macroを立てる
LispObject lisp_make_macro(LispObject params, LispObject body, LispObject env) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// 文字列オブジェクトを作る（milestone 15）。charsのlen文字分をヒープに
// コピーして0終端したバッファをstr_dataに持たせる（呼び出し元のバッファを
// 保持し続けるわけではないので、charsはこの呼び出し中だけ有効なスタック上の
// 一時バッファ等で構わない）
LispObject lisp_make_string(const char *chars, UINTN len) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

#define LISP_BIGNUM_MAX_LIMBS 64

// floatオブジェクトを作る（milestone 22）。LISP_TAG_CLOSUREのescape hatchを再利用する
LispObject lisp_make_float(double value) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

// bignumオブジェクトを作る（milestone 22）。digitsはlen個の有効桁（先頭ゼロ桁は
// trim済みでlen>=1であること）をヒープにコピーする。fixnum範囲に収まるかどうかの
// 判定は行わない生のコンストラクタなので、通常はlisp_make_number_from_magnitude経由で
// 呼ぶこと（正規化を保証するため）
LispObject lisp_make_bignum(const UINT32 *digits, UINTN len, int negative) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
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

#define LISP_MAX_SYMBOLS 256
static LispObject lisp_symbol_table[LISP_MAX_SYMBOLS];
static UINTN lisp_symbol_count = 0;

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

// 同じ名前でintern済みのシンボルがあれば同一のLispObjectを返す（eqで比較可能にする）。
// 無ければ新規に確保してテーブルに登録する。比較・格納の両方で先にLISP_SYMBOL_NAME_MAX-1文字に
// 切り詰めてから扱うことで、name自体を切り詰めずにlisp_streqへ渡すと「格納済みの切り詰め済み名」
// と「今回渡された切り詰め前のnama」が食い違って毎回別シンボルを生成してしまう
// （呼ぶたびにeqが成立せずunbound variableになる）バグを避ける
LispObject lisp_intern(const char *name) {
    char truncated[LISP_SYMBOL_NAME_MAX];
    UINTN len = 0;
    while (name[len] != '\0' && len < LISP_SYMBOL_NAME_MAX - 1) {
        truncated[len] = name[len];
        len++;
    }
    truncated[len] = '\0';

    for (UINTN i = 0; i < lisp_symbol_count; i++) {
        LispSymbol *sym = lisp_symbol_cell(lisp_symbol_table[i]);
        if (lisp_streq(sym->name, truncated)) {
            return lisp_symbol_table[i];
        }
    }

    if (lisp_symbol_count >= LISP_MAX_SYMBOLS) {
        lisp_panic(L"symbol table exhausted");
    }

    LispSymbol *sym = (LispSymbol *)lisp_alloc(sizeof(LispSymbol));
    UINTN i = 0;
    while (truncated[i] != '\0') {
        sym->name[i] = truncated[i];
        i++;
    }
    sym->name[i] = '\0';
    sym->is_special = 0;
    sym->value = LISP_NIL;

    LispObject obj = ((LispObject)sym) | LISP_TAG_SYMBOL;
    lisp_symbol_table[lisp_symbol_count] = obj;
    lisp_symbol_count++;
    return obj;
}

// gensym専用 (milestone 20): lisp_symbol_tableに登録せず新規のLispSymbolを確保するだけの
// シンボルを作る。eqはオブジェクトの同一性そのものであり、lisp_internの一致判定は
// テーブルに載っているシンボルしか見つけられないため、テーブルに載せないことで
// 「名前が何であっても、reader/internを経由する限り絶対にeqにならない」ユニークな
// シンボルになる
static LispObject lisp_make_uninterned_symbol(const char *name) {
    LispSymbol *sym = (LispSymbol *)lisp_alloc(sizeof(LispSymbol));
    UINTN i = 0;
    while (name[i] != '\0' && i < LISP_SYMBOL_NAME_MAX - 1) {
        sym->name[i] = name[i];
        i++;
    }
    sym->name[i] = '\0';
    sym->is_special = 0;
    sym->value = LISP_NIL;
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

// ASCII文字列(8bit char)をCHAR16に変換してコンソールへ出力する
void lisp_print_ascii(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    CHAR16 buf[LISP_INPUT_BUFFER_MAX];
    UINTN i = 0;
    while (str[i] != '\0' && i < LISP_INPUT_BUFFER_MAX - 1) {
        buf[i] = (CHAR16)str[i];
        i++;
    }
    buf[i] = 0;
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buf);
}


// --- プリンター (milestone 7) ---

// 10進数を表示する（符号付き。libcのitoa相当が無いため自前実装）
void lisp_print_fixnum(EFI_SYSTEM_TABLE *SystemTable, long long value) {
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
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"-");
    }

    // digitsには下位桁から積んでいるので、後ろから出力する
    while (i > 0) {
        i--;
        CHAR16 ch[2] = { (CHAR16)digits[i], 0 };
        SystemTable->ConOut->OutputString(SystemTable->ConOut, ch);
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
void lisp_print_bignum(EFI_SYSTEM_TABLE *SystemTable, LispClosure *big) {
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
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"-");
    }
    while (dcount > 0) {
        dcount--;
        CHAR16 ch[2] = { (CHAR16)decimal[dcount], 0 };
        SystemTable->ConOut->OutputString(SystemTable->ConOut, ch);
    }
}

// floatを固定小数点形式で表示する（指数表記は扱わない簡略化）。整数部はlisp_print_fixnumを
// 再利用し、小数部は10進6桁を手計算で求め、末尾の'0'は1桁残るまでtrimする
// （libcのsprintf/%f相当が無いため自前実装。値がlong longで表せない極端な大きさのfloatは
// 想定していない）
void lisp_print_float(EFI_SYSTEM_TABLE *SystemTable, double value) {
    int negative = value < 0.0;
    double v = negative ? -value : value;
    long long int_part = (long long)v;
    double frac = v - (double)int_part;

    if (negative) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"-");
    }
    lisp_print_fixnum(SystemTable, int_part);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L".");

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
        CHAR16 ch[2] = { (CHAR16)frac_digits[i], 0 };
        SystemTable->ConOut->OutputString(SystemTable->ConOut, ch);
    }
}

// LispObjectを人間が読める形式でコンソールに表示する。
// fixnumは10進、symbolは名前、consは(a b c)または(a . b)形式、nilはnilと表示する
void lisp_print(EFI_SYSTEM_TABLE *SystemTable, LispObject obj) {
    if (obj == LISP_NIL) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"nil");
        return;
    }

    if (lisp_is_fixnum(obj)) {
        lisp_print_fixnum(SystemTable, lisp_fixnum_value(obj));
        return;
    }

    if (lisp_is_float(obj)) {
        lisp_print_float(SystemTable, lisp_closure_cell(obj)->float_value);
        return;
    }

    if (lisp_is_bignum(obj)) {
        lisp_print_bignum(SystemTable, lisp_closure_cell(obj));
        return;
    }

    if (lisp_is_symbol(obj)) {
        lisp_print_ascii(SystemTable, lisp_symbol_cell(obj)->name);
        return;
    }

    if (lisp_is_cons(obj)) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"(");

        LispObject cur = obj;
        int first = 1;
        while (lisp_is_cons(cur)) {
            if (!first) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L" ");
            }
            first = 0;
            lisp_print(SystemTable, lisp_car(cur));
            cur = lisp_cdr(cur);
        }

        if (cur != LISP_NIL) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L" . ");
            lisp_print(SystemTable, cur);
        }

        SystemTable->ConOut->OutputString(SystemTable->ConOut, L")");
        return;
    }

    if (lisp_is_closure(obj)) {
        if (lisp_closure_cell(obj)->str_data != 0) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\"");
            lisp_print_ascii(SystemTable, lisp_closure_cell(obj)->str_data);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\"");
        } else if (lisp_closure_cell(obj)->builtin != 0) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"#<builtin>");
        } else if (lisp_closure_cell(obj)->is_macro) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"#<macro>");
        } else {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"#<closure>");
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
        if (expr == lisp_sym_t) {
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
static LispObject lisp_load_eval_buffer(const char *buf) {
    lisp_reader_pos = buf;
    LispObject result = lisp_sym_t;
    while (!lisp_reader_at_end()) {
        LispObject form = lisp_read();
        result = lisp_eval_toplevel(form);
    }
    return result;
}

static EFI_GUID lisp_guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID_VALUE;
static EFI_GUID lisp_guid_simple_file_system = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID_VALUE;

#define LISP_LOAD_BUFFER_MAX 8192
static char lisp_load_buffer[LISP_LOAD_BUFFER_MAX];

// (load "filename"): EfiMainのImageHandle→LoadedImage→DeviceHandle→
// SimpleFileSystemの順にHandleProtocolでたどり、ESPのルートディレクトリから
// filenameを読み込んで内容を評価する。GetInfoでファイルサイズを問い合わせず、
// 1回のReadで静的バッファに収まる分だけ読み取る（CLAUDE.mdの静的バッファ方針に従う）
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

    EFI_BOOT_SERVICES *bs = g_system_table->BootServices;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    if (bs->HandleProtocol(g_image_handle, &lisp_guid_loaded_image, (void **)&loaded_image) != 0) {
        lisp_panic(L"load: failed to get loaded image protocol");
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    if (bs->HandleProtocol(loaded_image->DeviceHandle, &lisp_guid_simple_file_system, (void **)&fs) != 0) {
        lisp_panic(L"load: failed to get simple file system protocol");
    }

    EFI_FILE_PROTOCOL *root;
    if (fs->OpenVolume(fs, &root) != 0) {
        lisp_panic(L"load: failed to open volume");
    }

    EFI_FILE_PROTOCOL *file;
    if (root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0) != 0) {
        lisp_panic(L"load: failed to open file");
    }

    UINTN size = LISP_LOAD_BUFFER_MAX - 1;
    if (file->Read(file, &size, lisp_load_buffer) != 0) {
        lisp_panic(L"load: failed to read file");
    }
    lisp_load_buffer[size] = '\0';
    file->Close(file);

    return lisp_load_eval_buffer(lisp_load_buffer);
}

// car/cdr/cons/eq/atom/+/-/load をグローバル環境に束縛して返す
LispObject lisp_builtins_init(void) {
    LispObject env = LISP_NIL;
    env = lisp_env_extend(env, lisp_intern("car"), lisp_make_builtin(lisp_builtin_car));
    env = lisp_env_extend(env, lisp_intern("cdr"), lisp_make_builtin(lisp_builtin_cdr));
    env = lisp_env_extend(env, lisp_intern("cons"), lisp_make_builtin(lisp_builtin_cons));
    env = lisp_env_extend(env, lisp_intern("eq"), lisp_make_builtin(lisp_builtin_eq));
    env = lisp_env_extend(env, lisp_intern("atom"), lisp_make_builtin(lisp_builtin_atom));
    env = lisp_env_extend(env, lisp_intern("+"), lisp_make_builtin(lisp_builtin_add));
    env = lisp_env_extend(env, lisp_intern("-"), lisp_make_builtin(lisp_builtin_sub));
    env = lisp_env_extend(env, lisp_intern("load"), lisp_make_builtin(lisp_builtin_load));
    env = lisp_env_extend(env, lisp_intern("gensym"), lisp_make_builtin(lisp_builtin_gensym));
    env = lisp_env_extend(env, lisp_intern("macroexpand-1"), lisp_make_builtin(lisp_builtin_macroexpand_1));

    // *macroexpand-hook*をdefvarと同じ形（is_special=1 + 初期値）で直接セットアップする
    // (milestone 21)。動的変数はenvチェーンに束縛を積まないため、global_envへの
    // lisp_env_extendは不要
    LispSymbol *hook_cell = lisp_symbol_cell(lisp_sym_macroexpand_hook);
    hook_cell->value = lisp_make_builtin(lisp_default_macroexpand_hook);
    hook_cell->is_special = 1;

    return env;
}
