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

#define LISP_TAG_MASK   0x3ULL
#define LISP_TAG_CONS   0x0ULL   // ポインタ（cons cellへの16byte境界アドレス、下位2bit=00）
#define LISP_TAG_FIXNUM 0x1ULL   // 整数（上位62bitに値、下位2bit=01）
#define LISP_TAG_SYMBOL 0x2ULL   // ポインタ（LispSymbolへの16byte境界アドレス、下位2bit=10）

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

static inline LispCons *lisp_cons_cell(LispObject obj) {
    return (LispCons *)(obj & ~LISP_TAG_MASK);
}

static inline LispSymbol *lisp_symbol_cell(LispObject obj) {
    return (LispSymbol *)(obj & ~LISP_TAG_MASK);
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

// よく使う特別なシンボル（nilはLISP_NILの即値のまま。tのみここでintern）
static LispObject lisp_sym_t;

void lisp_symbols_init(void) {
    lisp_sym_t = lisp_intern("t");
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

            LispObject test = lisp_cons(lisp_make_fixnum(42), LISP_NIL);

            CHAR16 hex_obj[20];
            CHAR16 hex_car[20];
            UINT64ToHexStr((UINT64)test, hex_obj);
            UINT64ToHexStr((UINT64)lisp_fixnum_value(lisp_car(test)), hex_car);

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Test cons cell: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_obj);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"car decoded as fixnum: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_car);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            lisp_symbols_init();

            LispObject sym_foo1 = lisp_intern("foo");
            LispObject sym_foo2 = lisp_intern("foo");
            LispObject sym_bar = lisp_intern("bar");

            CHAR16 hex_same[20];
            CHAR16 hex_diff[20];
            CHAR16 hex_is_sym[20];
            UINT64ToHexStr((UINT64)(sym_foo1 == sym_foo2), hex_same);
            UINT64ToHexStr((UINT64)(sym_foo1 == sym_bar), hex_diff);
            UINT64ToHexStr((UINT64)lisp_is_symbol(lisp_sym_t), hex_is_sym);

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"intern(foo) == intern(foo): ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_same);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"intern(foo) == intern(bar): ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_diff);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"lisp_sym_t is_symbol: ");
            SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_is_sym);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Type a line and press Enter: ");
            lisp_read_line(SystemTable);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"You typed: ");
            lisp_print_ascii(SystemTable, input_buffer);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
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
