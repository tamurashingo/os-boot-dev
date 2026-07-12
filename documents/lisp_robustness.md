# Lisp実行系の堅牢化マイルストーン

## 目的

このドキュメントは、[`boot.md`](./boot.md)（マイルストーン1〜11、UEFIブート〜最小Lisp REPL）と
[`init_lisp.md`](./init_lisp.md)（マイルストーン12〜16、`defun`/`macro`/文字列型/`load`）と
[`bare_metal_lisp.md`](./bare_metal_lisp.md)（マイルストーン17〜29、CommonLisp相当の機能拡張）で
完成した現在のBareMetalLispを土台に、実行系としての堅牢性——エラーから安全に復帰できるREPLと、
ヒープを使い切らずに長時間動作できるメモリ管理（GC）——を実現するための拡張群を、マイルストーン単位
で整理したものである。他の3文書と同様、実装の詳細設計は各マイルストーンに着手する際に別途行い、
本ドキュメントは全体の見取り図として保守する。

前提となる制約は`boot.md`/`init_lisp.md`/`bare_metal_lisp.md`と同じ（詳細は`CLAUDE.md`参照）:

- libc・既存のLispランタイム・ヒープアロケータは使わず、すべて`src`配下にスクラッチで書く。
- UEFIの構造体・プロトコルは、実際に使用するフィールドのみを手書きで定義する。
- テストフレームワークは無いため、各マイルストーンの検証はQEMU/OVMF上で実際に起動し、コンソール
  出力を目視確認することで行う。

## ファイル構成

`bare_metal_lisp.md`と同じ3ファイル構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／`src/main.c`）の
上に実装する。新規ソースファイルは想定しておらず、x86_64インラインアセンブリを含め全て`src/lisp.h`/
`src/lisp.c`内に書く。

## マイルストーン一覧

`bare_metal_lisp.md`のマイルストーン17〜29に続く番号で管理する。

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 30 | 自作setjmp/longjmp（レジスタ保存による大脱出機構） | ✅ 完了 | libcに依存せず、深いCの呼び出しスタックの奥底から特定の実行ポイントへ一気に脱出するコンテキストスイッチ機構を自作した。実装着手時に、当初この行が想定していた「Lisp内部のC呼び出し規約はgcc既定のSystem V AMD64 ABI」という前提が**誤り**であることが判明した。このプロジェクトのクロスコンパイラ（`x86_64-w64-mingw32-gcc`、`Makefile`に`-mabi`指定なし）は内部のCコードも含め全関数がデフォルトでMicrosoft x64呼び出し規約になる（検証用関数をコンパイルして生成された`.s`を確認し確定させた：引数が`%ecx`/`%edx`/`%r8d`/`%r9d`に渡る、全関数に`.seh_proc`等のWindows SEH用プロローグディレクティブが付く）。このABIでは`rdi`/`rsi`もSystem Vとは異なり呼び出し先保存レジスタに含まれるため、当初のユーザー指定8フィールド仕様（`rbx`/`rbp`/`rsp`/`r12`〜`r15`/`rip`、64byte）ではsetjmp呼び出し元が`rdi`/`rsi`にローカル変数を保持していた場合にlongjmp後に値が壊れる可能性があると判断し、ユーザーに確認のうえ`rdi`/`rsi`を追加した10フィールド・80byteの`lisp_jmp_buf`（`src/lisp.h`）に拡張した。`lisp_setjmp`/`lisp_longjmp`（`src/lisp.c`）は通常のC関数ではなく、ファイルスコープの生アセンブリ（関数本体の外に置く`__asm__("...")`ブロック）で実装した——通常のC関数にインラインアセンブリを埋め込む方式では、GCCが自動生成するプロローグ（`push rbp; mov rsp,rbp`等）がasm実行前に呼び出し元のレジスタ値を汚染してしまい、x86_64のGCCには信頼できる`naked`属性が無いため、この回避策（musl等のlibcと同じ手法）を採用した。`lisp_setjmp`宣言には`__attribute__((returns_twice))`（2回目の“返り”をまたいだ不正な最適化をGCCに防がせる標準属性）、`lisp_longjmp`には`__attribute__((noreturn))`を付与した。検証は`src/main.c`の`EfiMain`に恒久的な起動時自己テストを追加して行った：3段のネスト関数呼び出し（各段でスタックを消費するローカル配列を経由）の奥から`lisp_longjmp`を呼び、`lisp_setjmp`が戻り値0（1回目）→1（2回目、longjmp経由）で正しく“2回返る”こと、呼び出し元のローカル変数（マーカー値）が破壊されていないことをQEMU/OVMFのヘッドレス起動（socket-serial）で確認した。自己テスト後もREPL自体（`(+ 1 2)`→`3`、`(list 1 2 3)`→`(1 2 3)`）が正常動作することも確認し、レジスタ/スタック破壊が無いことを裏付けた。milestone31（REPLのエラー復旧）実現の前提となる基盤が完成した |
| 31 | REPLエラー復旧トラップとpanicの安全化 | ✅ 完了 | `src/main.c`の`EfiMain`のREPL`for(;;)`ループ先頭（プロンプト表示直前）に`lisp_jmp_buf repl_trap`を置き、そのポインタを新設のグローバル`lisp_active_trap`（`src/lisp.h`/`src/lisp.c`）に設定した上で毎ループ`lisp_setjmp(&repl_trap)`を呼ぶ。`lisp_panic`（`src/lisp.c`）は、エラーメッセージ出力後に`lisp_active_trap`が設置されていれば`lisp_longjmp(lisp_active_trap, 1)`でそこへ復帰し（トラップ未設置の起動処理中などは従来通り`for(;;){}`でハング）、通常経路とpanic復帰経路はどちらも同じ「プロンプト表示直前」に合流するため分岐コードは不要だった。fatal分類はユーザーに確認のうえ**固定容量資源の枯渇のみ致命的**と決定: `heap exhausted`／`package table exhausted`／`symbol table exhausted`の3箇所だけを新設の`lisp_panic_fatal`（常に`for(;;){}`、longjmpしない）に置き換え、型アサーション・reader/evalエラー・bignum overflow・printerのunknown type・load/sleepのUEFI呼び出し失敗など残り約27箇所の`lisp_panic`呼び出しは無変更のままREPL復帰対象とした（これらは1回の呼び出し限りの失敗でグローバル状態を破壊しないため）。**既知の制約（対応せず明記のみ）:** `let`/`let*`の動的変数退避・復元は`lisp_eval_progn`が通常のC関数リターンで戻った後のコードで行われるため、`lisp_panic`からの`lisp_longjmp`はCコールスタックを一気に飛び越えてこの復元コードを経由しない。したがって`(let ((*x* 2)) (car 5))`のように動的変数を再束縛した本体内でpanicが起きると、`*x*`はshadowされた値のまま復元されず残る（QEMU上で実際に確認済み）。これは本ドキュメントの「スコープ外として明記する項目」にある完全なcondition system非対応と同種の制約であり、milestone32/33で再考する。QEMU/OVMFヘッドレス起動（socket-serial、CODE+VARS両pflash）で型エラー・unbound variableいずれのpanicもハングせず次のプロンプトへ復帰し、その後の`(+ 1 2)`等の評価が正常に動作すること、milestone30の起動時自己テストが引き続き1回だけ正常表示されることを確認した |
| 32 | 全オブジェクト追跡アロケータへの拡張 | ✅ 完了 | `LispCons`/`LispSymbol`/`LispClosure`（`src/lisp.c`）の先頭に`LispObject gc_next`（確保順の全オブジェクト追跡リストへの次ポインタ）・`int gc_marked`（milestone33のmark&sweep用mark bit、本milestoneでは常に0）の2フィールドを追加した。mark bitの置き場所（構造体へ埋め込むか別配列で持つか）はユーザーに確認のうえ、既存の`str_data`/`is_float`等と同じ「構造体へ直接フィールドを足すescape hatch」パターンに揃えて構造体へ埋め込む方針に決定した（ヒープ内オブジェクト数が動的に増える中で別配列のサイズ変更・検索コストを避けるため）。3構造体とも`gc_next`/`gc_marked`を必ず先頭2フィールド・同じ型順で揃えたことで、新設の`lisp_alloc_tracked(size, tag)`ヘルパー（`lisp_alloc`の直後に追加）がオフセット0（`gc_next`）とオフセット`sizeof(LispObject)`（`gc_marked`）を型を問わず直接読み書きし、確保直後に`lisp_all_objects_head`（新設のグローバル、全追跡オブジェクトの先頭）へ連結できるようにした。既存コードは構造体フィールドを名前でアクセスしており位置指定の構造体リテラルが無いため、先頭へのフィールド追加によるオフセットずれの破損は発生しない。対象は`alloc_cons`と、`lisp_make_closure`/`lisp_make_builtin`/`lisp_make_macro`/`lisp_make_string`/`lisp_make_float`/`lisp_make_bignum`/`lisp_make_vector`（`LispClosure`本体の確保のみ）、`lisp_intern_in_package`/`lisp_make_uninterned_symbol`（`LispSymbol`確保）の合計9箇所で、いずれも`lisp_alloc(sizeof(...))`を`lisp_alloc_tracked(sizeof(...), TAG)`に置き換えた。`LispClosure`が内部で持つ2次バッファ（文字列の`str_data`・bignumの`big_digits`・vectorの`vec_data`）は独立した`LispObject`ではなく所有者の`LispClosure`経由でのみ到達可能なため、本milestoneでは意図的に追跡対象外とした（所有者の生死と一緒に扱う方針で、その具体的な回収方法はmilestone33で検討する）。検証はQEMU/OVMFヘッドレス起動（socket-serial）で`(cons 1 2)`・`(defvar *y* 10)`・文字列リテラル・`3.14`・巨大整数（bignum化）・`(make-vector 3 0)`と`svref`・`(+ 1 2)`・`(list 1 2 3)`が全て従来通り正しく評価・表示されることを確認し、構造体レイアウト変更によるオブジェクト破損が無いことを確認した。加えて`(car 5)`によるpanic後も次のプロンプトへ正常に復帰し`(+ 1 2)`が再度正しく評価できること（milestone31の回帰確認）も確認した |
| 33 | マーク＆スイープGCの実装と固定スロット再利用 | 未着手 | milestone32の全オブジェクト追跡を土台に、マーク＆スイープ方式のGCを実装する。マークフェーズは`global_env`（alist構造）を起点に、各シンボルの動的変数値（`is_special`なシンボルの`value`）、各パッケージテーブルに登録された全intern済みシンボル、`t`等の特殊シンボルキャッシュ、非局所脱出用シグナル（`lisp_return_tag`/`lisp_return_value`）から再帰的にマークする。スイープフェーズは未マークのオブジェクトをFREE状態にし、`lisp_alloc`がバンプ確保の前にFREE化済みスロットを再利用できるようにする（同一サイズのスロット再利用のみを対象とし、コンパクションは行わない）。評価中のCコールスタック上の一時的な中間結果の保護方法（GC起動タイミングの制約、あるいは限定的なスタックスキャン）が主要な難所であり、着手時に別途設計する |

## スコープ外として明記する項目

- 世代別GC・インクリメンタルGC・コンパクション（オブジェクトの移動）は対象外。固定サイズスロットの
  FREE化と再利用のみに限定する。
- CLの完全なcondition system（`signal`/`handler-case`/`restart`等）は対象外。REPLレベルで安全に次の
  プロンプトへ復帰できることのみを目標とする。
- マルチスレッド・並行GCは対象外（このLispはシングルスレッド実行前提）。
- Cコールスタック上の一時参照を正確に追跡する「正確なGC」は対象外とし、ルート集合を`global_env`・
  シンボルテーブル・パッケージ・特殊シンボルキャッシュ・非局所脱出シグナルに限定した保守的な方針を
  基本とする（評価中の中間値の扱いは各マイルストーン着手時に設計する）。

## 検証方針

テストフレームワークは無いため、他の3文書と同様、各マイルストーンの検証はQEMU/OVMF上での目視確認と
`test/lisp/`配下の`run-test-xxx`形式の自動テストフィクスチャを併用して行い、既存マイルストーン
17〜29の回帰確認も合わせて実施する。
