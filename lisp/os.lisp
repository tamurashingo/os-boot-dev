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
;   package    - fork時に生成する隔離パッケージ(milestone108以降で使用、この段階ではnil)
;   stackframe - per-processコンテキスト保存領域(milestone104以降で使用、この段階ではnil)
;   env        - make-process時点のレキシカル環境(milestone113のprocess-local-variableで使用)
;   status     - プロセスの実行状態を表すキーワード(この段階では値を設定しない、milestone112以降で
;                :active/:suspendedを使う)
(defclass os:process () (name package stackframe env status))

(defvar os:*all-processes* nil)

(defun os:get-all-processes () os:*all-processes*)
