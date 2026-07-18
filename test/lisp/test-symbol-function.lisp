; milestone 93 (関数セル/funcall系API) の動作確認用テスト。
; milestone94(Lisp-2化)により、組み込み関数名をbareで値位置に書くと
; unbound variableになるため、#'を付けて関数セルの値として渡す。
; QEMU起動後、(load "test\test-symbol-function.lisp") でロードしてから
; (run-test-symbol-function) を呼び出し、t が返れば全項目成功。

(defun run-test-symbol-function-set-and-get ()
  ; %set-symbol-functionで書き込んだ値がsymbol-functionでそのまま読める(eq)
  (%set-symbol-function 'm93-f #'car)
  (eq (symbol-function 'm93-f) #'car))

(defun run-test-symbol-function-fboundp ()
  ; 設定済みsymbolはfboundpがt
  (%set-symbol-function 'm93-g #'cdr)
  (if (fboundp 'm93-g) t nil))

(defun run-test-symbol-function-fboundp-unset ()
  ; 未設定symbolはfboundpがnil
  (if (fboundp 'm93-never-set) nil t))

(defun run-test-symbol-function-sharp-quote ()
  ; #'fooは(function foo)相当であり、%set-symbol-functionで設定した値をそのまま返す
  (%set-symbol-function 'm93-h #'cons)
  (eq #'m93-h (symbol-function 'm93-h)))

(defun run-test-symbol-function-function-special-form ()
  ; (function foo)自体もsymbol-functionと一致する
  (%set-symbol-function 'm93-i #'atom)
  (eq (function m93-i) (symbol-function 'm93-i)))

(defvar m93-j 111)

(defun run-test-symbol-function-independent-from-value ()
  ; fnセルとvalueセル(defvar)は独立している。同名でも互いに影響しない
  (%set-symbol-function 'm93-j #'eq)
  (and (eq (symbol-value 'm93-j) 111)
       (eq (symbol-function 'm93-j) #'eq)))

(defun run-test-symbol-function-redefine ()
  ; 再設定すれば新しい値に更新される
  (%set-symbol-function 'm93-k #'car)
  (%set-symbol-function 'm93-k #'cdr)
  (eq (symbol-function 'm93-k) #'cdr))

(defun run-test-symbol-function ()
  (and (run-test-symbol-function-set-and-get)
       (run-test-symbol-function-fboundp)
       (run-test-symbol-function-fboundp-unset)
       (run-test-symbol-function-sharp-quote)
       (run-test-symbol-function-function-special-form)
       (run-test-symbol-function-independent-from-value)
       (run-test-symbol-function-redefine)))
