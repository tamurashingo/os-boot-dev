; milestone 58 (統一トップレベル評価ドライバの導入) の動作確認用テスト。
; QEMU起動後、(load "test\test-toplevel-driver.lisp") でロードしてから
; (run-test-toplevel-driver) を呼び出し、t が返れば全項目成功。
;
; lisp_eval_toplevel(src/lisp.c)は「loadの1トップレベルform、またはREPLの1行」ごとに
; 1回だけ呼ばれる。そのためこのフラグの効果は、関数本体の中に置いたformには一切現れず
; (関数呼び出し時にはlisp_apply→lisp_evalが直接走る)、ファイルの直接のトップレベルに
; 置いたformにのみ現れる。以下のdefvar/defparameter/defmacro呼び出しはすべて
; このファイル自身のトップレベルに直接置き、その結果を後段のrun-test-*関数から確認する

; コンパイラ準備状態フラグは起動時、stdlib.lisp読み込み完了時点で既にtrueになっている
; (このテストファイル自身がloadされている時点でstdlib.lispは読み込み済みのため)
(defvar *m58-toplevel-var* (+ 40 2))            ; トップレベルのdefvar: 新経路で評価される
(defparameter *m58-toplevel-param* (+ 3 3 3))   ; トップレベルのdefparameter: 同上

(defmacro m58-double (x) (list '+ x x))         ; トップレベルのdefmacro: 恒久的にフォールバック

; マクロ呼び出しを含むトップレベルのdefvar。macroexpand-all→compile-exprを経由して
; m58-doubleが展開されてから実行されることを確認する
(defvar *m58-macro-result* (m58-double 10))

; トップレベルのdefun: フェーズ2(milestone60)完了までフォールバックし続けるが、
; 定義自体・呼び出し結果はどちらの経路でも変わらない
(defun m58-plain-defun (x) (+ x 1))

(defun run-test-toplevel-compiler-ready ()
  (eq (compiler-ready-p) t))

(defun run-test-toplevel-defvar ()
  (eq *m58-toplevel-var* 42))

(defun run-test-toplevel-defvar-no-overwrite ()
  ; 既にis_specialなので、関数本体内(=旧経路)からのdefvarは値を書き換えない
  (progn
    (defvar *m58-toplevel-var* 999)
    (eq *m58-toplevel-var* 42)))

(defun run-test-toplevel-defparameter ()
  (eq *m58-toplevel-param* 9))
; ^ (+ 3 3 3) = 9

(defun run-test-toplevel-macro-in-defvar ()
  (eq *m58-macro-result* 20))

(defun run-test-toplevel-defun-fallback ()
  (eq (m58-plain-defun 5) 6))

(defun run-test-toplevel-driver ()
  (and (run-test-toplevel-compiler-ready)
       (run-test-toplevel-defvar)
       (run-test-toplevel-defvar-no-overwrite)
       (run-test-toplevel-defparameter)
       (run-test-toplevel-macro-in-defvar)
       (run-test-toplevel-defun-fallback)))
