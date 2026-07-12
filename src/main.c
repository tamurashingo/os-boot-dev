#include "uefi.h"
#include "lisp.h"

char memory_map_buffer[1024 * 256];

// --- setjmp/longjmp自己テスト (milestone 30) ---
// 深いC呼び出しスタックの奥底からlisp_longjmpで一気に脱出できることを確認する。
// 各段でローカル配列を消費し、単純な同一フレーム内ジャンプでは検出できない
// スタック破壊が無いことも合わせて確認する
static void lisp_setjmp_selftest_level3(lisp_jmp_buf *buf) {
    volatile char padding[64];
    for (UINTN i = 0; i < sizeof(padding); i++) {
        padding[i] = (char)i;
    }
    lisp_longjmp(buf, 1);
}

static void lisp_setjmp_selftest_level2(lisp_jmp_buf *buf) {
    volatile char padding[64];
    for (UINTN i = 0; i < sizeof(padding); i++) {
        padding[i] = (char)(i * 2);
    }
    lisp_setjmp_selftest_level3(buf);
}

static void lisp_setjmp_selftest_level1(lisp_jmp_buf *buf) {
    volatile char padding[64];
    for (UINTN i = 0; i < sizeof(padding); i++) {
        padding[i] = (char)(i * 3);
    }
    lisp_setjmp_selftest_level2(buf);
}

// --- VMデータスタックGCルート自己テスト (milestone 34) ---
static void lisp_vm_gc_root_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_vm_gc_root_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM stack GC root self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM stack GC root self-test: FAIL\r\n");
        for (;;) {}
    }
}

static void lisp_setjmp_selftest(EFI_SYSTEM_TABLE *SystemTable) {
    lisp_jmp_buf buf;
    volatile UINT64 marker = 0xDEADBEEFCAFEULL;
    int rv = lisp_setjmp(&buf);

    if (rv == 0) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"setjmp/longjmp self-test: 1st pass (rv=0)\r\n");
        lisp_setjmp_selftest_level1(&buf);
        // lisp_longjmpは戻らないはずなので、ここに到達したら失敗
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"setjmp/longjmp self-test: FAIL (unreachable code reached)\r\n");
        for (;;) {}
    }

    if (rv == 1 && marker == 0xDEADBEEFCAFEULL) {
        CHAR16 hex_marker[20];
        UINT64ToHexStr((UINT64)marker, hex_marker);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"setjmp/longjmp self-test: 2nd pass OK (rv=1, marker=");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_marker);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L")\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"setjmp/longjmp self-test: FAIL (corrupted state)\r\n");
        for (;;) {}
    }
}

// --- エントリポイント ---
EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_system_table = SystemTable;
    g_image_handle = ImageHandle;

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

            lisp_packages_init();
            lisp_symbols_init();
            global_env = lisp_builtins_init();
            lisp_load_boot_file("stdlib.lisp"); // milestone 29: 標準ライブラリを起動時に読み込む
            lisp_setjmp_selftest(SystemTable); // milestone 30: setjmp/longjmp自己テスト
            lisp_vm_gc_root_selftest_run(SystemTable); // milestone 34: VMデータスタックGCルート自己テスト

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\nMinimal Lisp REPL. Type an expression and press Enter.\r\n");

            LispOutputStream console_stream = lisp_make_console_stream(SystemTable);

            lisp_jmp_buf repl_trap; // milestone 31: panic発生時にここへ復帰する
            lisp_active_trap = &repl_trap;

            for (;;) {
                lisp_setjmp(&repl_trap); // 戻り値は使わない: 通常経路とpanic復帰経路が
                                          // 完全に同じ地点（プロンプト表示直前）に合流するため
                if (lisp_heap_low()) { // milestone 33: ヒープが少ない時だけGCを起動する。
                                        // 評価中の中間値はCスタックからしか追跡できないため、
                                        // ここ（評価と評価の合間）以外では呼ばない
                    lisp_gc();
                }
                SystemTable->ConOut->OutputString(SystemTable->ConOut, L"> ");
                lisp_read_line(SystemTable);
                if (input_length == 0) {
                    continue;
                }
                LispObject expr = lisp_read_from_buffer(input_buffer);
                LispObject result = lisp_eval_toplevel(expr);
                lisp_print(&console_stream, result);
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
