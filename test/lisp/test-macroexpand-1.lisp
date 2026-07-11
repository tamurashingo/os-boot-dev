; milestone 21 (macroexpand-1 / *macroexpand-hook*) の動作確認用テスト。
; QEMU起動後、(load "test\test-macroexpand-1.lisp") でロードしてから
; (run-test-macroexpand-1) を呼び出し、t が返れば全項目成功。
; equal/listが無いので、展開結果の構造はcar/cdr/eqを手でたどって確認する。

(defmacro test-double-macro (x) (cons '+ (cons x (cons x nil))))

(defun run-test-macroexpand-1-basic ()
  (let ((expansion (macroexpand-1 '(test-double-macro 5))))
    (and (eq (car expansion) '+)
         (eq (car (cdr expansion)) 5)
         (eq (car (cdr (cdr expansion))) 5))))

(defun run-test-macroexpand-1-non-macro ()
  ; マクロ呼び出しでなければ、同じオブジェクトがそのまま返る(eqが真になる)
  (let ((form '(car (cons 1 2))))
    (eq form (macroexpand-1 form))))

(defun run-test-macroexpand-1-atom ()
  (eq 5 (macroexpand-1 5)))

(defvar *saved-macroexpand-hook* *macroexpand-hook*)

(defun run-test-macroexpand-1-hook-override ()
  ; *macroexpand-hook*を差し替えると、macroexpand-1自体の展開結果が変わることを確認する
  ; (defunの本体は単一formなので、複数の処理はprognでまとめる)
  (progn
    (setq *macroexpand-hook* (lambda (macro form env) 'overridden))
    (let ((result (macroexpand-1 '(test-double-macro 9))))
      (setq *macroexpand-hook* *saved-macroexpand-hook*)
      (eq result 'overridden))))

(defun run-test-macroexpand-1-hook-affects-eval ()
  ; *macroexpand-hook*の差し替えが、macroexpand-1経由だけでなく通常のマクロ呼び出しの
  ; 評価そのものにも反映されることを確認する
  (progn
    (setq *macroexpand-hook* (lambda (macro form env) (cons 'quote (cons 42 nil))))
    (let ((result (test-double-macro 5)))
      (setq *macroexpand-hook* *saved-macroexpand-hook*)
      (eq result 42))))

(defun run-test-macroexpand-1 ()
  (and (run-test-macroexpand-1-basic)
       (run-test-macroexpand-1-non-macro)
       (run-test-macroexpand-1-atom)
       (run-test-macroexpand-1-hook-override)
       (run-test-macroexpand-1-hook-affects-eval)))
