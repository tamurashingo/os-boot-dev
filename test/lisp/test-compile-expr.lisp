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

; milestone43でcompile-exprはコンパイル時レキシカル環境(milestone44でスコープ
; 構造に拡張された)を受け取る3引数になった。レキシカル変数を使わないテストでも、
; scope自身をnilにはできない(compile-variable-refが必ずcompile-resolve経由で
; scope-locals等を呼ぶため、milestone44以降はnilを渡すと(car nil)でpanicする)。
; そのため実体を持つトップレベルscope(compile-make-top-scope)を渡す

(defun run-test-compile-expr-number-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr 42 ctx (compile-make-top-scope))))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list 42))))))

(defun run-test-compile-expr-nil-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr nil ctx (compile-make-top-scope))))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list nil))))))

(defun run-test-compile-expr-t-literal ()
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr t ctx (compile-make-top-scope))))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list t))))))

(defun run-test-compile-expr-quote ()
  ; quoteの内側はデータであり評価されないため、リストそのものが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((ir (compile-expr '(quote (a b c)) ctx (compile-make-top-scope))))
      (and (struct-eq ir (list (list *op-const* 0)))
           (struct-eq (compile-ctx-constants ctx) (list '(a b c)))))))

(defun run-test-compile-expr-multiple-consts-share-ctx ()
  ; 同じctxを複数回のcompile-exprに使うと、定数プールのindexが連番で増える
  (let ((ctx (compile-make-ctx)))
    (let ((ir1 (compile-expr 10 ctx (compile-make-top-scope))))
      (let ((ir2 (compile-expr 20 ctx (compile-make-top-scope))))
        (and (struct-eq ir1 (list (list *op-const* 0)))
             (struct-eq ir2 (list (list *op-const* 1)))
             (struct-eq (compile-ctx-constants ctx) (list 10 20)))))))

(defun run-test-compile-expr-if-with-else ()
  ; (if 1 2 3)。ラベル名はgensymで毎回変わるため、IRを直接比較せず
  ; assembleした最終バイト列(ラベル解決済みの絶対offsetのみが残る)で比較する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 2 3) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 4 12 0 0 1 0 3 15 0 0 2 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3))))))

(defun run-test-compile-expr-if-without-else ()
  ; (if 0 1)。elseを省略するとnilが定数として登録される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 0 1) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 4 12 0 0 1 0 3 15 0 0 2 0))
           (struct-eq (compile-ctx-constants ctx) (list 0 1 nil))))))

(defun run-test-compile-expr-nested-if ()
  ; (if 1 (if 2 3 4) 5)。入れ子のifが生成するラベルが互いに衝突しないこと、
  ; ctxの定数プールが内側外側を通して正しく1つに保たれることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(if 1 (if 2 3 4) 5) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 4 24 0 0 1 0 4 18 0 0 2 0 3 21 0 0 3 0 3 27 0 0 4 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2 3 4 5))))))

; --- milestone43: コンパイル時レキシカル環境とlet/変数参照/setq ---

; milestone83/84でOP_MAKE_LOCALは2byteのFP相対slot indexオペランドを取るようになった
; (1byte→3byte)。以下、let/lambdaを含む全テストのバイト列・closure-template構造
; (nargsの直後にmax-localsが1要素追加)をこれに合わせて更新している

(defun run-test-compile-expr-let-simple ()
  ; (let ((x 5)) x) -> xの初期値5をOP_CONSTで積みOP_MAKE_LOCAL 0でボックス化(slot0)、
  ; 本体のxはOP_LOAD_LOCAL 0に解決される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 5)) x) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 5 0 0))
           (struct-eq (compile-ctx-constants ctx) (list 5))))))

(defun run-test-compile-expr-let-multiple-bindings ()
  ; (let ((x 1) (y 2)) y) -> xがslot0、yがslot1に割り当てられ、
  ; 本体のyはOP_LOAD_LOCAL 1に解決される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1) (y 2)) y) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 0 1 0 7 1 0 5 1 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-setq ()
  ; (let ((x 1)) (setq x 2)) -> setqはOP_STORE_LOCALの後にOP_LOAD_LOCALで
  ; 同じスロットを読み直し、代入した値2をそのまま式の値として残す
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) (setq x 2)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 0 1 0 6 0 0 5 0 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-nested-shadow ()
  ; (let ((x 1)) (let ((x 2)) x)) -> 内側のxは外側のxとは別のslot1に割り当てられ、
  ; 本体のxは内側の束縛(slot1)を正しくシャドーイングする(外側slot0は読まれない)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) (let ((x 2)) x)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 0 1 0 7 1 0 5 1 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-let-unbound-symbol-falls-back ()
  ; (let ((x 1)) y) -> yはどの束縛にも無いレキシカル変数名なので、milestone51以降は
  ; グローバル参照(OP_GLOBAL_REF)としてコンパイルされる(milestone42〜50時点では
  ; 自己評価的な定数へフォールバックしていたが、それは誤りだったため置き換えた)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1)) y) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 16 1 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 'y))))))

(defun run-test-compile-expr-let-parallel-bindings-dont-see-each-other ()
  ; (let ((x 1) (y x)) y) -> letはlet*と異なり全init-formを束縛前の外側scopeで
  ; コンパイルするため、yの初期値であるxはまだ見えておらず、ローカル変数参照ではなく
  ; グローバル参照(OP_GLOBAL_REF)としてコンパイルされる(milestone51)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 1) (y x)) y) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 16 1 0 7 1 0 5 1 0))
           (struct-eq (compile-ctx-constants ctx) (list 1 'x))))))

(defun run-test-compile-expr-let-with-if ()
  ; (let ((x 5)) (if 1 x 9)) -> ifの各分岐のコンパイルにletで拡張したscopeが
  ; 正しく渡り、then節のxがOP_LOAD_LOCAL 0に解決されることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((x 5)) (if 1 x 9)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 0 1 0 4 18 0 5 0 0 3 21 0 0 2 0))
           (struct-eq (compile-ctx-constants ctx) (list 5 1 9))))))

; --- milestone44: compile-expr(lambdaとクロージャ捕捉) ---

(defun run-test-compile-expr-lambda-no-capture ()
  ; (lambda (x) x) -> 自由変数を一切持たないlambda。パッケージは
  ; (closure-template nargs=1 max-locals=1 bytecode=(OP_LOAD_LOCAL 0 OP_RETURN) constants=() upvalue-descs=())
  ; (milestone46: compile-lambdaが本体コード末尾に明示的にOP_RETURNを追加するようになった。
  ; milestone83/84: closure-templateのnargsの直後にmax-locals要素が追加された)
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(lambda (x) x) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 9 0 0))
           (struct-eq (compile-ctx-constants ctx)
                      (list (list 'closure-template 1 1 (list 5 0 0 2) nil nil)))))))

(defun run-test-compile-expr-lambda-nargs ()
  ; (lambda (x y) x) -> 仮引数は宣言順にslot0,1へ割り当てられ、パッケージの
  ; 2番目の要素(nargs)が2になることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(lambda (x y) x) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 9 0 0))
           (struct-eq (compile-ctx-constants ctx)
                      (list (list 'closure-template 2 2 (list 5 0 0 2) nil nil)))))))

(defun run-test-compile-expr-lambda-captures-outer-local ()
  ; (let ((n 5)) (lambda () n)) -> nはlambdaの外側scopeのローカル(slot0)なので、
  ; kind=0(直接ローカル捕捉)のupvalue記述子(0 . 0)が1つ登録され、
  ; 本体はOP_LOAD_UPVALUE 0に解決される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((n 5)) (lambda () n)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 7 0 0 9 1 0))
           (struct-eq (compile-ctx-constants ctx)
                      (list 5 (list 'closure-template 0 0 (list 10 0 0 2) nil (list (cons 0 0)))))))))

(defun run-test-compile-expr-lambda-nested-capture-flattens ()
  ; (let ((n 5)) (lambda () (lambda () n))) -> nを直接使うのは2段内側のlambdaだけ。
  ; 中間のlambdaはkind=0(外側letのslot0を直接捕捉)、内側のlambdaはkind=1
  ; (中間lambda自身のupvalue0をそのままコピー)でnを捕捉し、多段の探索をせずに
  ; フラット化できることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((n 5)) (lambda () (lambda () n))) ctx (compile-make-top-scope)))))
      (let ((innermost-package (list 'closure-template 0 0 (list 10 0 0 2) nil (list (cons 1 0)))))
        (let ((middle-package (list 'closure-template 0 0 (list 9 0 0 2) (list innermost-package) (list (cons 0 0)))))
          (and (struct-eq bytes (list 0 0 0 7 0 0 9 1 0))
               (struct-eq (compile-ctx-constants ctx) (list 5 middle-package))))))))

(defun run-test-compile-expr-lambda-two-captures-memoized ()
  ; (let ((a 1) (b 2)) (lambda () (if a b b))) -> aとbの2つを別々のupvalue index
  ; (0と1)に捕捉すること、bをif式のthen/elseの両方で参照しても記述子は1個しか
  ; 増えず(memoize)、両方の参照が同じOP_LOAD_UPVALUE 1に解決されることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((a 1) (b 2)) (lambda () (if a b b))) ctx (compile-make-top-scope)))))
      (let ((package (list 'closure-template 0 0 (list 10 0 0 4 12 0 10 1 0 3 15 0 10 1 0 2) nil (list (cons 0 0) (cons 0 1)))))
        (and (struct-eq bytes (list 0 0 0 7 0 0 0 1 0 7 1 0 9 2 0))
             (struct-eq (compile-ctx-constants ctx) (list 1 2 package)))))))

(defun run-test-compile-expr-lambda-param-and-capture-together ()
  ; (let ((n 100)) (lambda (x) (if x n x))) -> 自分の仮引数xはOP_LOAD_LOCAL、
  ; 外側letのnはOP_LOAD_UPVALUEに解決され、ローカル参照とupvalue捕捉が
  ; 1つの本体の中で共存できることを検証する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((n 100)) (lambda (x) (if x n x))) ctx (compile-make-top-scope)))))
      (let ((package (list 'closure-template 1 1 (list 5 0 0 4 12 0 10 0 0 3 15 0 5 0 0 2) nil (list (cons 0 0)))))
        (and (struct-eq bytes (list 0 0 0 7 0 0 9 1 0))
             (struct-eq (compile-ctx-constants ctx) (list 100 package)))))))

; --- milestone45: compile-expr(関数呼び出し・プリミティブ呼び出し) ---

(defun run-test-compile-expr-cons ()
  ; (cons 1 2) -> OP_CONSはcdr_valを先にpop・car_valを後にpopするので、
  ; carになる1を先に、cdrになる2を後にコンパイルして積む
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(cons 1 2) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 0 1 0 12))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-car ()
  ; (car (cons 1 2)) -> 引数式のコンパイル結果の直後にOP_CARを1つ追加するだけ
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(car (cons 1 2)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 0 1 0 12 13))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-cdr ()
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(cdr (cons 1 2)) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 0 1 0 12 14))
           (struct-eq (compile-ctx-constants ctx) (list 1 2))))))

(defun run-test-compile-expr-eq ()
  ; (eq 1 1) -> compile-literalは値の同一性を確認せず毎回新しい定数indexを
  ; 発行するため、同じ値1でもconstantsには2回登録される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(eq 1 1) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 0 1 0 15))
           (struct-eq (compile-ctx-constants ctx) (list 1 1))))))

(defun run-test-compile-expr-call-simple ()
  ; (let ((f (lambda (x) x))) (f 5)) -> letでfにクロージャを束縛し、本体でfを
  ; 呼び出す。OP_CALLの呼び出し規約に合わせ、引数5を先に積み、関数式fを
  ; その後に積んでからOP_CALL 1を発行する
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(let ((f (lambda (x) x))) (f 5)) ctx (compile-make-top-scope)))))
      (let ((package (list 'closure-template 1 1 (list 5 0 0 2) nil nil)))
        (and (struct-eq bytes (list 9 0 0 7 0 0 0 1 0 5 0 0 8 1 0))
             (struct-eq (compile-ctx-constants ctx) (list package 5)))))))

(defun run-test-compile-expr-call-immediate-lambda-multiple-args ()
  ; ((lambda (x y) x) 1 2) -> 関数式自体がlambda式であっても(carがsymbolでは
  ; ない場合)compile-exprに委ねるだけで呼び出しをコンパイルできることを検証する。
  ; 引数は宣言順(1, 2)に先に積まれ、その後にlambda式のコンパイル結果(クロージャの
  ; 生成コード)が積まれてからOP_CALL 2が発行される
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '((lambda (x y) x) 1 2) ctx (compile-make-top-scope)))))
      (let ((package (list 'closure-template 2 2 (list 5 0 0 2) nil nil)))
        (and (struct-eq bytes (list 0 0 0 0 1 0 9 2 0 8 2 0))
             (struct-eq (compile-ctx-constants ctx) (list 1 2 package)))))))

(defun run-test-compile-expr-call-primitive-composition ()
  ; (cons (car (quote (a b))) 3) -> プリミティブ呼び出しが入れ子になり、
  ; 呼び出しの引数の中でさらに別のプリミティブ呼び出しが使われるケース
  (let ((ctx (compile-make-ctx)))
    (let ((bytes (assemble (compile-expr '(cons (car (quote (a b))) 3) ctx (compile-make-top-scope)))))
      (and (struct-eq bytes (list 0 0 0 13 0 1 0 12))
           (struct-eq (compile-ctx-constants ctx) (list (list 'a 'b) 3))))))

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
       (run-test-compile-expr-let-with-if)
       (run-test-compile-expr-lambda-no-capture)
       (run-test-compile-expr-lambda-nargs)
       (run-test-compile-expr-lambda-captures-outer-local)
       (run-test-compile-expr-lambda-nested-capture-flattens)
       (run-test-compile-expr-lambda-two-captures-memoized)
       (run-test-compile-expr-lambda-param-and-capture-together)
       (run-test-compile-expr-cons)
       (run-test-compile-expr-car)
       (run-test-compile-expr-cdr)
       (run-test-compile-expr-eq)
       (run-test-compile-expr-call-simple)
       (run-test-compile-expr-call-immediate-lambda-multiple-args)
       (run-test-compile-expr-call-primitive-composition)))
