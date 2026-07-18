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

; milestone91: whileと同じくdoの上に構築するdolistマクロ。CLの(dolist (var list-form
; [result-form]) body...)と同じ形。list-varはgensymで衝突を避ける。end-test成立時
; (list-var=nil)はbodyのletに一切入らないため(car nil)は評価されない
(defmacro dolist (var-list &rest body)
  (let* ((var (car var-list))
         (list-form (car (cdr var-list)))
         (result-form (if (cdr (cdr var-list)) (car (cdr (cdr var-list))) nil))
         (list-var (gensym)))
    (list 'do
          (list (list list-var list-form (list 'cdr list-var)))
          (list (list 'null list-var) result-form)
          (cons 'let (cons (list (list var (list 'car list-var)))
                            body)))))

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
(defun > (&rest args) (apply #'< (reverse args)))

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

; milestone91: do-symbols/do-external-symbols/do-all-symbolsの下地となる、パッケージの
; シンボル一覧を組み立てるヘルパー群。%package-symbols/%package-exported-symbols
; （src/lisp.cの新規ビルトイン、pkg_symbols/pkg_exportsをそのまま返す）とappendのみで
; 構成する

(defun package-uses-symbols-list (used-packages)
  (if (null used-packages)
      nil
      (append (%package-exported-symbols (car used-packages))
              (package-uses-symbols-list (cdr used-packages)))))

; package内からアクセス可能な全シンボル(ローカル+useしている各パッケージのexport)。
; do-symbolsの下地
(defun package-accessible-symbols-list (package)
  (append (%package-symbols package)
          (package-uses-symbols-list (package-use-list package))))

; package自身がexportしているシンボルのみ。do-external-symbolsの下地
(defun package-external-symbols-list (package)
  (%package-exported-symbols package))

; 登録済み全パッケージのローカルシンボルを合算したもの。do-all-symbolsの下地
(defun package-all-symbols-list-helper (packages)
  (if (null packages)
      nil
      (append (%package-symbols (car packages))
              (package-all-symbols-list-helper (cdr packages)))))

(defun package-all-symbols-list ()
  (package-all-symbols-list-helper (list-all-packages)))

; milestone91: do-symbols/do-external-symbols/do-all-symbolsは、対象シンボルの集合を
; 上記ヘルパーで組み立ててdolistへ委譲するだけの薄いマクロ。(var [package [result]])を
; car/cdrで手動destructuring(defpackage等と同じ既存の方針)。packageを省略した場合の
; 既定値は*package*(動的変数)そのもの
(defmacro do-symbols (var-list &rest body)
  (let* ((var (car var-list))
         (package-form (if (cdr var-list) (car (cdr var-list)) '*package*))
         (result-form (if (cdr (cdr var-list)) (car (cdr (cdr var-list))) nil)))
    (cons 'dolist
          (cons (list var (list 'package-accessible-symbols-list package-form) result-form)
                body))))

(defmacro do-external-symbols (var-list &rest body)
  (let* ((var (car var-list))
         (package-form (if (cdr var-list) (car (cdr var-list)) '*package*))
         (result-form (if (cdr (cdr var-list)) (car (cdr (cdr var-list))) nil)))
    (cons 'dolist
          (cons (list var (list 'package-external-symbols-list package-form) result-form)
                body))))

; do-all-symbolsはCL同様packageを取らない((var [result])のみ)
(defmacro do-all-symbols (var-list &rest body)
  (let* ((var (car var-list))
         (result-form (if (cdr var-list) (car (cdr var-list)) nil)))
    (cons 'dolist
          (cons (list var (list 'package-all-symbols-list) result-form)
                body))))

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

; milestone92: :shadow句(export/use-formと同じ「1名前=1呼び出し」パターン)。名前はshadow
; ビルトインが直接受け取れる文字列designatorなのでintern等を経由せず直接渡す
(defun defpackage-shadow-form (name sym-name)
  (list 'shadow sym-name (list 'find-package name)))

; milestone92: :import-from/:shadowing-import-from句は(:import-from "src-pkg" "sym1" "sym2")
; という「先頭が対象パッケージ名、残りがシンボル名の列」という:export/:useとは異なる形の
; 句なので、defpackage-clause-namesとは別に(source-pkg . sym-name)ペアの列へ展開する
; ヘルパーが必要。1つの句に複数シンボル名を書けるので、句ごとにペアを複数生成してから
; 句をまたいでappendする(defpackage-clause-namesと同じ再帰スキャンパターンの派生)
(defun defpackage-pairs-for-clause (clause)
  (let ((source-pkg (car (cdr clause))))
    (mapcar (lambda (sym-name) (cons source-pkg sym-name))
            (cdr (cdr clause)))))

(defun defpackage-clause-package-symbol-pairs (clauses keyword)
  (if (null clauses)
      nil
      (if (eq (car (car clauses)) keyword)
          (append (defpackage-pairs-for-clause (car clauses))
                  (defpackage-clause-package-symbol-pairs (cdr clauses) keyword))
          (defpackage-clause-package-symbol-pairs (cdr clauses) keyword))))

(defun defpackage-import-from-form (name pair)
  (list 'import (list 'list (list 'intern (cdr pair) (list 'find-package (car pair))))
        (list 'find-package name)))

(defun defpackage-shadowing-import-from-form (name pair)
  (list 'shadowing-import (list 'list (list 'intern (cdr pair) (list 'find-package (car pair))))
        (list 'find-package name)))

; milestone89: &restラムダリストキーワードにより、以前のように仮引数リスト全体を単一symbol
; にしてcar/cdrで手動destructuringしていた書き方(milestone29のrest-arg機構)を、パッケージ名
; nameと可変長の句((:export ...)/(:use ...))を受け取るclausesの明示的な仮引数に置き換えた。
; milestone91で:nicknames句を追加した。nicknamesは文字列のリストなので、export/use-formの
; ような文字列単体(自己評価)と異なりquoteしないとmake-packageへの引数がフォームとして
; 誤評価されてしまうため、(list 'quote nicknames)で包む。
; milestone92で:shadow/:import-from/:shadowing-import-from句を追加した。生成順序は
; make-package(nicknames込み)→:shadow→:shadowing-import-from→:use→:import-from→:export→
; (find-package name)。:shadowを:useより先に処理することで、use-package実行時点で
; shadowingが既に有効になっている(use-packageの名前衝突チェックがshadowingを見て
; 例外扱いできる)
; milestone96: 最小CLOSサブセット。superclassesは0または1個(2個以上は(car superclasses)に
; より無条件に無視、多重継承はスコープ外)。slotsはbare symbolのリストのみ
; (:initarg/:initform/:accessorは無し、全スロットnilで初期化される)。
; この処理系のcar/cdrはnilを渡すとpanicする(CommonLispの(car nil)=>nilとは異なる)ため、
; (car superclasses)を直接テストせずsuperclasses自体の真偽をまず見てからcarを取る。
(defmacro defclass (name superclasses slots)
  (list '%make-class
        (list 'quote name)
        (if superclasses
            (list 'find-class (list 'quote (car superclasses)))
            nil)
        (list 'quote slots)))

; milestone97: defmethodのパラメータ1個をspecializer無し(bare symbol)/有り((name class-name))
; どちらの形かで分岐する2つのヘルパー。atomはcar/cdrを一切呼ばないため、bare symbol(non-cons)
; を渡してもmilestone96で踏んだcar-on-nil panicの心配は無い
(defun defmethod-param-name (spec)
  (if (atom spec) spec (car spec)))

(defun defmethod-param-specializer-form (spec)
  (if (atom spec) nil (list 'find-class (list 'quote (car (cdr spec))))))

; (defmethod name ((p1 c1) (p2 c2) p3) body...)。各パラメータはbare symbol(無指定)または
; (name class-name)(指定)。&optional/&rest/&keyは非対応(positional-onlyでmulti-arg
; specializer matchingを単純に保つ)。bodyは複数formを取れるので(cons 'progn body)で単一式へ
; 畳む(defun/lambda本体は単一formのみという既存の制約、milestone96のclos-move-pointと同型)。
; mapcarへ渡す関数名はLisp-2化(milestone93〜95)後の規約に従い#'で関数セルから取る
(defmacro defmethod (name params &rest body)
  (list 'progn
        (list '%ensure-generic-function (list 'quote name))
        (list '%add-method
              (list 'quote name)
              (cons 'list (mapcar #'defmethod-param-specializer-form params))
              (list 'lambda (mapcar #'defmethod-param-name params) (cons 'progn body)))))

(defmacro defpackage (name &rest clauses)
  (let* ((nicknames (defpackage-clause-names clauses :nicknames))
         (shadow-forms (mapcar (lambda (n) (defpackage-shadow-form name n))
                                (defpackage-clause-names clauses :shadow)))
         (shadowing-import-forms (mapcar (lambda (p) (defpackage-shadowing-import-from-form name p))
                                          (defpackage-clause-package-symbol-pairs clauses :shadowing-import-from)))
         (use-forms (mapcar (lambda (n) (defpackage-use-form name n))
                             (defpackage-clause-names clauses :use)))
         (import-forms (mapcar (lambda (p) (defpackage-import-from-form name p))
                                (defpackage-clause-package-symbol-pairs clauses :import-from)))
         (export-forms (mapcar (lambda (n) (defpackage-export-form name n))
                                (defpackage-clause-names clauses :export))))
    (cons 'progn
          (append (list (list 'make-package name (list 'quote nicknames)))
                  (append shadow-forms
                          (append shadowing-import-forms
                                  (append use-forms
                                          (append import-forms
                                                  (append export-forms
                                                          (list (list 'find-package name)))))))))))
