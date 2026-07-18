; milestone 102: osパッケージ・processクラス・グローバルレジストリのテスト
;
; osパッケージはmain.cの起動時にlisp/os-package.lisp・lisp/os.lispが自動loadされて既に
; 存在し、process/*all-processes*/get-all-processesはexport済みの前提。このファイルは
; common-lisp-userのままload・評価されるため、os:修飾子(milestone74のリーダー修飾子構文)で
; 参照する

(defun run-test-os-process-class-exists ()
  (eq (find-class 'os:process) (find-class 'os:process)))

; string=は本処理系には存在しない(組み込み文字列比較はeqの同一性比較のみ)ため、set-slot-valueの
; 戻り値・後続slot-valueの結果を、同一のlet変数(同一オブジェクト)と比較することでeqの範囲内で
; 内容確認する
(defun run-test-os-process-slots ()
  (let ((p (make-instance 'os:process))
        (name-val "proc-a"))
    (and (eq (slot-value p 'name) nil)
         (eq (slot-value p 'package) nil)
         (eq (slot-value p 'stackframe) nil)
         (eq (slot-value p 'env) nil)
         (eq (slot-value p 'status) nil)
         (eq (set-slot-value p 'name name-val) name-val)
         (eq (slot-value p 'name) name-val))))

; lengthは本処理系には存在しないため、cons前後のcar/cdrで直接registry更新を確認する
(defun run-test-os-all-processes-registry ()
  (let ((before os:*all-processes*)
        (p (make-instance 'os:process)))
    (setq os:*all-processes* (cons p os:*all-processes*))
    (and (eq (car os:*all-processes*) p)
         (eq (cdr os:*all-processes*) before)
         (eq (car (os:get-all-processes)) p))))

(defun run-test-os ()
  (and (run-test-os-process-class-exists)
       (run-test-os-process-slots)
       (run-test-os-all-processes-registry)))
