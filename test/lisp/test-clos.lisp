; milestone 96 (最小CLOSサブセット: defclass/make-instance/slot-value、単一継承、
; ディスパッチ無し)の動作確認用テスト。
; (load "test\test-clos.lisp")でロードしてから(run-test-clos)を呼び出す。
; find-classのpanic(未知クラス名)等のシナリオはこのファイルでは対象外とし、
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

(defun run-test-clos ()
  (and (run-test-clos-basic)
       (run-test-clos-inheritance)
       (run-test-clos-instance-independence)
       (run-test-clos-class-of)
       (run-test-clos-defun-unaffected)
       (run-test-clos-redefine)))
