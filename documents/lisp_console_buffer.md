# カーソル制御・画面ダブルバッファリングロードマップ

## 目的

ユーザーから、このLispOSへカーソル制御を組み込みたいという要望が提示された。要件は次の通り
(ユーザー原文の意訳):

- UEFIが持つ標準機能(`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`の`SetCursorPosition`/`QueryMode`/
  `ClearScreen`)を使って実装する。
- Lispから呼べるCビルトインを3つ用意する: `%set-cursor-position(col, row)`・
  `%get-screen-size() -> (cols, rows)`・`%clear-screen()`。
- その上にLispの薄いラッパーを書く。ユーザー提示のコード例:
  ```lisp
  (defun os:goto-xy (x y)
    (%set-cursor-position x y))

  (defun os:print-at (x y string)
    (os:goto-xy x y)
    (write-string string))
  ```
- `SetCursorPosition`を直接叩くと画面のチラツキが発生するため、画面バッファを用意し、
  各プロセスはそこへ書き込み、UEFIコンソールへの実転送は1命令ごとに行うダブルバッファリング
  方式にする。

「マイルストンを作成して」という依頼であり、本ドキュメントはそのマイルストーン化の結果である。
**本ドキュメント作成時点では、以下のマイルストーンは一切実装されておらず、全て未着手である。**

事前調査(Exploreエージェント、既存コード)で以下が判明し、方針を確定した:

1. **`src/uefi.h`の`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`(`:205-213`)は7フィールドで終わっており、
   UEFI仕様に存在する`SetCursorPosition`/`EnableCursor`/`Mode`が未定義**。`QueryMode`も`void*`の
   プレースホルダのままで呼び出し不可能な型になっている。
   → 既存フィールドの個数・順序・サイズを変えずに追記する(フェーズI)。
2. **既存コンソール出力(`write-string`/`write-line`/`princ`)は全て`lisp_console_stream_write`
   (`src/lisp.c:1380`)経由で、ASCII文字列をCHAR16へ変換して`ConOut->OutputString`を1回呼ぶ
   だけ**。ダブルバッファリング化にはこの経路の書き換えが必要になる。
3. **VM命令ディスパッチループ(`lisp_vm_run`)には、milestone106で追加した「他プロセスからの
   中断要求を毎命令チェックする」yieldフックが既にあり、「1命令ごとに何かする」ための実装
   ポイントとして同じ場所が使える**(ツリーウォーク経路(`lisp_eval`/`lisp_apply`)は milestone106
   と同様に対象外のまま残る既知の制約)。
4. **`os:process`クラス・`LispProcessStack`にはコンソール/画面関連のフィールドは一切無く、
   コンソール出力は現状全プロセス共通でグローバル`g_system_table`経由の単一`ConOut`を叩く
   設計**。→ 画面バッファは**プロセス毎ではなく単一の共有バッファ**とする(実機が物理的に
   1画面しか持たないことに対応し、コルーチン方式(常に1プロセスのみ実行中)なので書き込み競合の
   心配が無いことを理由とする)。
5. **`scripts/run_test.py`はQEMUのシリアル出力から ANSI/VT100エスケープ
   (`\x1b\[[0-?]*[ -/]*[@-~]`)を正規表現で除去した上で改行区切りの行としてPASS/FAILマーカーを
   検出する**。既存の`ClearScreen`(`ESC[2J`)は既に正しく除去されている実績があり、
   `SetCursorPosition`が出す`ESC[<row>;<col>H`も同じ正規表現で除去されることを確認済み。
   **重要な制約**: バッファのflushが改行を「セル内容の差分」としてしか扱わず実際の`\r\n`バイトを
   送出しなくなると、既存28+フィクスチャが行検出できずタイムアウトする。したがってflush処理は
   「改行が発生した回数分`\r\n`を実際に送出する」処理を、セル差分とは別に持つ必要がある。

マイルストーン番号は既存の1〜118に続く**119から**開始する。

## 現状把握

- **`src/uefi.h`の`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`**: `Reset`/`OutputString`/`TestString`/
  `QueryMode`(`void*`)/`SetMode`/`SetAttribute`/`ClearScreen`の7フィールドのみ。UEFI仕様上の
  `SetCursorPosition`/`EnableCursor`/`Mode`が欠落している。
- **コンソール出力の実装**: `lisp_console_stream_write`(`src/lisp.c:1380`付近)が
  `LispOutputStream`(milestone24)のwrite関数として登録されており、`write-string`/
  `write-line`/`princ`/REPLプロンプト/panicメッセージ等、全てのコンソール出力がここを通る。
  ASCII文字列を1文字ずつCHAR16へ変換して固定長バッファへ詰め、最後に`OutputString`を1回
  呼ぶだけの実装であり、カーソル位置やバッファリングの概念を一切持たない。
- **VM命令ディスパッチループのyieldフック**: `lisp_vm_run`のループ先頭に、milestone106で
  追加した`lisp_vm_current_process`/`lisp_vm_yield_target`/`lisp_vm_yield_budget`による
  軽量チェックが既にある。「1命令ごとに何かする」ための実装ポイントとして同じ場所(直後)が
  再利用できる。
- **プロセスオブジェクト(`os:process`、milestone102)・`LispProcessStack`(milestone104-107)**:
  コンソール/画面関連のフィールドは一切無い。コンソール出力は全プロセス共通でグローバル
  `g_system_table->ConOut`を叩く設計であり、画面バッファもプロセス毎ではなく単一の共有
  グローバルとする。
- **テストハーネス(`scripts/run_test.py`)の制約**: ANSIエスケープ除去後、改行区切りの行として
  `RESULT <name> PASS/FAIL`・`TEST-DONE`マーカーを検出する。実際の`\r\n`バイトが送出されなく
  なると既存28+フィクスチャが崩れる。

## マイルストーン一覧

### フェーズI: UEFI基盤拡張

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 119 | `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`拡張 | 完了 | `src/uefi.h`に`EFI_TEXT_QUERY_MODE`/`EFI_TEXT_SET_CURSOR_POSITION`/`EFI_TEXT_ENABLE_CURSOR`型(いずれも第1引数は`struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *`)と`EFI_SIMPLE_TEXT_OUTPUT_MODE`構造体(`MaxMode`/`Mode`/`Attribute`/`CursorColumn`/`CursorRow`の`INT32`5個+`CursorVisible`の`BOOLEAN`。プロジェクトに無かった`INT32`(`int`)・`BOOLEAN`(`unsigned char`)の2型を新規追加)を追加した。`QueryMode`を`void*`から`EFI_TEXT_QUERY_MODE`へ変更し、既存7フィールドの並び・個数はそのまま、`ClearScreen`の後ろに`SetCursorPosition`/`EnableCursor`/`Mode`を追記した。C自己テスト`lisp_console_output_mode_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`の起動シーケンス最初期・milestone116/117の自己テスト直後に組み込み)を追加し、実際の`g_system_table->ConOut->QueryMode`が`EFI_SUCCESS`を返しCols/Rowsが妥当な範囲(1〜1000)であること、`SetCursorPosition(1,1)`後に`ConOut->Mode->CursorColumn`/`CursorRow`が実際に1へ反映されること、`(0,0)`へ戻すと反映も0に戻ることを確認した。`make build`/`make test`(28ファイル全PASS、新規自己テスト`Console output mode (QueryMode/SetCursorPosition) self-test: PASS`のログを含む)で既存フィクスチャへの回帰が無いことを確認した。 |

### フェーズJ: カーソル制御ビルトイン単体(バッファ無し、暫定実装)

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 120 | `%clear-screen`/`%set-cursor-position`(直接ConOut) | 完了 | Cビルトイン`lisp_builtin_clear_screen`/`lisp_builtin_set_cursor_position`(`src/lisp.c`、`lisp_console_output_mode_selftest`と同じ`g_system_table->ConOut`を直接叩く暫定実装)を追加し、`%clear-screen`/`%set-cursor-position`として`lisp_builtins_init`へ登録した。`%set-cursor-position`は col/row を`lisp_assert_fixnum`で必須fixnum検証する。新規`test/lisp/test-console.lisp`(`run-test-console-clear-screen`/`run-test-console-set-cursor-position`/集約`run-test-console`)を追加し、両ビルトインがpanicせず`t`を返すことを確認した。`make build`/`make test`(29ファイル全PASS)で既存フィクスチャへの回帰が無いことを確認した。 |
| 121 | `%get-screen-size`(QueryMode経由) | 完了 | Cビルトイン`lisp_builtin_get_screen_size`(`src/lisp.c`、`lisp_console_output_mode_selftest`と同じ`QueryMode(ConOut, ConOut->Mode->Mode, &cols, &rows)`呼び出し)を追加し、`%get-screen-size`として登録した。戻り値は`(cons cols rows)`(fixnum2個)。`test-console.lisp`に`run-test-console-get-screen-size`を追加し、戻り値がcons(atomでない)であること・car/cdrがともに0より大きく1000未満のfixnumであることを確認した(`consp`/`not`はこの処理系に存在しないため、`atom`+`eq`で代用)。`make build`/`make test`(29ファイル全PASS)で回帰が無いことを確認した。 |

### フェーズK: ダブルバッファリング化

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 122 | `LispScreenBuffer`構造体新設・初期化 | 完了 | `src/lisp.c`に`LispScreenBuffer`構造体(`cols`/`rows`/固定上限`LISP_SCREEN_COLS_MAX`(200)×`LISP_SCREEN_ROWS_MAX`(80)の`back`/`front`2次元`CHAR16`配列/`cursor_col`/`cursor_row`/`pending_newlines`/`dirty`/`initialized`)と単一の共有グローバル`lisp_screen_buffer`を追加した。`lisp_screen_buffer_init`は`QueryMode`で実際のcols/rowsを確定(上限超過はpanic)し`ClearScreen`で実画面をクリアした後、back/front両方をスペースで埋めカーソル/`pending_newlines`/`dirty`を0に戻す。C自己テスト`lisp_screen_buffer_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`の起動シーケンス・milestone119自己テスト直後に組み込み)で初期化直後の状態を確認した。既存の出力経路(`lisp_console_stream_write`等)には未接続で、単体で状態を保持するだけの段階。`make build`/`make test`(29ファイル全PASS、新規自己テストのPASSログを含む)で回帰が無いことを確認した。 |
| 123 | `lisp_screen_putc`(1文字書き込み・改行・スクロール) | 完了 | `src/lisp.c`に`lisp_screen_putc`(UEFI呼び出しを一切含まない純粋なバッファ操作関数)を追加した。改行(`'\n'`)は`pending_newlines`を加算しカーソルを次行の先頭へ移すのみ(実際の`"\r\n"`送出はmilestone124の`lisp_screen_flush`側の責務)、通常文字は`back`へ書き込み`dirty`を立ててカーソルを1つ進め、行末に達したら次行の先頭へ折り返る。画面最下行を超えた場合は1行分shiftしてスクロールし最下行をスペースで初期化する。C自己テスト`lisp_screen_putc_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`のmilestone122自己テスト直後に組み込み)で、都度`lisp_screen_buffer_init`でリセットしながら基本の1文字書き込み・改行・行末折り返り・スクロール(目印文字が実際に1行shiftされ最下行が空になること)をそれぞれ独立に確認した。`make build`/`make test`(29ファイル全PASS、新規自己テストのPASSログを含む)で回帰が無いことを確認した。 |
| 124 | `lisp_screen_flush`(back/front差分反映) | 完了 | `src/lisp.c`に`lisp_screen_flush`を追加した。`dirty`が0かつ`pending_newlines`が0なら何もしない。各行を左から走査し、`back`と`front`が異なるセルの連続区間(run)ごとに1回の`SetCursorPosition`+`OutputString`で反映しつつ`front`を同期する(画面全体を毎回送り直さない)。その後`pending_newlines`分だけ実`"\r\n"`を送出する(milestone123で改行をセル差分として表現しないことにした帰結であり、内容が変化していなくても`scripts/run_test.py`の行区切り検出のために必要)。最後にハードウェアカーソルを論理カーソル位置(`cursor_col`/`cursor_row`)へ合わせ、`dirty`/`pending_newlines`を0に戻す。実ファームウェア呼び出し自体はモックせず、本関数が`SetCursorPosition`/`OutputString`を呼ぶと決定した回数を静的カウンタ(`lisp_screen_flush_cell_output_count`/`_set_cursor_count`/`_newline_output_count`)に記録する方式にし、C自己テスト`lisp_screen_flush_selftest`(`src/lisp.c`/`src/lisp.h`、`main.c`のmilestone123自己テスト直後に組み込み)で、無変化・1文字・隣接2文字(1run)・離れた2文字(2run)・改行のみ(セル差分無しでも`\r\n`は送出)の5パターンについて呼び出し回数の差分を検証した。`make build`/`make test`(29ファイル全PASS、新規自己テストのPASSログを含む)で回帰が無いことを確認した。 |
| 125 | `lisp_console_stream_write`のバッファ化(暫定: 呼び出し毎flush) | 完了 | `src/lisp.c`の`lisp_console_stream_write`を、1文字ずつ`lisp_screen_putc`へ書き込み呼び出し単位で`lisp_screen_flush`する実装へ切り替えた(初回呼び出し時にバッファが未初期化なら自動的に`lisp_screen_buffer_init`する。起動シーケンスへの明示的な統合はmilestone128で行う)。**バグ修正**: `make test`実行で`RESULT while PASS`が`RESULTwhilePASS`に破損する回帰を発見した。原因はmilestone124の`lisp_screen_flush`が`back`と`front`の値比較でrunを決めていたため、書き込んだ文字が偶然既存の表示内容(スペース等)と一致すると`OutputString`自体をスキップしてしまい、実機の画面(端末)上は正しくても`scripts/run_test.py`が読む生のシリアルバイト列からその文字が欠落することだった。`LispScreenBuffer`に行ごとの`row_touched`/`touched_min`/`touched_max`を追加し、`lisp_screen_putc`が実際に書き込んだセルの範囲を記録するようにした上で、`lisp_screen_flush`を値比較ではなく「touchされた範囲は値が不変でも必ず送出する」方式へ修正した(スクロール時は全行を丸ごとtouchする)。これにより見た目の再描画範囲(書き込まれていないセルは送出しない)を保ったまま、書き込んだ内容が必ずバイト列として現れることを保証した。この修正に伴い`lisp_screen_flush_selftest`の1パターン(離れた2文字)の期待値を「2run」から「1run(間のスペースも含めて送出)」に更新した。`make build`/`make test`(29ファイル全PASS、`RESULT ... PASS`の空白が正しく再現されることを含む)で確認した。 |
| 126 | 3ビルトインのバッファ化 | 完了 | `src/lisp.c`の`lisp_builtin_clear_screen`/`lisp_builtin_set_cursor_position`/`lisp_builtin_get_screen_size`(milestone120/121の暫定・直接ConOut実装)を、それぞれ新設した`lisp_screen_clear`/`lisp_screen_move_cursor`/`lisp_screen_get_size`経由に切り替えた。`lisp_screen_clear`は未初期化なら`lisp_screen_buffer_init`に委ね、初期化済みなら実`ClearScreen`を呼んだ上でback/front/カーソル/touch状態を初期化直後と同じ状態に戻す(QueryModeは再実行せずcols/rowsは起動時確定値のまま維持する設計方針を継続)。`lisp_screen_move_cursor`は論理カーソル位置を更新した上で、`lisp_screen_flush`のdirty/pending_newlinesゲートとは無関係に必ず即座に実`SetCursorPosition`を発行する専用経路とした(カーソル移動は他の描画内容の変化有無に関係なく即時反映されるべき操作のため、既存のflush呼び出し回数の自己テストに影響を与えずに済む設計)。`lisp_screen_get_size`は起動時にQueryModeで確定済みの`lisp_screen_buffer.cols`/`.rows`をそのまま返す(実行時の画面サイズ変更はスコープ外)。3ビルトイン自体はLisp側の呼び出し形・戻り値の型を変更しておらず、内部実装の差し替えのみ。 |
| 127 | VM命令ディスパッチループへの1命令ごとflushフック追加 | 完了 | `lisp_vm_run`のディスパッチループ内、既存の milestone106 yieldチェック直後・次のopcodeをフェッチする直前に、常時有効な無条件`lisp_screen_flush()`呼び出しを追加した。yieldチェックと同じ位置にあるため、`OP_RETURN`を含む全ての opcode がフェッチされる前に必ず1回flushが走ることになり、125で入れた「コンソール書き込み毎に即時flush」する仕組みは不要になったため`lisp_console_stream_write`から`lisp_screen_flush()`呼び出しを削除した。ツリーウォーク経路(`lisp_eval`/`lisp_apply`、現状`defmacro`本体と rest-arg 系`defun`のみが通る)はmilestone106のyieldフックと同様この位置のフックを通らないため対象外のままで、そちらで書かれた内容はVMディスパッチループへ制御が戻った時点でまとめてflushされる(内容が失われるわけではない)という既知の制約として明記する。手作りbytecode(`OP_CONST`でtestビルトインをロード→`OP_CALL`→`OP_RETURN`)を使うC自己テスト`lisp_vm_flush_hook_selftest`(`src/lisp.c`/`src/lisp.h`)を追加し、testビルトイン内で`lisp_screen_putc`のみ呼び出し明示的なflushを一切呼ばずに`lisp_vm_exec`した後、`front`バッファに実際に反映されていること(=ディスパッチループ側のフックが動いたこと)を確認した。**バグ修正**: 本自己テストは実際のLispオブジェクト確保(`lisp_make_builtin`/`lisp_make_compiled`)を行うため、`main.c`の起動シーケンス内で`lisp_heap_init()`より前の位置(milestone122〜124のスクリーンバッファ系自己テストの直後)へ呼び出しを置いたところ、ヒープ未初期化のまま最初の確保で`Lisp fatal panic: heap exhausted`が発生した。スクリーンバッファ系自己テストは静的な`LispScreenBuffer`構造体のみを操作しLispヒープに触れないためこの問題が今まで露見していなかった。呼び出し位置を`lisp_heap_init()`・`lisp_packages_init()`・`lisp_builtins_init()`・compiler.lisp/stdlib.lispの読込・milestone107のプロセスGCルート自己テストの後(既にLispヒープへの確保を要求する他のVM/プロセス系自己テスト群と同じクラスタ内)へ移動して解決した。`make build`/`make test`(29ファイル全PASS、新規自己テストのPASSログを含む。`test-call-dispatch`含む全フィクスチャで回帰無し)で確認した。 |
| 128 | 境界処理の統合 | 未着手 | `lisp_screen_buffer_init`をmain.c既存の`ClearScreen`呼び出し位置に統合する。REPLプロンプトをバッファ経由+明示的flushへ切り替える。panic冒頭にflush呼び出しを追加する。キー入力エコーは対象外のまま維持する。`make test`で最終回帰確認する。 |

### フェーズL: Lispラッパー

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 129 | `os:goto-xy`/`os:print-at`/`os:clear-screen` | 未着手 | `lisp/os.lisp`にユーザー提示のコード例どおりのラッパーを追加する。`test-console.lisp`にエラーにならないことの確認を追加し、本ドキュメントを確定稿にする。 |

## スコープ外として明記する項目

- マルチカーソル/複数ウィンドウ・プロセス毎の独立ウィンドウ合成(単一共有バッファ方式の帰結)
- 色属性(`SetAttribute`)の実装
- `EnableCursor`のLisp公開(型は仕様の完全性のため定義するが、ビルトインとしては公開しない)
- 実行時の画面サイズ変更対応(`QueryMode`は起動時に1回だけ呼び、以後固定値として使う)
- スクロールバッファ/履歴(画面外に流れた内容の保存)
- `lisp_read_line`のキー入力エコーのバッファ統合(対話性優先で直接ConOutのまま)
- グラフィックスモード(GOP)対応(テキストコンソールのみ)

## 検証方針

各マイルストーンで`make build`(クロスコンパイル)・`make test`(既存28+フィクスチャ全PASS、
回帰無し)を確認する。flush処理が改行イベントを実際の`\r\n`バイトとして再現すること
(milestone124/125の中心的検証観点)が既存テストハーネスとの互換性の要になる。実際の視覚的
チラツキ低減・`os:goto-xy`/`os:print-at`の見た目そのものは`make test`のヘッドレス環境
(`-display none`)では検証できないため、既存フェーズC/Hと同様に個別のQEMU対話セッション
(またはGUI付き`make run`)での目視確認に委ねる。
