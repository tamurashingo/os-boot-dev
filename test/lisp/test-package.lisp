; milestone 23 (最小限のpackageシステム: common-lisp-user/keyword) の動作確認用テスト。
; QEMU起動後、(load "test\test-package.lisp") でロードしてから
; (run-test-package) を呼び出し、t が返れば全項目成功。
;
; keywordの自己評価そのもの(quote無しで評価してもエラーにならないこと)や、印字時に
; ":"が前置されることは、evalの結果を直接eqで比較できてもtrue/falseの二値には落とせないため、
; このファイルではeqで自動確認できる範囲(同名keywordのintern同一性、cl-userとkeywordの
; namespace分離)のみを検証する。自己評価・印字結果はQEMUのREPLに直接式を打ち込んで
; 目視確認する(documents/bare_metal_lisp.mdのchangelog参照)。

(defun run-test-package-keyword-identity ()
  (and (eq :foo :foo)
       (eq :foo (quote :foo))))

(defun run-test-package-namespace-separation ()
  ; cl-userパッケージのシンボルfooとkeywordパッケージのシンボルfooは別オブジェクト
  ; (notが無いため、eqの結果がnilであることをさらにeqでnilと比較して確認する)
  (and (eq (eq (quote foo) :foo) nil)
       (eq (quote foo) (quote foo))))

; milestone 73: *package*が通常のdefvarと同じ動的変数として確立されており、既定値が
; nilでない(=common-lisp-userパッケージオブジェクト)ことを確認する。*package*自身の
; パッケージオブジェクトはこの時点ではLispからfind-package等で取り出せないため
; (milestone75)、eqで直接比較はできず、special-variable-pとnilでないことのみ検証する。
(defun run-test-package-star-package-var ()
  (and (special-variable-p (quote *package*))
       (eq (eq *package* nil) nil)))

; milestone 75: make-packageは同じ名前で2度呼んでも同一オブジェクト(eq)を返し(冪等性)、
; find-packageはその同一オブジェクトを見つけられ、未存在の名前にはnilを返すことを確認する
(defun run-test-package-make-find ()
  (and (eq (make-package "test-pkg75") (make-package "test-pkg75"))
       (eq (find-package "test-pkg75") (make-package "test-pkg75"))
       (eq (find-package "no-such-package-75") nil)
       (eq (find-package "common-lisp-user") (make-package "common-lisp-user"))))

; milestone 76: exportはシンボルをexportリストへ追加しtを返す。1個のシンボル・複数シンボルの
; リスト・packageを省略した場合(*package*、現時点ではcommon-lisp-user)のいずれでもエラーなく
; tが返ることを確認する。exportされたシンボルが実際にpkg:sym(単一コロン)で解決できることの検証は、
; "load"がファイル全体を読み切ってから評価する実装(milestone72の既知の制約と同根)であるため、
; 同一ファイル内でexport実行→その効果を読み取り時に要求するpkg:sym修飾子を使うテストが書けない
; (readerがexport実行前の状態でファイル全体を読もうとして失敗する)。この組み合わせの検証は
; C内部API直呼びの自己テスト(lisp_reader_export_selftest、lisp.c)と個別の対話REPLセッションで
; 別途行う。
(defun run-test-package-export ()
  (and (eq (export (quote export-test-sym-76a)) t)
       (eq (export (list (quote export-test-sym-76b) (quote export-test-sym-76c))) t)
       (eq (export (quote export-test-sym-76d) (find-package "common-lisp-user")) t)
       (eq (export (quote export-test-sym-76a)) t)))

; milestone 77: use-packageは対象パッケージのpkg_usesへ追加しtを返す。単一パッケージ・
; パッケージのリスト・パッケージ名の文字列・packageを省略した場合(*package*、現時点では
; common-lisp-user)・同じパッケージを再度useした場合(冪等)のいずれでもエラーなくtが返ることを
; 確認する。use-packageの本質的な効果(無修飾名がuse-list経由で別パッケージのexportシンボルに
; 解決されること)の検証は、対象を切り替えるには本来*package*を切り替える(in-package、
; milestone78で未実装)必要があり、かつ"load"がファイル全体を読み切ってから評価する実装
; (milestone72/76と同根の制約)であるため、同一ファイル内で「use-packageを実行→その効果に
; 依存する無修飾名を読む」という順序のテストが書けない。この組み合わせはC内部API直呼びの
; 自己テスト(lisp_reader_use_package_selftest、lisp.c)と個別の対話REPLセッションで別途行う。
(defun run-test-package-use ()
  (and (eq (use-package (make-package "test-pkg77a")) t)
       (eq (use-package (list (make-package "test-pkg77b") (make-package "test-pkg77c"))) t)
       (eq (use-package "test-pkg77a") t)
       (eq (use-package (make-package "test-pkg77d") (find-package "common-lisp-user")) t)
       (eq (use-package (make-package "test-pkg77a")) t)))

; milestone 78: internは指定パッケージ(省略時は*package*)へシンボルを帰属させ、同名の
;再internは同一オブジェクト(eq)を返す。defpackageは:export/:use句を文字列で受け取り、
; make-package/export/use-packageへ展開してエラーなく対象パッケージオブジェクトを返し、
; 同名で再度defpackageしても同一オブジェクト(#69の重複名冪等性)を返す。いずれも本ファイル内
; での呼び出しだけで検証できる範囲(*package*の切替や、切替後に無修飾名を読むことには
; 依存しない)に限る。in-packageで*package*を切り替えた後に無修飾名がexportされたシンボルと
; 同一オブジェクトに解決されること自体は、milestone76/77と同根の理由(load はファイル全体を
; 読み切ってから評価する)でこのファイルでは検証できず、C内部API直呼びの自己テスト
; (lisp_reader_defpackage_selftest、lisp.c)と個別の対話REPLセッションで別途行う。
(defun run-test-package-intern ()
  (and (eq (intern "intern-test-78a") (intern "intern-test-78a"))
       (eq (intern "intern-test-78b" (find-package "common-lisp-user"))
           (intern "intern-test-78b"))))

(defun run-test-package-defpackage ()
  (and (eq (defpackage "test-pkg78" (:export "pkg78-sym-a") (:use "common-lisp-user"))
           (find-package "test-pkg78"))
       (eq (defpackage "test-pkg78" (:export "pkg78-sym-a") (:use "common-lisp-user"))
           (defpackage "test-pkg78" (:export "pkg78-sym-a") (:use "common-lisp-user")))))

; milestone 82: 修飾子リーダーの統合テスト。main.cの自己テストlisp_reader_package_qualifier_selftest
; (milestone74)が起動時に既に作成済みの"selftest-pkg74"(exported-symをexport・internal-symは非export)
; を利用し、pkg:sym/pkg::sym修飾子で読んだシンボルが、intern関数で直接同じ名前をinternした結果と
; eq(同一オブジェクト)であることを確認する。このパッケージ・exportの用意自体を本ファイル内で行うと、
; "load"がファイル全体を読み切ってから評価する実装のため「export実行→その効果を本ファイル内で修飾子
; 経由で読む」という順序が組めない(milestone72/76と同根の制約)が、main.cの自己テストは本ファイルの
; loadより前に実行済みのため、この制約に触れずに検証できる
(defun run-test-package-qualifier-reader ()
  (and (eq (quote selftest-pkg74:exported-sym)
           (intern "exported-sym" (find-package "selftest-pkg74")))
       (eq (quote selftest-pkg74::internal-sym)
           (intern "internal-sym" (find-package "selftest-pkg74")))))

; milestone 82: in-packageによる*package*切替のround-trip統合テスト。ファイル全体は*package*が
; common-lisp-userのまま読み切られるため、本テスト自身の記述(in-package/intern/find-package/eq/let
; のいずれも無修飾)はreader側の可視性制約(milestone79/81で発見)に触れない。in-packageはランタイム
; 関数呼び出しであり、その効果(*package*の切替)は評価順に沿って実行時にのみ発生するため、
; 切替→切替中のintern→common-lisp-userへの復帰、という手続きをそのまま1つのdefun本体として書ける。
; 復帰後の*package*が確実にcommon-lisp-userへ戻ることも確認する(以降のテスト・REPL評価に影響しない
; ことの保証)
(defun run-test-package-in-package-roundtrip ()
  (in-package "selftest-pkg74")
  (let ((sym-during (intern "roundtrip-sym-82")))
    (in-package "common-lisp-user")
    (and (eq sym-during (intern "roundtrip-sym-82" (find-package "selftest-pkg74")))
         (eq *package* (find-package "common-lisp-user")))))

(defun run-test-package ()
  (and (run-test-package-keyword-identity)
       (run-test-package-namespace-separation)
       (run-test-package-star-package-var)
       (run-test-package-make-find)
       (run-test-package-export)
       (run-test-package-use)
       (run-test-package-intern)
       (run-test-package-defpackage)
       (run-test-package-qualifier-reader)
       (run-test-package-in-package-roundtrip)))
