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
| 132 | `force_full_redraw`フラグ | 完了 | `LispScreenBuffer`に追加し、`lisp_screen_flush`冒頭でフラグが立っていれば全セルをtouched化してから通常のdiff処理に入るようにする(単一グローバルバッファのままC自己テストで検証)。 |
| 133 | `LispProcessStack`へ画面バッファ埋め込み | 完了 | mainコンテキスト・プール各要素に`LispScreenBuffer screen`を追加。`lisp_context_switch`自体ではなく`%process-resume`/`%process-suspend`(実際に「表示中のプロセス」が切り替わる場所)で退避/復元し、切替後`force_full_redraw`を立てる。未初期化(新規プロセス初回resume時)は`lisp_screen_buffer_init_blank`相当の初期化を行う。新規C自己テスト`lisp_process_screen_separation_selftest`(Cの関数をプロセスのthunkとして直接使い、同じCスタックフレーム内で%process-suspend呼び出しをまたいで実行を継続させるコルーチン的パターン)で、back内容の直接比較により(1)呼び出し元が書いた内容はプロセスB実行中は一切見えない、(2)Bが書いた内容は呼び出し元の画面に影響せずB自身のバッファにのみ残る、(3)suspend/resume・自然終了のいずれの経路でも呼び出し元の画面が正しく復元され続けることを確認した。実装時に、thunkが`%process-suspend`を一度も呼ばず自然終了した場合、終了直前にtarget自身が表示していた内容が`target`側の`screen`へ保存されずに古い(直前suspend時点の)内容のまま残るという非対称バグを自己テストで発見し、`%process-resume`側で`lisp_process_thunk_finished`時のみ追加のコピーを行うよう修正して解消した。既存のContext switch/Process suspend/resume系セルフテスト・`make test`全29ファイルの回帰が無いことも確認した(1/3回、既知のbug#3系非決定的`SetCursorPosition`失敗によるtest-os panicを観測したが、再実行2回は連続でALL PASSしており、本マイルストーンの実装起因ではなくQEMU/OVMFタイミング依存の既知の再発ガチャと判断)。 |
| 134 | 状態行とプロセス切替の連動 | 完了 | 計画では`os:process-resume`/`os:process-suspend`(`lisp/os.lisp`)側で切替直前にLisp側から`%set-status-line`を呼ぶ設計だったが、実装時に次の技術的な矛盾を発見した: `%process-resume`はthunk自身のクロージャの`env`フィールドをそのまま`process-local-variable`(milestone113)/`os:inspect-process`(milestone114)の参照先として使う実装であり、`os:process-resume`側でthunkを別の`(lambda () (%set-status-line ...) (funcall thunk))`でラップすると、C側が捕捉する`env`が元のthunkの定義時レキシカル環境ではなくラッパー自身の環境に変わってしまい、既存の`process-local-variable`/`os:inspect-process`のテストが壊れる。この矛盾はLisp側では解消できない(ラッパーに元のthunkと同じ`env`を後付けする手段がLispコードには公開されていない)ため、C側のみで実装する方式に変更した: `lisp_builtin_process_resume`内で対象プロセスが初めてresumeされる瞬間(`screen_fresh_start`)にだけ、対象プロセス自身の`name`スロットを状態行(行0)へ`lisp_screen_set_status_line`で直接書き込む。以降はsuspend/resumeの度に`screen`構造体全体がそのまま退避/復元される(milestone133)ため、行0の内容もそのまま持ち越され、明示的な再設定は不要(`os:process-suspend`側の変更は無し)。mainコンテキスト自身の状態行は`main.c`の起動シーケンス内で固定文字列`"REPL"`を1回だけ設定する。新規C自己テスト`lisp_process_status_line_selftest`で、`%make-process`(`lisp_builtin_make_process`)経由で生成した実プロセスを初めてresumeした直後に、`target->screen.back[0]`の内容がプロセス自身の`name`スロットの文字列と一致すること(自然終了後もmilestone133の保存修正により持ち越されることの再確認も兼ねる)をback内容の直接比較で確認した。`make test`を複数回実行し既存フィクスチャ全29ファイルの回帰が無いことを確認した(新規自己テスト`Process status line self-test`はこの複数回の実行中に一度もFAILせず全てPASSした)。実行中、既知のbug#3系非決定的`SetCursorPosition`失敗によるpanicがある回では複数ファイル・別の回では1ファイルという形で発生ファイルを変えながら再現したが、いずれもmilestone133までで確認済みの`lisp_screen_flush`のQEMU/OVMFタイミング依存panicと同一のメッセージであり、再実行のたびに発生箇所が変わることから本マイルストーンの実装起因ではなく既存の非決定的事象と判断した。ヘッドレスでは実際の画面上の見た目切り替わりを検証できないため、QEMU対話/実機での目視確認は既存フェーズC/H/続報と同方針で今後に委ねる。 |

### フェーズO: Ctrl2回押下によるライブなプロセス切替ダイアログ

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 135 | `lisp_read_line`のEx protocol一本化 | 完了 | `lisp_read_line`冒頭を`g_text_input_ex != (void *)0`で分岐し、検出済みなら`g_text_input_ex->ReadKeyStrokeEx`で、未検出時のみ既存の`ConIn->ReadKeyStroke`で読み取るようにした。`EFI_KEY_DATA.Key`は`EFI_INPUT_KEY`と同一レイアウト(`ScanCode`/`UnicodeChar`)であることを確認済みのため、どちらの経路で読んでも`unicode_char`という共通のローカル変数に落とし込み、以降のEnter判定(`L'\r'`)・Backspace判定(`8`)・制御キー無視(`UnicodeChar==0`)・エコー表示・通常文字のバッファ格納ロジックは分岐せず完全に共通のまま(挙動を一切変えない)にした。`KeyState`(修飾キー状態)はこの段階では一切参照せず、Ctrl検知の組み込みはmilestone136に見送った。既存のUEFIプロトコル関数ポインタをスタブ化する自己テストパターンがコードベースに存在しないこと、および本マイルストーンの計画自体が`make test`の既存フィクスチャ回帰確認のみを求めていることから、新規C自己テストは追加しなかった。`make test`を2回連続実行し、通算29ファイル×2回のうち一部の回でTIMEOUTを観測したが、(1)発生ファイルの組み合わせが1回目(test-clos/test-console/test-locals-region)と2回目(test-call-dispatch/test-clos/test-defun-compile)で完全に入れ替わっている、(2)panicを伴うTIMEOUTは全て既知のbug#3系`Lisp panic: lisp_screen_flush: SetCursorPosition failed`と同一メッセージで、しかも本マイルストーンで変更した`lisp_read_line`が実際のテストファイル内容に対して呼ばれる前の起動時Cセルフテスト列(「Process local-variable self-test」直後の「Process screen separation self-test」内部のflush)で発生している、(3)test-call-dispatchの1回のみpanicメッセージ無しの単純な300秒ハングだったが、ハング地点である`lisp_ctrl_wait_classify_selftest`はタイマー・WaitForEvent等のブロッキングI/Oを一切持たない純粋な計算のみの関数であり、コード上ハングする経路が存在しないことを確認した——ことから、いずれも本マイルストーンの実装起因ではなく既存のQEMU/OVMFタイミング依存の非決定的事象と判断した。`make build`はエラー・警告無しで一度で成功した。 |
| 136 | ライブなCtrl2回検知とシグナル化 | 完了 | `lisp_read_line`(milestone135でEx protocolへ一本化済み)の`g_text_input_ex`側の読み取り分岐に、`lisp_key_state_is_lone_ctrl`(milestone116)によるCtrl単体押下判定を追加した。1回目の検知で`CreateEvent(EVT_TIMER)`/`SetTimer(TimerRelative, LISP_DOUBLE_CTRL_WINDOW_100NS)`(0.5秒、新規定数)の使い捨てタイマーを起動し、既存のノンブロッキング・ビジーポーリングループの各周回冒頭で新規`EFI_CHECK_EVENT`(`src/uefi.h`、既存の`void *CheckEvent`フィールドを同じ位置・同じポインタサイズのまま型だけ厳密化。ABIレイアウトは不変)経由でタイマー満了を確認する(既存の`lisp_wait_for_double_ctrl`(milestone117)のブロッキングWaitForEvent方式とは別に、非ブロッキングのポーリング方式を新設した。呼び出し元が無かった`lisp_wait_for_double_ctrl`自体はそのまま変更せず残した)。ウィンドウ内に2回目のCtrl単体押下が来たら入力行(`input_length`)を破棄し、`lisp_read_line`自体をEnterと同様に即座に終了させ、ワンショットのグローバルフラグ`lisp_double_ctrl_detected`(`src/lisp.h`/`src/lisp.c`、`lisp_process_thunk_finished`と同じ「立てる→消費側が読んだら即クリア」規約)を立てる。ペンディング中に通常のEnterで終了した場合や、ウィンドウが尽きた場合も含め、タイマーイベントは全経路で確実に`CloseEvent`されるようにした。`%read-console-expr`(`lisp_builtin_read_console_expr`)側で`lisp_read_line`から返った直後にこのフラグを確認し、立っていれば消費(0にクリア)して`nil`を返すキャンセル扱いに変更した(この関数は「1式返す必要がある」呼び出し規約のため、REPL本体のような`continue`による再プロンプトはできない)。milestone137で予定するmain REPLループでの実際のダイアログ起動への変換は行わず、あくまで信号を立てる/消費するところまでに留めた。合わせて、milestone135で`lisp_read_line`がEx protocolへ一本化されたことで古くなっていた`lisp_builtin_read_console_expr`直前のコメント(「ConIn以外に一切触れない」という記述)を、実際には単一経路への統一によってキー入力キュー競合リスクが構造的に解消されている、という現状に合わせて更新した。ヘッドレスではCtrl自体を送出できないため、実際の2回押下検知そのものの動作確認はQEMU対話/実機に委ねる(milestone118と同じ既知の制約)。`make build`はエラー・警告無しで一度で成功した。`make test`を2回連続実行し、通算29ファイル×2回で発生したTIMEOUT(1回目5ファイル、2回目1ファイル)は全て既知のbug#3系`SetCursorPosition failed`パニック(一部はパニックメッセージ自体がflushされずに無音でハングした)であり、いずれも本マイルストーンで変更した`lisp_read_line`のCtrl検知分岐が実際に実行される前の起動時Cセルフテスト列内で発生していることをログで確認した。発生ファイルの組み合わせが2回の実行で完全に入れ替わっていること(1回目でTIMEOUTした5ファイルは2回目に全てPASSし、2回目のtest-assemblerは1回目にPASSしていた)から、既存のQEMU/OVMFタイミング依存の非決定的事象であり本マイルストーンの実装起因ではないと判断した。 |
| 137 | メインREPLループでのダイアログ起動 | 完了 | `main.c`のトップレベルREPLループ(`lisp_read_line`呼び出し直後、`input_length == 0`の空行チェックより前)に、`lisp_double_ctrl_detected`(milestone136)を確認する分岐を追加した。立っていれば消費(0にクリア)し、`lisp_read_from_buffer("(os:switch-process)")`+`lisp_eval_toplevel`で既存の`os:switch-process`(`lisp/os.lisp`、プロセス一覧表示→`%read-console-expr`での選択→`os:process-resume`)をそのまま呼び出して`continue`する。`os:`パッケージ修飾子はリーダーが`*package*`に関わらず常に解決するため、現在のパッケージ状態を考慮する必要は無い。この呼び出しは既存の「ユーザーが`(os:switch-process)`と直接入力した場合」と全く同じ経路(`lisp_read_from_buffer`+`lisp_eval_toplevel`)であり、新規のC↔Lisp呼び出しヘルパーは追加していない。`os:process-resume`(`%process-resume`)はブロッキングであり、切替先プロセスが`suspend`または終了するまでこの呼び出しからは戻らない(戻ってきた時点で制御が再びこのREPLループに戻ったことを意味する)。milestone133/134の画面バッファ退避・復元・`force_full_redraw`は`lisp_context_switch`/`%process-resume`/`%process-suspend`側で既に行われるため、`continue`後の通常のループ先頭(プロンプト表示+`lisp_screen_flush`)がそのまま全面再描画を担い、この分岐自体に追加の画面処理は不要であることを確認した。`%read-console-expr`など他の`lisp_read_line`呼び出し元は136の設計通りキャンセル(nil)止まりのままで、ダイアログの実起動はこのメインREPLループの分岐だけに限定されている(ネスト回避)。既存の`os:switch-process`コマンドをユーザーが直接入力する経路も変更せず残した。ヘッドレスではCtrl自体を送出できないため、実際のCtrl2回押下→ダイアログ起動→プロセス切替という一連の動作そのものの目視確認はQEMU対話/実機に委ねる(milestone118と同じ既知の制約)。`make build`はエラー・警告無しで一度で成功した。`make test`を2回連続実行し、通算29ファイル×2回で発生したTIMEOUT(各回2ファイル、1回目: test-clos/test-defun-compile、2回目: test-hash-code/test-numeric-tower)は全て既知のbug#3系`SetCursorPosition failed`パニックであり、いずれも本マイルストーンで変更したREPLループのCtrl検知分岐が実際に実行される前の起動時Cセルフテスト列内で発生していることをログで確認した。発生ファイルの組み合わせが2回の実行で完全に入れ替わっていることから、既存のQEMU/OVMFタイミング依存の非決定的事象であり本マイルストーンの実装起因ではないと判断した。 |

### フェーズP: 実運用フィードバックによる追加マイルストーン(ロードマップ完了後)

milestone137完了(130〜137のロードマップ完了)後、実際の使用感を踏まえてユーザーから追加要望が
出た(原文):

> なるほど。タイミングがシビアなので、Ctrlを1回入力したら画面上部のOSが表示する欄に C と
> 表示するようにしたい
> 画面上部の左側3つはシステムが使う場所で予約し、その一番左にCを出すようにする
> プロセス名は4番目から表示をするようにして
>
> また、CTRLは○秒間に2回ではなく、1回目が押されて、キーが離れてそのつぎに押されたらにする
> Ctrlを1回目押してキーが離れたら左上にCを表示する。その状態でもう一度Ctrlを押したら切り替え
> ダイアログを表示する。
> Cが表示中に他のキーが押されたらCの表示を消す

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 138 | Ctrl検知の押下ベース化とOS予約列 | 完了 | milestone136の時間ウィンドウ(0.5秒タイマー)方式を廃止し、期限のない「armed状態」機械に変更した: `lisp_read_line`内の`ctrl_wait_pending`/タイマー(`CreateEvent`/`SetTimer`/`CheckEvent`/`CloseEvent`)を、単純な`int ctrl_armed`ローカル変数へ置き換えた。1回目のCtrl単体押下(`lisp_key_state_is_lone_ctrl`)でarmedへ入り、期限なく2回目を待つ。armed中に2回目のCtrl単体押下が来たら確定(入力行破棄+`lisp_double_ctrl_detected`を立てて終了、milestone136までと同じ終端動作)。armed中にCtrl単体以外の何らかのキーが来た場合はarmedを解除するだけで、そのキー自体は捨てずに通常どおり処理する(Enter/Backspace/通常文字いずれでも成立)。UEFIの`ReadKeyStrokeEx`は離鍵イベントを区別できず個々の押下イベントのみを報告するため、「1回目の押下」→「2回目の押下」という2つの別イベントが届くこと自体が、キーが一度離されてから再度押されたことを物理的に意味する、という解釈のもと「期限なく待ち、他のキーで解除する」という設計へ落とし込んだ(ユーザーへの確認は行わず、この解釈を本マイルストーンの記述として明記する判断とした)。armed状態を画面上に見せるため、新規`lisp_screen_show_ctrl_indicator`(`src/lisp.c`/`src/lisp.h`)を追加し、状態行左端(列0)に'C'を表示/消去する。`lisp_read_line`がキー入力を待ってブロックしている間は`lisp_screen_flush`が一切呼ばれない(milestone125以降のキーecho説明と同じ理由)ため、back bufferへ書くだけでは画面に反映されない。そのため既存のキーecho(`lisp_screen_track_echoed_wstring`)と同じ「バッファ非経由、ConOutへ直接描画」方式を採用し、入力行編集中の実カーソル位置(`cursor_col`/`cursor_row`、echoが追従管理)を書き込み前後で`SetCursorPosition`により一時的に(0,0)へ退避・復帰させる形にした。同時にユーザー要望どおり状態行の左3列(`LISP_SCREEN_STATUS_RESERVED_COLS`=3、新規定数)をシステム予約領域とし、`lisp_screen_set_status_line`(milestone131)が書き込むプロセス名の開始列を0から3へ変更した(予約領域側のtouched範囲を無条件に上書きしないよう、`lisp_screen_touch_cell`経由のtouch化に変更)。この列オフセット変更に伴い、milestone134の自己テスト`lisp_process_status_line_selftest`が列0基準で比較していた箇所を列3基準(`LISP_SCREEN_STATUS_RESERVED_COLS + i`)へ修正した。不要になった`LISP_DOUBLE_CTRL_WINDOW_100NS`定数は削除した(`EFI_CHECK_EVENT`型・`CheckEvent`フィールド自体は他のUEFI構造体レイアウトを保つため`uefi.h`にそのまま残し、単純に呼び出し箇所が無くなった状態にした)。`make build`はエラー・警告無しで一度で成功した。`make test`を2回連続実行し、既存フィクスチャ全29ファイルの回帰が無いことを確認した(新規/修正した`Process status line self-test`は両実行を通して一度もFAILせず全てPASS)。1回目はtest-clos/test-osの2ファイルがTIMEOUTしたが、いずれも既知のbug#3系`Lisp panic: lisp_screen_flush: SetCursorPosition failed`と同一メッセージであり、本マイルストーンで変更した`lisp_read_line`のCtrl検知分岐が実際のテストファイル内容に対して呼ばれる前の起動時Cセルフテスト列内(Process suspend/resume〜status line self-test付近)で発生していることをログで確認した。2回目は再実行しTIMEOUTなしの`=== ALL PASS ===`だったため、1回目の事象は本マイルストーンの実装起因ではなく既存のQEMU/OVMFタイミング依存の非決定的事象と判断した。ヘッドレスではCtrl自体を送出できないため、実際のCインジケータ表示・押下ベースの2回検知そのものの目視確認はQEMU対話/実機に委ねる(milestone118と同じ既知の制約)。 |
| 138続報3 | Ctrl方式の廃止とF2単発押下方式への変更 | 完了 | milestone138完了後、実機でユーザーからCtrlが検知できないという報告を受け、`os:text-input-ex-found-p`(既にt)・`os:key-debug-log`(`ReadKeyStrokeEx`の生イベントをリングバッファに記録する診断ビルトイン、当初32件→実機検証で自分自身の入力に上書きされることが判明し128件へ拡大)の2つの診断ツールを追加投入して段階的に原因を切り分けた。実機で`(os:key-debug-log)`を2度呼んでもらったログはいずれも診断コマンド自身の打鍵イベントのみで占められ、事前にCtrlを押していたにもかかわらず`shift-state`にCtrlビット(0x4/0x8)が立ったエントリが一件も無く、`EFI_SHIFT_STATE_VALID`(0x80000000)自体は常に立っていた(=修飾キー状態の報告機構自体は機能している)。これにより「(a)`LocateProtocol`が別ハンドルのEx protocolを返している」「(b)ファームウェアが`EFI_SHIFT_STATE_VALID`を立てていない」の2仮説を排除し、「(c)このファームウェアの`ReadKeyStrokeEx`は修飾キー単体(`ScanCode`/`UnicodeChar`ともに0)の押下に対してキーストロークイベント自体を生成しない」という、UEFI `SimpleTextInputEx`実装の一部に見られる既知の制約が確定した。この結論をユーザーに説明し、`AskUserQuestion`で対応方針(実機でのCtrl検知をあきらめずさらに調査/別のキー組み合わせへ変更/他機種での検証)を確認したところ「別のキー組み合わせに変更する」との回答を得た。続けてどのキーにするか(Ctrl+P/別の特定の文字キー/F2等の修飾キー不要のファンクションキー)・何回押下にするか(現行の2回押下を維持/1回押下で即時起動)を確認し、「F2等のファンクションキーへ変更」「1回押下で即時ダイアログ起動」との回答を得た。この決定に基づき、`SCAN_F2`(`=0x0C`、UEFI仕様Appendix Bのスキャンコード表、`uefi.h`に新規定義)を採用し、milestone136/138のCtrl検知機構(`ctrl_armed`ローカル変数・`lisp_screen_show_ctrl_indicator`によるCインジケータ描画)を全て削除した。`lisp_read_line`(`src/lisp.c`)は、Ex protocol分岐・ConIn(base protocol)フォールバック分岐の両方で共通に`scan_code`を取り出し、`scan_code == SCAN_F2`なら入力行を破棄してワンショットのグローバルフラグ(`lisp_double_ctrl_detected`から`lisp_process_switch_requested`へ改名、Ctrl固有の名前をキー中立な名前に変更)を立てて即座に`break`する処理へ一本化した。`ScanCode`は`KeyState`(修飾キー状態)を持たない`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`(ConIn、milestone135以前の互換フォールバック経路)でも共通に報告されるため、旧Ctrl方式が`g_text_input_ex`未検出時は機能停止していたのに対し、F2方式はConInフォールバック時でも同様に機能する(uefi.hの`SCAN_F2`コメントに明記)。`lisp_screen_show_ctrl_indicator`は他の呼び出し元・専用セルフテストが無いことを確認した上で削除した。一方、`lisp_key_state_is_lone_ctrl`/`lisp_key_state_selftest`(現役の自己テストあり)、`lisp_ctrl_wait_classify`/`lisp_ctrl_wait_classify_selftest`/`lisp_wait_for_double_ctrl`(milestone136/138で意図的に残された非呼び出し済みコード、ABI/参照用)は、既に決着済みの過去の判断を再び開くスコープ拡大を避けるため今回は一切変更しなかった。`%read-console-expr`(`lisp_builtin_read_console_expr`)・`main.c`のトップレベルREPLループは`lisp_process_switch_requested`という新しい名前でmilestone136/137と全く同じ消費規約(前者はキャンセル/nil、後者は`(os:switch-process)`起動)のまま追従させた。`make build`はエラー・警告無しで一度で成功した。`make test`を実行し、29ファイル中`test-vector`が1回TIMEOUT(`Lisp panic: lisp_screen_flush: OutputString failed`、既知のbug#3系パニックで起動時Cセルフテスト列内で発生)したが、単体再実行では再現せずPASSしたため既存のQEMU/OVMFタイミング依存の非決定的事象と判断し、他の28ファイルは全てPASSした。ヘッドレスではF2キー自体を送出できないため、実際のF2単発押下→ダイアログ起動そのものの目視確認はQEMU対話/実機に委ねる(milestone118/138と同じ既知の制約)。'C'インジケータ表示のUXはこの変更で無くなった(armed/待機状態自体が存在しなくなったため)。 |

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
