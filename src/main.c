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

// --- VM最小実行ループ自己テスト (milestone 35) ---
// OP_CONST 1, OP_CONST 2, OP_ADD, OP_RETURN相当を手動でバイトコード配列として構築し、
// lisp_vm_execに渡して3が返ることを確認する。定数オブジェクトはlisp_make_fixnum相当の
// 内部APIを直接使わず、既に公開されているlisp_read_from_bufferを経由して作る
static void lisp_vm_arith_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char code[] = { OP_CONST, 0, OP_CONST, 1, OP_ADD, OP_RETURN };
    LispObject constants[2] = { lisp_read_from_buffer("1"), lisp_read_from_buffer("2") };
    LispObject fn = lisp_make_compiled(code, sizeof(code), constants, 2, 0);
    LispObject result = lisp_vm_exec(fn);

    if (result == lisp_read_from_buffer("3")) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM arithmetic self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM arithmetic self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VM制御フロー・ボックス化ローカル変数自己テスト (milestone 36) ---
// Lisp相当: (let ((x 10)) (if <test> (setq x 20) (setq x 30)) x)
// を手動バイトコードで構築し、testがnil/非nilそれぞれの場合に正しい分岐を通り、
// ローカル変数のボックス経由での読み書きも正しく行われることを確認する。
//   [0]  OP_CONST 0        ; 10をpush
//   [2]  OP_MAKE_LOCAL      ; local0 = box(10)
//   [3]  OP_CONST 1         ; testをpush
//   [5]  OP_JUMP_IF_FALSE 13 ; nilならelseへ
//   [7]  OP_CONST 2         ; 20をpush (then)
//   [9]  OP_STORE_LOCAL 0   ; local0 <- 20
//   [11] OP_JUMP 17          ; elseを飛び越す
//   [13] OP_CONST 3         ; 30をpush (else)
//   [15] OP_STORE_LOCAL 0   ; local0 <- 30
//   [17] OP_LOAD_LOCAL 0    ; local0を読む
//   [19] OP_RETURN
static void lisp_vm_control_flow_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char code[] = {
        OP_CONST, 0,
        OP_MAKE_LOCAL,
        OP_CONST, 1,
        OP_JUMP_IF_FALSE, 13,
        OP_CONST, 2,
        OP_STORE_LOCAL, 0,
        OP_JUMP, 17,
        OP_CONST, 3,
        OP_STORE_LOCAL, 0,
        OP_LOAD_LOCAL, 0,
        OP_RETURN
    };

    LispObject ten = lisp_read_from_buffer("10");
    LispObject twenty = lisp_read_from_buffer("20");
    LispObject thirty = lisp_read_from_buffer("30");
    LispObject nil = lisp_read_from_buffer("()");

    LispObject constants_false[4] = { ten, nil, twenty, thirty };
    LispObject fn_false = lisp_make_compiled(code, sizeof(code), constants_false, 4, 0);
    LispObject result_false = lisp_vm_exec(fn_false);

    LispObject constants_true[4] = { ten, lisp_read_from_buffer("1"), twenty, thirty };
    LispObject fn_true = lisp_make_compiled(code, sizeof(code), constants_true, 4, 0);
    LispObject result_true = lisp_vm_exec(fn_true);

    if (result_false == thirty && result_true == twenty) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM control flow self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM control flow self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VM関数呼び出し・スタックフレーム自己テスト (milestone 37) ---
// f(self, n)相当を手動バイトコードで構築する。fはまだ通常のシンボル束縛経由で自分自身を
// 参照できない（defun等既存evalとの統合は対象外）ため、自分自身を呼び出す代わりに
// 「自分自身を第1引数selfとして受け取り、再帰呼び出し時にはselfをそのまま次の呼び出しの
// 関数参照兼selfとして渡す」という自己適用（self-application）方式で真の再帰呼び出しを行う。
//   n（第2引数、local1）がnilなら基底部（0を返す）。
//   nilでなければ、self(=local0)を関数参照・self引数の両方として使い、nをnilに固定して
//   もう1段だけ再帰呼び出しし、戻り値に自分のn（local1、まだ元の値のまま）を加算して返す。
// バイトコードはどの再帰段でも同一（selfもnも実行時の引数として渡ってくる）ため、これは
// 見せかけではない本物の再帰呼び出しであり、2段のネストしたスタックフレーム構築・
// 引数ボックス化・戻り値伝播を検証する
//   [0]  OP_LOAD_LOCAL 1     ; n をpush
//   [2]  OP_JUMP_IF_FALSE 16 ; nilなら基底部へ
//   [4]  OP_LOAD_LOCAL 0     ; self をpush（次段呼び出しのself引数）
//   [6]  OP_CONST 0          ; nil をpush（次段呼び出しのn引数）
//   [8]  OP_LOAD_LOCAL 0     ; self をpush（呼び出す関数参照）
//   [10] OP_CALL 2           ; self(self, nil) を呼び出す
//   [12] OP_LOAD_LOCAL 1     ; 自分のn をpush
//   [14] OP_ADD               ; 再帰結果 + 自分のn
//   [15] OP_RETURN
//   [16] OP_CONST 1          ; 0 をpush（基底部）
//   [18] OP_RETURN
static void lisp_vm_call_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char f_code[] = {
        OP_LOAD_LOCAL, 1,
        OP_JUMP_IF_FALSE, 16,
        OP_LOAD_LOCAL, 0,
        OP_CONST, 0,
        OP_LOAD_LOCAL, 0,
        OP_CALL, 2,
        OP_LOAD_LOCAL, 1,
        OP_ADD,
        OP_RETURN,
        OP_CONST, 1,
        OP_RETURN
    };
    LispObject nil = lisp_read_from_buffer("()");
    LispObject zero = lisp_read_from_buffer("0");
    LispObject f_constants[2] = { nil, zero };
    LispObject f = lisp_make_compiled(f_code, sizeof(f_code), f_constants, 2, 2);

    // driver: f(f, 5) を呼び出す。driverの定数配列はfの構築が終わった後に組むので、
    // f自身が自分の値を知らなくてもここでは自己参照の鶏と卵問題は生じない
    unsigned char driver_code[] = {
        OP_CONST, 0,
        OP_CONST, 1,
        OP_CONST, 0,
        OP_CALL, 2,
        OP_RETURN
    };
    LispObject five = lisp_read_from_buffer("5");
    LispObject driver_constants[2] = { f, five };
    LispObject driver = lisp_make_compiled(driver_code, sizeof(driver_code), driver_constants, 2, 0);

    LispObject result = lisp_vm_exec(driver);

    if (result == five) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM function call self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM function call self-test: FAIL\r\n");
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
            lisp_vm_arith_selftest_run(SystemTable); // milestone 35: VM最小実行ループ自己テスト
            lisp_vm_control_flow_selftest_run(SystemTable); // milestone 36: VM制御フロー・ボックス化ローカル変数自己テスト
            lisp_vm_call_selftest_run(SystemTable); // milestone 37: VM関数呼び出し・スタックフレーム自己テスト

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
