# プロセス毎画面バッファ・OS予約行・Ctrl2回ダイアログ切替ロードマップ

## 目的

カーソル制御・画面ダブルバッファリング(`documents/lisp_console_buffer.md`、マイルストーン
119〜129)完了後、ユーザーから新規に要望が出た(原文):

> 次のマイルストンを作成する
> 次は、プロセスごとにバッファを切り替えられるようにする
> バッファの一番上の行をOSが使う部分とし、そこにプロセスの名前を表示する
> Ctrl2回押すとプロセス選択ダイアログを表示する
> そこでプロセスを切り替えるとそのプロセスのバッファに切り替わるようにする

要件を整理すると次の4点になる:

1. プロセスごとに画面(バッファ)の内容を独立させ、切り替えられるようにする。
2. バッファの先頭行はOSが使う予約領域とし、そこに現在アクティブなプロセスの名前を表示する。
3. Ctrlキーを2回連続で押すと、プロセス選択ダイアログを表示する。
4. そのダイアログでプロセスを選択すると、実際にそのプロセスの画面バッファへ切り替わる。

「これを最終目標として、マイルストンを作成し、documents/配下にドキュメントを作成してほしい」
という依頼であり、本ドキュメントはそのマイルストーン化・ロードマップ化の結果である。
**本ドキュメント作成時点では、以下のマイルストーンは一切実装されておらず、全て未着手である。**

なお、要件4の「Ctrl2回連続押下によるプロセス切替」自体は、マルチプロセス化ロードマップ
(`documents/lisp_os_process.md`)のmilestone118で一度検討されたが、「ConInとEx protocolが
実機で同一キューを共有している可能性があり、ヘッドレスQEMU環境では検証も反証もできない」
という理由で、ライブな自動発火(常時ポーリング)への統合は明示的に見送られ、ユーザーが
明示的に呼び出すコマンド(`os:switch-process`)としてのみ実装された。今回のユーザー要望は
まさにこの見送られた挙動そのものを求めているため、本ロードマップではこのリスクの解消方法
(フェーズO)も合わせて設計する。

## 現状把握

- `LispScreenBuffer`(`src/lisp.c:5749`)は`back`/`front`(各`CHAR16[80][200]`)・`cursor_col`/
  `cursor_row`・`pending_newlines`・`dirty`・`initialized`・行単位のtouched情報を持つ
  静的グローバル1個(`lisp_screen_buffer`)。全ての操作関数(`lisp_screen_putc`/
  `lisp_screen_flush`/`lisp_screen_buffer_init`等)がこのグローバルを直接参照する作りで、
  ポインタ経由の抽象化は無い。`documents/lisp_console_buffer.md`に明記済みの設計判断として、
  「コルーチン方式で常に1プロセスのみ実行中」という前提のもとプロセス毎ではなく単一の
  共有グローバルとされている。今回はこの前提そのものを見直す。
- `LispProcessStack`(`src/lisp.h:116`)は`vm_stack`/`vm_sp`/`active_trap`という「プロセス毎の
  実行状態」を保持し、`lisp_context_switch`(`src/lisp.c:7502`)が`from`へ退避・`to`から復元
  するcopy-in/copy-out方式で切替を行う。この切替は`%process-resume`/`%process-suspend`
  (`src/lisp.c:6785`以降、`lisp_current_process_stack`/`lisp_process_context_pool[16]`/
  `lisp_process_main_context`)からのみ呼ばれ、**ユーザー明示操作(process-resume呼び出し)以外
  では発生しない**(VM yieldフック(milestone106)はセルフテスト専用で本番の自動プリエンプション
  には使われていない)。つまり画面バッファの切替も「明示的なプロセス切替のタイミングでのみ
  一括入替すればよい」という既存設計とそのまま整合する。
- `lisp/os.lisp:174-194`にmilestone118の見送り理由が明記されている: `ConIn`
  (`lisp_read_line`が使う、`src/lisp.c:1350`)と`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`
  (`g_text_input_ex`、Ctrl検知に使う、`src/lisp.c:5482`の`lisp_wait_for_double_ctrl`)が実機で
  同一キューを共有している可能性があり、ライブに両方を読むと入力を横取りするリスクがあった。
  `EFI_KEY_DATA`(`src/uefi.h:262`)は`EFI_INPUT_KEY`(`Key`)と修飾キー状態(`KeyState.
  KeyShiftState`)の両方を持つため、`lisp_read_line`を`ReadKeyStrokeEx`(Ex protocol)経由に
  一本化すれば、読み取り経路そのものを単一化してこのリスクを構造的に解消できる
  (2つの経路を同時に読むのではなく、そもそも1つに統一する)。
- `os:switch-process`(`lisp/os.lisp:248`)・`%print-process-menu`・`%read-console-expr`
  (`src/lisp.c:5725`)が既存の「プロセス一覧表示→番号選択→process-resume」を実装済みで、
  これはCtrl2回検知後のダイアログ本体としてそのまま再利用できる。

### 採用する設計方針

1. **OS予約行(先頭行)**: `LISP_SCREEN_STATUS_ROWS`(=1)を導入し、`lisp_screen_putc`の折り返し・
   スクロールと`lisp_screen_buffer_init`の初期カーソル位置を「行1始まり」に変更する(行0は
   通常の文字書き込み・スクロールの対象外にする)。新規Cビルトイン`%set-status-line`が行0へ
   直接書き込む(cols幅にpadding/truncate)専用の書き込み経路を追加する。行0の内容そのもの
   (プロセス名)はLisp側(`os.lisp`)の関心事とし、C側は「行0は特別」という区画だけを保証する。
2. **プロセス毎バッファ**: `LispScreenBuffer`を`LispProcessStack`に埋め込みフィールドとして
   持たせ(mainコンテキスト1個+プール16個=計17個、既存の`vm_stack`と同じ「毎プロセス固定
   静的確保」方針)、`lisp_context_switch`の既存のvm_stack/vm_sp/active_trap入替と同じ場所で
   `from`のバッファへ現在のグローバル`lisp_screen_buffer`を退避し、`to`のバッファをグローバル
   へ復元する。復元直後は実画面(ハードウェア)が退避前(=`from`)の内容を表示したままなので、
   `front`のみを無効化して次回`lisp_screen_flush`で全セルを再送出させる「強制全面再描画」
   フラグ(`force_full_redraw`)を追加する(既存milestone125のtouched機構にそのまま乗せる)。
   新規プロセスのバッファは初回resume時に`lisp_screen_buffer_init`と同じ初期化を1回行う。
3. **状態行の更新タイミング**: `os:process-resume`が実際に対象プロセスへ切替える直前、
   `os:process-suspend`でmainへ戻る直前に、Lisp側からそれぞれ対象/mainの名前で
   `%set-status-line`を呼ぶ(mainの表示名は固定文字列、例"REPL")。これによりC側は「誰が
   今アクティブか」を一切知らなくて済む(既存の`os:goto-xy`/`%set-cursor-position`と同じ、
   C=機構・Lisp=方針という分担を継承)。
4. **Ctrl2回押下のライブ検知**: `lisp_read_line`を`g_text_input_ex`経由の`ReadKeyStrokeEx`に
   一本化する(未検出時は既存の`ConIn->ReadKeyStroke`へフォールバックし、Ctrl検知機能自体を
   無効化するのみで読み取り自体は動作継続させる)。読み取りループ内で毎キーストロークの
   `KeyState.KeyShiftState`を見て`lisp_key_state_is_lone_ctrl`相当の判定を行い、既存
   `lisp_wait_for_double_ctrl`と同じウィンドウ定数内に2回連続でCtrl単体押下を検知したら、
   現在入力中の行を破棄し、グローバルフラグで「ダイアログ要求」を呼び出し元へ知らせる
   (`lisp_panic`のlongjmpとは違い、正常な戻り値ベースの一度限りのシグナルとする)。
5. **ダイアログの起動場所を限定**: このシグナルを実際に「プロセス選択ダイアログ起動」に
   変換するのはmain.cのトップレベルREPLループのみとする(既存`os:switch-process`をLisp側から
   呼び出す)。`%read-console-expr`など他の`lisp_read_line`呼び出し元では、同じシグナルは
   単に「その場の入力をキャンセルしてnilを返す」動作に留め、再帰的にダイアログを起動しない
   (ネスト・無限ループを避けるための明示的スコープ限定)。

## マイルストーン一覧

### フェーズM: OS予約行(先頭行)の導入

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 130 | 先頭行を予約領域化 | 完了 | `LISP_SCREEN_STATUS_ROWS`導入。`lisp_screen_putc`の折り返し・スクロール、`lisp_screen_buffer_init`の初期カーソル位置を行1始まりに変更(行0は通常経路の対象外)。C自己テストで、通常の書き込み・スクロールが行0を一切変更しないことを確認。 |
| 131 | `%set-status-line`ビルトイン | 完了 | 行0へ直接書き込み(padding/truncate込み)・touched化する新規Cビルトインを追加。`test/lisp/test-console.lisp`にエラーなく呼べることの確認を追加。 |

### フェーズN: プロセス毎画面バッファ化

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 132 | `force_full_redraw`フラグ | 未着手 | `LispScreenBuffer`に追加し、`lisp_screen_flush`冒頭でフラグが立っていれば全セルをtouched化してから通常のdiff処理に入るようにする(単一グローバルバッファのままC自己テストで検証)。 |
| 133 | `LispProcessStack`へ画面バッファ埋め込み | 未着手 | mainコンテキスト・プール各要素に`LispScreenBuffer screen`を追加。`lisp_context_switch`のvm_stack入替と同じ場所で退避/復元し、復元後`force_full_redraw`を立てる。未初期化(新規プロセス初回resume時)は`lisp_screen_buffer_init`相当の初期化を行う。新規C自己テストで「プロセスAが書いた内容はB実行中は見えず、Aに戻ると復元される」ことをback内容比較で確認する。既存のContext switch/Process suspend/resume系セルフテストの回帰が無いことも確認。 |
| 134 | 状態行とプロセス切替の連動 | 未着手 | mainの初期化時に固定文字列(例"REPL")で`%set-status-line`。`os:process-resume`/`os:process-suspend`の実行箇所(`lisp/os.lisp`)で、切替直前に対象/mainの名前を`%set-status-line`する。`make test`回帰確認に加え、既存`os:switch-process`を使ったQEMU対話で実際に画面内容・1行目表示が切り替わることを目視確認する(ヘッドレスでは視覚確認不可、既存フェーズC/H/続報と同方針)。 |

### フェーズO: Ctrl2回押下によるライブなプロセス切替ダイアログ

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 135 | `lisp_read_line`のEx protocol一本化 | 未着手 | `g_text_input_ex`経由の`ReadKeyStrokeEx`で読み取るよう変更(未検出時は既存`ConIn`経路へフォールバック)。通常文字/Backspace/Enterの挙動は変えない。`make test`で既存フィクスチャ全部の回帰が無いことを確認。Ctrl検知はまだ組み込まない。 |
| 136 | ライブなCtrl2回検知とシグナル化 | 未着手 | 135の読み取りループ内でCtrl単体押下を判定し、ウィンドウ内2回検知で入力行を破棄してワンショットのグローバルフラグを立てる。`%read-console-expr`側はこのフラグを見て単純にキャンセル(nil)扱いする。`make test`回帰確認(ヘッドレスではCtrl自体を送出できないため、この部分の実挙動確認はQEMU対話/実機に委ねる。milestone118と同じく検証できないリスクであることを明記する)。 |
| 137 | メインREPLループでのダイアログ起動 | 未着手 | `main.c`のREPLループが136のフラグを検知したら、現在の入力表示を後始末した上で`os:switch-process`相当のLisp呼び出しを行い、選択されたプロセスへ実際に切り替わることを確認する(133/134の画面切替・状態行更新が実際に発生する)。既存の`os:switch-process`コマンドも残し、Ctrl2回はその起動手段が1つ増える形にする。QEMU対話/実機での目視確認に委ねる。 |

## スコープ外として明記する項目

- Ctrl2回検知を`%read-console-expr`等、main REPLループ以外の`lisp_read_line`呼び出し元からの
  ダイアログ起動トリガーとして使うこと(ネスト回避のため、その場のキャンセルのみに限定)
- 実行中のVMバイトコード(ツリーウォークやbytecode実行中、`lisp_read_line`を呼んでいない間)への
  真のプリエンプティブなCtrl2回割り込み(ポーリングループで待機中の場合のみが対象)
- 画面バッファ切替のアニメーション・スクロール履歴の保存
- OS予約行(先頭行)への複数プロセス名の同時表示・タブバー的なUI
- 色属性・複数ウィンドウ分割表示

## 検証方針

各マイルストーンで`make build`・`make test`(既存フィクスチャ全PASS、回帰無し)を確認する。
Ctrl2回押下の実際の検知・画面切替の見た目そのものはヘッドレス(`-display none`)環境では
検証できないため、既存フェーズC/H/続報と同様、個別のQEMU対話セッションまたはGUI付き
`make run`での目視確認に委ねる(milestone136/137で明記する既知の制約)。プロセス毎バッファの
内容分離自体(milestone133)はC自己テストでヘッドレスに検証可能。
