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

; milestone 103: make-processの名前一意性のみを検証する(実行機構は無いので生成・登録のみ)。
; 名前衝突panicシナリオは検証方針(documents/lisp_os_process.md)どおりmake testでは検証せず、
; 個別のQEMU対話セッションで確認する
(defun run-test-os-make-process-auto-name ()
  (let ((before os:*all-processes*)
        (p1 (os:make-process))
        (p2 (os:make-process)))
    (and (eq (class-of p1) (find-class 'os:process))
         (not (eq (slot-value p1 'name) nil))
         (not (eq (slot-value p1 'name) (slot-value p2 'name)))
         (eq (car (cdr os:*all-processes*)) p1)
         (eq (car os:*all-processes*) p2)
         (eq (cdr (cdr os:*all-processes*)) before))))

(defun run-test-os-make-process-named ()
  (let ((name-val "make-process-test-a")
        (before os:*all-processes*))
    (let ((p (os:make-process name-val)))
      (and (eq (slot-value p 'name) name-val)
           (eq (car os:*all-processes*) p)
           (eq (cdr os:*all-processes*) before)))))

(defun run-test-os ()
  (and (run-test-os-process-class-exists)
       (run-test-os-process-slots)
       (run-test-os-all-processes-registry)
       (run-test-os-make-process-auto-name)
       (run-test-os-make-process-named)))
