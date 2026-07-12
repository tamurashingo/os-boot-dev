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

(defun run-test-compile-and-run ()
  (and (run-test-compile-and-run-arithmetic)
       (run-test-compile-and-run-if-then)
       (run-test-compile-and-run-if-else)
       (run-test-compile-and-run-let-locals)
       (run-test-compile-and-run-setq)
       (run-test-compile-and-run-primitives)
       (run-test-compile-and-run-recursive-call)
       (run-test-compile-and-run-closure-captures-state)))
