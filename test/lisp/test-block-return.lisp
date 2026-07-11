; milestone 19 (block/return-from, 非局所脱出の基盤) の動作確認用テスト。
; QEMU起動後、(load "test\test-block-return.lisp") でロードしてから
; (run-test-block-return) を呼び出し、t が返れば全項目成功。
;
; 注意: return-fromに対応するblockが無いケース(unbound tag)はpanicして
; QEMUが停止するため、ここには含めない。手動での別確認とする。

(defun run-test-block-basic ()
  (eq (block blk (return-from blk 42) 999) 42))

(defun run-test-block-no-return ()
  ; return-fromが無ければblockは通常のprognとして最後の値を返す
  (eq (block blk 1 2 3) 3))

(defun run-test-block-skips-rest ()
  ; return-from以降の本体formは評価されない
  (eq (block blk (return-from blk 1) (return-from blk 2)) 1))

(defun run-test-block-nested-inner-tag ()
  ; innerブロック宛のreturn-fromはinnerで捕捉され、outerの本体は続行する
  (eq (block outer
        (block inner
          (return-from inner 10))
        20)
      20))

(defun run-test-block-nested-outer-tag ()
  ; outerブロック宛のreturn-fromはinnerを素通りしてouterまで伝播する
  (eq (block outer
        (block inner
          (return-from outer 99))
        20)
      99))

(defun scan-for-target (lst target)
  ; 再帰呼び出しを何段か経由してもreturn-fromが呼び出し元のblockまで
  ; 正しく伝播することを確認する
  (if (atom lst)
      nil
      (if (eq (car lst) target)
          (return-from scan-loop (car lst))
          (scan-for-target (cdr lst) target))))

(defun run-test-block-escapes-recursion ()
  (eq (block scan-loop (scan-for-target '(1 2 3 4 5) 3) 999) 3))

(defvar *br-v* 1)

(defun run-test-block-restores-dynamic ()
  ; return-fromがletの本体評価中に発生しても、letを抜ける際の動的変数の
  ; 復元処理は必ず実行される
  (and (eq (block blk
             (let ((*br-v* 2))
               (return-from blk *br-v*)))
           2)
       (eq *br-v* 1)))

(defun run-test-block-restores-dynamic-let-star ()
  (and (eq (block blk2
             (let* ((*br-v* 3))
               (return-from blk2 *br-v*)))
           3)
       (eq *br-v* 1)))

(defun run-test-block-restores-dynamic-let-star-mid-bindings ()
  ; let*の初期値式の評価中(まだ本体に入る前)にreturn-fromが発生した場合も、
  ; それより前のbindingで書き換えた動的変数は正しく復元される
  (and (eq (block blk3
             (let* ((*br-v* 4) (ignored (return-from blk3 *br-v*)))
               999))
           4)
       (eq *br-v* 1)))

(defun run-test-block-return ()
  (and (run-test-block-basic)
       (run-test-block-no-return)
       (run-test-block-skips-rest)
       (run-test-block-nested-inner-tag)
       (run-test-block-nested-outer-tag)
       (run-test-block-escapes-recursion)
       (run-test-block-restores-dynamic)
       (run-test-block-restores-dynamic-let-star)
       (run-test-block-restores-dynamic-let-star-mid-bindings)))
