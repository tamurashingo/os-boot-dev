; milestone87 (do特殊形式の上に構築したwhileマクロ) の動作確認用テスト。
; QEMU起動後、(load "test\test-while.lisp") でロードしてから
; (run-test-while) を呼び出し、t が返れば全項目成功。

(defun run-test-while-basic-sum ()
  ; testが真の間だけbodyを実行し、外側の変数を副作用で更新する
  (eq (let ((i 0) (sum 0))
        (while (< i 5)
          (setq sum (+ sum i))
          (setq i (+ i 1)))
        sum)
      10))

(defun run-test-while-returns-nil ()
  ; do同様、result-formsを持たないためwhile自身の戻り値は常にnil
  (eq (let ((i 0))
        (while (< i 3) (setq i (+ i 1))))
      nil))

(defun run-test-while-zero-iterations ()
  ; testが最初から偽ならbodyは一度も実行されない
  (eq (let ((count 0))
        (while nil (setq count (+ count 1)))
        count)
      0))

(defun run-test-while-return-from-escapes ()
  ; return-fromでwhileの外側のblockまで直接脱出できる(end-testには到達しない)
  (eq (block outer
        (let ((i 0))
          (while (< i 100)
            (if (eq i 3) (return-from outer i) nil)
            (setq i (+ i 1)))))
      3))

(defun run-test-while-nested ()
  ; ネストしたwhileが正しく独立して動作する(内側のループが外側の変数を破壊しない)
  (eq (let ((i 0) (total 0))
        (while (< i 3)
          (let ((j 0))
            (while (< j 3)
              (setq total (+ total 1))
              (setq j (+ j 1))))
          (setq i (+ i 1)))
        total)
      9))

(defun run-test-while-many-iterations ()
  ; 大きなイテレーション数でもCコールスタックを消費しない(doと同じ理由、milestone87の目的)
  (eq (let ((i 0) (sum 0))
        (while (< i 50000)
          (setq sum (+ sum 1))
          (setq i (+ i 1)))
        sum)
      50000))

(defun run-test-while ()
  (and (run-test-while-basic-sum)
       (run-test-while-returns-nil)
       (run-test-while-zero-iterations)
       (run-test-while-return-from-escapes)
       (run-test-while-nested)
       (run-test-while-many-iterations)))
