; milestone83/84 (ローカル変数領域とオペランドスタックの分離) の動作確認用テスト。
; letが非tail位置の兄弟式として使われても、まだスタックに残っている他の兄弟式の
; 一時値に影響されずfp相対の正しいスロットへ書き込まれることを確認する。
; QEMU起動後、(load "test\test-locals-region.lisp") でロードしてから
; (run-test-locals-region) を呼び出し、t が返れば全項目成功。

(defun run-test-locals-region-let-as-non-tail-sibling ()
  ; consの第1引数(まだpop されていない一時値)が残っている状態で、第2引数として
  ; 評価されるletがボックス化スロットへ正しく書き込めることを確認する
  ; (consは毎回新しいセルを作るのでeqでは比較できず、car/cdrをそれぞれ比較する)
  (let ((result (cons 1 (let ((y 2)) y))))
    (and (eq (car result) 1)
         (eq (cdr result) 2))))

(defun run-test-locals-region-multiple-lets-as-siblings ()
  ; 3つ以上の兄弟位置(関数呼び出しの各引数)でそれぞれletを使っても
  ; 互いのスロットを破壊しないことを確認する
  (eq (+ (let ((a 1)) a) (+ (let ((b 2)) b) (let ((c 3)) c)))
      6))

(defun run-test-locals-region-let-sibling-inside-let-body ()
  ; 外側のletで束縛した変数がまだスタック上に残っている状況で、その本体の
  ; 非tail位置(list の先頭以外の引数)に別のletを置いても外側の値が破壊されない
  ; (consは毎回新しいセルを作るのでeqでは比較できず、car/cdrをそれぞれ比較する)
  (let ((result (let ((outer 10))
                  (cons outer (let ((inner 20)) inner)))))
    (and (eq (car result) 10)
         (eq (cdr result) 20))))

; ループ本体で繰り返し評価されるletがスタックへボックスを積み増さないことの確認
; (milestone87のnested-while回帰と同じ原因の別角度確認)は、doおよびそれを使う
; whileマクロが導入されるmilestone87側のtest-while.lisp(run-test-while-nested)で
; 確認する。milestone83/84の時点ではdo/whileが未実装のため本ファイルには含めない

(defun run-test-locals-region ()
  (and (run-test-locals-region-let-as-non-tail-sibling)
       (run-test-locals-region-multiple-lets-as-siblings)
       (run-test-locals-region-let-sibling-inside-let-body)))
