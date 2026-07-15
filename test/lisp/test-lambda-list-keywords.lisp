; milestone89 (CommonLispラムダリストキーワード: &optional/&rest/&key/&aux/&allow-other-keys)
; の動作確認用テスト。QEMU起動後、(load "test\test-lambda-list-keywords.lisp") でロードしてから
; (run-test-lambda-list-keywords) を呼び出し、t が返れば全項目成功。
;
; 異常終了系(必須引数不足・未知キーワード・キーワード順序違反・コンパイル済みコード内ネスト
; lambdaでのpanic)はこのファイルでは検証しない。panicはREPL全体を巻き込むため、
; make testの一括実行とは別に、個別にmake test-<name>でQEMU実行して確認する。

; equalが無いため、S式の構造をcar/cdr/eqで手でたどって比較する汎用ヘルパー
; (milestone40のtest-compile.lisp struct-eqと同じ実装)
(defun struct-eq (a b)
  (if (atom a)
      (eq a b)
      (and (not (atom b))
           (struct-eq (car a) (car b))
           (struct-eq (cdr a) (cdr b)))))

(defun kw-opt (a &optional (b 10) (c 20 c-p))
  (list a b c c-p))

(defun run-test-lambda-list-keywords-optional-defaults ()
  ; 未指定の&optionalはdefault-formを評価した値になり、supplied-pはnil
  (struct-eq (kw-opt 1) (list 1 10 20 nil)))

(defun run-test-lambda-list-keywords-optional-supplied ()
  ; 実引数が渡された&optionalはその値になり、supplied-pはt
  (struct-eq (kw-opt 1 2 3) (list 1 2 3 t)))

(defun kw-rest (a &rest r)
  (list a r))

(defun run-test-lambda-list-keywords-rest-alone ()
  ; &restは残りの実引数すべてをリストとして束縛する(0個でもnilではなくnilリスト)
  (and (struct-eq (kw-rest 1 2 3) (list 1 (list 2 3)))
       (struct-eq (kw-rest 1) (list 1 nil))))

(defun kw-rest-opt (a &optional (b 5) &rest r)
  (list a b r))

(defun run-test-lambda-list-keywords-rest-with-optional ()
  ; &optionalが実引数を消費し終えた後の残りだけが&restに渡る
  (and (struct-eq (kw-rest-opt 1) (list 1 5 nil))
       (struct-eq (kw-rest-opt 1 2 3 4) (list 1 2 (list 3 4)))))

(defun kw-key (&key (x 1) (y 2 y-p))
  (list x y y-p))

(defun run-test-lambda-list-keywords-key-defaults ()
  ; &keyが1つも渡されなければ全てdefault-form、supplied-pはnil
  (struct-eq (kw-key) (list 1 2 nil)))

(defun run-test-lambda-list-keywords-key-supplied ()
  ; :y 99のように渡されたキーワードはその値になり、supplied-pはt。渡されなかった:xは
  ; default-formのまま
  (and (struct-eq (kw-key :y 99) (list 1 99 t))
       (struct-eq (kw-key :x 7 :y 8) (list 7 8 t))))

(defun kw-rest-key (&rest r &key (z 42))
  (list r z))

(defun run-test-lambda-list-keywords-rest-with-key ()
  ; &restと&keyは同じ残り実引数を別の形で見る(&restは消費しないので&keyが続けて
  ; キーワード/値ペアとして解釈できる)
  (struct-eq (kw-rest-key :z 100) (list (list :z 100) 100)))

(defun kw-aux (a &aux (b (+ a 1)) (c (+ b 1)))
  (list a b c))

(defun run-test-lambda-list-keywords-aux-sequential ()
  ; 後のaux変数の init-form は前のaux変数を参照できる(let*同様の逐次初期化)
  (struct-eq (kw-aux 1) (list 1 2 3)))

(defun kw-allow (&key x &allow-other-keys)
  (list x))

(defun run-test-lambda-list-keywords-allow-other-keys ()
  ; &allow-other-keysがあれば宣言されていないキーワード(:unknown)も許容される
  (struct-eq (kw-allow :x 1 :unknown 2) (list 1)))

; kw-opt/kw-rest/kw-keyはいずれもlisp_defun_params_needs_interpreterによりツリーウォーク
; インタプリタのクロージャのままになる。call-kw-opt-from-compiledは通常の(キーワードを
; 含まない)defunなのでコンパイルされ、OP_CALLからkw-optを呼ぶ経路がlisp_applyへ
; フォールバックして正しく動作することを確認する(既存のlist/append等と同じ経路の回帰確認)
(defun call-kw-opt-from-compiled (a b)
  (kw-opt a b))

(defun run-test-lambda-list-keywords-called-from-compiled-code ()
  (and (struct-eq (call-kw-opt-from-compiled 5 6) (list 5 6 20 nil))
       ; mapcarもコンパイル済みのstdlib関数であり、その内部から渡されたlambda経由で
       ; kw-keyを呼び出しても正しく動作する
       (struct-eq (mapcar (lambda (n) (kw-key :x n)) (list 1 2 3))
                  (list (list 1 2 nil) (list 2 2 nil) (list 3 2 nil)))))

(defun run-test-lambda-list-keywords ()
  (and (run-test-lambda-list-keywords-optional-defaults)
       (run-test-lambda-list-keywords-optional-supplied)
       (run-test-lambda-list-keywords-rest-alone)
       (run-test-lambda-list-keywords-rest-with-optional)
       (run-test-lambda-list-keywords-key-defaults)
       (run-test-lambda-list-keywords-key-supplied)
       (run-test-lambda-list-keywords-rest-with-key)
       (run-test-lambda-list-keywords-aux-sequential)
       (run-test-lambda-list-keywords-allow-other-keys)
       (run-test-lambda-list-keywords-called-from-compiled-code)))
