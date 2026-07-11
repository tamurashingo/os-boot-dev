; milestone 28 (アイデンティティベースのhash-code) の動作確認用テスト。
; (load "test\test-hash-code.lisp")でロードしてから(run-test-hash-code)を呼び出す。
; hash-codeは衝突を許容するハッシュ関数であり「異なるオブジェクトは異なるhash-codeを
; 返す」ことは保証しないため、このファイルでは「同一オブジェクトは常に同じhash-codeを
; 返す」という不変条件のみを自動確認する。

(defun run-test-hash-code-returns-atom ()
  (atom (hash-code 'foo)))

(defun run-test-hash-code-fixnum-repeatable ()
  (eq (hash-code 42) (hash-code 42)))

(defun run-test-hash-code-symbol-repeatable ()
  (eq (hash-code 'foo) (hash-code 'foo)))

(defun run-test-hash-code-eq-identity ()
  ; 同じcons-cellを別の変数から参照してもhash-codeは同じ値を返す
  (let* ((c (cons 1 2))
         (c2 c))
    (eq (hash-code c) (hash-code c2))))

(defun run-test-hash-code ()
  (and (run-test-hash-code-returns-atom)
       (run-test-hash-code-fixnum-repeatable)
       (run-test-hash-code-symbol-repeatable)
       (run-test-hash-code-eq-identity)))
