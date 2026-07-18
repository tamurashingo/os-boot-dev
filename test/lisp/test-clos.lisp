; milestone 96(最小CLOSサブセット: defclass/make-instance/slot-value、単一継承、
; ディスパッチ無し)・97(defmethod/総称関数/多重ディスパッチ)の動作確認用テスト。
; (load "test\test-clos.lisp")でロードしてから(run-test-clos)を呼び出す。
; find-classのpanic(未知クラス名)・no applicable method・ambiguous method call・
; incongruent lambda list等のpanicシナリオはこのファイルでは対象外とし、
; documents/lisp_clos.mdの検証方針に従い個別のQEMU対話で目視確認する。

(defclass clos-point () (x y))

(defun run-test-clos-basic ()
  (let ((p (make-instance 'clos-point)))
    (and (eq (slot-value p 'x) nil)
         (eq (slot-value p 'y) nil)
         (eq (set-slot-value p 'x 1) 1)
         (eq (set-slot-value p 'y 2) 2)
         (eq (slot-value p 'x) 1)
         (eq (slot-value p 'y) 2))))

(defclass clos-shape () (color))
(defclass clos-circle (clos-shape) (radius))

; 継承: サブクラスのinstanceは親クラスのスロット(color)と自身のスロット(radius)の
; どちらも読み書きできる
(defun run-test-clos-inheritance ()
  (let ((c (make-instance 'clos-circle)))
    (and (eq (set-slot-value c 'color 'red) 'red)
         (eq (set-slot-value c 'radius 10) 10)
         (eq (slot-value c 'color) 'red)
         (eq (slot-value c 'radius) 10))))

; instanceの独立性: 同じクラスから作った別々のinstanceはスロットを共有しない
(defun run-test-clos-instance-independence ()
  (let ((p1 (make-instance 'clos-point))
        (p2 (make-instance 'clos-point)))
    (set-slot-value p1 'x 100)
    (set-slot-value p2 'x 200)
    (and (eq (slot-value p1 'x) 100)
         (eq (slot-value p2 'x) 200))))

; class-ofはinstanceの生成元クラスをeq同一性で返す(find-classの解決結果と同じオブジェクト)
(defun run-test-clos-class-of ()
  (let ((p (make-instance 'clos-point)))
    (eq (class-of p) (find-class 'clos-point))))

; defunは既存の挙動を変えない: instanceを普通の値として受け取り・返す関数が
; slot-value/set-slot-valueと組み合わせて動作すること
; defun/lambdaのbodyは単一form(既存のprogn gotcha)なので、複数の副作用を明示的にprognで包む
(defun clos-move-point (p dx dy)
  (progn
    (set-slot-value p 'x (+ (slot-value p 'x) dx))
    (set-slot-value p 'y (+ (slot-value p 'y) dy))
    p))

(defun run-test-clos-defun-unaffected ()
  (let ((p (make-instance 'clos-point)))
    (set-slot-value p 'x 1)
    (set-slot-value p 'y 1)
    (clos-move-point p 10 20)
    (and (eq (slot-value p 'x) 11)
         (eq (slot-value p 'y) 21))))

; defclassの再定義冪等性: 同名クラスを再定義してもfind-classはeqな同一オブジェクトを
; 返し続け、スロット構成のみが更新される
(defclass clos-redef () (a))

(defun run-test-clos-redefine ()
  (let ((cls1 (find-class 'clos-redef)))
    (defclass clos-redef () (a b))
    (let ((cls2 (find-class 'clos-redef))
          (inst (make-instance 'clos-redef)))
      (and (eq cls1 cls2)
           (eq (set-slot-value inst 'b 99) 99)
           (eq (slot-value inst 'b) 99)))))

; milestone97: defmethod/総称関数/多重ディスパッチの動作確認。ambiguous method call/
; no applicable method/incongruent lambda list/find-classのpanic等のpanicシナリオは
; milestone96同様このファイルでは対象外とし、documents/lisp_clos.mdの検証方針に従い
; 個別のQEMU対話で目視確認する

(defclass clos-square (clos-shape) (side))

; 単一specializerディスパッチ+継承経由のフォールバック: clos-circle/clos-squareそれぞれの
; 専用methodと、どちらにも一致しないclos-shapeそのものに対するデフォルトmethod
(defmethod clos-area ((s clos-circle)) (+ (slot-value s 'radius) 1000))
(defmethod clos-area ((s clos-square)) (+ (slot-value s 'side) 2000))
(defmethod clos-area ((s clos-shape)) 9999)

(defun run-test-clos-defmethod-single-dispatch ()
  (let ((circle (make-instance 'clos-circle))
        (square (make-instance 'clos-square))
        (shape (make-instance 'clos-shape)))
    (set-slot-value circle 'radius 5)
    (set-slot-value square 'side 7)
    (and (eq (clos-area circle) 1005)
         (eq (clos-area square) 2007)
         (eq (clos-area shape) 9999))))

; 多重ディスパッチ(2引数): 両方specializer指定/片方無指定の3つのmethodを定義し、
; 「無指定より指定ありが詳細」という優先規則が正しく効くことを確認する
(defmethod clos-combine ((a clos-point) (b clos-point)) 'point-point)
(defmethod clos-combine ((a clos-point) (b clos-circle)) 'point-circle)
(defmethod clos-combine (a (b clos-circle)) 'any-circle)

(defun run-test-clos-defmethod-multi-dispatch ()
  (and (eq (clos-combine (make-instance 'clos-point) (make-instance 'clos-point)) 'point-point)
       (eq (clos-combine (make-instance 'clos-point) (make-instance 'clos-circle)) 'point-circle)
       (eq (clos-combine 42 (make-instance 'clos-circle)) 'any-circle)))

; メソッド再定義の冪等性: 同じspecializer-listでdefmethodし直しても新規entryが増えず、
; 常に最新の本体が使われる(ambiguousにならないことの確認でもある)
(defmethod clos-redef-gf ((p clos-point)) 'first-version)

(defun run-test-clos-defmethod-redefine ()
  (let ((p (make-instance 'clos-point))
        (first-result (clos-redef-gf (make-instance 'clos-point))))
    (defmethod clos-redef-gf ((p clos-point)) 'second-version)
    (and (eq first-result 'first-version)
         (eq (clos-redef-gf p) 'second-version))))

; defunは総称関数の存在下でも既存の挙動を変えない: instanceを受け取るdefunがslot-value経由の
; 総称関数(clos-area)と共存し、どちらも正しく動作すること
(defun clos-describe-circle (c)
  (+ (slot-value c 'radius) (clos-area c)))

(defun run-test-clos-defmethod-defun-coexist ()
  (let ((c (make-instance 'clos-circle)))
    (set-slot-value c 'radius 3)
    (eq (clos-describe-circle c) 1006)))

(defun run-test-clos ()
  (and (run-test-clos-basic)
       (run-test-clos-inheritance)
       (run-test-clos-instance-independence)
       (run-test-clos-class-of)
       (run-test-clos-defun-unaffected)
       (run-test-clos-redefine)
       (run-test-clos-defmethod-single-dispatch)
       (run-test-clos-defmethod-multi-dispatch)
       (run-test-clos-defmethod-redefine)
       (run-test-clos-defmethod-defun-coexist)))
