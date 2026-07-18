; milestone 29 (標準ライブラリの自己ホスティング化) の動作確認用テスト。
; (load "test\test-stdlib.lisp")でロードしてから(run-test-stdlib)を呼び出す。
; list/append/reverse/nth/mapcar/not/null/1+/1-/比較演算子群/zerop等はEfiMainの
; 起動シーケンスでstdlib.lispとして既に読み込まれている前提（このテストファイルでの
; 再定義は不要）。

(defun run-test-stdlib-list ()
  (equal-ints (list 1 2 3) (cons 1 (cons 2 (cons 3 nil)))))

; listもequalも標準では存在しないため、consチェーンの各要素をeqで比較する
; 小さなヘルパーをここで定義する
(defun equal-ints (a b)
  (if (null a)
      (null b)
      (and (not (null b))
           (eq (car a) (car b))
           (equal-ints (cdr a) (cdr b)))))

(defun run-test-stdlib-append ()
  (equal-ints (append (list 1 2) (list 3 4)) (list 1 2 3 4)))

(defun run-test-stdlib-reverse ()
  (equal-ints (reverse (list 1 2 3)) (list 3 2 1)))

(defun run-test-stdlib-nth ()
  (eq (nth 1 (list 'a 'b 'c)) 'b))

(defun run-test-stdlib-mapcar ()
  ; lambda式リテラルを直接渡すケース。mapcar内部は(milestone94のLisp-2化により)
  ; ローカル変数fnをfuncall経由で呼ぶ実装になっている(compile-call自体は無関係、
  ; mapcarはインタプリタクロージャなのでlisp_evalの通常の呼び出し経路を通る)
  (equal-ints (mapcar (lambda (x) (+ x 1)) (list 1 2 3)) (list 2 3 4)))

(defun run-test-stdlib-comparisons ()
  (and (< 1 2 3)
       (not (< 1 3 2))
       (> 3 1)
       (= 2 2)
       (<= 2 2)
       (not (>= 2 3))
       (/= 2 3)))

(defun run-test-stdlib-predicates ()
  (and (zerop 0)
       (not (zerop 1))
       (plusp 1)
       (not (plusp -1))
       (minusp -1)
       (not (minusp 1))))

(defun run-test-stdlib-arithmetic-shorthand ()
  (and (eq (1+ 5) 6)
       (eq (1- 5) 4)))

(defun run-test-stdlib-not-null ()
  (and (null nil)
       (not (null 1))
       (not nil)
       (not (not t))))

(defun run-test-stdlib ()
  (and (run-test-stdlib-list)
       (run-test-stdlib-append)
       (run-test-stdlib-reverse)
       (run-test-stdlib-nth)
       (run-test-stdlib-mapcar)
       (run-test-stdlib-comparisons)
       (run-test-stdlib-predicates)
       (run-test-stdlib-arithmetic-shorthand)
       (run-test-stdlib-not-null)))
