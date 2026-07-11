; milestone 22 (数値タワー: bignum/float) の動作確認用テスト。
; QEMU起動後、(load "test\test-numeric-tower.lisp") でロードしてから
; (run-test-numeric-tower) を呼び出し、t が返れば全項目成功。
;
; 数値比較(=/<)がまだ無く、bignum/floatはヒープに確保されたオブジェクトなので
; eqで値の一致を確認できない。よってこのファイルではeqで自動確認できる範囲
; (通常サイズのfixnum演算の回帰、およびbignum→fixnumの正規化の往復)のみを
; 検証する。bignum/floatの実際の値そのものはQEMUのREPLに式を打ち込んで
; 印字結果を目視確認する(documents/bare_metal_lisp.mdのchangelog参照)。

(defun run-test-numeric-tower-fixnum-regression ()
  (and (eq (+ 1 2) 3)
       (eq (- 5 3) 2)
       (eq (- 5) -5)
       (eq (+) 0)))

(defun run-test-numeric-tower-bignum-demote ()
  ; 2^61-1(fixnumの最大値)を2回足すとbignumへ昇格する。そこから十分小さい値を
  ; 引いてfixnum範囲に戻すと、正規化によって再びeqで比較できるfixnumになる
  (eq (- (+ 2305843009213693951 2305843009213693951) 4611686018427387899) 3))

(defun run-test-numeric-tower ()
  (and (run-test-numeric-tower-fixnum-regression)
       (run-test-numeric-tower-bignum-demote)))
