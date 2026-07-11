; milestone 27 (破壊的変更: rplaca/rplacd) の動作確認用テスト。
; (load "test\test-rplac.lisp")でロードしてから(run-test-rplac)を呼び出す。

(defun run-test-rplac-basic ()
  ; rplaca/rplacdは書き換えたcons-cell自身を返す（svsetがvalueを返すのとは異なる）
  (let ((c (cons 1 2)))
    (and (eq (rplaca c 10) c)
         (eq (car c) 10)
         (eq (cdr c) 2)
         (eq (rplacd c 20) c)
         (eq (car c) 10)
         (eq (cdr c) 20))))

(defun run-test-rplac-eq-identity ()
  ; rplaca/rplacdがコピーせず同一オブジェクトを破壊的に書き換えることを、
  ; 別の変数に退避した参照(c2)からも変更が見えることで確認する
  (let* ((c (cons 1 2))
         (c2 c))
    (rplaca c 100)
    (rplacd c 200)
    (and (eq (car c2) 100)
         (eq (cdr c2) 200))))

(defun run-test-rplac-list-structure ()
  ; リストの一部を書き換えても他の要素に影響しないことを確認する
  ; （このインタプリタにはlistビルトインが無いため、consを連結して構築する）
  (let ((lst (cons 1 (cons 2 (cons 3 nil)))))
    (rplaca (cdr lst) 99)
    (and (eq (car lst) 1)
         (eq (car (cdr lst)) 99)
         (eq (car (cdr (cdr lst))) 3))))

(defun run-test-rplac ()
  (and (run-test-rplac-basic)
       (run-test-rplac-eq-identity)
       (run-test-rplac-list-structure)))
