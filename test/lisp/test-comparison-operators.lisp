; milestone90(比較演算子の多引数対応: >/=/<=/>=/はCLと同じ可変長引数に対応)の動作確認用テスト。
; QEMU起動後、(load "test\test-comparison-operators.lisp")でロードしてから
; (run-test-comparison-operators)を呼び出し、tが返れば全項目成功。
;
; test-stdlib.lispのrun-test-stdlib-comparisonsは従来の2引数呼び出しの回帰を確認済みのため、
; ここでは3引数以上の可変長ケースと、既存の2引数呼び出しでは表面化しない/=の推移律の
; 非自明ケースに焦点を絞る。

(defun run-test-comparison-operators-lt-variadic ()
  (and (< 1 2 3)
       (not (< 1 3 2))
       (< 1)
       (< 1 2 3 4 5)))

(defun run-test-comparison-operators-gt-variadic ()
  (and (> 5 3 1)
       (not (> 5 1 3))
       (> 1)
       (> 5 4 3 2 1)))

(defun run-test-comparison-operators-le-variadic ()
  (and (<= 1 2 2 3)
       (not (<= 1 3 2))
       (<= 1)
       (<= 1 1 1)))

(defun run-test-comparison-operators-ge-variadic ()
  (and (>= 3 2 2 1)
       (not (>= 3 1 2))
       (>= 1)
       (>= 1 1 1)))

(defun run-test-comparison-operators-eq-variadic ()
  (and (= 2 2 2)
       (not (= 2 2 3))
       (= 2)))

; /=は「どの2つも等しくない」という意味で推移律が効かないため、1番目と3番目が等しいだけの
; ケース((/= 1 2 1))が誤ってtにならないことを確認するのが本テストの核心
(defun run-test-comparison-operators-ne-variadic ()
  (and (/= 1 2 3)
       (not (/= 1 2 1))
       (not (/= 1 1 2))
       (/= 1)))

(defun run-test-comparison-operators ()
  (and (run-test-comparison-operators-lt-variadic)
       (run-test-comparison-operators-gt-variadic)
       (run-test-comparison-operators-le-variadic)
       (run-test-comparison-operators-ge-variadic)
       (run-test-comparison-operators-eq-variadic)
       (run-test-comparison-operators-ne-variadic)))
