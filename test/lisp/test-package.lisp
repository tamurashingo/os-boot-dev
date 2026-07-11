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

(defun run-test-package ()
  (and (run-test-package-keyword-identity)
       (run-test-package-namespace-separation)))
