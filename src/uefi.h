#ifndef OS_BOOT_DEV_UEFI_H
#define OS_BOOT_DEV_UEFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned short CHAR16;
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef int INT32;
typedef unsigned char BOOLEAN;
typedef long long EFI_STATUS;
typedef void *EFI_HANDLE;

// UEFIプロトコルを一意に識別するGUID（milestone 16でHandleProtocolに渡すため必要）
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

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
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct _EFI_FILE_PROTOCOL;

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


#define EfiLoaderData 2
#define EfiConventionalMemory 7
#define EfiBootServicesData 4

typedef UINT64 EFI_PHYSICAL_ADDRESS;

// milestone 88: EfiMain起動直後に自前の大きいCスタック領域を確保するために必要
#define AllocateAnyPages 0

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    UINTN Type,
    UINT32 MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

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

// milestone 16: LoadedImage/SimpleFileSystemプロトコルを取得するために必要
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface
);

// milestone 116: EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOLをハンドル指定無しで取得するために必要
// （HandleProtocolはEfiMainのImageHandle経由で辿れるハンドルにしか使えないが、
// コンソール入力の拡張プロトコルはConsoleInHandle以外にインストールされている場合もある
// ため、システム全体から検索するLocateProtocolが必要）
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    void *Registration,
    void **Interface
);

// milestone 25: sleep相当のLisp関数実装のために必要
typedef void *EFI_EVENT;
typedef UINTN EFI_TPL;

typedef enum {
    TimerCancel,
    TimerPeriodic,
    TimerRelative
} EFI_TIMER_DELAY;

#define EVT_TIMER 0x80000000
#define TPL_APPLICATION 4

typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, void *Context);

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(
    UINT32 Type,
    EFI_TPL NotifyTpl,
    EFI_EVENT_NOTIFY NotifyFunction,
    void *NotifyContext,
    EFI_EVENT *Event
);
typedef EFI_STATUS (EFIAPI *EFI_SET_TIMER)(
    EFI_EVENT Event,
    EFI_TIMER_DELAY Type,
    UINT64 TriggerTime
);
typedef EFI_STATUS (EFIAPI *EFI_WAIT_FOR_EVENT)(
    UINTN NumberOfEvents,
    EFI_EVENT *Event,
    UINTN *Index
);
typedef EFI_STATUS (EFIAPI *EFI_CLOSE_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void *AllocatePool;
    void *FreePool;

    EFI_CREATE_EVENT CreateEvent;
    EFI_SET_TIMER SetTimer;
    EFI_WAIT_FOR_EVENT WaitForEvent;
    void *SignalEvent;
    EFI_CLOSE_EVENT CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;

    // milestone 116: UEFI仕様上HandleProtocolとLocateProtocolの間に並ぶ20フィールド分の
    // オフセットを合わせるためのプレースホルダ（このバイナリレイアウトはUEFI仕様の
    // vtable順序と一致させる必要があるため、使わないフィールドでも省略できない。
    // uefi.h冒頭のコメント参照）
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;

    EFI_LOCATE_PROTOCOL LocateProtocol;
} EFI_BOOT_SERVICES;


typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);

// UEFI仕様のEFI_SIMPLE_TEXT_OUTPUT_MODE(SetCursorPosition/QueryModeが参照する現在のモード情報)
typedef struct {
    INT32 MaxMode;
    INT32 Mode;
    INT32 Attribute;
    INT32 CursorColumn;
    INT32 CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    EFI_TEXT_QUERY_MODE QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR EnableCursor;
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
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

// --- milestone 116: Ctrl単体押下検知のためのEFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL ---
//
// 既存のEFI_SIMPLE_TEXT_INPUT_PROTOCOL（ReadKeyStroke）はScanCode/UnicodeCharの組しか
// 報告できず、修飾キー単体の押下状態を一切表現できない（事前調査で確認済みの制約、
// documents/lisp_os_process.md参照）。ReadKeyStrokeExが返すEFI_KEY_DATA.KeyStateの
// KeyShiftStateには、そのキーストローク発生時点で押されている修飾キーの集合が
// ビットフラグで入るため、修飾キー単体の押下（Key.ScanCode/UnicodeCharがいずれも0で、
// KeyShiftStateにCtrlビットが立っている）を判別できる

typedef unsigned char EFI_KEY_TOGGLE_STATE;

typedef struct {
    UINT32 KeyShiftState;
    EFI_KEY_TOGGLE_STATE KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY Key;
    EFI_KEY_STATE KeyState;
} EFI_KEY_DATA;

// KeyShiftStateの最上位ビットが立っていない場合、その値自体が無効
// （ファームウェアが修飾キー状態を報告できない）ことを意味する
#define EFI_SHIFT_STATE_VALID     0x80000000U
#define EFI_RIGHT_SHIFT_PRESSED   0x00000001U
#define EFI_LEFT_SHIFT_PRESSED    0x00000002U
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004U
#define EFI_LEFT_CONTROL_PRESSED  0x00000008U
#define EFI_RIGHT_ALT_PRESSED     0x00000010U
#define EFI_LEFT_ALT_PRESSED      0x00000020U
#define EFI_RIGHT_LOGO_PRESSED    0x00000040U
#define EFI_LEFT_LOGO_PRESSED     0x00000080U
#define EFI_MENU_KEY_PRESSED      0x00000100U
#define EFI_SYS_REQ_PRESSED       0x00000200U

struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY_EX)(
    struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
    EFI_KEY_DATA *KeyData
);

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    void *Reset;
    EFI_INPUT_READ_KEY_EX ReadKeyStrokeEx;
    EFI_EVENT WaitForKeyEx;
    void *SetState;
    void *RegisterKeyNotify;
    void *UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID_VALUE \
    {0xdd9e7534, 0x7762, 0x4698, {0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa}}


// --- milestone 16: FAT32のESPからファイルを読み込むためのプロトコル ---

// EfiMainのImageHandleにHandleProtocolで問い合わせ、DeviceHandleから
// EFI_SIMPLE_FILE_SYSTEM_PROTOCOLを取得するための起点
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID_VALUE \
    {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    struct _EFI_FILE_PROTOCOL *This,
    struct _EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    UINT64 OpenMode,
    UINT64 Attributes
);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct _EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct _EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(struct _EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);

typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void *Delete;
    EFI_FILE_READ Read;
    EFI_FILE_WRITE Write;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME)(
    struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root
);

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID_VALUE \
    {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

static inline void UINT64ToHexStr(UINT64 val, CHAR16 *str) {
    char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        str[i + 2] = hex[val & 0xF];
        val >>= 4;
    }
    str[0] = L'0';
    str[1] = L'x';
    str[18] = L'\0';
}

#endif // OS_BOOT_DEV_UEFI_H
