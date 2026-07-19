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
| 120 | `%clear-screen`/`%set-cursor-position`(直接ConOut) | 未着手 | Cビルトイン2つを実装・登録。直接`ConOut`を叩く暫定実装。新規`test/lisp/test-console.lisp`でエラーにならないことを確認する。 |
| 121 | `%get-screen-size`(QueryMode経由) | 未着手 | `QueryMode(ConOut, ConOut->Mode->Mode, &cols, &rows)`の結果を`(cons cols rows)`として返す。`test-console.lisp`に戻り値の型検証を追加する。 |

### フェーズK: ダブルバッファリング化

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 122 | `LispScreenBuffer`構造体新設・初期化 | 未着手 | back/front/カーソル/dirtyを持つ構造体と`lisp_screen_buffer_init`(QueryModeでcols/rows確定、ClearScreenで実画面初期化)を追加する。既存経路には未接続。C自己テストで初期値を確認する。 |
| 123 | `lisp_screen_putc`(1文字書き込み・改行・スクロール) | 未着手 | UEFI呼び出しを含まない純粋なバッファ操作関数。改行時は`pending_newlines`加算、画面末尾到達時は1行shiftでスクロールする。C自己テストで検証する。 |
| 124 | `lisp_screen_flush`(back/front差分反映) | 未着手 | 変化したセルのみ`SetCursorPosition`+`OutputString`で反映、`pending_newlines`分の実`"\r\n"`を送出、最後にハードウェアカーソルを論理カーソルへ合わせる。C自己テストで呼び出し回数を検証する。 |
| 125 | `lisp_console_stream_write`のバッファ化(暫定: 呼び出し毎flush) | 未着手 | `write-string`/`write-line`/`princ`をバッファ経由+即時flushへ切り替える中間段階。`make test`で既存28+フィクスチャの回帰無しを確認する(改行の実バイト再現性の実質検証)。 |
| 126 | 3ビルトインのバッファ化 | 未着手 | `%set-cursor-position`/`%clear-screen`/`%get-screen-size`をバッファ経由(即時flush)へ切り替える。 |
| 127 | VM命令ディスパッチループへの1命令ごとflushフック追加 | 未着手 | 既存yieldチェック直後に常時有効な無条件flushチェックを追加し、125/126の即時flushを削除する。ツリーウォーク経路は対象外という制約を明記する。C自己テストで手作りbytecodeを使い検証する。 |
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
