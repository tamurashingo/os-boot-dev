; milestone 29: 標準ライブラリの自己ホスティング化。
; cons/car/cdr・型判定・算術の核（+/-/<）・IOなど「本当に低レベルな操作」だけをC側の
; 組み込みに残し、それ以外はここでLisp自身で定義する。EfiMainが起動時に自動でこの
; ファイルをloadする（documents/bare_metal_lisp.md参照）。
;
; milestone63: コンパイラ本体（macroexpand-all・アセンブラ・compile-expr一式・
; compile-and-run）はlisp/compiler.lispへ分割した。
;
; milestone65: 起動シーケンスを2段階化し、EfiMainはこのファイルより先にcompiler.lisp
; を読み込むよう順序を変更した。ただしmacroexpand-all/compile-expr自身の実装が
; not/null/list/append/reverse/mapcarに依存しているため、この6つはcompiler.lisp側に
; 移設した（コンパイラというプログラム自身のブートストラップに必要なため。詳細は
; lisp/compiler.lispの冒頭コメント参照）。compiler.lisp末尾のmark-compiler-readyで
; フラグがtrueになった直後にこのファイルが読み込まれるため、このファイル自身が
; stdlib.lisp/compiler.lisp分割後初めてコンパイル駆動（macroexpand-all→compile-expr→
; vm-exec）で読み込まれるファイルになる。

(defun 1+ (x) (+ x 1))
(defun 1- (x) (- x 1))

; milestone87: doの上に構築するwhileマクロ。milestone89で&restラムダリストキーワードが
; 導入されたため、以前のように仮引数リスト全体を単一symbolにしてcar/cdrで手動destructuring
; していた書き方(milestone29のbare-symbol rest-arg)をtest/bodyの明示的な仮引数に置き換えた。
; do自身がCの再帰を使わないwhile(1)ループなので、whileも(bindingsが無いdoとして)Cスタックを
; 消費しない
(defmacro while (test &rest body)
  (cons 'do (cons nil (cons (list (list 'not test)) body))))

; C側の算術の核として組み込んだ<だけを使い、比較演算子群の残りをすべてLispで導出する。
; <自体はmilestone29から可変長引数(隣接ペアの単調増加判定)に対応済みだったが、
; ここから導出する>/=/<=/>=/=も従来は「CLと異なり2引数のみサポートする」という明示的な
; 制限を掛けていた。milestone90で&restキーワード(milestone89)を使い、CLと同じ可変長引数
; 対応に書き換えた

; 隣接ペアがすべてpredを満たせばt(推移律が効く=/<=/>=はこれで十分)
(defun every-adjacent-pair (pred args)
  (if (or (null args) (null (cdr args)))
      t
      (and (funcall pred (car args) (car (cdr args)))
           (every-adjacent-pair pred (cdr args)))))

; (a b c ...)が単調減少(a>b>c>...)かどうかは、逆順にした列に<の単調増加判定を
; かけるのと同値
(defun > (&rest args) (apply < (reverse args)))

(defun <= (&rest args) (every-adjacent-pair (lambda (a b) (not (< b a))) args))
(defun >= (&rest args) (every-adjacent-pair (lambda (a b) (not (< a b))) args))
(defun = (&rest args) (every-adjacent-pair (lambda (a b) (and (not (< a b)) (not (< b a)))) args))

; /=は「どの2つも等しくない」という意味で、=と違い推移律が効かない((/= 1 2 1)は
; 1番目と3番目が等しいのでnilだが、隣接ペアだけでは1≠2かつ2≠1で誤ってtになってしまう)。
; そのため全ペアの相異性を見る専用のヘルパーが必要
(defun distinct-from-rest (x lst)
  (if (null lst)
      t
      (and (not (= x (car lst)))
           (distinct-from-rest x (cdr lst)))))

(defun all-distinct (lst)
  (if (null lst)
      t
      (and (distinct-from-rest (car lst) (cdr lst))
           (all-distinct (cdr lst)))))

(defun /= (&rest args) (all-distinct args))

(defun zerop (n) (= n 0))
(defun plusp (n) (< 0 n))
(defun minusp (n) (< n 0))

(defun nth (n lst)
  (if (zerop n)
      (car lst)
      (nth (1- n) (cdr lst))))

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

; milestone 78: defpackageマクロ。:export/:use句の値はmake-package/find-packageと同じ文字列
; とする(bare symbolを使わない理由): "load"はファイル全体を読み切ってから評価するため
; (milestone72/76/77と同根の制約)、:exportにbare symbolを書くと、defpackageのマクロ展開
; (=評価)より前にリーダーが呼び出し元の*package*へ先にinternしてしまう。後で
; (in-package name)して同じ無修飾名を書いても、lisp_intern_in_packageのfallback(見つから
; なければ新規作成)が別オブジェクトを作ってしまい、eq同一性が壊れる。文字列で受け取り、
; 展開内でintern(milestone78の新規組み込み関数)を明示的に呼んで対象パッケージへ帰属させて
; おくことでこの順序問題を回避する
(defun defpackage-clause-names (clauses keyword)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) keyword)
          (append (cdr (car clauses)) (defpackage-clause-names (cdr clauses) keyword))
          (defpackage-clause-names (cdr clauses) keyword))))

(defun defpackage-export-form (name sym-name)
  (list 'export (list 'intern sym-name (list 'find-package name)) (list 'find-package name)))

(defun defpackage-use-form (name pkg-name)
  (list 'use-package (list 'find-package pkg-name) (list 'find-package name)))

; milestone89: &restラムダリストキーワードにより、以前のように仮引数リスト全体を単一symbol
; にしてcar/cdrで手動destructuringしていた書き方(milestone29のrest-arg機構)を、パッケージ名
; nameと可変長の句((:export ...)/(:use ...))を受け取るclausesの明示的な仮引数に置き換えた
(defmacro defpackage (name &rest clauses)
  (let* ((export-forms (mapcar (lambda (n) (defpackage-export-form name n))
                                (defpackage-clause-names clauses :export)))
         (use-forms (mapcar (lambda (n) (defpackage-use-form name n))
                             (defpackage-clause-names clauses :use))))
    (cons 'progn
          (append (list (list 'make-package name))
                  (append export-forms
                          (append use-forms
                                  (list (list 'find-package name))))))))
