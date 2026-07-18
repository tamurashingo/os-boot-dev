; milestone98(print-object総称関数・write-string・princ)の動作確認用テスト。
; (load "test\test-print-object.lisp")でロードしてから(run-test-print-object)を呼び出す。
; defun/lambdaのbodyは単一form(既存のprogn gotcha)なので、複数の副作用を持つ本体は
; 明示的にprognで包む(milestone96のclos-move-pointと同型)。
; 個別QEMU対話で確認する項目(documents/lisp_print_object.mdの検証方針参照):
; - オーバーライド無しinstanceの表示が従来と同じ#<name instance>のままであること
; - オーバーライドmethod内のwrite-string/princ呼び出しが実際にコンソールへ出力されること
; - (print-object 5)(非instance直接呼び出し)がlisp_assert_instanceによりpanicすること
; これらはこのファイルでは対象外とする

(defvar *print-object-flag* nil)

(defclass po-plain () (x))

; オーバーライド無し: 既定methodが呼ばれ、instance自身をpanic無しで返す
(defun run-test-print-object-default-no-override ()
  (let ((obj (make-instance 'po-plain)))
    (eq (print-object obj) obj)))

(defclass po-shape () (color))
(defclass po-circle (po-shape) (radius))

(defmethod print-object ((s po-circle))
  (setq *print-object-flag* 'circle-override)
  (write-string "circle")
  s)

(defmethod print-object ((s po-shape))
  (setq *print-object-flag* 'shape-override)
  (write-string "shape")
  s)

; オーバーライドが既定methodの代わりに実際に呼ばれること
(defun run-test-print-object-override-fires ()
  (progn
    (setq *print-object-flag* nil)
    (let ((c (make-instance 'po-circle)))
      (print-object c)
      (eq *print-object-flag* 'circle-override))))

; 継承経由のフォールバック: po-circle専用methodが無いと親po-shape側が呼ばれる
; (specificity比較、test-clos.lispの単一dispatchパターンと同型)
(defun run-test-print-object-specificity-fallback ()
  (progn
    (setq *print-object-flag* nil)
    (let ((s (make-instance 'po-shape)))
      (print-object s)
      (eq *print-object-flag* 'shape-override))))

; write-stringは文字列を要求し、常にtを返す
(defun run-test-print-object-write-string-returns-t ()
  (eq (write-string "hello") t))

; princはCommonLisp同様、引数自身を返す(文字列以外の任意のLispObjectでも良い)
(defun run-test-print-object-princ-returns-arg ()
  (and (eq (princ 42) 42)
       (eq (princ 'foo) 'foo)))

; run-test-print-object-default-no-override/override-fires/specificity-fallbackは
; print-object経由でコンソールへ改行無しの文字列を書き込む副作用を持つため、末尾に
; write-line ""で改行を1つ流し込み、テストハーネスが書き出す"RESULT ..."行と
; 混ざらないようにする(scripts/run_test.pyは行頭が"RESULT "の行を探すため)
(defun run-test-print-object ()
  (progn
    (let ((result (and (run-test-print-object-default-no-override)
                        (run-test-print-object-override-fires)
                        (run-test-print-object-specificity-fallback)
                        (run-test-print-object-write-string-returns-t)
                        (run-test-print-object-princ-returns-arg))))
      (write-line "")
      result)))
