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

; milestone 108: make-processが生成する隔離パッケージ(packageスロット)の検証
(defun run-test-os-make-process-fork-package ()
  (let ((p1 (os:make-process))
        (p2 (os:make-process)))
    (let ((pkg1 (slot-value p1 'package))
          (pkg2 (slot-value p2 'package)))
      (and (not (eq pkg1 nil))
           (not (eq pkg2 nil))
           (not (eq pkg1 pkg2))
           (eq (intern "car" pkg1) (intern "car"))
           (eq (intern "car" pkg2) (intern "car"))))))

; milestone 109: fork側でのローカル関数再定義(shadow+defunによる委譲上書き)。
;
; 実際の運用手順は「in-packageでfork先パッケージへ切替→shadowでベースと同名の別シンボルを
; 確保→そのシンボルへdefun」という3ステップだが、これはREPL/loadが1トップレベルフォームずつ
; read→evalを繰り返すことに依存する。この関数1つの本体の中に文字通り(defun car ...)と書いても、
; 本体全体がcommon-lisp-userのまま1度に読み切られてしまうため、内側の"car"は結局
; common-lisp-userのcarとして読まれてしまい検証にならない(milestone79/81で確認したreader
; 可視性制約と対称の問題)。そのためこのテストでは、実際の再定義そのものは
; %set-symbol-function(milestone93、シンボルオブジェクトを直接受け取る)を使い、shadowで
; 確保したシンボルをintern(文字列引数、*package*に対してランタイムに解決される)経由で取得して
; 結び付ける。in-package/shadow自体は実際の運用手順どおりに実行する
(defun run-test-os-make-process-fork-redefine ()
  (let* ((p (os:make-process))
         (fork-pkg (slot-value p 'package))
         (fork-pkg-name (package-name fork-pkg))
         (base-car-sym (intern "car"))
         (base-result (car (cons 1 2))))
    (in-package fork-pkg-name)
    (shadow "car")
    (let ((fork-car-sym (intern "car")))
      (%set-symbol-function fork-car-sym (lambda (x) 'shadowed-car))
      (let ((fork-result (funcall (symbol-function fork-car-sym) (cons 1 2))))
        (in-package "common-lisp-user")
        (and (not (eq fork-car-sym base-car-sym))
             (eq fork-result 'shadowed-car)
             (eq base-result 1)
             (eq (car (cons 1 2)) 1)
             (eq (intern "car") base-car-sym)
             (eq (intern "car" fork-pkg) fork-car-sym))))))

; milestone 112: process-suspend/process-resume(実際の実行機構)の検証。
;
; thunkはlambdaでcounter/pの両方をレキシカルに捕捉する通常のクロージャなので、C側selftest
; (lisp_process_suspend_resume_selftest、ダイナミック変数経由)と異なりダイナミック変数は
; 不要。単一実行コンテキストの協調的切替である以上、os:process-resumeの呼び出し元(この
; テスト自身)が制御を取り戻した時点でそのプロセス自身が:activeであることは原理的に無いため、
; 自分自身をos:process-suspendした直後まで進んだ状態(1回目のインクリメント後)ではstatusは
; :suspendedであることを検証する。再度os:process-resumeした後に閉包が正常に戻った状態
; (2回目のインクリメント後、statusが:finished)も検証する。プロセスを外部から強制停止する
; 誤用シナリオ(自分自身以外へのprocess-suspend)は検証方針どおりmake testでは検証せず、
; 個別のQEMU対話セッションで確認する
(defun run-test-os-process-suspend-resume ()
  (let ((p (os:make-process))
        (counter 0))
    ; defun/lambdaの本体は単一formのみ(milestone21で確認済みのprogn gotcha)なので、
    ; 複数formを実行するには明示的にprognで束ねる必要がある
    (let ((thunk (lambda ()
                   (progn
                     (setq counter (+ counter 1))
                     (os:process-suspend p)
                     (setq counter (+ counter 1))))))
      (os:process-resume p thunk)
      (let ((status-after-suspend (slot-value p 'status))
            (counter-after-suspend counter))
        (os:process-resume p)
        (and (eq status-after-suspend :suspended)
             (eq counter-after-suspend 1)
             (eq (slot-value p 'status) :finished)
             (eq counter 2))))))

(defun run-test-os ()
  (and (run-test-os-process-class-exists)
       (run-test-os-process-slots)
       (run-test-os-all-processes-registry)
       (run-test-os-make-process-auto-name)
       (run-test-os-make-process-named)
       (run-test-os-make-process-fork-package)
       (run-test-os-make-process-fork-redefine)
       (run-test-os-process-suspend-resume)))
