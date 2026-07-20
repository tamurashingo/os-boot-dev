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
;   stackframe - per-processコンテキスト保存領域。未起動ならnil、起動後はコンテキストプールの
;                indexを表すfixnum(milestone112の%process-resumeが設定する)
;   env        - make-process時点のレキシカル環境(milestone113のprocess-local-variableで使用)
;   status     - プロセスの実行状態を表すキーワード。未起動ならnil、実行中は:active、
;                process-suspend後は:suspended、thunkが戻って終了した後は:finished
;                (milestone112の%process-resume/%process-suspendが設定する)
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

; milestone 112: process-suspend/process-resume(実際の実行機構)。
;
; 実体はC組み込み関数%process-resume/%process-suspend(src/lisp.c)にあり、ここでは
; make-process/%make-processと同じ「薄いラッパー」パターンを踏襲する。
;
; os:process-resumeは、対象プロセスが未起動(stackframeスロットがnil)なら第2引数thunk
; (0引数のLisp関数、例: (lambda () ...))を新規コンテキストで開始し、起動済みなら直前の
; os:process-suspendの中断点から再開する。いずれの場合も、そのプロセスが次にsuspendするか
; 実行を終えるまで呼び出し元をブロックする。
;
; os:process-suspendは「今実際に実行中のプロセス自身」からのみ呼べる(自分自身をsuspendする、
; という設計。他プロセスの強制停止はできない)。中断中に他プロセスが(gc)を誘発すると、
; ツリーウォーク経路のC局所変数が未追跡である既知の制約が残る(documents/lisp_os_process.md
; マイルストーン112参照)
(defun os:process-resume (p &optional thunk) (%process-resume p thunk))
(defun os:process-suspend (p) (%process-suspend p))

; milestone 113: process-local-variable(プロセスのレキシカル環境を外部から覗く)。
;
; 実体はC組み込み関数%process-local-variable(src/lisp.c)にあり、ここでも同じ「薄いラッパー」
; パターンを踏襲する。
;
; %process-resumeが初回起動時にthunkクロージャ自身のenvフィールド(生成時点で捕捉した
; レキシカル環境)をenvスロットへコピーしておくため、process-local-variableはそのenvスロットを
; lisp_env_lookup相当の規則で検索する。したがって見えるのは「thunk定義時点で既にレキシカル
; スコープにあった変数」のみであり、thunk本体の実行中に新たに導入されたlet束縛(中断中でも
; ツリーウォーク経路のC呼び出しスタック上にのみ存在する)は見えない。また動的/special変数は
; 環境チェーンを経由せずシンボル自身のvalueを直接返す(全プロセス共有、プロセス毎の分離は
; 無い既知の制約、documents/lisp_os_process.md参照)。プロセスが未起動(stackframeスロットがnil)
; ならpanicする。
;
; 重要な制約: envフィールドはツリーウォーク(lisp_eval)経由で作られたクロージャのみが持つ。
; 通常のdefun/lambda(ラムダリストキーワード無し)はlisp_eval_toplevelがデフォルトでcompile-and-run
; 経路(VMバイトコード)へ委譲し、コンパイル済みclosureは変数を位置ベースのupvalue(変数名を
; 保持しない)で捕捉するため、そのようなthunkに対してはprocess-local-variableは
; (動的変数を除き)何も見つけられない。現状これが機能するのは、thunkの生成自体が
; &optional/&rest等でツリーウォークへフォールバックする経路の場合のみである
(defun os:process-local-variable (p sym) (%process-local-variable p sym))

; milestone 114: プロセス環境インスペクタ。
;
; 他プロセスの状態を、通常のシェル/REPL(このプロセス自身)から安全に覗くためのLispレベルの
; 対話ユーティリティ。新規Cビルトインは追加せず、既存のdo-symbols系マクロ(milestone91、
; documents/lisp_package_operations.md)とprocess-local-variable(113)の組み合わせのみで
; 実装する。
;
; fork側パッケージ(milestone108)そのものを返す薄いアクセサ
(defun os:process-package (p) (slot-value p 'package))

; プロセスのfork側パッケージからアクセス可能な全シンボル(ローカル+useしている
; common-lisp-user経由の継承分。milestone91のdo-symbolsが辿るのと同じ集合)のうち、
; 関数束縛(fboundp)されているものを(symbol . function)の連想リストとして返す。
; 「他プロセスの…関数定義を覗く」の中心部分。do-symbolsは値を集約する機能を持たない
; 素朴なループマクロ(milestone91)なので、setqによる明示的なリスト構築
; (test-package.lispのrun-test-package-do-symbols等と同じ既存パターン)で結果を組み立てる
(defun os:process-function-definitions (p)
  (let ((acc nil))
    (do-symbols (s (os:process-package p))
      (if (fboundp s)
          (setq acc (cons (cons s (symbol-function s)) acc))
          nil))
    acc))

; プロセスのレキシカル環境(envスロット。%process-resumeが初回起動時にthunkのenvを捕捉した
; alist、milestone113)に束縛されている変数名一覧。process-local-variableは名前を渡して値を
; 読む一方向のAPIなので、対象の名前自体を知る手段が別途必要になる。envスロット自体は素の
; alistなので、carを取るだけで名前一覧が得られる(未起動なら常にnilなので空リストになる
; だけで安全にスキップされる)。os:を付けない内部ヘルパーであり、exportしない
(defun process-lexical-variable-names (p)
  (mapcar #'car (slot-value p 'env)))

(defun process-lexical-variables-helper (p names)
  (if (null names)
      nil
      (cons (cons (car names) (os:process-local-variable p (car names)))
            (process-lexical-variables-helper p (cdr names)))))

; 変数名一覧(envスロットの直接参照)と、process-local-variable(113、公式アクセサ)経由の
; 値読み取りを組み合わせる、本マイルストーンの中心的な結合部分。(symbol . value)の
; 連想リストを返す
(defun os:process-lexical-variables (p)
  (process-lexical-variables-helper p (process-lexical-variable-names p)))

; 上記2つ(関数定義一覧・レキシカル変数一覧)とプロセス自身のname/status/パッケージを
; 1つの構造にまとめた、インスペクタのトップレベルエントリポイント。本処理系にはprintf的な
; 整形出力手段が無いため、「対話ユーティリティ」はslot-value/car/cdrで覗ける構造化データを
; 返す関数として実装する(既存のprocess関連ビルトイン群と同じ設計方針)。
;
; package-nameはpkg_nameから毎回新規に文字列を作って返すため(string=が本処理系に無いことと
; 対称に、呼び出し毎にeqでない別オブジェクトが返る)、呼び出し元がeqで同一性確認したい場合は
; package自体(オブジェクト)を使うべきなので、'packageで生のパッケージオブジェクトも
; 併せて返す。'package-nameは表示用の文字列として別途保持する
(defun os:inspect-process (p)
  (list (cons 'name (slot-value p 'name))
        (cons 'status (slot-value p 'status))
        (cons 'package (os:process-package p))
        (cons 'package-name (package-name (os:process-package p)))
        (cons 'functions (os:process-function-definitions p))
        (cons 'lexical-variables (os:process-lexical-variables p))))

; milestone 115: 関数の「差し戻し」コマンド。
;
; fork側パッケージ(milestone109の運用手順どおり、shadowでベースと同名の別シンボルを確保し
; そこへ再定義したもの)でshadowされた関数を、ベースパッケージ(common-lisp-user)の元の定義に
; 戻すデバッグコマンド。新規Cビルトインは追加せず、既存のfind-symbol(milestone91)・
; intern・symbol-function・%set-symbol-function(milestone93)のみを組み合わせて実装する。
;
; find-symbol(name fork-pkg)のstatusが:inherited(fork側にローカルな別シンボルを持たず、
; useしているcommon-lisp-userのシンボルをそのまま共有している=そもそも差し戻す対象が
; 無い、shadowされていない)ならnil(何もしない)を返す。:internal/:externalの場合は
; fork側パッケージ内にshadow等でローカルに確保された別シンボルオブジェクトが実在するので、
; そのシンボルの関数セルをcommon-lisp-user側の同名シンボルの関数定義で上書きし、対象の
; シンボル自身を返す(%set-symbol-functionは対象シンボルのホームパッケージ=fork-pkgが
; ロック済みでない限りブロックしないため、fork側パッケージへの書込として問題なく通る。
; common-lisp-user自身は起動時にロックされるが、対象はfork-pkgのシンボルなので無関係)。
; ベース側にnameという関数定義が存在しない場合は、既存のsymbol-functionの仕様どおりpanicする
(defun os:revert-function (p name)
  (let* ((fork-pkg (slot-value p 'package))
         (found (find-symbol name fork-pkg)))
    (if (null found)
        nil
        (if (eq (cdr found) :inherited)
            nil
            (let ((base-sym (intern name)))
              (%set-symbol-function (car found) (symbol-function base-sym))
              (car found))))))

; milestone 118: プロセス一覧UI・REPL切替。
;
; documents/lisp_os_process.mdの計画ではCtrl2回連続押下(117)からの自動発火でこの機能へ
; 入ることを想定していたが、ConIn(EFI_SIMPLE_TEXT_INPUT_PROTOCOL、REPL本体のlisp_read_line
; が使う)とEFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL(116/117が使う)が実機/ファームウェア上で
; 同一の下位キーストロークキューを共有している可能性があり、その場合メインREPLループの
; ホットパスへライブなCtrl検知を組み込むと、REPL通常入力に向かうべき文字を横取りして
; 静かに入力を破壊するリスクがある。この可能性は-display noneのヘッドレスQEMUテスト
; 環境では検証も反証もできない(GUI/実キーボードが無い)。make testが常時実行する既存28
; テストフィクスチャ全てが通るメインREPLループにこの未検証のリスクを晒さないため、本
; マイルストーンはスコープを絞り、ユーザーが明示的に呼び出すコマンド(os:switch-process)
; としてのみ実装する。ライブなCtrl-Ctrl自動発火のメインループへの統合は、実機/GUI環境で
; 検証可能になるまで意図的に見送る(隠さず明記する)
;
; %read-console-expr(新規Cビルトイン、src/lisp.c)はConInのみを使う既存のlisp_read_line/
; lisp_read_from_buffer(milestone6/8、REPL本体と全く同じ経路)をそのまま再利用するだけで
; あり、EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOLには一切触れないため、上記のキュー競合リスクを
; 新たに持ち込まない
; defun本体は単一formのみという既存の制約(milestone21のprogn gotcha、
; documents/lisp_macroexpand.md等参照)に従い、複数の出力呼び出しを明示的な
; (progn ...)でまとめて1つのformにする
(defun %print-process-entry (p idx)
  (progn
    (write-string "  ")
    (princ idx)
    (write-string ") ")
    (princ (slot-value p 'name))
    (write-string " [")
    (princ (slot-value p 'status))
    (write-string "]")
    (write-line "")))

(defun %print-process-menu (processes idx)
  (if (null processes)
      nil
      (progn
        (%print-process-entry (car processes) idx)
        (%print-process-menu (cdr processes) (+ idx 1)))))

(defun %nth-process-or-nil (processes idx)
  (if (null processes)
      nil
      (if (zerop idx)
          (car processes)
          (%nth-process-or-nil (cdr processes) (1- idx)))))

; フェーズCのコンテキスト切替機構(104-107)とプロセスレジストリ(102)・process-resume(112)を
; そのまま使う。選択したプロセスがまだ一度もresumeされていない(statusスロットがnil)場合、
; %process-resumeはthunkが無いとpanicするため、事前にstatusを見て案内する(thunkはmake-process
; 時点では持たないので、この関数からは起動できない=既存のos:process-resumeを直接thunk付きで
; 呼んでもらう必要がある)。
;
; os:process-resumeはブロッキングであり、選択したプロセスがsuspendするか実行を終えるまで
; この関数自体が戻らない。つまり呼び出しが戻ってきた時点で制御がこのREPL(呼び出し元プロセス)
; に戻ったことを意味し、これが「アクティブなREPLを選択したプロセスへ切り替える」の実体になる
; milestone 129: カーソル制御・画面ダブルバッファリングロードマップの最終マイルストーン。
; フェーズI〜K(119〜128)でCビルトイン%set-cursor-position/%get-screen-size/%clear-screen
; (画面ダブルバッファ・VM命令ディスパッチループ経由のflush込み)まで整備済みなので、ここでは
; ユーザーが元々提示していたコード例どおりの薄いLispラッパーのみを追加する。
;
; os:goto-xyは%set-cursor-position(col row、0始まり、milestone120/126)への1対1の委譲
(defun os:goto-xy (x y) (%set-cursor-position x y))

; os:clear-screenは%clear-screen(milestone120/126)への1対1の委譲
(defun os:clear-screen () (%clear-screen))

; os:print-atはos:goto-xyでカーソルを移動した後、write-string(milestone98)で文字列を
; 出力する。defun本体は単一form限定という既存の制約(milestone21のprogn gotcha)に従い、
; 2つの呼び出しを明示的なprognでまとめる
(defun os:print-at (x y string)
  (progn
    (os:goto-xy x y)
    (write-string string)))

(defun os:switch-process ()
  (let ((processes (os:get-all-processes)))
    (if (null processes)
        (write-line "No processes registered.")
        (progn
          (write-line "Processes:")
          (%print-process-menu processes 0)
          (let ((selection (%read-console-expr "Select process number (nil to cancel): ")))
            (if (null selection)
                (write-line "Cancelled.")
                (let ((chosen (%nth-process-or-nil processes selection)))
                  (if (null chosen)
                      (write-line "Invalid selection.")
                      (if (null (slot-value chosen 'status))
                          (write-line "That process has not started yet (needs a thunk; call os:process-resume directly).")
                          (progn
                            (os:process-resume chosen)
                            (write-line "Returned to this REPL.")))))))))))

; milestone138続報: 起動ログの"Text Input Ex Protocol: FOUND"/"NOT FOUND"は起動直後の
; 一瞬しか表示されず流れてしまうため、REPLから同じ判定条件をいつでも参照できるようにする。
; g_text_input_exは起動時にLocateProtocolで一度だけ確定し以後不変なので、この値がnilなら
; ReadKeyStrokeEx経由のCtrl検知(milestone116以降)はそもそも動作しえない(KeyState自体が
; 得られない既存のConIn->ReadKeyStrokeフォールバックに落ちているため)
(defun os:text-input-ex-found-p () (%text-input-ex-found-p))
