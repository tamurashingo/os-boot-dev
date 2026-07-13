; milestone 46 (統合検証、目標2完了) の動作確認用テスト。
; QEMU起動後、(load "test\test-compile-and-run.lisp") でロードしてから
; (run-test-compile-and-run) を呼び出し、t が返れば全項目成功。
; compile-expr/assemble/compile-and-run等はlisp/stdlib.lispでEfiMainの起動
; シーケンス中に既に読み込まれている前提（このテストファイルでの再定義は不要）。
;
; milestone34-39で手動バイトコードとして検証した一連のケース（算術・分岐・
; ローカル変数・再帰呼び出し・外側変数を捕捉するクロージャ）を、同じ意味のS式
; からcompile-and-run経由で生成・実行し、目標1の手動バイトコード検証結果と
; 一致することを確認する。この段階のcompile-exprはグローバル環境への関数呼び出し
; 統合をスコープ外としているため（defun等の既存eval経路とは繋がっていない）、
; VMが直接サポートする+/cons/car/cdr/eqとlambda/let/if/setqの組み合わせのみで
; 各ケースを表現する

; 構造比較用ヘルパー(milestone40/41と同じもの。equalが無いためcar/cdr/eqで手で辿る)
(defun struct-eq (a b)
  (if (atom a)
      (eq a b)
      (and (not (atom b))
           (struct-eq (car a) (car b))
           (struct-eq (cdr a) (cdr b)))))

(defun run-test-compile-and-run-arithmetic ()
  ; milestone35の(OP_CONST 1, OP_CONST 2, OP_ADD, OP_RETURN)相当。
  ; +はmilestone39のインライン化対象ではないため、milestone46で
  ; compile-exprにOP_ADDへの直接コンパイルを追加した(compile-add)
  (eq (compile-and-run '(+ 1 2)) 3))

(defun run-test-compile-and-run-if-then ()
  ; milestone36の分岐(OP_JUMP_IF_FALSE)相当。条件が非nilならthen節の値を返す
  (eq (compile-and-run '(if 1 10 20)) 10))

(defun run-test-compile-and-run-if-else ()
  ; 条件がnilならelse節の値を返す
  (eq (compile-and-run '(if nil 10 20)) 20))

(defun run-test-compile-and-run-let-locals ()
  ; milestone36のボックス化ローカル変数(OP_MAKE_LOCAL/OP_LOAD_LOCAL)相当
  (eq (compile-and-run '(let ((x 5) (y 3)) (+ x y))) 8))

(defun run-test-compile-and-run-setq ()
  ; OP_STORE_LOCALでボックスを書き換えた後、代入した値がそのまま式の値になることを確認する
  (eq (compile-and-run '(let ((x 1)) (setq x (+ x 1)))) 2))

(defun run-test-compile-and-run-primitives ()
  ; milestone39のインライン化プリミティブ(OP_CONS/OP_CAR/OP_CDR/OP_EQ)の組み合わせ
  (eq (compile-and-run '(eq (car (cons 1 2)) 1)) t))

(defun run-test-compile-and-run-recursive-call ()
  ; milestone37の再帰呼び出し(階乗相当)を、この段階で使える命令(+/cons/car/cdr/eq/if)
  ; だけで表現したもの: 自分自身をouter letのボックス経由で捕捉する再帰的なlen関数で
  ; consリストの長さを数える。letの本体は単一式のみなので、setqでlenへ実体を
  ; 代入する副作用を「使わない束縛の初期化式」として発生させ、その後の本体で呼び出す
  (eq (compile-and-run
        '(let ((len nil))
           (let ((ignored (setq len (lambda (lst) (if lst (+ 1 (len (cdr lst))) 0)))))
             (len (cons 1 (cons 2 (cons 3 nil)))))))
      3))

(defun run-test-compile-and-run-closure-captures-state ()
  ; milestone38のクロージャ捕捉(カウンタ)相当: make-counterが返すlambdaは
  ; 呼び出しごとに新しいボックスnをkind=0で捕捉し、setqで書き換えた状態が
  ; 同じクロージャの複数回の呼び出しをまたいで共有されることを検証する
  (struct-eq
    (compile-and-run
      '(let ((make-counter (lambda () (let ((n 0)) (lambda () (setq n (+ n 1)))))))
         (let ((c1 (make-counter)))
           (cons (c1) (cons (c1) (cons (c1) nil))))))
    (list 1 2 3)))

; milestone 50: compile-and-runがmacroexpand-allを経由していなかった配線漏れの回帰テスト。
; doubleはcompile-exprが知らないマクロ呼び出しなので、macroexpand-allで(+ 21 21)へ
; 展開されてから渡らなければコンパイルできない(展開前のまま渡ると、doubleという
; 未解決のグローバル関数参照をOP_CALLしようとしてVM側でpanicする)
(defmacro double (x) (list '+ x x))

(defun run-test-compile-and-run-macro-expansion ()
  (eq (compile-and-run '(double 21)) 42))

; milestone 51: OP_GLOBAL_REF/OP_GLOBAL_SETの回帰テスト。*compile51-global-var*は
; defvarで束縛した動的変数で、レキシカルスコープに一切現れないままcompile-and-run
; 経由で読む/書く(is_specialな変数もlisp_env_lookup/lisp_env_setの対象なので、
; OP_GLOBAL_REF/OP_GLOBAL_SETからも同じように解決できることを確認する)
(defvar *compile51-global-var* 41)

(defun run-test-compile-and-run-global-ref ()
  (eq (compile-and-run '*compile51-global-var*) 41))

(defun run-test-compile-and-run-global-setq ()
  (and (eq (compile-and-run '(setq *compile51-global-var* (+ *compile51-global-var* 1))) 42)
       (eq *compile51-global-var* 42)))

; *compile51-global-fn*はOP_CALLがコンパイル済みクロージャしか呼べない(milestone52の
; OP_CALL汎用ディスパッチ化より前)という制約の下でグローバル関数呼び出しを確認するため、
; あらかじめvm-make-closureでコンパイル済みクロージャを作って束縛しておく((lambda (x) (+ x 1))相当)
(defvar *compile51-global-fn*
  (vm-make-closure 1
                    (list *op-load-local* 0 *op-const* 0 *op-add* *op-return*)
                    (list 1)
                    nil))

(defun run-test-compile-and-run-global-call ()
  (eq (compile-and-run '(*compile51-global-fn* 10)) 11))

; milestone 52: OP_CALLの汎用ディスパッチ化の回帰テスト。OP_CALLがコンパイル済みでない
; 呼び出し先(Cビルトイン・従来のdefunによるインタプリタクロージャ)をlisp_applyへ
; フォールバックすることを確認する。atomはCビルトイン(builtin!=0)、listはstdlib.lispの
; defunで定義されたインタプリタクロージャ(body/envを持つ通常のクロージャ)なので、
; lisp_apply内の2つの分岐(closure->builtin呼び出し/lisp_eval呼び出し)を両方通す
(defun run-test-compile-and-run-call-c-builtin ()
  (eq (compile-and-run '(atom 5)) t))

(defun run-test-compile-and-run-call-interpreter-function ()
  (struct-eq (compile-and-run '(list 1 2 3)) (list 1 2 3)))

; milestone 53: lisp_applyがコンパイル済みクロージャをlisp_vm_runへ委譲する回帰テスト
; (milestone52とは逆方向、インタプリタ→VM)。compile-and-runを経由せず、通常のツリー
; ウォークインタプリタ(lisp_eval)から直接コンパイル済みクロージャを呼び出す
(defun run-test-lisp-apply-compiled-closure-direct ()
  (let ((f (vm-make-closure 1
                             (list *op-load-local* 0 *op-const* 0 *op-add* *op-return*)
                             (list 1)
                             nil)))
    (eq (f 10) 11)))

; mapcarはstdlib.lispのdefunによる従来のインタプリタクロージャで、その本体(fn (car lst))が
; lisp_evalの通常の関数呼び出し経路(lisp_apply)を通る。fnにコンパイル済みクロージャを渡すことで、
; 既存の高階ビルトイン(mapcar)がコンパイル済み・インタプリタ済みいずれのクロージャも区別なく
; 第一級の値として扱えることを確認する
(defun run-test-lisp-apply-compiled-closure-via-mapcar ()
  (let ((f (vm-make-closure 1
                             (list *op-load-local* 0 *op-const* 0 *op-add* *op-return*)
                             (list 1)
                             nil)))
    (struct-eq (mapcar f (list 1 2 3)) (list 2 3 4))))

; milestone 54: compile-expr: progn/let*/cond/and/or/when/unless対応の回帰テスト。
; test-special-forms.lisp(milestone17、lisp_evalのインタプリタ側の同じ7フォームの
; テスト)と同じ入出力の組をcompile-and-run経由で再現し、既存のif/let脱糖への
; 書き換えが元のlisp_evalの挙動と一致することを確認する
(defun run-test-compile-and-run-progn ()
  (and (eq (compile-and-run '(progn 1 2 3)) 3)
       (eq (compile-and-run '(progn)) nil)))

(defun run-test-compile-and-run-let-star ()
  (eq (compile-and-run '(let ((x 1)) (let* ((x 2) (y (+ x 1))) y))) 3))

(defun run-test-compile-and-run-cond ()
  (and (eq (compile-and-run '(cond ((eq 1 2) 100) ((eq 1 1) 200) (t 300))) 200)
       (eq (compile-and-run '(cond ((eq 1 2) 100))) nil)
       (eq (compile-and-run '(cond (5))) 5)))

(defun run-test-compile-and-run-and ()
  (and (eq (compile-and-run '(and 1 2 3)) 3)
       (eq (compile-and-run '(and 1 nil 3)) nil)
       (eq (compile-and-run '(and)) t)
       ; 短絡評価: nilに到達した時点で(car 5)は評価されずpanicしない
       (eq (compile-and-run '(and nil (car 5))) nil)))

(defun run-test-compile-and-run-or ()
  (and (eq (compile-and-run '(or nil nil 5)) 5)
       (eq (compile-and-run '(or nil nil)) nil)
       (eq (compile-and-run '(or)) nil)
       ; 短絡評価: 1が返った時点で(car 5)は評価されずpanicしない
       (eq (compile-and-run '(or 1 (car 5))) 1)))

(defun run-test-compile-and-run-when ()
  (and (eq (compile-and-run '(when (eq 1 1) 10 20)) 20)
       (eq (compile-and-run '(when (eq 1 2) 10)) nil)))

(defun run-test-compile-and-run-unless ()
  (and (eq (compile-and-run '(unless (eq 1 2) 10 20)) 20)
       (eq (compile-and-run '(unless (eq 1 1) 10)) nil)))

; milestone 55: compile-expr: block/return-from対応の回帰テスト。
; test-block-return.lisp(milestone19、lisp_evalのインタプリタ側のblock/return-from
; テスト)のケース1-6と同じ入出力の組をcompile-and-run経由で再現する。ケース7-9
; (let/let*の動的変数復元)はcompile-let/compile-let*がまだ特殊変数に対応していない
; ため対象外(milestone57で追加予定)
(defun run-test-compile-and-run-block-basic ()
  (eq (compile-and-run '(block blk (return-from blk 42) 999)) 42))

(defun run-test-compile-and-run-block-no-return ()
  (eq (compile-and-run '(block blk 1 2 3)) 3))

(defun run-test-compile-and-run-block-skips-rest ()
  (eq (compile-and-run '(block blk (return-from blk 1) (return-from blk 2))) 1))

(defun run-test-compile-and-run-block-nested-inner-tag ()
  (eq (compile-and-run '(block outer
                           (block inner
                             (return-from inner 10))
                           20))
      20))

(defun run-test-compile-and-run-block-nested-outer-tag ()
  (eq (compile-and-run '(block outer
                           (block inner
                             (return-from outer 99))
                           20))
      99))

; run-test-compile-and-run-recursive-callと同じ「使わない束縛の初期化式」の技法で
; 自己参照するlambdaを作り、再帰呼び出しを何段か経由してもreturn-fromが呼び出し元の
; blockまで正しく伝播することを確認する(OP_CALLの汎用伝播チェックの回帰テスト)
(defun run-test-compile-and-run-block-escapes-recursion ()
  (eq (compile-and-run
        '(block scan-loop
           (let ((scan nil))
             (let ((ignored (setq scan (lambda (lst target)
                                          (if lst
                                              (if (eq (car lst) target)
                                                  (return-from scan-loop (car lst))
                                                  (scan (cdr lst) target))
                                              nil)))))
               (scan (cons 1 (cons 2 (cons 3 (cons 4 (cons 5 nil))))) 3)))
           999))
      3))

(defun run-test-compile-and-run-block-return ()
  (and (run-test-compile-and-run-block-basic)
       (run-test-compile-and-run-block-no-return)
       (run-test-compile-and-run-block-skips-rest)
       (run-test-compile-and-run-block-nested-inner-tag)
       (run-test-compile-and-run-block-nested-outer-tag)
       (run-test-compile-and-run-block-escapes-recursion)))

; milestone 56: compile-expr: quasiquote対応の回帰テスト。既存インタプリタの
; lisp_qq_expand(src/lisp.c)と同じ入出力になることをcompile-and-run経由で確認する。
; unquote内でレキシカル変数を参照するケースも含め、compile-qq-desugarが組み立てる
; cons/append呼び出しが正しいスコープ(外側のlet等)でコンパイルされることを検証する
(defun run-test-compile-and-run-quasiquote-literal ()
  ; unquoteが無ければ単なるリテラルデータと同じ(quoteと同じ結果)
  (struct-eq (compile-and-run '`(1 2 3)) (list 1 2 3)))

(defun run-test-compile-and-run-quasiquote-unquote ()
  ; unquoteされた式は通常のcompile-exprと同じくその場で評価される
  (struct-eq (compile-and-run '`(1 ,(+ 1 1) 3)) (list 1 2 3)))

(defun run-test-compile-and-run-quasiquote-lexical-scope ()
  ; unquote内の式が外側のletのレキシカル変数を参照できることを確認する
  ; (compile-qq-desugarが組み立てた式がcompile-exprへ渡る際、外側のscopeを
  ; 引き続き見えるまま再帰していることの検証)
  (struct-eq (compile-and-run '(let ((x 5)) `(a ,x b))) (list 'a 5 'b)))

(defun run-test-compile-and-run-quasiquote-splicing ()
  ; unquote-splicingは評価結果のリストを周囲へ継ぎ足す(compile-qq-desugarの
  ; appendへの脱糖、milestone52のOP_CALL→lisp_apply委譲経由でappendを呼ぶ)
  (struct-eq (compile-and-run '`(1 ,@(list 2 3) 4)) (list 1 2 3 4)))

(defun run-test-compile-and-run-quasiquote-nested-literal ()
  ; ネストしたリスト構造の中の非unquote部分がそのまま保たれることを確認する
  (struct-eq (compile-and-run '`(a (b c) ,(+ 1 2))) (list 'a (list 'b 'c) 3)))

(defun run-test-compile-and-run-quasiquote ()
  (and (run-test-compile-and-run-quasiquote-literal)
       (run-test-compile-and-run-quasiquote-unquote)
       (run-test-compile-and-run-quasiquote-lexical-scope)
       (run-test-compile-and-run-quasiquote-splicing)
       (run-test-compile-and-run-quasiquote-nested-literal)))

(defun run-test-compile-and-run ()
  (and (run-test-compile-and-run-arithmetic)
       (run-test-compile-and-run-if-then)
       (run-test-compile-and-run-if-else)
       (run-test-compile-and-run-let-locals)
       (run-test-compile-and-run-setq)
       (run-test-compile-and-run-primitives)
       (run-test-compile-and-run-recursive-call)
       (run-test-compile-and-run-closure-captures-state)
       (run-test-compile-and-run-macro-expansion)
       (run-test-compile-and-run-global-ref)
       (run-test-compile-and-run-global-setq)
       (run-test-compile-and-run-global-call)
       (run-test-compile-and-run-call-c-builtin)
       (run-test-compile-and-run-call-interpreter-function)
       (run-test-lisp-apply-compiled-closure-direct)
       (run-test-lisp-apply-compiled-closure-via-mapcar)
       (run-test-compile-and-run-progn)
       (run-test-compile-and-run-let-star)
       (run-test-compile-and-run-cond)
       (run-test-compile-and-run-and)
       (run-test-compile-and-run-or)
       (run-test-compile-and-run-when)
       (run-test-compile-and-run-unless)
       (run-test-compile-and-run-block-return)
       (run-test-compile-and-run-quasiquote)))

