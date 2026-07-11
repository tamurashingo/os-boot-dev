; milestone 18 (defvar/defparameter, letによる動的変数の再束縛) の動作確認用テスト。
; QEMU起動後、(load "test\test-dynamic-vars.lisp") でロードしてから
; (run-test-dynamic-vars) を呼び出し、t が返れば全項目成功。

(defvar *dv-x* 1)

(defun run-test-defvar-basic ()
  (eq *dv-x* 1))

(defun run-test-defvar-no-overwrite ()
  ; 既にis_specialなので、この2回目のdefvarは値を書き換えない
  (progn
    (defvar *dv-x* 999)
    (eq *dv-x* 1)))

(defun run-test-defparameter-overwrite ()
  ; defparameterは既存の値があっても常に上書きする
  (progn
    (defparameter *dv-x* 42)
    (eq *dv-x* 42)))

(defvar *dv-y* 100)

(defun read-dv-y ()
  ; letの外側で定義された関数から動的変数を参照する。呼び出し側のletでの
  ; 再束縛がレキシカルスコープに関係なく見えることを確認する
  *dv-y*)

(defun run-test-let-rebinds-dynamic ()
  (and (eq (let ((*dv-y* 200)) (read-dv-y)) 200)
       ; letを抜けたら元の値に復元されている
       (eq *dv-y* 100)))

(defun run-test-let-star-sees-own-rebinding ()
  ; let*は逐次束縛なので、同じlet*内で後続の初期値式が直前の動的変数の
  ; 再束縛を見られる
  (eq (let* ((*dv-y* 300) (z (read-dv-y))) z) 300))

(defun run-test-let-parallel-does-not-see-own-rebinding ()
  ; letは並列束縛なので、同じlet内の他の初期値式は再束縛前の外側の値を見る
  (and (eq (let ((*dv-y* 400) (z (read-dv-y))) z) 100)
       (eq *dv-y* 100)))

(defun run-test-setq-dynamic ()
  (eq (let ((*dv-y* 500)) (setq *dv-y* 600) *dv-y*) 600))

(defun run-test-dynamic-vars ()
  (and (run-test-defvar-basic)
       (run-test-defvar-no-overwrite)
       (run-test-defparameter-overwrite)
       (run-test-let-rebinds-dynamic)
       (run-test-let-star-sees-own-rebinding)
       (run-test-let-parallel-does-not-see-own-rebinding)
       (run-test-setq-dynamic)))
