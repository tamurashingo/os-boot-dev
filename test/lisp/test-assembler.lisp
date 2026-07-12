; milestone 41 (バイトコード中間表現とアセンブラ) の動作確認用テスト。
; QEMU起動後、(load "test\test-assembler.lisp") でロードしてから
; (run-test-assembler) を呼び出し、t が返れば全項目成功。
; asm-*/assembleはlisp/stdlib.lispでEfiMainの起動シーケンス中に
; 既に読み込まれている前提（このテストファイルでの再定義は不要）。

; 結果はfixnumのフラットなリストなので、struct-eqでconsの構造とeqで
; 比較できる（test-compile.lispと同じヘルパーをこのファイルでも再定義する）
(defun struct-eq (a b)
  (if (atom a)
      (eq a b)
      (and (not (atom b))
           (struct-eq (car a) (car b))
           (struct-eq (cdr a) (cdr b)))))

(defun run-test-assembler-no-operand ()
  ; オペランド無し命令は1byteだけを出力する
  (struct-eq (assemble (list (list *op-add*))) (list *op-add*)))

(defun run-test-assembler-literal-operand ()
  ; オペランドがfixnumならそのままバイト値として使う
  (struct-eq (assemble (list (list *op-const* 5) (list *op-return*)))
             (list *op-const* 5 *op-return*)))

(defun run-test-assembler-forward-jump ()
  ; まだ出現していないラベルへの前方参照も解決できる。
  ; (op-jump (ref end)) [2byte] -> (op-const 5) [2byte] -> (label end) -> (op-return) [1byte]
  ; endの絶対位置は4
  (struct-eq (assemble (list (list *op-jump* (list 'ref 'end))
                              (list *op-const* 5)
                              (list 'label 'end)
                              (list *op-return*)))
             (list *op-jump* 4 *op-const* 5 *op-return*)))

(defun run-test-assembler-backward-jump ()
  ; 既に出現したラベルへの後方参照も解決できる。startの絶対位置は0
  (struct-eq (assemble (list (list 'label 'start)
                              (list *op-const* 1)
                              (list *op-jump* (list 'ref 'start))))
             (list *op-const* 1 *op-jump* 0)))

(defun run-test-assembler-loop-shape ()
  ; loop先頭へ戻るジャンプ(後方参照)とloop外へ抜けるジャンプ(前方参照)を
  ; 両方含む、実際のコンパイラが生成しそうな形を通しで検証する
  (struct-eq
   (assemble (list (list 'label 'loop)
                    (list *op-const* 1)
                    (list *op-jump-if-false* (list 'ref 'end))
                    (list *op-jump* (list 'ref 'loop))
                    (list 'label 'end)
                    (list *op-return*)))
   (list *op-const* 1 *op-jump-if-false* 6 *op-jump* 0 *op-return*)))

(defun run-test-assembler-label-emits-no-bytes ()
  ; ラベル定義そのものはバイトを出力しない(連続するラベルは0byteのまま)
  (struct-eq (assemble (list (list 'label 'a) (list 'label 'b) (list *op-return*)))
             (list *op-return*)))

(defun run-test-assembler ()
  (and (run-test-assembler-no-operand)
       (run-test-assembler-literal-operand)
       (run-test-assembler-forward-jump)
       (run-test-assembler-backward-jump)
       (run-test-assembler-loop-shape)
       (run-test-assembler-label-emits-no-bytes)))
