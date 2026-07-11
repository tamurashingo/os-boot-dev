; milestone 20 (gensym) の動作確認用テスト。
; QEMU起動後、(load "test\test-gensym.lisp") でロードしてから
; (run-test-gensym) を呼び出し、t が返れば全項目成功。

(defun run-test-gensym-unique ()
  ; 呼ぶたびに別のシンボルが返る（notがまだ無いのでifで反転する）
  (if (eq (gensym) (gensym)) nil t))

(defun run-test-gensym-many-unique ()
  (let ((a (gensym)) (b (gensym)) (c (gensym)))
    (and (if (eq a b) nil t)
         (if (eq a c) nil t)
         (if (eq b c) nil t))))

(defun run-test-gensym-is-symbol-like-atom ()
  ; gensymの結果はconsではない（シンボルとして扱える）
  (atom (gensym)))

(defun run-test-gensym-not-nil-not-t ()
  ; gensymは常にLISP_TAG_SYMBOLの新規オブジェクトを返すため、nilやtとは絶対にeqにならない
  (and (if (eq (gensym) nil) nil t)
       (if (eq (gensym) t) nil t)))

(defun run-test-gensym-with-prefix ()
  ; prefix付きでもプレフィックスが同じだけでeqにはならない（別オブジェクト）
  (if (eq (gensym "foo") (gensym "foo")) nil t))

(defun run-test-gensym ()
  (and (run-test-gensym-unique)
       (run-test-gensym-many-unique)
       (run-test-gensym-is-symbol-like-atom)
       (run-test-gensym-not-nil-not-t)
       (run-test-gensym-with-prefix)))
