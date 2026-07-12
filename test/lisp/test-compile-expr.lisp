; milestone 42 (compile-expr: リテラル・quote・ifのコンパイル) の動作確認用テスト。
; QEMU起動後、(load "test\test-compile-expr.lisp") でロードしてから
; (run-test-compile-expr) を呼び出し、t が返れば全項目成功。
; compile-expr/assemble等はlisp/stdlib.lispでEfiMainの起動シーケンス中に
; 既に読み込まれている前提（このテストファイルでの再定義は不要）。

; 構造比較用ヘルパー(milestone40/41と同じもの。equalが無いためcar/cdr/eqで手で辿る)
(defun struct-eq (a b)
  (if (atom a)
      (eq a b)
      (and (not (atom b))
           (struct-eq (car a) (car b))
           (struct-eq (cdr a) (cdr b)))))

(defun run-test-compile-expr-number-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr 42 ctx)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list 42))))))

(defun run-test-compile-expr-nil-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr nil ctx)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list nil))))))

(defun run-test-compile-expr-t-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr t ctx)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list t))))))

(defun run-test-compile-expr-quote ()
  ; quoteの内側はデータであり評価されないため、リストそのものが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr '(quote (a b c)) ctx)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list '(a b c)))))))

(defun run-test-compile-expr-multiple-consts-share-ctx ()
  ; 同じctxを複数回のcompile-exprに使うと、定数プールのindexが連番で増える
  (let ((ctx (compile-make-ctx)))
    (let ((ir1 (compile-expr 10 ctx)))
      (let ((ir2 (compile-expr 20 ctx)))
        (and (struct-eq ir1 (list (list *op-const* 0)))
             (struct-eq ir2 (list (list *op-const* 1)))
             (struct-eq (compile-ctx-constants ctx) (list 10 20)))))))

(defun run-test-compile-expr-if-with-else ()
  ; (if 1 2 3)。ラベル名はgensymで毎回変わるため、IRを直接比較せず
  ; assembleした最終バイト列(ラベル解決済みの絶対offsetのみが残る)で比較する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 2 3) ctx))))
      (and (struct-eq bytes (list 0 0 4 8 0 1 3 10 0 2))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3))))))

(defun run-test-compile-expr-if-without-else ()
  ; (if 0 1)。elseを省略するとnilが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 0 1) ctx))))
      (and (struct-eq bytes (list 0 0 4 8 0 1 3 10 0 2))
           (struct-eq (compile-ctx-constants ctx) (list 0 1 nil))))))

(defun run-test-compile-expr-nested-if ()
  ; (if 1 (if 2 3 4) 5)。入れ子のifが生成するラベルが互いに衝突しないこと、
  ; ctxの定数プールが内側外側を通して正しく1つに保たれることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 (if 2 3 4) 5) ctx))))
      (and (struct-eq bytes (list 0 0 4 16 0 1 4 12 0 2 3 14 0 3 3 18 0 4))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3 4 5))))))

(defun run-test-compile-expr ()
  (and (run-test-compile-expr-number-literal)
       (run-test-compile-expr-nil-literal)
       (run-test-compile-expr-t-literal)
       (run-test-compile-expr-quote)
       (run-test-compile-expr-multiple-consts-share-ctx)
       (run-test-compile-expr-if-with-else)
       (run-test-compile-expr-if-without-else)
       (run-test-compile-expr-nested-if)))
