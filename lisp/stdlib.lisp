; milestone 29: 標準ライブラリの自己ホスティング化。
; cons/car/cdr・型判定・算術の核（+/-/<）・IOなど「本当に低レベルな操作」だけをC側の
; 組み込みに残し、それ以外はここでLisp自身で定義する。EfiMainが起動時に自動でこの
; ファイルをloadする（documents/bare_metal_lisp.md参照）。

(defun not (x) (eq x nil))
(defun null (x) (eq x nil))

; 仮引数リスト全体を単一のsymbolにする書き方(milestone29でlisp_env_bind_paramsに
; 追加したrest-arg機構)で可変長引数を受け取る。評価済みの実引数がそのままリストとして
; argsに束縛されるので、それをそのまま返すだけでCLのlistと同じ挙動になる
(defun list args args)

; CLのappendは可変長だが、このスコープでは2引数のみサポートする(明示的な範囲の限定)
(defun append (a b)
  (if (null a)
      b
      (cons (car a) (append (cdr a) b))))

(defun reverse (lst)
  (if (null lst)
      nil
      (append (reverse (cdr lst)) (cons (car lst) nil))))

(defun 1+ (x) (+ x 1))
(defun 1- (x) (- x 1))

; C側の算術の核として組み込んだ<だけを使い、比較演算子群の残りをすべてLispで導出する。
; CLと異なり2引数のみサポートする(明示的な範囲の限定)
(defun > (a b) (< b a))
(defun = (a b) (and (not (< a b)) (not (< b a))))
(defun <= (a b) (not (< b a)))
(defun >= (a b) (not (< a b)))
(defun /= (a b) (not (= a b)))

(defun zerop (n) (= n 0))
(defun plusp (n) (< 0 n))
(defun minusp (n) (< n 0))

(defun nth (n lst)
  (if (zerop n)
      (car lst)
      (nth (1- n) (cdr lst))))

; fnはローカル変数（仮引数）だが、lisp_evalの関数呼び出し分岐は呼び出し式の演算子位置を
; 常に汎用evalで評価するため、ローカル変数が指すクロージャをそのまま(fn ...)の形で
; 呼び出せる。新しいfuncall/applyプリミティブは不要（milestone29の調査で確認済み）
(defun mapcar (fn lst)
  (if (null lst)
      nil
      (cons (fn (car lst)) (mapcar fn (cdr lst)))))

; --- コンパイラフロントエンド: マクロ展開 (milestone 40, documents/lisp_vm.md 目標2着手) ---
; macroexpand-all は既存の macroexpand-1 (milestone 21) を式全体に再帰的に適用し、
; 入力S式からマクロ呼び出しを完全に消し去る。quote/quasiquoteの内側はデータであり
; 評価されないため展開しない。各特殊形式は、変数名・タグ名など評価されない位置は
; そのまま保持し、評価される位置のサブフォームだけをmacroexpand-allする
(defun macroexpand-all-let-binding (binding)
  (list (car binding) (macroexpand-all (car (cdr binding)))))

(defun macroexpand-all-let (form)
  (cons (car form)
        (cons (mapcar macroexpand-all-let-binding (car (cdr form)))
              (mapcar macroexpand-all (cdr (cdr form))))))

(defun macroexpand-all-cond-clause (clause)
  (mapcar macroexpand-all clause))

; formはこの時点でトップレベルのマクロ呼び出しではないと分かっているconsである。
; いずれの特殊形式にも該当しない場合は関数呼び出しとみなし、演算子位置も含め
; 全要素を展開する（lisp_evalは演算子位置も汎用evalするため、lambda式の直書きなども
; そのままmacroexpand-allに乗る。stdlib.lispのmapcar冒頭の注釈参照）
(defun macroexpand-all-forms (form)
  (let ((op (car form)))
    (cond
      ((eq op 'quote) form)
      ((eq op 'quasiquote) form)
      ((eq op 'defmacro) form)
      ((eq op 'if) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'progn) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'and) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'or) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'when) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'unless) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'setq)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'let) (macroexpand-all-let form))
      ((eq op 'let*) (macroexpand-all-let form))
      ((eq op 'cond) (cons op (mapcar macroexpand-all-cond-clause (cdr form))))
      ((eq op 'block)
       (cons op (cons (car (cdr form)) (mapcar macroexpand-all (cdr (cdr form))))))
      ((eq op 'return-from)
       (if (null (cdr (cdr form)))
           form
           (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form)))))))
      ((eq op 'lambda)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'defvar)
       (if (null (cdr (cdr form)))
           form
           (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form)))))))
      ((eq op 'defparameter)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'defun)
       (list op (car (cdr form)) (car (cdr (cdr form)))
             (macroexpand-all (car (cdr (cdr (cdr form)))))))
      (t (mapcar macroexpand-all form)))))

; macroexpand-1(lisp_macroexpand_1)は呼び出し式の演算子を無条件にlisp_evalで評価して
; マクロかどうかを確認するため、if/let/quoteなど特殊形式のキーワードをそのまま渡すと
; 「unbound variable」でpanicする（特殊形式のキーワードは通常の変数として束縛されておらず、
; lisp_eval側では関数呼び出し分岐に入る前の専用チェックで捕まえているため）。従って特殊形式は
; マクロ呼び出しであり得ないと分かった時点でmacroexpand-1を呼ばずに直接macroexpand-all-forms
; へ渡す。マクロ呼び出しの可能性がある（特殊形式のキーワードではない）formだけがmacroexpand-1
; の対象になる
(defun macroexpand-all-special-form-p (op)
  (or (eq op 'quote) (eq op 'quasiquote) (eq op 'if) (eq op 'progn)
      (eq op 'let) (eq op 'let*) (eq op 'setq) (eq op 'cond)
      (eq op 'and) (eq op 'or) (eq op 'when) (eq op 'unless)
      (eq op 'block) (eq op 'return-from) (eq op 'lambda)
      (eq op 'defvar) (eq op 'defparameter) (eq op 'defun) (eq op 'defmacro)))

(defun macroexpand-all (form)
  (if (atom form)
      form
      (if (macroexpand-all-special-form-p (car form))
          (macroexpand-all-forms form)
          (let ((expanded (macroexpand-1 form)))
            (if (eq expanded form)
                (macroexpand-all-forms expanded)
                (macroexpand-all expanded))))))

; compileはS式をVM用バイトコードへ変換する入口。まずmacroexpand-allでマクロを
; 全て消し去ってから本番のコード生成に渡す。本番のコード生成(compile-expr、
; milestone42以降)はまだ存在しないため、現時点では展開結果をそのまま返す
(defun compile (form) (macroexpand-all form))
