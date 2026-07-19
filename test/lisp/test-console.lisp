; milestone120: %clear-screen/%set-cursor-position(直接ConOutを叩く暫定実装)の動作確認用テスト。
; どちらも画面表示の見た目自体はヘッドレスQEMU(-display none)のmake testでは検証できないため、
; エラー(panic)にならず正常に戻り値を返すことのみを確認する。
(defun run-test-console-clear-screen ()
  (eq (%clear-screen) t))

(defun run-test-console-set-cursor-position ()
  (and (eq (%set-cursor-position 0 0) t)
       (eq (%set-cursor-position 1 1) t)
       (eq (%set-cursor-position 0 0) t)))

; milestone121: %get-screen-size(QueryMode経由)の戻り値が(cons cols rows)で、
; cols/rowsがともに妥当な範囲(0より大きく1000未満)のfixnumであることを確認する
(defun run-test-console-get-screen-size ()
  (let ((size (%get-screen-size)))
    (and (eq (atom size) nil)
         (< 0 (car size))
         (< (car size) 1000)
         (< 0 (cdr size))
         (< (cdr size) 1000))))

; milestone129: os:goto-xy/os:print-at/os:clear-screen(lisp/os.lispの薄いラッパー)の
; 動作確認。%clear-screen/%set-cursor-positionと同様、見た目自体はヘッドレスQEMUの
; make testでは検証できないため、エラー(panic)にならず正常に戻り値を返すことのみを確認する
(defun run-test-console-os-goto-xy ()
  (and (eq (os:goto-xy 0 0) t)
       (eq (os:goto-xy 1 1) t)
       (eq (os:goto-xy 0 0) t)))

(defun run-test-console-os-clear-screen ()
  (eq (os:clear-screen) t))

; os:print-atは実際に1文字("x")を画面へ書き込むため、直後にwrite-lineで実"\r\n"を
; 送出しておかないと、続くRESULT行がその途切れた行末に連結されてしまい
; (scripts/run_test.pyの行検出が壊れる、milestone125と同型の問題)テストハーネスが
; 壊れる。そのためos:print-atの戻り値をletで保持してから明示的に改行を挟む
(defun run-test-console-os-print-at ()
  (let ((ok (eq (os:print-at 0 0 "x") t)))
    (write-line "")
    ok))

(defun run-test-console ()
  (and (run-test-console-clear-screen)
       (run-test-console-set-cursor-position)
       (run-test-console-get-screen-size)
       (run-test-console-os-goto-xy)
       (run-test-console-os-clear-screen)
       (run-test-console-os-print-at)))
