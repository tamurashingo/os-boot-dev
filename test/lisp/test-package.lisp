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

; milestone91以降で使う汎用のeqベースmember。stdlib.lispにmember/memq相当が無いため
; テストファイル内に局所ヘルパーとして定義する
(defun member-eq-p (x lst)
  (if (null lst)
      nil
      (if (eq (car lst) x)
          t
          (member-eq-p x (cdr lst)))))

; milestone91: make-package/find-package/use-package/internがpackage designatorとして
; 文字列だけでなくkeyword/symbolも受け付けることを確認する。文字列版と同じオブジェクトに
; 解決される(eq)ことがポイント
(defun run-test-package-designator ()
  (and (eq (make-package "test-pkg91a") (make-package :test-pkg91a))
       (eq (find-package "test-pkg91a") (find-package :test-pkg91a))
       (eq (find-package "test-pkg91a") (find-package (quote test-pkg91a)))
       (eq (use-package :test-pkg91a (find-package "common-lisp-user")) t)
       (eq (intern "designator-sym-91" :test-pkg91a)
           (intern "designator-sym-91" (find-package "test-pkg91a")))))

; milestone91: exportの第2引数(package)が従来designator解決を全く通していなかった既存の
; 抜けの回帰確認。文字列/keywordのいずれのdesignatorでもpanicせず、対象パッケージの
; pkg_exportsへ正しく追加されることを確認する
(defun run-test-package-export-designator ()
  (make-package "test-pkg91-exp")
  (let ((sym-a (intern "exp-sym-91a" "test-pkg91-exp"))
        (sym-b (intern "exp-sym-91b" "test-pkg91-exp")))
    (and (eq (export sym-a "test-pkg91-exp") t)
         (eq (export sym-b :test-pkg91-exp) t)
         (member-eq-p sym-a (%package-exported-symbols "test-pkg91-exp"))
         (member-eq-p sym-b (%package-exported-symbols "test-pkg91-exp")))))

; milestone91: use-packageのused-arg(単一/リストの判定)がkeyword designator単体でも
; 単一パッケージ扱いになることを確認する(以前はlisp_is_symbolが判定条件に無かった)
(defun run-test-package-use-designator ()
  (make-package "test-pkg91-usea")
  (eq (use-package :test-pkg91-usea (make-package "test-pkg91-useb")) t))

; milestone91: package-name/package-nicknames/make-packageの第2引数(nicknames)。
; 文字列比較のビルトインが無いため、package-nameが返した文字列をfind-packageへそのまま
; 渡して元のパッケージオブジェクトに戻ってくること(round-trip)で間接的に内容を検証する
(defun run-test-package-name-and-nicknames ()
  (let ((pkg (make-package "test-pkg91-nick" (list "tpn91a" :tpn91b))))
    (and (eq (find-package (package-name pkg)) pkg)
         (eq (find-package "tpn91a") pkg)
         (eq (find-package :tpn91b) pkg)
         (member-eq-p (car (package-nicknames pkg)) (package-nicknames pkg)))))

; milestone91: package-use-list/list-all-packagesがそれぞれpkg_uses/global_packagesを
; そのまま反映することを確認する
(defun run-test-package-use-list-and-all-packages ()
  (let* ((pkg-used (make-package "test-pkg91-pula"))
         (pkg (make-package "test-pkg91-pulb")))
    (use-package pkg-used pkg)
    (and (member-eq-p pkg-used (package-use-list pkg))
         (member-eq-p pkg (list-all-packages))
         (member-eq-p (find-package "common-lisp-user") (list-all-packages)))))

; milestone91: find-symbolはローカル(internal/external)→use先のexport(inherited)の順で
; 探索し、(cons symbol status)またはnilを返す(複数値が無いためCLからの明示的な逸脱)
(defun run-test-package-find-symbol ()
  (let ((internal-sym (intern "find-symbol-internal-91"))
        (external-sym (intern "find-symbol-external-91")))
    (export external-sym)
    (and (eq (car (find-symbol "find-symbol-internal-91")) internal-sym)
         (eq (cdr (find-symbol "find-symbol-internal-91")) :internal)
         (eq (car (find-symbol "find-symbol-external-91")) external-sym)
         (eq (cdr (find-symbol "find-symbol-external-91")) :external)
         (eq (find-symbol "no-such-symbol-91") nil))))

(defun run-test-package-find-symbol-inherited ()
  (let* ((pkg-a (make-package "test-pkg91-fsa"))
         (pkg-b (make-package "test-pkg91-fsb"))
         (sym (intern "inherited-sym-91" pkg-a)))
    (export sym pkg-a)
    (use-package pkg-a pkg-b)
    (eq (cdr (find-symbol "inherited-sym-91" pkg-b)) :inherited)))

; milestone91: find-all-symbolsは全パッケージを走査し、同名のローカルシンボルを
; (パッケージが違えば別オブジェクトのまま)すべて返す
(defun run-test-package-find-all-symbols ()
  (let* ((pkg-a (make-package "test-pkg91-fasa"))
         (pkg-b (make-package "test-pkg91-fasb"))
         (sym-a (intern "shared-name-91" pkg-a))
         (sym-b (intern "shared-name-91" pkg-b))
         (results (find-all-symbols "shared-name-91")))
    (and (member-eq-p sym-a results)
         (member-eq-p sym-b results)
         (eq (eq sym-a sym-b) nil))))

; milestone91: dolistマクロ(doベース)の基本動作。リストの全要素を訪問して合計できること
(defun run-test-package-dolist ()
  (let ((total 0))
    (dolist (x (list 1 2 3))
      (setq total (+ total x)))
    (eq total 6)))

; milestone91: do-symbolsはローカルシンボル(export有無問わず)とuseしている先のexport
; シンボルの両方を訪問することを確認する
(defun run-test-package-do-symbols ()
  (let* ((pkg-u (make-package "test-pkg91-dsu"))
         (used-sym (intern "used-exported-91" pkg-u)))
    (export used-sym pkg-u)
    (let* ((pkg (make-package "test-pkg91-ds")))
      (use-package pkg-u pkg)
      (let ((local-sym (intern "local-sym-91" pkg))
            (found-local nil)
            (found-used nil))
        (do-symbols (s pkg)
          (if (eq s local-sym) (setq found-local t) nil)
          (if (eq s used-sym) (setq found-used t) nil))
        (and found-local found-used)))))

; milestone91: do-external-symbolsはexportされたシンボルのみを訪問し、非exportの
; ローカルシンボルは訪問しないことを確認する
(defun run-test-package-do-external-symbols ()
  (let* ((pkg (make-package "test-pkg91-des"))
         (ext-sym (intern "ext-sym-91" pkg))
         (int-sym (intern "int-sym-91" pkg))
         (found-ext nil)
         (found-int nil))
    (export ext-sym pkg)
    (do-external-symbols (s pkg)
      (if (eq s ext-sym) (setq found-ext t) nil)
      (if (eq s int-sym) (setq found-int t) nil))
    (and found-ext (eq found-int nil))))

; milestone91: do-all-symbolsは登録済み全パッケージを走査することを確認する
; (packageを取らない(var [result])形式)
(defun run-test-package-do-all-symbols ()
  (let ((sym (intern "all-symbols-marker-91"))
        (found nil))
    (do-all-symbols (s)
      (if (eq s sym) (setq found t) nil))
    found))

; milestone91: defpackageの:nicknames句
(defun run-test-package-defpackage-nicknames ()
  (let ((pkg (defpackage "test-pkg91-dpn" (:nicknames "tpdn91a" :tpdn91b))))
    (and (eq (find-package "tpdn91a") pkg)
         (eq (find-package :tpdn91b) pkg))))

; milestone92: shadowはローカルシンボルをpkg_shadowing_symbolsへ登録し、以後use-packageが
; 同名だが別オブジェクトのexportシンボルと衝突しても(通常ならpanicする状況で)panicせず、
; shadowで確保したローカルシンボルへの無修飾名解決が変わらないことを確認する
; (shadow未設定時に引き続きpanicすることの回帰確認はQEMU個別対話で行う、#30)
(defun run-test-package-shadow ()
  (let* ((pkg-src (make-package "test-pkg92-shsrc"))
         (src-sym (intern "shadow-name-92" pkg-src))
         (pkg (make-package "test-pkg92-sh")))
    (export src-sym pkg-src)
    (shadow "shadow-name-92" pkg)
    (let ((local-sym (intern "shadow-name-92" pkg)))
      (and (eq (use-package pkg-src pkg) t)
           (eq (intern "shadow-name-92" pkg) local-sym)
           (eq (eq local-sym src-sym) nil)))))

; milestone92: unexportはpkg_exportsからeqで除去する
(defun run-test-package-unexport ()
  (let* ((pkg (make-package "test-pkg92-unexp"))
         (sym (intern "unexport-sym-92" pkg)))
    (export sym pkg)
    (and (member-eq-p sym (%package-exported-symbols pkg))
         (eq (unexport sym pkg) t)
         (eq (member-eq-p sym (%package-exported-symbols pkg)) nil))))

; milestone92: unuse-packageはpkg_usesからeqで除去する
(defun run-test-package-unuse-package ()
  (let* ((pkg-used (make-package "test-pkg92-unusesrc"))
         (pkg (make-package "test-pkg92-unuse")))
    (use-package pkg-used pkg)
    (and (member-eq-p pkg-used (package-use-list pkg))
         (eq (unuse-package pkg-used pkg) t)
         (eq (member-eq-p pkg-used (package-use-list pkg)) nil))))

; milestone92: importは対象のpkg_symbolsへhome packageを変えずに追加し、以後同名の
; internがimportされたシンボル自身(eq)を返すようになることを確認する
(defun run-test-package-import ()
  (let* ((pkg-src (make-package "test-pkg92-impsrc"))
         (sym (intern "import-sym-92" pkg-src))
         (pkg (make-package "test-pkg92-imp")))
    (and (eq (import sym pkg) t)
         (member-eq-p sym (%package-symbols pkg))
         (eq (intern "import-sym-92" pkg) sym))))

; milestone92: shadowing-importは既存の同名ローカルシンボルを(eq問わず)置き換え、
; pkg_shadowing_symbolsにも登録する。登録の効果は、置き換え後に別パッケージの同名
; exportをuse-packageしても(通常ならpanicする状況で)panicしないことで確認する
(defun run-test-package-shadowing-import ()
  (let* ((pkg-src (make-package "test-pkg92-simpsrc"))
         (src-sym (intern "shimp-sym-92" pkg-src))
         (pkg-other (make-package "test-pkg92-simpother"))
         (other-sym (intern "shimp-sym-92" pkg-other))
         (pkg (make-package "test-pkg92-simp")))
    (export src-sym pkg-src)
    (export other-sym pkg-other)
    (shadowing-import src-sym pkg)
    (and (eq (intern "shimp-sym-92" pkg) src-sym)
         (eq (use-package pkg-other pkg) t))))

; milestone92: delete-packageはglobal_packagesおよび他パッケージのpkg_usesからeqで
; 除去する(*package*自身の削除がpanicすることの確認はQEMU個別対話で行う、#30)
(defun run-test-package-delete-package ()
  (let* ((pkg-del (make-package "test-pkg92-del"))
         (pkg-other (make-package "test-pkg92-delother")))
    (use-package pkg-del pkg-other)
    (and (member-eq-p pkg-del (list-all-packages))
         (eq (delete-package pkg-del) t)
         (eq (member-eq-p pkg-del (list-all-packages)) nil)
         (eq (member-eq-p pkg-del (package-use-list pkg-other)) nil))))

; milestone92: rename-packageはpkg_nameを書き換え、新しい名前で見つかり旧名では
; 見つからなくなる。new-nicknamesを渡した場合はpkg_nicknamesも置き換わる
; (既存の別パッケージへの改名がpanicすることの確認はQEMU個別対話で行う、#30)
(defun run-test-package-rename-package ()
  (let* ((pkg (make-package "test-pkg92-ren"))
         (renamed (rename-package pkg "test-pkg92-renamed" (list "tpr92nick"))))
    (and (eq renamed pkg)
         (eq (find-package "test-pkg92-renamed") pkg)
         (eq (find-package "test-pkg92-ren") nil)
         (eq (find-package "tpr92nick") pkg))))

; milestone92: defpackageの:shadow句が:useより先に処理されるため、use-package実行時点で
; shadowingが有効になっており、同名exportとの衝突でpanicしないことを確認する
(defun run-test-package-defpackage-shadow ()
  (let* ((pkg-src (make-package "test-pkg92-dpshsrc"))
         (src-sym (intern "dpsh-sym-92" pkg-src)))
    (export src-sym pkg-src)
    (let ((pkg (defpackage "test-pkg92-dpsh" (:shadow "dpsh-sym-92") (:use "test-pkg92-dpshsrc"))))
      (eq (eq (intern "dpsh-sym-92" pkg) src-sym) nil))))

; milestone92: defpackageの:import-from句は指定パッケージのシンボルをそのままimportする
(defun run-test-package-defpackage-import-from ()
  (let* ((pkg-src (make-package "test-pkg92-dpifsrc"))
         (src-sym (intern "dpif-sym-92" pkg-src)))
    (let ((pkg (defpackage "test-pkg92-dpif" (:import-from "test-pkg92-dpifsrc" "dpif-sym-92"))))
      (eq (intern "dpif-sym-92" pkg) src-sym))))

; milestone92: defpackageの:shadowing-import-from句は:useより先に処理されるため、
; use-package実行時点でshadowingが有効になっており、同名exportとの衝突でpanicしないことを
; 確認する
(defun run-test-package-defpackage-shadowing-import-from ()
  (let* ((pkg-src (make-package "test-pkg92-dpsifsrc"))
         (src-sym (intern "dpsif-sym-92" pkg-src))
         (pkg-other (make-package "test-pkg92-dpsifother"))
         (other-sym (intern "dpsif-sym-92" pkg-other)))
    (export src-sym pkg-src)
    (export other-sym pkg-other)
    (let ((pkg (defpackage "test-pkg92-dpsif"
                 (:shadowing-import-from "test-pkg92-dpsifsrc" "dpsif-sym-92")
                 (:use "test-pkg92-dpsifother"))))
      (eq (intern "dpsif-sym-92" pkg) src-sym))))

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
       (run-test-package-in-package-roundtrip)
       (run-test-package-designator)
       (run-test-package-export-designator)
       (run-test-package-use-designator)
       (run-test-package-name-and-nicknames)
       (run-test-package-use-list-and-all-packages)
       (run-test-package-find-symbol)
       (run-test-package-find-symbol-inherited)
       (run-test-package-find-all-symbols)
       (run-test-package-dolist)
       (run-test-package-do-symbols)
       (run-test-package-do-external-symbols)
       (run-test-package-do-all-symbols)
       (run-test-package-defpackage-nicknames)
       (run-test-package-shadow)
       (run-test-package-unexport)
       (run-test-package-unuse-package)
       (run-test-package-import)
       (run-test-package-shadowing-import)
       (run-test-package-delete-package)
       (run-test-package-rename-package)
       (run-test-package-defpackage-shadow)
       (run-test-package-defpackage-import-from)
       (run-test-package-defpackage-shadowing-import-from)))
