; milestone 61 (呼び出しディスパッチの一貫性検証・仕上げ) の動作確認用テスト。
; QEMU起動後、(load "test\test-call-dispatch.lisp") でロードしてから
; (run-test-call-dispatch) を呼び出し、t が返れば全項目成功。
;
; milestone61で新設したfuncall/apply(src/lisp.c、既存のlisp_apply(milestone53)への
; 薄いラッパー)と、それらを使って書いたreduce/sort(lisp/stdlib.lisp)が、呼び出し先の
; 種別(Cビルトイン・コンパイル済みクロージャ(milestone60でdefunの既定)・
; インタプリタクロージャ(lambda式の値、milestone58以降もコンパイルされない))
; いずれに対しても一貫して動作することを確認する。

; equal-ints: test-stdlib.lispで既に定義済みの構造比較ヘルパーと同じ実装
(defun cd-equal-ints (a b)
  (if (null a)
      (null b)
      (and (not (null b))
           (eq (car a) (car b))
           (cd-equal-ints (cdr a) (cdr b)))))

(defun cd-double (x) (+ x x))

; funcall: builtin・コンパイル済みクロージャ(defun)・インタプリタクロージャ(lambda)の
; 3種いずれもfnとして渡して一貫して呼び出せることを確認する
(defun run-test-call-dispatch-funcall ()
  (and (eq (funcall car (list 1 2 3)) 1)
       (eq (funcall cd-double 5) 10)
       (eq (funcall (lambda (x) (+ x 100)) 5) 105)))

; apply: 引数は評価済みの実引数「リスト」として渡す点がfuncallと異なる
(defun run-test-call-dispatch-apply ()
  (and (eq (apply car (list (list 1 2 3))) 1)
       (eq (apply cd-double (list 5)) 10)
       (eq (apply (lambda (x) (+ x 100)) (list 5)) 105)
       (eq (apply + (list 1 2)) 3)))

; reduce(内部でfuncallを使う): builtin(+)・コンパイル済み(cd-double相当の畳み込み用
; 関数)・lambdaいずれをfnとして渡しても一貫した結果になることを確認する
(defun cd-add (a b) (+ a b))

(defun run-test-call-dispatch-reduce ()
  (and (eq (reduce + (list 1 2 3 4) 0) 10)
       (eq (reduce cd-add (list 1 2 3 4) 0) 10)
       (eq (reduce (lambda (acc x) (+ acc x)) (list 1 2 3 4) 0) 10)))

; sort(内部でapplyを使う): builtin(<)・コンパイル済み・lambdaいずれをpredとして
; 渡しても一貫して昇順にソートできることを確認する
(defun cd-lt (a b) (< a b))

(defun run-test-call-dispatch-sort ()
  (and (cd-equal-ints (sort < (list 3 1 4 1 5 9 2 6)) (list 1 1 2 3 4 5 6 9))
       (cd-equal-ints (sort cd-lt (list 3 1 2)) (list 1 2 3))
       (cd-equal-ints (sort (lambda (a b) (< a b)) (list 3 1 2)) (list 1 2 3))
       (null (sort < nil))))

(defun run-test-call-dispatch ()
  (and (run-test-call-dispatch-funcall)
       (run-test-call-dispatch-apply)
       (run-test-call-dispatch-reduce)
       (run-test-call-dispatch-sort)))
