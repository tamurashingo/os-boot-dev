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

(defun run-test-package ()
  (and (run-test-package-keyword-identity)
       (run-test-package-namespace-separation)
       (run-test-package-star-package-var)
       (run-test-package-make-find)))
