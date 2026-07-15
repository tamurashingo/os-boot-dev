; milestone 87 (do特殊形式、Cスタックを消費しないループ機構) の動作確認用テスト。
; QEMU起動後、(load "test\test-do.lisp") でロードしてから
; (run-test-do) を呼び出し、t が返れば全項目成功。

(defun run-test-do-basic-sum ()
  ; 0から4まで加算しながらiを1ずつ進め、5になったら終了してsumを返す
  (eq (do ((i 0 (+ i 1)) (sum 0 (+ sum i)))
          ((eq i 5) sum))
      10))

(defun run-test-do-result-forms ()
  ; end-test後のresult-formsは複数個評価でき、最後の値が返る
  (eq (do ((i 0 (+ i 1)))
          ((eq i 3) 100 200))
      200))

(defun run-test-do-no-result-forms ()
  ; result-formsが無ければnilを返す
  (eq (do ((i 0 (+ i 1)))
          ((eq i 3)))
      nil))

(defun run-test-do-body-side-effect ()
  ; 本体は毎イテレーション評価される(外側の変数をsetqで書き換える副作用の確認)
  (eq (let ((count 0))
        (do ((i 0 (+ i 1)))
            ((eq i 5) count)
          (setq count (+ count 1))))
      5))

(defun run-test-do-var-no-step ()
  ; step-formを省略した変数は自動更新されない(初期値のまま)
  (eq (do ((i 0 (+ i 1)) (fixed 42))
          ((eq i 3) fixed))
      42))

(defun run-test-do-var-shorthand ()
  ; (var)単体はinit/stepともに省略、nilに束縛される
  (eq (do ((i 0 (+ i 1)) (x))
          ((eq i 2) x))
      nil))

(defun run-test-do-parallel-step ()
  ; 並行ステップ: 各step-formは更新前の値を参照する(letの並行束縛と同じ意味)。
  ; 終了判定はiだけに依存させ、a/bの計算が誤って逐次評価(let*相当)になっていても
  ; 無限ループせず、期待値と異なる結果を返すことで検出できるようにしてある
  (eq (do ((a 0 b) (b 1 (+ a b)) (i 0 (+ i 1)))
          ((eq i 5) a))
      5))

(defvar *do-dyn* 1)

(defun run-test-do-dynamic-var ()
  ; 動的変数もdoのbindingで束縛・並行ステップ・復元ができる(letと同型)
  (and (eq (do ((*do-dyn* 100 (+ *do-dyn* 1)) (i 0 (+ i 1)))
               ((eq i 3) *do-dyn*))
           103)
       (eq *do-dyn* 1))) ; doを抜けたら元の値に復元される

(defun run-test-do-return-from-escapes ()
  ; return-fromでdoの外側のblockまで直接脱出できる(end-testには到達しない)
  (eq (block outer
        (do ((i 0 (+ i 1)))
            ((eq i 100) 999)
          (if (eq i 3) (return-from outer i) nil)))
      3))

(defun run-test-do-many-iterations ()
  ; 大きなイテレーション数でもCコールスタックを消費しない(milestone87の目的そのもの)
  (eq (do ((i 0 (+ i 1)) (sum 0 (+ sum 1)))
          ((eq i 50000) sum))
      50000))

(defun run-test-do ()
  (and (run-test-do-basic-sum)
       (run-test-do-result-forms)
       (run-test-do-no-result-forms)
       (run-test-do-body-side-effect)
       (run-test-do-var-no-step)
       (run-test-do-var-shorthand)
       (run-test-do-parallel-step)
       (run-test-do-dynamic-var)
       (run-test-do-return-from-escapes)
       (run-test-do-many-iterations)))
