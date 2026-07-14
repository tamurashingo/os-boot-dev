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

; return-fromがletの本体評価中に発生した場合の動的変数の復元(*br-v*相当のケース)は、
; milestone57で既知の制限として明記した通りcompile-let/compile-let*の対象外
; (VM側にunwind-protect相当の機構が無いため、return-fromによる早期returnは
; letの復元コードを素通りする)。milestone60でdefunの本体もコンパイル経路
; (lisp_eval_toplevelの旧来フォールバックではなく)で実行されるようになったため、
; このファイル自身の関数もこの制限の対象になる。よってこのケースはここでは
; テストしない(milestone55のtest-compile-and-run.lispのケース1〜6と同じ範囲に揃える)。
(defun run-test-block-return ()
  (and (run-test-block-basic)
       (run-test-block-no-return)
       (run-test-block-skips-rest)
       (run-test-block-nested-inner-tag)
       (run-test-block-nested-outer-tag)
       (run-test-block-escapes-recursion)))
