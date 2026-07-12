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

; milestone43でcompile-exprはコンパイル時レキシカル環境envを受け取る3引数に
; なった。レキシカル変数を使わないテストはenvにnilを渡す

(defun run-test-compile-expr-number-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr 42 ctx nil)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list 42))))))

(defun run-test-compile-expr-nil-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr nil ctx nil)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list nil))))))

(defun run-test-compile-expr-t-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr t ctx nil)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list t))))))

(defun run-test-compile-expr-quote ()
  ; quoteの内側はデータであり評価されないため、リストそのものが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr '(quote (a b c)) ctx nil)))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list '(a b c)))))))

(defun run-test-compile-expr-multiple-consts-share-ctx ()
  ; 同じctxを複数回のcompile-exprに使うと、定数プールのindexが連番で増える
  (let ((ctx (compile-make-ctx)))
    (let ((ir1 (compile-expr 10 ctx nil)))
      (let ((ir2 (compile-expr 20 ctx nil)))
        (and (struct-eq ir1 (list (list *op-const* 0)))
             (struct-eq ir2 (list (list *op-const* 1)))
             (struct-eq (compile-ctx-constants ctx) (list 10 20)))))))

(defun run-test-compile-expr-if-with-else ()
  ; (if 1 2 3)。ラベル名はgensymで毎回変わるため、IRを直接比較せず
  ; assembleした最終バイト列(ラベル解決済みの絶対offsetのみが残る)で比較する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 2 3) ctx nil))))
      (and (struct-eq bytes (list 0 0 4 8 0 1 3 10 0 2))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3))))))

(defun run-test-compile-expr-if-without-else ()
  ; (if 0 1)。elseを省略するとnilが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 0 1) ctx nil))))
      (and (struct-eq bytes (list 0 0 4 8 0 1 3 10 0 2))
           (struct-eq (compile-ctx-constants ctx) (list 0 1 nil))))))

(defun run-test-compile-expr-nested-if ()
  ; (if 1 (if 2 3 4) 5)。入れ子のifが生成するラベルが互いに衝突しないこと、
  ; ctxの定数プールが内側外側を通して正しく1つに保たれることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 (if 2 3 4) 5) ctx nil))))
      (and (struct-eq bytes (list 0 0 4 16 0 1 4 12 0 2 3 14 0 3 3 18 0 4))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3 4 5))))))

; --- milestone43: コンパイル時レキシカル環境とlet/変数参照/setq ---

(defun run-test-compile-expr-let-simple ()
  ; (let ((x 5)) x) -> xの初期値5をOP_CONSTで積みOP_MAKE_LOCALでボックス化(slot0)、
  ; 本体のxはOP_LOAD_LOCAL 0に解決される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 5)) x) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 5 0))
           (struct-eq (compile-ctx-constants ctx) (list 5))))))

(defun run-test-compile-expr-let-multiple-bindings ()
  ; (let ((x 1) (y 2)) y) -> xがslot0、yがslot1に割り当てられ、
  ; 本体のyはOP_LOAD_LOCAL 1に解決される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1) (y 2)) y) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1 7 5 1))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-setq ()
  ; (let ((x 1)) (setq x 2)) -> setqはOP_STORE_LOCALの後にOP_LOAD_LOCALで
  ; 同じスロットを読み直し、代入した値2をそのまま式の値として残す
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) (setq x 2)) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1 6 0 5 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-nested-shadow ()
  ; (let ((x 1)) (let ((x 2)) x)) -> 内側のxは外側のxとは別のslot1に割り当てられ、
  ; 本体のxは内側の束縛(slot1)を正しくシャドーイングする(外側slot0は読まれない)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) (let ((x 2)) x)) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1 7 5 1))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-unbound-symbol-falls-back ()
  ; (let ((x 1)) y) -> yはどの束縛にも無いレキシカル変数名なので、
  ; milestone42と同じ自己評価的な定数としてフォールバックする(既知の制約)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) y) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1))
           (struct-eq (compile-ctx-constants ctx) (list 1 'y))))))

(defun run-test-compile-expr-let-parallel-bindings-dont-see-each-other ()
  ; (let ((x 1) (y x)) y) -> letはlet*と異なり全init-formを束縛前の外側envで
  ; コンパイルするため、yの初期値であるxはまだ見えておらず、
  ; ローカル変数参照ではなく自己評価的な定数xとして扱われる
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1) (y x)) y) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1 7 5 1))
           (struct-eq (compile-ctx-constants ctx) (list 1 'x))))))

(defun run-test-compile-expr-let-with-if ()
  ; (let ((x 5)) (if 1 x 9)) -> ifの各分岐のコンパイルにletで拡張したenvが
  ; 正しく渡り、then節のxがOP_LOAD_LOCAL 0に解決されることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 5)) (if 1 x 9)) ctx nil))))
      (and (struct-eq bytes (list 0 0 7 0 1 4 11 5 0 3 13 0 2))
           (struct-eq (compile-ctx-constants ctx) (list 5 1 9))))))

(defun run-test-compile-expr ()
  (and (run-test-compile-expr-number-literal)
       (run-test-compile-expr-nil-literal)
       (run-test-compile-expr-t-literal)
       (run-test-compile-expr-quote)
       (run-test-compile-expr-multiple-consts-share-ctx)
       (run-test-compile-expr-if-with-else)
       (run-test-compile-expr-if-without-else)
       (run-test-compile-expr-nested-if)
       (run-test-compile-expr-let-simple)
       (run-test-compile-expr-let-multiple-bindings)
       (run-test-compile-expr-let-setq)
       (run-test-compile-expr-let-nested-shadow)
       (run-test-compile-expr-let-unbound-symbol-falls-back)
       (run-test-compile-expr-let-parallel-bindings-dont-see-each-other)
       (run-test-compile-expr-let-with-if)))
