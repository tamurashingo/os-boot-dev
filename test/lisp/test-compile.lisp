; milestone 40 (コンパイラフロントエンド: マクロ展開) の動作確認用テスト。
; QEMU起動後、(load "test\test-compile.lisp") でロードしてから
; (run-test-compile) を呼び出し、t が返れば全項目成功。
; macroexpand-all/compileはlisp/stdlib.lispでEfiMainの起動シーケンス中に
; 既に読み込まれている前提（このテストファイルでの再定義は不要）。

; equal/listのequal相当が無いため、S式の構造をcar/cdr/eqで手でたどって比較する
; 汎用ヘルパー。symbolはinterningでeqが効き、fixnumも値がそのままタグ付き
; ビット列になるためeqで値比較になる（nilも非consなのでatom）
(defun struct-eq (a b)
  (if (atom a)
      (eq a b)
      (and (not (atom b))
           (struct-eq (car a) (car b))
           (struct-eq (cdr a) (cdr b)))))

(defmacro mx-double (x) (list '+ x x))
(defmacro mx-quad (x) (list 'mx-double (list 'mx-double x)))

(defun run-test-compile-non-macro ()
  ; マクロ呼び出しを含まない式はそのままの構造で返る
  (struct-eq (macroexpand-all '(+ 1 2)) '(+ 1 2)))

(defun run-test-compile-macro-basic ()
  (struct-eq (macroexpand-all '(mx-double 5)) '(+ 5 5)))

(defun run-test-compile-macro-recursive ()
  ; マクロが別のマクロ呼び出しへ展開される場合、再度展開されるまで繰り返す
  (struct-eq (macroexpand-all '(mx-quad 3)) '(+ (+ 3 3) (+ 3 3))))

(defun run-test-compile-macro-in-if ()
  (struct-eq (macroexpand-all '(if (mx-double 1) (mx-double 2) (mx-double 3)))
             '(if (+ 1 1) (+ 2 2) (+ 3 3))))

(defun run-test-compile-macro-in-let ()
  ; let束縛の変数名(a, b)自体は展開されず、初期値式と本体だけが展開される
  (struct-eq (macroexpand-all '(let ((a (mx-double 1)) (b 2)) (mx-double a)))
             '(let ((a (+ 1 1)) (b 2)) (+ a a))))

(defun run-test-compile-macro-in-lambda ()
  (struct-eq (macroexpand-all '(lambda (x) (mx-double x)))
             '(lambda (x) (+ x x))))

(defun run-test-compile-macro-in-setq ()
  ; 変数名(y)は展開対象ではなく、代入する値の式だけが展開される
  (struct-eq (macroexpand-all '(setq y (mx-double 4))) '(setq y (+ 4 4))))

(defun run-test-compile-macro-in-cond ()
  (struct-eq (macroexpand-all '(cond ((mx-double 1) (mx-double 2)) (t (mx-double 3))))
             '(cond ((+ 1 1) (+ 2 2)) (t (+ 3 3)))))

(defun run-test-compile-macro-in-defun ()
  ; 関数名(foo)・仮引数リスト(x)は展開対象ではなく、本体だけが展開される
  (struct-eq (macroexpand-all '(defun foo (x) (mx-double x)))
             '(defun foo (x) (+ x x))))

(defun run-test-compile-quote-untouched ()
  ; quoteの内側はデータであり評価されないため、マクロ呼び出しに見える形でも展開しない
  (struct-eq (macroexpand-all '(quote (mx-double 5))) '(quote (mx-double 5))))

(defun run-test-compile-basic ()
  ; 目標2のcompileはまだcompile-exprを持たないため、現時点ではmacroexpand-allの
  ; 結果をそのまま返すだけであることを確認する
  (struct-eq (compile '(mx-double 7)) '(+ 7 7)))

(defun run-test-compile ()
  (and (run-test-compile-non-macro)
       (run-test-compile-macro-basic)
       (run-test-compile-macro-recursive)
       (run-test-compile-macro-in-if)
       (run-test-compile-macro-in-let)
       (run-test-compile-macro-in-lambda)
       (run-test-compile-macro-in-setq)
       (run-test-compile-macro-in-cond)
       (run-test-compile-macro-in-defun)
       (run-test-compile-quote-untouched)
       (run-test-compile-basic)))
