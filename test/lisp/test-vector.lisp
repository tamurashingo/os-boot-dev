; milestone 26 (汎用vector primitive: make-vector/svref/svset) の動作確認用テスト。
; (load "test\test-vector.lisp")でロードしてから(run-test-vector)を呼び出す。
; 範囲外indexのsvref/svsetはpanicでREPLを停止させるため、このファイルでは対象外とし
; QEMUのREPLへ直接打ち込んでpanicすることを目視確認する（documents/bare_metal_lisp.md参照）。

(defun run-test-vector-make-default-fill ()
  (let ((v (make-vector 3)))
    (and (eq (svref v 0) nil)
         (eq (svref v 1) nil)
         (eq (svref v 2) nil))))

(defun run-test-vector-make-with-fill ()
  (let ((v (make-vector 3 'x)))
    (and (eq (svref v 0) 'x)
         (eq (svref v 1) 'x)
         (eq (svref v 2) 'x))))

(defun run-test-vector-svset-roundtrip ()
  (let ((v (make-vector 3 'x)))
    (and (eq (svset v 1 'y) 'y)
         (eq (svref v 0) 'x)
         (eq (svref v 1) 'y)
         (eq (svref v 2) 'x))))

(defun run-test-vector-eq-identity ()
  (let* ((v (make-vector 2 0))
         (v2 v))
    (svset v 0 42)
    (eq (svref v2 0) 42)))

(defun run-test-vector ()
  (and (run-test-vector-make-default-fill)
       (run-test-vector-make-with-fill)
       (run-test-vector-svset-roundtrip)
       (run-test-vector-eq-identity)))
