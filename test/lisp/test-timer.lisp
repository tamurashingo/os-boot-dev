; milestone 25 (タイマー支援: sleep) の動作確認用テスト。
; (load "test\test-timer.lisp")でロードしてから(run-test-timer)を呼び出す。
; sleepの実際の待ち時間はeqで自動検証できないため、戻り値がnilであることのみを
; このテストで確認する。実際に指定秒数だけ待たされるかはQEMUのREPLで目視確認する
; (documents/bare_metal_lisp.md参照)。
(defun run-test-timer ()
  (and (eq (sleep 0) nil)
       (eq (sleep 0.0) nil)))
