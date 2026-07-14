; milestone 60 (defunのコンパイル時コード生成化) の動作確認用テスト。
; QEMU起動後、(load "test\test-defun-compile.lisp") でロードしてから
; (run-test-defun-compile) を呼び出し、t が返れば全項目成功。
;
; milestone60により、通常のparams(rest-arg形式でないもの)を持つdefunの本体は
; lisp_eval_toplevel経由でcompile-expr/vm-execへコンパイルされ、コンパイル済み
; クロージャとしてglobal_envへ束縛される(このファイル自身のdefunも含む)。
; 観測できる動作(戻り値)は旧来のツリーウォークと変わらないことを確認する。

(defun run-test-defun-compile-basic ()
  (eq (+ 1 2) 3))

; m60-fwd-caller(先に定義)がm60-fwd-callee(後で定義)を呼ぶ。milestone51のグローバル
; 参照が実行時にシンボル同一性で再解決するため、コンパイル時にはまだ存在しない
; 前方参照先でも問題なく呼び出せることを確認する
(defun m60-fwd-caller (x) (m60-fwd-callee x))
(defun m60-fwd-callee (x) (+ x 100))

(defun run-test-defun-compile-forward-ref ()
  (eq (m60-fwd-caller 1) 101))

; 相互再帰(お互いを前方参照する2つのコンパイル済みクロージャ)
(defun m60-mutual-even (n)
  (if (eq n 0) t (m60-mutual-odd (- n 1))))
(defun m60-mutual-odd (n)
  (if (eq n 0) nil (m60-mutual-even (- n 1))))

(defun run-test-defun-compile-mutual-recursion ()
  (and (eq (m60-mutual-even 10) t)
       (eq (m60-mutual-odd 10) nil)))

; rest-arg形式(paramsがbare symbol)のdefunはコンパイル済みクロージャの呼び出し規約が
; 対応しないため、lisp_eval_toplevelが個別にツリーウォークへフォールバックする
; (lisp_defun_params_is_restarg)。フォールバックしても結果は変わらないことを確認する
(defun m60-collect args args)

(defun run-test-defun-compile-restarg-fallback ()
  (let ((r (m60-collect 1 2 3)))
    (and (eq (nth 0 r) 1)
         (eq (nth 1 r) 2)
         (eq (nth 2 r) 3))))

(defvar *m60-dv* 1)

; letで動的変数を束縛したコンパイル済みdefun本体の中で、letを正常に(return-fromの
; 早期脱出を経由せず)抜ける場合は退避・復元が正しく行われることを確認する
; (return-fromで中断されるケースはmilestone57で既知の制限として明記済みで対象外)
(defun m60-read-dv () *m60-dv*)

(defun run-test-defun-compile-let-dynamic ()
  (and (eq (let ((*m60-dv* 2)) (m60-read-dv)) 2)
       (eq *m60-dv* 1)))

(defun run-test-defun-compile ()
  (and (run-test-defun-compile-basic)
       (run-test-defun-compile-forward-ref)
       (run-test-defun-compile-mutual-recursion)
       (run-test-defun-compile-restarg-fallback)
       (run-test-defun-compile-let-dynamic)))
