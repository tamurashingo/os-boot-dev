; milestone 29: 標準ライブラリの自己ホスティング化。
; cons/car/cdr・型判定・算術の核（+/-/<）・IOなど「本当に低レベルな操作」だけをC側の
; 組み込みに残し、それ以外はここでLisp自身で定義する。EfiMainが起動時に自動でこの
; ファイルをloadする（documents/bare_metal_lisp.md参照）。
;
; milestone63: コンパイラ本体（macroexpand-all・アセンブラ・compile-expr一式・
; compile-and-run）はlisp/compiler.lispへ分割した。EfiMainはこのファイルの直後に
; compiler.lispを読み込む（compile-expr内部で使うmapcar等はこのファイルで先に
; 定義されている必要があるため、読み込み順序はstdlib.lisp→compiler.lispを維持する）。

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

; milestone61: funcall/apply(C側のlisp_applyへの薄いラッパー、コンパイル済み・
; インタプリタ・ビルトインいずれのクロージャも一貫して呼び出せる)を使って、
; fnがローカル変数(仮引数)の場合の呼び出しディスパッチも検証する。CLのreduceと
; 異なりinitは必須(空リストの場合の初期値をfnから逆算する仕組みは持たない)、
; 2引数のみサポートする(明示的な範囲の限定、append/>等と同じ方針)
(defun reduce (fn lst init)
  (if (null lst)
      init
      (reduce fn (cdr lst) (funcall fn init (car lst)))))

; predは2引数の述語(a bでa<bならt相当)。挿入ソート(O(n^2))で十分な規模の想定
; (このLisp処理系にheap-remaining程度の性能インセンティブしか無いため、複雑な
; アルゴリズムを導入する理由が無い)。applyは(pred a b)の代わりに明示的に使い、
; 引数リストを経由する呼び出し経路(funcallとは異なる)も一貫して動くことを検証する
(defun sort-insert (pred x lst)
  (if (null lst)
      (cons x nil)
      (if (apply pred (list x (car lst)))
          (cons x lst)
          (cons (car lst) (sort-insert pred x (cdr lst))))))

(defun sort (pred lst)
  (if (null lst)
      nil
      (sort-insert pred (car lst) (sort pred (cdr lst)))))
