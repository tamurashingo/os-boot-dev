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

; milestone131: %set-status-line(先頭行(OS予約行)への直接書き込み)の動作確認。
; 実ConOutは直接叩かずback bufferのみを書き換えるが、touchされた内容はVM命令
; ディスパッチループの1命令ごとflushフック(milestone127)により実行中に自動的に
; 実画面へ送出される。os:print-atと同型の理由(実"\r\n"を伴わないため、続く
; RESULT行がその途切れた行末に連結されテストハーネスの行検出が壊れる、milestone125)
; により、明示的にwrite-lineで改行を挟む必要がある
(defun run-test-console-set-status-line ()
  (let ((ok (and (eq (%set-status-line "REPL") t)
                 (eq (%set-status-line "") t))))
    (write-line "")
    ok))

; milestone138続報: os:text-input-ex-found-p(g_text_input_exがFOUNDだったかどうかを
; 起動ログを読み返さずにREPLから確認する診断用関数)。値そのものは実行環境(実機/QEMU)に
; 依存するため、t/nilのいずれかを返すことのみを確認する
(defun run-test-console-os-text-input-ex-found-p ()
  (let ((r (os:text-input-ex-found-p)))
    (or (eq r t) (eq r nil))))

; milestone138続報2: os:key-debug-log(ReadKeyStrokeExが実際に返した生のKey/KeyStateを
; 確認する診断用関数)。ヘッドレスQEMUのload実行中はlisp_read_lineが一度も呼ばれていない
; ため空リスト(nil)が期待値だが、将来この前提が変わっても壊れないよう「nilまたはcons」の
; いずれかであることのみを確認する
(defun run-test-console-os-key-debug-log ()
  (let ((log (os:key-debug-log)))
    (or (null log) (consp log))))

(defun run-test-console ()
  (and (run-test-console-clear-screen)
       (run-test-console-set-cursor-position)
       (run-test-console-get-screen-size)
       (run-test-console-os-goto-xy)
       (run-test-console-os-clear-screen)
       (run-test-console-os-print-at)
       (run-test-console-set-status-line)
       (run-test-console-os-text-input-ex-found-p)
       (run-test-console-os-key-debug-log)))
