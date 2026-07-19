; milestone120: %clear-screen/%set-cursor-position(直接ConOutを叩く暫定実装)の動作確認用テスト。
; どちらも画面表示の見た目自体はヘッドレスQEMU(-display none)のmake testでは検証できないため、
; エラー(panic)にならず正常に戻り値を返すことのみを確認する。
(defun run-test-console-clear-screen ()
  (eq (%clear-screen) t))

(defun run-test-console-set-cursor-position ()
  (and (eq (%set-cursor-position 0 0) t)
       (eq (%set-cursor-position 1 1) t)
       (eq (%set-cursor-position 0 0) t)))

(defun run-test-console ()
  (and (run-test-console-clear-screen)
       (run-test-console-set-cursor-position)))
