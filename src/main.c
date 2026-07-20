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

// --- リーダーのpkg:sym/pkg::sym修飾子自己テスト (milestone 74) ---
static void lisp_reader_package_qualifier_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_package_qualifier_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader package qualifier self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader package qualifier self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- exportビルトイン+リーダー修飾子の自己テスト (milestone 76) ---
static void lisp_reader_export_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_export_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader export self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader export self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- use-packageビルトイン+use-list継承intern解決の自己テスト (milestone 77) ---
static void lisp_reader_use_package_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_use_package_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader use-package self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader use-package self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- intern/in-package/defpackageマクロの自己テスト (milestone 78) ---
static void lisp_reader_defpackage_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_defpackage_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader defpackage self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Reader defpackage self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- ブートストラップのパッケージ文脈自己テスト (milestone 80) ---
static void lisp_bootstrap_package_context_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_bootstrap_package_context_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Bootstrap package context self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Bootstrap package context self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- グローバル参照とシンボル同一性の回帰自己テスト (milestone 81) ---
static void lisp_global_ref_package_identity_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_global_ref_package_identity_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Global ref package identity self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Global ref package identity self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 特殊形式export自己テスト (milestone 100) ---
static void lisp_reader_special_form_export_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_special_form_export_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Special form export self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Special form export self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- Ctrl単体押下判定ロジック自己テスト (milestone 116) ---
static void lisp_key_state_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_key_state_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Key state (lone Ctrl) self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Key state (lone Ctrl) self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- Ctrl2回連続押下判定ロジック自己テスト (milestone 117) ---
static void lisp_ctrl_wait_classify_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_ctrl_wait_classify_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Ctrl double-press classify self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Ctrl double-press classify self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- コンソール出力モード自己テスト (milestone 119) ---
static void lisp_console_output_mode_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_console_output_mode_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Console output mode (QueryMode/SetCursorPosition) self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Console output mode (QueryMode/SetCursorPosition) self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 画面バッファ初期化自己テスト (milestone 122) ---
static void lisp_screen_buffer_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_screen_buffer_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen buffer init self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen buffer init self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 画面バッファ1文字書き込み自己テスト (milestone 123) ---
static void lisp_screen_putc_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_screen_putc_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen putc self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen putc self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 画面バッファflush自己テスト (milestone 124) ---
static void lisp_screen_flush_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_screen_flush_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen flush self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen flush self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- force_full_redrawフラグ自己テスト (milestone 132) ---
static void lisp_screen_force_full_redraw_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_screen_force_full_redraw_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen force full redraw self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Screen force full redraw self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VM命令ディスパッチループの1命令ごとflushフック自己テスト (milestone 127) ---
static void lisp_vm_flush_hook_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_vm_flush_hook_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM flush hook self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM flush hook self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- ビルトインexport自己テスト (milestone 101) ---
static void lisp_reader_builtin_export_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_reader_builtin_export_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Builtin export self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Builtin export self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- per-processスタック領域とコンテキスト保存自己テスト (milestone 104) ---
static void lisp_context_switch_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_context_switch_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Context switch self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Context switch self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- per-process vm_stack/vm_sp/lisp_active_trap分離自己テスト (milestone 105) ---
static void lisp_process_vm_state_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_process_vm_state_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process VM state separation self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process VM state separation self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- コルーチンyieldチェック自己テスト (milestone 106) ---
static void lisp_vm_yield_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_vm_yield_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM yield check self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM yield check self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 全プロセスGCルート登録自己テスト (milestone 107) ---
static void lisp_process_gc_root_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_process_gc_root_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process GC root self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process GC root self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- fork時の一意パッケージ生成自己テスト (milestone 108) ---
// os:process/os:*all-processes*(os.lisp、milestone102)がまだ定義されていない起動段階では
// 実行できないため、os.lisp読み込み後の呼び出し箇所で呼ぶ
static void lisp_process_fork_package_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    if (lisp_process_fork_package_selftest()) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process fork package self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process fork package self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- process-suspend/process-resume自己テスト (milestone 112) ---
// os:process/os.lisp読み込み後にのみ実行できるため、lisp_process_fork_package_selftest_run
// と同じ箇所(os.lisp読み込み後)から呼ぶ
static void lisp_process_suspend_resume_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    int ok = lisp_process_suspend_resume_selftest();
    // milestone133: このセルフテストは実際の%process-resume/%process-suspendを経由するため、
    // 画面バッファの退避/復元・force_full_redrawも本物の処理として発生する。ここでの
    // 退避/復元自体は「見た目上は何も変わっていない」再描画(このセルフテスト用の使い捨て
    // プロセスは一切画面へ書き込まないため)だが、pending状態のまま次回の(無関係な)VM命令
    // フック契機まで持ち越すと、その持ち越し先の出力(実運用ではRESULT行など)へ改行無しで
    // 連結され、テストハーネスの改行前提の行検出を壊す実害があった。ここで明示的に
    // lisp_screen_flushを呼んで即座に消費し、直後に一度改行を送出して次の出力から
    // 独立させる
    lisp_screen_flush();
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    if (ok) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process suspend/resume self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process suspend/resume self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- process-local-variable自己テスト (milestone 113) ---
// 同じ理由(os:process/os.lisp読み込み後)から、他のprocess系self-testと同じ箇所で呼ぶ
static void lisp_process_local_variable_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    int ok = lisp_process_local_variable_selftest();
    // milestone133: lisp_process_suspend_resume_selftest_runと同じ理由(このセルフテストも
    // 実際の%process-resumeを経由するため、pendingのforce_full_redrawを即座にここで消費し、
    // 明示的な改行で次の出力から切り離す)
    lisp_screen_flush();
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    if (ok) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process local-variable self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process local-variable self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- プロセス毎画面バッファ分離自己テスト (milestone 133) ---
// 同じ理由(os:process/os.lisp読み込み後)から、他のprocess系self-testと同じ箇所で呼ぶ
static void lisp_process_screen_separation_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    int ok = lisp_process_screen_separation_selftest();
    // milestone133: 他のprocess系self-testと同じ理由(実際の%process-resume/%process-suspendを
    // 経由するため、pendingのforce_full_redrawを即座にここで消費し、明示的な改行で次の出力
    // から切り離す)
    lisp_screen_flush();
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    if (ok) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process screen separation self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process screen separation self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- 状態行とプロセス切替の連動自己テスト (milestone 134) ---
// 同じ理由(os:process/os.lisp読み込み後)から、他のprocess系self-testと同じ箇所で呼ぶ
static void lisp_process_status_line_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    int ok = lisp_process_status_line_selftest();
    // milestone134: 他のprocess系self-testと同じ理由(実際の%process-resumeを経由するため、
    // pendingのforce_full_redrawを即座にここで消費し、明示的な改行で次の出力から切り離す)
    lisp_screen_flush();
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    if (ok) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process status line self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Process status line self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VM最小実行ループ自己テスト (milestone 35) ---
// OP_CONST 1, OP_CONST 2, OP_ADD, OP_RETURN相当を手動でバイトコード配列として構築し、
// lisp_vm_execに渡して3が返ることを確認する。定数オブジェクトはlisp_make_fixnum相当の
// 内部APIを直接使わず、既に公開されているlisp_read_from_bufferを経由して作る
static void lisp_vm_arith_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char code[] = { OP_CONST, 0, 0, OP_CONST, 1, 0, OP_ADD, OP_RETURN };
    LispObject constants[2] = { lisp_read_from_buffer("1"), lisp_read_from_buffer("2") };
    LispObject fn = lisp_make_compiled(code, sizeof(code), constants, 2, 0, 0);
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
// milestone83/84でOP_MAKE_LOCALが2byteのFP相対indexオペランドを取るようになったため、
// これより後にあるジャンプ先オフセットは全て+2ずれている。
//   [0]  OP_CONST 0            ; 10をpush
//   [3]  OP_MAKE_LOCAL 0       ; local0 = box(10)
//   [6]  OP_CONST 1            ; testをpush
//   [9]  OP_JUMP_IF_FALSE 21   ; nilならelseへ
//   [12] OP_CONST 2            ; 20をpush (then)
//   [15] OP_STORE_LOCAL 0      ; local0 <- 20
//   [18] OP_JUMP 27            ; elseを飛び越す
//   [21] OP_CONST 3            ; 30をpush (else)
//   [24] OP_STORE_LOCAL 0      ; local0 <- 30
//   [27] OP_LOAD_LOCAL 0       ; local0を読む
//   [30] OP_RETURN
static void lisp_vm_control_flow_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char code[] = {
        OP_CONST, 0, 0,
        OP_MAKE_LOCAL, 0, 0,
        OP_CONST, 1, 0,
        OP_JUMP_IF_FALSE, 21, 0,
        OP_CONST, 2, 0,
        OP_STORE_LOCAL, 0, 0,
        OP_JUMP, 27, 0,
        OP_CONST, 3, 0,
        OP_STORE_LOCAL, 0, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_RETURN
    };

    LispObject ten = lisp_read_from_buffer("10");
    LispObject twenty = lisp_read_from_buffer("20");
    LispObject thirty = lisp_read_from_buffer("30");
    LispObject nil = lisp_read_from_buffer("()");

    LispObject constants_false[4] = { ten, nil, twenty, thirty };
    LispObject fn_false = lisp_make_compiled(code, sizeof(code), constants_false, 4, 0, 1);
    LispObject result_false = lisp_vm_exec(fn_false);

    LispObject constants_true[4] = { ten, lisp_read_from_buffer("1"), twenty, thirty };
    LispObject fn_true = lisp_make_compiled(code, sizeof(code), constants_true, 4, 0, 1);
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
//   [3]  OP_JUMP_IF_FALSE 23 ; nilなら基底部へ
//   [6]  OP_LOAD_LOCAL 0     ; self をpush（次段呼び出しのself引数）
//   [9]  OP_CONST 0          ; nil をpush（次段呼び出しのn引数）
//   [12] OP_LOAD_LOCAL 0     ; self をpush（呼び出す関数参照）
//   [15] OP_CALL 2           ; self(self, nil) を呼び出す
//   [18] OP_LOAD_LOCAL 1     ; 自分のn をpush
//   [21] OP_ADD               ; 再帰結果 + 自分のn
//   [22] OP_RETURN
//   [23] OP_CONST 1          ; 0 をpush（基底部）
//   [26] OP_RETURN
static void lisp_vm_call_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char f_code[] = {
        OP_LOAD_LOCAL, 1, 0,
        OP_JUMP_IF_FALSE, 23, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_CONST, 0, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_CALL, 2, 0,
        OP_LOAD_LOCAL, 1, 0,
        OP_ADD,
        OP_RETURN,
        OP_CONST, 1, 0,
        OP_RETURN
    };
    LispObject nil = lisp_read_from_buffer("()");
    LispObject zero = lisp_read_from_buffer("0");
    LispObject f_constants[2] = { nil, zero };
    LispObject f = lisp_make_compiled(f_code, sizeof(f_code), f_constants, 2, 2, 2);

    // driver: f(f, 5) を呼び出す。driverの定数配列はfの構築が終わった後に組むので、
    // f自身が自分の値を知らなくてもここでは自己参照の鶏と卵問題は生じない
    unsigned char driver_code[] = {
        OP_CONST, 0, 0,
        OP_CONST, 1, 0,
        OP_CONST, 0, 0,
        OP_CALL, 2, 0,
        OP_RETURN
    };
    LispObject five = lisp_read_from_buffer("5");
    LispObject driver_constants[2] = { f, five };
    LispObject driver = lisp_make_compiled(driver_code, sizeof(driver_code), driver_constants, 2, 0, 0);

    LispObject result = lisp_vm_exec(driver);

    if (result == five) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM function call self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM function call self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VMクロージャ生成・upvalue自己テスト (milestone 38) ---
// (defun make-counter (n) (lambda () (setq n (+ n 1)) n)) 相当を手動バイトコードで構築する。
// inc（カウンタ本体、nargs=0）は自分のupvalue[0]（make-counterのローカルnのボックス）を
// 読み書きするテンプレートで、upvalue_descs=[(kind=0 . index=0)]（「生成元フレームのFP+0を
// そのまま捕捉」）を持つ。make-counter（nargs=1）はOP_MAKE_CLOSUREでincのインスタンスを
// 1つ作って返すだけ。driverはmake-counterを2回呼んで独立したクロージャc1/c2を作り、
// c1を2回・c2を1回呼んだ結果を全て加算する（11+12+101=124）。この値は次を同時に検証する:
//   - c1を複数回呼んでもcaptureしたボックスの変更が次の呼び出しに正しく持続する（11→12）
//   - c1とc2はmake-counterの呼び出しごとに異なるボックスを捕捉し、状態が独立している
//     （c2が10ベースの値へ汚染されていれば合計は124にならない）
//   inc_code:
//     [0]  OP_LOAD_UPVALUE 0   ; 現在のnをpush
//     [3]  OP_CONST 0           ; 1をpush
//     [6]  OP_ADD                 ; n+1
//     [7]  OP_STORE_UPVALUE 0  ; upvalue[0]へ書き戻す
//     [10] OP_LOAD_UPVALUE 0   ; 更新後のnをpushして返り値にする
//     [13] OP_RETURN
//   make_counter_code:
//     [0] OP_MAKE_CLOSURE 0    ; 定数0番（inc template）からインスタンスを作る
//     [3] OP_RETURN
static void lisp_vm_closure_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char inc_code[] = {
        OP_LOAD_UPVALUE, 0, 0,
        OP_CONST, 0, 0,
        OP_ADD,
        OP_STORE_UPVALUE, 0, 0,
        OP_LOAD_UPVALUE, 0, 0,
        OP_RETURN
    };
    LispObject one = lisp_read_from_buffer("1");
    LispObject inc_constants[1] = { one };
    LispObject inc = lisp_make_compiled(inc_code, sizeof(inc_code), inc_constants, 1, 0, 0);
    UINTN desc_kinds[1] = { 0 };
    UINTN desc_indices[1] = { 0 };
    lisp_compiled_set_upvalue_descs(inc, lisp_make_upvalue_descs(desc_kinds, desc_indices, 1));

    unsigned char make_counter_code[] = {
        OP_MAKE_CLOSURE, 0, 0,
        OP_RETURN
    };
    LispObject make_counter_constants[1] = { inc };
    LispObject make_counter = lisp_make_compiled(make_counter_code, sizeof(make_counter_code),
                                                  make_counter_constants, 1, 1, 1);

    // driver: c1=make-counter(10), c2=make-counter(100)を作り、c1を2回・c2を1回呼んで
    // 11+12+101=124を返す
    unsigned char driver_code[] = {
        OP_CONST, 0, 0,
        OP_CONST, 2, 0,
        OP_CALL, 1, 0,
        OP_MAKE_LOCAL, 0, 0,
        OP_CONST, 1, 0,
        OP_CONST, 2, 0,
        OP_CALL, 1, 0,
        OP_MAKE_LOCAL, 1, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_CALL, 0, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_CALL, 0, 0,
        OP_ADD,
        OP_LOAD_LOCAL, 1, 0,
        OP_CALL, 0, 0,
        OP_ADD,
        OP_RETURN
    };
    LispObject ten = lisp_read_from_buffer("10");
    LispObject hundred = lisp_read_from_buffer("100");
    LispObject driver_constants[3] = { ten, hundred, make_counter };
    LispObject driver = lisp_make_compiled(driver_code, sizeof(driver_code), driver_constants, 3, 0, 2);

    LispObject result = lisp_vm_exec(driver);
    LispObject expected = lisp_read_from_buffer("124");

    if (result == expected) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM closure/upvalue self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM closure/upvalue self-test: FAIL\r\n");
        for (;;) {}
    }
}

// --- VM総合自己テスト: プリミティブ最適化命令+目標1全機能の組み合わせ (milestone 39) ---
// f(self, n, counter) を手動バイトコードで構築する。nがeq 0（新設OP_EQ）でなければ、
// counter()（クロージャ呼び出し+upvalue読み書き）でstepを取り、cons(n, step)（新設OP_CONS）
// をローカル変数としてボックス化し、car/cdr（新設OP_CAR/OP_CDR）でnとstepを取り出し、
// n+(-1)（OP_ADD、負の定数による減算相当）した値・counter・自分自身を引数に自己適用方式
// （milestone37）で再帰呼び出し（OP_CALL）し、carval+cdrval+recを返す。milestone37と異なり
// OP_EQで0との真の比較ができるため、今回は見せかけではない実際のfixnum減算による停止判定になっている
//   [0]  OP_LOAD_LOCAL 1       ; n をpush
//   [3]  OP_CONST 0             ; 0 をpush
//   [6]  OP_EQ                    ; n==0 ?
//   [7]  OP_JUMP_IF_FALSE 14   ; 等しくなければelseへ
//   [10] OP_CONST 0             ; 基底部: 0 をpush
//   [13] OP_RETURN
//   [14] OP_LOAD_LOCAL 1       ; n をpush
//   [17] OP_LOAD_LOCAL 2       ; counter をpush
//   [20] OP_CALL 0                ; step = counter()
//   [23] OP_CONS                  ; pair = cons(n, step)
//   [24] OP_MAKE_LOCAL           ; local3 = box(pair)
//   [25] OP_LOAD_LOCAL 3       ; pair をpush
//   [28] OP_CAR                    ; carval = car(pair)  (== n)
//   [29] OP_LOAD_LOCAL 3       ; pair をpush
//   [32] OP_CDR                    ; cdrval = cdr(pair)  (== step)
//   [33] OP_LOAD_LOCAL 0       ; self をpush（再帰呼び出しのarg1）
//   [36] OP_LOAD_LOCAL 1       ; n をpush
//   [39] OP_CONST 1             ; -1 をpush
//   [42] OP_ADD                    ; n + (-1)   （再帰呼び出しのarg2）
//   [43] OP_LOAD_LOCAL 2       ; counter をpush（再帰呼び出しのarg3）
//   [46] OP_LOAD_LOCAL 0       ; self をpush（再帰呼び出しの関数参照）
//   [49] OP_CALL 3                ; rec = self(self, n-1, counter)
//   [52] OP_ADD                    ; cdrval + rec
//   [53] OP_ADD                    ; carval + (cdrval + rec)
//   [54] OP_RETURN
static void lisp_vm_integrated_selftest_run(EFI_SYSTEM_TABLE *SystemTable) {
    unsigned char inc_code[] = {
        OP_LOAD_UPVALUE, 0, 0,
        OP_CONST, 0, 0,
        OP_ADD,
        OP_STORE_UPVALUE, 0, 0,
        OP_LOAD_UPVALUE, 0, 0,
        OP_RETURN
    };
    LispObject one = lisp_read_from_buffer("1");
    LispObject inc_constants[1] = { one };
    LispObject inc = lisp_make_compiled(inc_code, sizeof(inc_code), inc_constants, 1, 0, 0);
    UINTN desc_kinds[1] = { 0 };
    UINTN desc_indices[1] = { 0 };
    lisp_compiled_set_upvalue_descs(inc, lisp_make_upvalue_descs(desc_kinds, desc_indices, 1));

    unsigned char make_counter_code[] = {
        OP_MAKE_CLOSURE, 0, 0,
        OP_RETURN
    };
    LispObject make_counter_constants[1] = { inc };
    LispObject make_counter = lisp_make_compiled(make_counter_code, sizeof(make_counter_code),
                                                  make_counter_constants, 1, 1, 1);

    unsigned char f_code[] = {
        OP_LOAD_LOCAL, 1, 0,
        OP_CONST, 0, 0,
        OP_EQ,
        OP_JUMP_IF_FALSE, 14, 0,
        OP_CONST, 0, 0,
        OP_RETURN,
        OP_LOAD_LOCAL, 1, 0,
        OP_LOAD_LOCAL, 2, 0,
        OP_CALL, 0, 0,
        OP_CONS,
        OP_MAKE_LOCAL, 3, 0,
        OP_LOAD_LOCAL, 3, 0,
        OP_CAR,
        OP_LOAD_LOCAL, 3, 0,
        OP_CDR,
        OP_LOAD_LOCAL, 0, 0,
        OP_LOAD_LOCAL, 1, 0,
        OP_CONST, 1, 0,
        OP_ADD,
        OP_LOAD_LOCAL, 2, 0,
        OP_LOAD_LOCAL, 0, 0,
        OP_CALL, 3, 0,
        OP_ADD,
        OP_ADD,
        OP_RETURN
    };
    LispObject zero = lisp_read_from_buffer("0");
    LispObject neg_one = lisp_read_from_buffer("-1");
    LispObject f_constants[2] = { zero, neg_one };
    LispObject f = lisp_make_compiled(f_code, sizeof(f_code), f_constants, 2, 3, 4);

    // driver: counter=make-counter(0)をf呼び出しのちょうどarg3の位置に積んでから
    // f(f, 3, counter)を呼び出す
    unsigned char driver_code[] = {
        OP_CONST, 0, 0,
        OP_CONST, 1, 0,
        OP_CONST, 2, 0,
        OP_CONST, 3, 0,
        OP_CALL, 1, 0,
        OP_CONST, 0, 0,
        OP_CALL, 3, 0,
        OP_RETURN
    };
    LispObject three = lisp_read_from_buffer("3");
    LispObject driver_constants[4] = { f, three, zero, make_counter };
    LispObject driver = lisp_make_compiled(driver_code, sizeof(driver_code), driver_constants, 4, 0, 0);

    LispObject result = lisp_vm_exec(driver);
    LispObject expected = lisp_read_from_buffer("12");

    if (result == expected) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM integrated self-test: PASS\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"VM integrated self-test: FAIL\r\n");
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

// --- 本処理(自前スタックへ切り替えた後に実行される) ---
static EFI_STATUS EFIAPI EfiMainImpl(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_system_table = SystemTable;
    g_image_handle = ImageHandle;

    lisp_key_state_selftest_run(SystemTable); // milestone 116: Ctrl単体押下判定ロジック自己テスト
                                               // (実機キー入力に依存しない純粋な判定ロジックのみ検証)
    lisp_ctrl_wait_classify_selftest_run(SystemTable); // milestone 117: Ctrl2回連続押下判定の
                                                        // 分類ロジック自己テスト(同様に実機キー
                                                        // 入力に依存しない)
    lisp_console_output_mode_selftest_run(SystemTable); // milestone 119: 実際のConOut->QueryMode/
                                                         // SetCursorPositionを呼び戻り値・
                                                         // Cols/Rows・カーソル位置反映を確認する
    lisp_screen_buffer_selftest_run(SystemTable); // milestone 122: LispScreenBuffer(back/front/
                                                   // カーソル/dirty)の初期化状態を確認する。
                                                   // 既存の出力経路には未接続の単体テスト
    lisp_screen_putc_selftest_run(SystemTable); // milestone 123: 1文字書き込み・改行・
                                                 // 行末折り返し・スクロールを確認する
    lisp_screen_flush_selftest_run(SystemTable); // milestone 124: back/front差分のみを
                                                  // SetCursorPosition+OutputStringで反映し、
                                                  // pending_newlines分の実"\r\n"送出を
                                                  // 呼び出し回数ベースで確認する
    lisp_screen_force_full_redraw_selftest_run(SystemTable); // milestone 132: force_full_redraw
                                                               // フラグが立っていれば次のflushで
                                                               // 全行が送出されフラグ自身も
                                                               // クリアされることを確認する
    lisp_input_ex_init(); // milestone 116: EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOLをLocateProtocolで
                           // 取得する(見つからなければg_text_input_exはNULLのまま、panicしない)
    if (g_text_input_ex != (void *)0) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Text Input Ex Protocol: FOUND\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Text Input Ex Protocol: NOT FOUND\r\n");
    }

    // clear screen
    // milestone 128: 直接ClearScreenを呼ぶのではなく、lisp_screen_buffer_initへ統合する
    // (QueryModeでcols/rows確定・実ClearScreen・back/front初期化を1回で済ませる。
    // Lisp heapへは触れないためlisp_heap_init前のこの位置でも安全)
    lisp_screen_buffer_init();
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
            lisp_builtins_init();
            lisp_load_boot_file("compiler.lisp"); // milestone 65: コンパイラ本体を先に読み込む(フラグfalseのままツリーウォークで評価され、末尾のmark-compiler-readyでtrueへ切り替わる)
            lisp_load_boot_file("stdlib.lisp"); // milestone 29/65: 標準ライブラリを読み込む(この時点でフラグがtrueになっているため、新しいコンパイル駆動の経路で読み込まれる)
            lisp_setjmp_selftest(SystemTable); // milestone 30: setjmp/longjmp自己テスト
            lisp_vm_gc_root_selftest_run(SystemTable); // milestone 34: VMデータスタックGCルート自己テスト
            lisp_vm_arith_selftest_run(SystemTable); // milestone 35: VM最小実行ループ自己テスト
            lisp_vm_control_flow_selftest_run(SystemTable); // milestone 36: VM制御フロー・ボックス化ローカル変数自己テスト
            lisp_vm_call_selftest_run(SystemTable); // milestone 37: VM関数呼び出し・スタックフレーム自己テスト
            lisp_vm_closure_selftest_run(SystemTable); // milestone 38: VMクロージャ生成・upvalue自己テスト
            lisp_vm_integrated_selftest_run(SystemTable); // milestone 39: プリミティブ最適化命令+目標1総合自己テスト
            lisp_reader_package_qualifier_selftest_run(SystemTable); // milestone 74: リーダーのpkg:sym/pkg::sym修飾子自己テスト
            lisp_reader_export_selftest_run(SystemTable); // milestone 76: exportビルトイン+リーダー修飾子自己テスト
            lisp_reader_use_package_selftest_run(SystemTable); // milestone 77: use-packageビルトイン+use-list継承intern解決自己テスト
            lisp_reader_defpackage_selftest_run(SystemTable); // milestone 78: intern/in-packageビルトイン+defpackageマクロ自己テスト
            lisp_bootstrap_package_context_selftest_run(SystemTable); // milestone 80: ブートストラップのパッケージ文脈自己テスト
            lisp_global_ref_package_identity_selftest_run(SystemTable); // milestone 81: グローバル参照とシンボル同一性の回帰自己テスト
            lisp_reader_special_form_export_selftest_run(SystemTable); // milestone 100: 特殊形式export自己テスト
            lisp_reader_builtin_export_selftest_run(SystemTable); // milestone 101: ビルトインexport自己テスト
            lisp_context_switch_selftest_run(SystemTable); // milestone 104: per-processスタック領域とコンテキスト保存自己テスト
            lisp_process_vm_state_selftest_run(SystemTable); // milestone 105: per-process vm_stack/vm_sp/lisp_active_trap分離自己テスト
            lisp_vm_yield_selftest_run(SystemTable); // milestone 106: コルーチンyieldチェック自己テスト
            lisp_process_gc_root_selftest_run(SystemTable); // milestone 107: 全プロセスGCルート登録自己テスト
            lisp_vm_flush_hook_selftest_run(SystemTable); // milestone 127: VM命令ディスパッチループの
                                                           // 1命令ごとflushフックが、呼び出し元の
                                                           // 明示的なflush無しに動作することを確認する
                                                           // (Lisp heapへのオブジェクト確保を伴うため
                                                           // lisp_heap_init後のこの位置が必須)

            lisp_load_boot_file("os-package.lisp"); // milestone 102: osパッケージ作成(別ファイルに分離、下記コメント参照)
            lisp_load_boot_file("os.lisp"); // milestone 102: processクラス・os:*all-processes*・os:get-all-processes

            lisp_process_fork_package_selftest_run(SystemTable); // milestone 108: fork時の一意パッケージ生成自己テスト
            lisp_process_suspend_resume_selftest_run(SystemTable); // milestone 112: process-suspend/process-resume自己テスト
            lisp_process_local_variable_selftest_run(SystemTable); // milestone 113: process-local-variable自己テスト
            lisp_process_screen_separation_selftest_run(SystemTable); // milestone 133: プロセス毎画面バッファ分離自己テスト
            lisp_process_status_line_selftest_run(SystemTable); // milestone 134: 状態行とプロセス切替の連動自己テスト

            lisp_lock_cl_user_package(); // milestone 111: 起動処理完了後にcommon-lisp-userをデフォルトでロックする

            lisp_load_init_file(); // milestone 47: EFI/BOOT/init.lispがあれば読み込む(無ければ何もしない)

            // milestone134: mainコンテキスト自身の状態行(行0)へ固定文字列を1回だけ設定する。
            // 以降はos:process-resume/os:process-suspendが起こすscreen退避/復元(milestone133)で
            // そのまま持ち越されるため、明示的な再設定はここ1箇所のみで足りる
            lisp_screen_set_status_line("REPL", 4);

            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\nMinimal Lisp REPL. Type an expression and press Enter.\r\n");

            LispOutputStream console_stream = lisp_make_console_stream(SystemTable);

            lisp_jmp_buf repl_trap; // milestone 31: panic発生時にここへ復帰する
            lisp_active_trap = &repl_trap;

            for (;;) {
                lisp_setjmp(&repl_trap); // 戻り値は使わない: 通常経路とpanic復帰経路が
                                          // 完全に同じ地点（プロンプト表示直前）に合流するため
                lisp_vm_reset_stack(); // milestone 48: panicのlongjmpはvm_spを復元しないため、
                                        // このトラップ復帰直後で確実にゼロへ戻す
                if (lisp_heap_low()) { // milestone 33: ヒープが少ない時だけGCを起動する。
                                        // 評価中の中間値はCスタックからしか追跡できないため、
                                        // ここ（評価と評価の合間）以外では呼ばない
                    lisp_gc();
                }
                // milestone 128: プロンプトもバッファ経由にし、lisp_read_line(ブロッキング
                // なキー入力待ち)の直前に明示的flushして実際に表示されるようにする
                lisp_print_ascii(&console_stream, "> ");
                lisp_screen_flush();
                lisp_read_line(SystemTable);
                if (lisp_double_ctrl_detected) { // milestone 137: メインREPLループのみ、Ctrl2回検知を
                                                  // 実際のプロセス選択ダイアログ起動へ変換する
                                                  // (%read-console-exprなど他の呼び出し元は
                                                  // milestone136通りキャンセル/nil止まりのまま)
                    lisp_double_ctrl_detected = 0;
                    lisp_eval_toplevel(lisp_read_from_buffer("(os:switch-process)"));
                    continue;
                }
                if (input_length == 0) {
                    continue;
                }
                LispObject expr = lisp_read_from_buffer(input_buffer);
                LispObject result = lisp_eval_toplevel(expr);
                lisp_print(&console_stream, result);
                lisp_print_ascii(&console_stream, "\r\n");
                lisp_screen_flush(); // 次のプロンプト表示前に結果を確実に反映する
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

// --- 自前スタックへの切り替え (milestone 88) ---
// OVMFのビルド(2M/4Mフラッシュ)によってDXE Coreが用意するUEFIアプリケーション起動時の
// Cスタックのサイズ・配置が異なり(PEヘッダのSizeOfStackReserveはこのファームウェアの
// イメージローダーに無視されることを実験で確認済み)、コンパイラ自身の再帰
// (compile-if/compile-and等のAST降下)がその差だけでスタックオーバーフローしうることが
// 判明した(test-compile-exprがGitHub ActionsのOVMF_CODE_4M.fdでのみクラッシュしていた)。
// ヒープと同様にファームウェア依存を無くすため、起動直後にBootServices経由で大きな
// メモリ領域を確保し、そこへRSPを切り替えてから本処理(EfiMainImpl)を呼び出す。
#define LISP_BOOT_STACK_PAGES 4096 // 4096 * 4KB = 16MB

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_PHYSICAL_ADDRESS new_stack_base = 0;
    EFI_STATUS alloc_status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages, EfiLoaderData, LISP_BOOT_STACK_PAGES, &new_stack_base);

    if (alloc_status != 0) {
        // 確保に失敗した場合はファームウェア既定のスタックのまま続行する
        return EfiMainImpl(ImageHandle, SystemTable);
    }

    // call命令が積むリターンアドレス(8バイト)を差し引いてもcallee内で16バイト境界を
    // 保てるよう、切り替え先の先頭(スタックの高位側の終端)を16バイト境界に揃える
    UINT64 new_stack_top = (new_stack_base + (UINT64)LISP_BOOT_STACK_PAGES * 4096) & ~(UINT64)0xF;
    EFI_STATUS result;
    UINT64 saved_rsp;

    // rcx/rdxの入力制約により、asm本文が実行される時点でImageHandle/SystemTableは
    // 既にMS x64 ABI呼び出し規約どおりrcx/rdxへ積まれている。asm内で行うのはrspの
    // 一時的な切り替え・呼び出し・復元のみで、C側にrspの変化を一切見せない
    // (呼び出し後に元の値へ戻すため、rspをclobber宣言する必要もない)
    asm volatile(
        "mov %%rsp, %[saved]\n"
        "mov %[newtop], %%rsp\n"
        "call *%[fn]\n"
        "mov %[saved], %%rsp\n"
        : [saved] "=&r"(saved_rsp), "=a"(result)
        : [newtop] "r"(new_stack_top), [fn] "r"(EfiMainImpl), "c"(ImageHandle), "d"(SystemTable)
        : "r8", "r9", "r10", "r11", "memory"
    );

    return result;
}
