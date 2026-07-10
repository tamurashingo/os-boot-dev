; milestone 17 (let/let*/progn/setq/cond/and/or/when/unless) の動作確認用テスト。
; QEMU起動後、(load "test\test-special-forms.lsp") でロードしてから
; (run-test-special-forms) を呼び出し、t が返れば全項目成功。
; 個別に確認したい場合は run-test-progn 等を単独で呼び出す。

(defun run-test-progn ()
  (and (eq (progn 1 2 3) 3)
       (eq (progn) nil)))

(defun run-test-let ()
  (and (eq (let ((a 1) (b 2)) (+ a b)) 3)
       ; letは並列束縛: yの初期値式は外側のx(=1)を見る
       (eq (let ((x 1)) (let ((x 2) (y (+ x 1))) y)) 2)))

(defun run-test-let-star ()
  ; let*は逐次束縛: yの初期値式は直前で束縛したx(=2)を見る
  (eq (let ((x 1)) (let* ((x 2) (y (+ x 1))) y)) 3))

(defun run-test-setq ()
  (eq (let ((x 1)) (setq x (+ x 10)) x) 11))

(defun run-test-cond ()
  (and (eq (cond ((eq 1 2) 100) ((eq 1 1) 200) (t 300)) 200)
       (eq (cond ((eq 1 2) 100)) nil)
       (eq (cond (5)) 5)))

(defun run-test-and ()
  (and (eq (and 1 2 3) 3)
       (eq (and 1 nil 3) nil)
       (eq (and) t)
       ; 短絡評価: nilに到達した時点で(car 5)は評価されずpanicしない
       (eq (and nil (car 5)) nil)))

(defun run-test-or ()
  (and (eq (or nil nil 5) 5)
       (eq (or nil nil) nil)
       (eq (or) nil)
       ; 短絡評価: 1が返った時点で(car 5)は評価されずpanicしない
       (eq (or 1 (car 5)) 1)))

(defun run-test-when ()
  (and (eq (when (eq 1 1) 10 20) 20)
       (eq (when (eq 1 2) 10) nil)))

(defun run-test-unless ()
  (and (eq (unless (eq 1 2) 10 20) 20)
       (eq (unless (eq 1 1) 10) nil)))

(defun run-test-special-forms ()
  (and (run-test-progn)
       (run-test-let)
       (run-test-let-star)
       (run-test-setq)
       (run-test-cond)
       (run-test-and)
       (run-test-or)
       (run-test-when)
       (run-test-unless)))
