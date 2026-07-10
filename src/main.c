#define EFIAPI __attribute__((ms_abi))

typedef unsigned short CHAR16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef long long EFI_STATUS;
typedef void *EFI_HANDLE;

// UEFI共通ヘッダー（24バイト）
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_BOOT_SERVICES;

// --- 64bit仕様に完全準拠したシステムテーブル ---
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    UINT32 __pad; // 64bitアライメント用
    
    EFI_HANDLE ConsoleInHandle;
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    
    EFI_HANDLE ConsoleOutHandle;
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut; // 72バイト目

    void *StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    struct _EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;


typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;


#define EfiConventionalMemory 7
#define EfiBootServicesData 4

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    UINT32 PoolType,
    UINTN Size,
    void **Buffer
);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    void *AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
} EFI_BOOT_SERVICES;


char memory_map_buffer[1024 * 256];

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    unsigned short ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    void *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;


void UINT64ToHexStr(UINT64 val, CHAR16 *str) {
    char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        str[i + 2] = hex[val & 0xF];
        val >>= 4;
    }
    str[0] = L'0';
    str[1] = L'x';
    str[18] = L'\0';
}


// --- Lisp Object System ---
typedef UINT64 LispObject;

#define LISP_TAG_MASK    0x3ULL
#define LISP_TAG_CONS    0x0ULL   // ポインタ（cons cellへの16byte境界アドレス、下位2bit=00）
#define LISP_TAG_FIXNUM  0x1ULL   // 整数（上位62bitに値、下位2bit=01）
#define LISP_TAG_SYMBOL  0x2ULL   // ポインタ（LispSymbolへの16byte境界アドレス、下位2bit=10）
#define LISP_TAG_CLOSURE 0x3ULL   // ポインタ（LispClosureへの16byte境界アドレス、下位2bit=11）

// 予約アドレス0。ヒープはPhysicalStart==0を使わないため衝突しない。
// nilはシンボルとしてintern済みテーブルに載せず、この専用の即値のまま扱う
// （空リストの終端とfalse相当を1つの値で表す、という一般的なLisp実装の簡略化）
#define LISP_NIL ((LispObject)0)

#define LISP_SYMBOL_NAME_MAX 32

typedef struct {
    LispObject car;
    LispObject cdr;
} LispCons;

typedef struct {
    char name[LISP_SYMBOL_NAME_MAX];
} LispSymbol;

typedef LispObject (*LispBuiltinFn)(LispObject args);

typedef struct {
    LispObject params; // 仮引数シンボルのリスト（builtinの場合は未使用）
    LispObject body;   // 本文（単一式、builtinの場合は未使用）
    LispObject env;    // 生成時の環境（レキシカルスコープ用、builtinの場合は未使用）
    LispBuiltinFn builtin; // NULLならlambda由来、非NULLならC実装の組み込み関数
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

static UINT64 lisp_heap_ptr;
static UINT64 lisp_heap_end;
static EFI_SYSTEM_TABLE *g_system_table; // panic時にConOutへ出力するため

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
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
}

LispObject lisp_make_builtin(LispBuiltinFn fn) {
    LispClosure *closure = (LispClosure *)lisp_alloc(sizeof(LispClosure));
    closure->params = LISP_NIL;
    closure->body = LISP_NIL;
    closure->env = LISP_NIL;
    closure->builtin = fn;
    return ((LispObject)closure) | LISP_TAG_CLOSURE;
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
// 無ければ新規に確保してテーブルに登録する
LispObject lisp_intern(const char *name) {
    for (UINTN i = 0; i < lisp_symbol_count; i++) {
        LispSymbol *sym = lisp_symbol_cell(lisp_symbol_table[i]);
        if (lisp_streq(sym->name, name)) {
            return lisp_symbol_table[i];
        }
    }

    if (lisp_symbol_count >= LISP_MAX_SYMBOLS) {
        lisp_panic(L"symbol table exhausted");
    }

    LispSymbol *sym = (LispSymbol *)lisp_alloc(sizeof(LispSymbol));
    UINTN i = 0;
    while (name[i] != '\0' && i < LISP_SYMBOL_NAME_MAX - 1) {
        sym->name[i] = name[i];
        i++;
    }
    sym->name[i] = '\0';

    LispObject obj = ((LispObject)sym) | LISP_TAG_SYMBOL;
    lisp_symbol_table[lisp_symbol_count] = obj;
    lisp_symbol_count++;
    return obj;
}

// よく使う特別なシンボル（nilはLISP_NILの即値のまま。それ以外はここでintern）
static LispObject lisp_sym_t;
static LispObject lisp_sym_quote;
static LispObject lisp_sym_if;
static LispObject lisp_sym_lambda;

void lisp_symbols_init(void) {
    lisp_sym_t = lisp_intern("t");
    lisp_sym_quote = lisp_intern("quote");
    lisp_sym_if = lisp_intern("if");
    lisp_sym_lambda = lisp_intern("lambda");
}


// --- 文字入力 (milestone 6) ---
#define LISP_INPUT_BUFFER_MAX 256
static char input_buffer[LISP_INPUT_BUFFER_MAX];
static UINTN input_length;

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
        if (lisp_closure_cell(obj)->builtin != 0) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"#<builtin>");
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
    return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')';
}

static void lisp_reader_skip_ws(void) {
    while (*lisp_reader_pos == ' ' || *lisp_reader_pos == '\t' ||
           *lisp_reader_pos == '\r' || *lisp_reader_pos == '\n') {
        lisp_reader_pos++;
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

static long long lisp_token_to_fixnum(const char *token) {
    int negative = token[0] == '-';
    UINTN i = negative ? 1 : 0;
    long long value = 0;
    for (; token[i] != '\0'; i++) {
        value = value * 10 + (token[i] - '0');
    }
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
// "("なら再帰的にリストを読み取り、それ以外は整数リテラルまたはシンボルのトークンとして読む
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
        return lisp_make_fixnum(lisp_token_to_fixnum(token));
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


// --- 評価器 (milestone 9) ---

// トップレベルの永続グローバル環境 (milestone 12)。EfiMainのREPLループが起動時に
// lisp_builtins_init()の結果で初期化する。EfiMainのローカル変数ではなくファイルスコープの
// static変数にすることで、defun/loadなど今後の特殊形式がここを直接書き換えて新しい束縛を
// 追加すれば、その後のすべてのREPL入力から見えるようになる（引数↔値のバインディング自体は
// マイルストーン9のlisp_env_bind_paramsのままで変更しない）
static LispObject global_env = LISP_NIL;

// symをvalueに束縛したペアをenvの先頭に追加した新しい環境を返す
LispObject lisp_env_extend(LispObject env, LispObject sym, LispObject value) {
    return lisp_cons(lisp_cons(sym, value), env);
}

// alist envをsymで線形探索し、見つかった値を返す。無ければlisp_panic
LispObject lisp_env_lookup(LispObject env, LispObject sym) {
    LispObject cur = env;
    while (lisp_is_cons(cur)) {
        LispObject pair = lisp_car(cur);
        if (lisp_car(pair) == sym) {
            return lisp_cdr(pair);
        }
        cur = lisp_cdr(cur);
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
    LispObject tail = lisp_eval_list(lisp_cdr(list), env);
    return lisp_cons(head, tail);
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

// exprをenv上で評価する。fixnum/nil/tは自己評価、symbolは変数参照、
// consはquote/if/lambdaの特殊形式または関数呼び出しとして扱う
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

        if (op == lisp_sym_if) {
            LispObject test = lisp_eval(lisp_car(lisp_cdr(expr)), env);
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

        if (op == lisp_sym_lambda) {
            LispObject params = lisp_car(lisp_cdr(expr));
            LispObject body = lisp_car(lisp_cdr(lisp_cdr(expr)));
            return lisp_make_closure(params, body, env);
        }

        LispObject fn = lisp_eval(op, env);
        LispObject args = lisp_eval_list(lisp_cdr(expr), env);
        return lisp_apply(fn, args);
    }

    if (lisp_is_closure(expr)) {
        return expr;
    }

    lisp_panic(L"cannot evaluate this object");
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

LispObject lisp_builtin_add(LispObject args) {
    long long sum = 0;
    LispObject cur = args;
    while (lisp_is_cons(cur)) {
        LispObject v = lisp_car(cur);
        if (!lisp_is_fixnum(v)) {
            lisp_panic(L"+ expects fixnum arguments");
        }
        sum += lisp_fixnum_value(v);
        cur = lisp_cdr(cur);
    }
    return lisp_make_fixnum(sum);
}

LispObject lisp_builtin_sub(LispObject args) {
    if (!lisp_is_cons(args)) {
        lisp_panic(L"- requires at least 1 argument");
    }
    LispObject first = lisp_car(args);
    if (!lisp_is_fixnum(first)) {
        lisp_panic(L"- expects fixnum arguments");
    }
    long long result = lisp_fixnum_value(first);
    LispObject cur = lisp_cdr(args);
    if (!lisp_is_cons(cur)) {
        return lisp_make_fixnum(-result); // 単項: 符号反転
    }
    while (lisp_is_cons(cur)) {
        LispObject v = lisp_car(cur);
        if (!lisp_is_fixnum(v)) {
            lisp_panic(L"- expects fixnum arguments");
        }
        result -= lisp_fixnum_value(v);
        cur = lisp_cdr(cur);
    }
    return lisp_make_fixnum(result);
}

// car/cdr/cons/eq/atom/+/- をグローバル環境に束縛して返す
LispObject lisp_builtins_init(void) {
    LispObject env = LISP_NIL;
    env = lisp_env_extend(env, lisp_intern("car"), lisp_make_builtin(lisp_builtin_car));
    env = lisp_env_extend(env, lisp_intern("cdr"), lisp_make_builtin(lisp_builtin_cdr));
    env = lisp_env_extend(env, lisp_intern("cons"), lisp_make_builtin(lisp_builtin_cons));
    env = lisp_env_extend(env, lisp_intern("eq"), lisp_make_builtin(lisp_builtin_eq));
    env = lisp_env_extend(env, lisp_intern("atom"), lisp_make_builtin(lisp_builtin_atom));
    env = lisp_env_extend(env, lisp_intern("+"), lisp_make_builtin(lisp_builtin_add));
    env = lisp_env_extend(env, lisp_intern("-"), lisp_make_builtin(lisp_builtin_sub));
    return env;
}


// --- エントリポイント ---
EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_system_table = SystemTable;

    // clear screen
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Hello World!\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Memory cheking....\r\n");


    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = (void *)0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    EFI_STATUS status;

    // 1回目: 必要なバッファサイズを問い合わせる（EFI_BUFFER_TOO_SMALLが返る想定）
    status = SystemTable->BootServices->GetMemoryMap(
        &memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version
    );

    // 2回の呼び出しの間にメモリマップが変化する可能性があるため、少し余裕を持たせる
    memory_map_size += descriptor_size * 4;

    if (memory_map_size > sizeof(memory_map_buffer)) {
        CHAR16 hex_needed[20];
        UINT64ToHexStr((UINT64)memory_map_size, hex_needed);

        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Memory map buffer too small. Needed Size: ");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_needed);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    } else {
        memory_map = (EFI_MEMORY_DESCRIPTOR *)memory_map_buffer;
        memory_map_size = sizeof(memory_map_buffer);

        status = SystemTable->BootServices->GetMemoryMap(
            &memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version
        );

        if (status == 0) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Success to get Memory Map!\r\n");

            UINTN entries = memory_map_size / descriptor_size;
            UINT64 max_free_size = 0;
            UINT64 heap_start = 0;

            for (UINTN i = 0; i < entries; i++) {
                EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((char *)memory_map + (i * descriptor_size));

                // Type が EfiConventionalMEmory のものがフリーな領域
                if (desc->Type == EfiConventionalMemory) {
                    UINT64 size = desc->NumberOfPages * 4096;
                    if (size > max_free_size) {
                        max_free_size = size;
                        heap_start = desc->PhysicalStart;
                    }
                }
            }

            CHAR16 hex_addr[20];
            CHAR16 hex_size[20];
            UINT64ToHexStr(heap_start, hex_addr);
            UINT64ToHexStr(max_free_size, hex_size);

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"OS Heap Target Address: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_addr);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Available Heap Size: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_size);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            lisp_heap_init(heap_start, max_free_size);

            lisp_symbols_init();
            global_env = lisp_builtins_init();

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\nMinimal Lisp REPL. Type an expression and press Enter.\r\n");

            for (;;) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L"> ");
                lisp_read_line(SystemTable);
                if (input_length == 0) {
                    continue;
                }
                LispObject expr = lisp_read_from_buffer(input_buffer);
                LispObject result = lisp_eval(expr, global_env);
                lisp_print(SystemTable, result);
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
            }
        } else {
            CHAR16 hex_status[20];
            CHAR16 hex_needed[20];
            UINT64ToHexStr((UINT64)status, hex_status);
            UINT64ToHexStr((UINT64)memory_map_size, hex_needed);

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Failed to get Memory Map. Status: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_status);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L" / Needed Size: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_needed);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
        }
    }
    
    for (;;) {
    }

    return 0;
}
