; milestone 102: osパッケージの作成。process/*all-processes*/get-all-processesをexportする。
;
; defpackageの評価効果(パッケージ生成・export)を、続くos.lispの読み込み時に使う
; os:process等の修飾子(単一コロン、pkg_exportsを検索する)の解決より前に確定させる必要が
; あるため、compiler.lisp/stdlib.lispと同様に「別ファイル=main.cから別々にlisp_load_boot_file
; を呼ぶ」という形で分離している(loadは1回の呼び出し内でファイル全体を読み切ってから評価する
; ため、同一ファイル内ではdefpackageの効果を後続フォームの読み取りに反映できない、
; milestone72/76/78/79/81/100/101と同根の制約)
(defpackage "os"
  (:use "common-lisp-user")
  (:export "process" "*all-processes*" "get-all-processes" "make-process"
           "process-resume" "process-suspend" "process-local-variable"))
