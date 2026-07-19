; milestone 102: processクラス・グローバルレジストリ本体。
;
; osパッケージ自体はos-package.lispで既に作成済み(process/*all-processes*/get-all-processesは
; export済み)なので、ここではos:修飾子でそれらのシンボルを直接参照して定義する
; (*package*自体はcommon-lisp-userのまま切り替えない)。
;
; スロット名(name/package/stackframe/env/status)は無修飾のままcommon-lisp-user所属にしている。
; slot-value/set-slot-valueによる照合は単純なシンボルeqであり、CommonLispのアクセサ関数のような
; パッケージ分離をスロット名自体に持たせる必要は無いという設計判断(全プロセス関連コードが
; 無修飾の'name/'status等で一貫してslot-valueを呼べる方が、常にos::修飾子を書かせるより単純)。
;
; この段階のprocessは単なるデータ構造であり、実行機構(fork/一時停止/再開)は一切持たない
; (documents/lisp_os_process.md フェーズB)。
;   name       - プロセス名(文字列。milestone103のmake-processが一意性を保証する)
;   package    - fork時に生成する隔離パッケージ(milestone108の%make-processが生成・格納する。
;                一意名で新規作成され、common-lisp-userをuse-packageしているため、fork側で
;                ベースの関数・変数へ無修飾のままアクセスできる)
;   stackframe - per-processコンテキスト保存領域(milestone104以降で使用、この段階ではnil)
;   env        - make-process時点のレキシカル環境(milestone113のprocess-local-variableで使用)
;   status     - プロセスの実行状態を表すキーワード(この段階では値を設定しない、milestone112以降で
;                :active/:suspendedを使う)
(defclass os:process () (name package stackframe env status))

(defvar os:*all-processes* nil)

(defun os:get-all-processes () os:*all-processes*)

; milestone 103: make-process(名前一意性のみ、実行機構無し)。
;
; 名前の自動生成("PROCESS-<N>"形式、gensymと同じカウンタ方式)・既存os:*all-processes*との
; 内容比較による一意性チェックのいずれも、str_data(文字列の生バイト列)への直接アクセスが
; 必要でLisp側からは行えない(本処理系にはstring=もlengthも無い、milestone102で確認済みの
; 制約)ため、実体は%make-process(Cビルトイン、src/lisp.c)にあり、ここでは&optional引数の
; 展開のみを担当する薄いラッパーにしている(%make-class/defclassと同じパターン)。
;
; 名前が衝突した場合(ユーザー指定名が既存プロセスと内容一致)は%make-process内でpanicする。
;
; milestone108: %make-processは名前決定に続けて、一意名を持つ隔離パッケージも
; lisp_make_package_strict(黙って既存を共有せずpanicする安全な作成経路)で新規作成し、
; common-lisp-userをuse-packageした上でprocessインスタンスのpackageスロットへ格納するように
; 拡張された(パッケージ名生成はプロセス名生成と別カウンタ・別接頭辞"FORK-PKG-<N>"を使うため、
; 互いに衝突する余地は無い)。この段階ではまだfork実行・スタック確保は行わない
(defun os:make-process (&optional name) (%make-process name))
