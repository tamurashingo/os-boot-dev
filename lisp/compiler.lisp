; milestone63: lisp/stdlib.lispからコンパイラ本体（マクロ展開・アセンブラ・
; compile-expr一式・compile-and-run）を切り出したファイル。
;
; milestone65: 起動シーケンスを2段階化し、EfiMainはlisp/stdlib.lispより先にこの
; ファイルを読み込むよう順序を変更した。読み込み中はlisp_compiler_readyがまだfalse
; なので既存のツリーウォークで評価される（compile-expr自身をcompile-expr自身で
; コンパイルする鶏と卵問題を避ける）。
;
; ただし、macroexpand-all-forms/compile-expr自身の実装がnot/null/list/append/reverse/
; mapcarを内部的に呼び出している（defun本体の話ではなく、コンパイラというプログラム
; そのものの実装がこれらに依存している）。これらは元々stdlib.lisp側にあったため、
; 「compiler.lispを先に読み込み、その後にstdlib.lispを読み込む」という2段階の順序と
; 噛み合わず、stdlib.lispの最初のdefunをコンパイルしようとした瞬間に
; unbound variableでpanicする（起動シーケンスにはREPLのlongjmp復帰トラップがまだ
; 無いため、panicはそのままハングする）。したがってこの6つだけはコンパイラ自身の
; ブートストラップ用の前提としてこのファイルに置き、stdlib.lisp側からは削除した
; （どちらのファイルに書いてもglobal_envへの束縛結果は同じであり、REPLからの見え方は
; 変わらない）。
(defun not (x) (eq x nil))
(defun null (x) (eq x nil))

; 仮引数リスト全体を単一のsymbolにする書き方(milestone29でlisp_env_bind_paramsに
; 追加したrest-arg機構)で可変長引数を受け取る。評価済みの実引数がそのままリストとして
; argsに束縛されるので、それをそのまま返すだけでCLのlistと同じ挙動になる。rest-arg形式
; のdefunはコンパイル済みクロージャの呼び出し規約がrest-argをサポートしないため、
; lisp_eval_toplevelが個別にツリーウォークへフォールバックする（既存の
; lisp_defun_params_is_restarg判定、src/lisp.c）。したがってlist自身は
; lisp_compiler_readyの値に関わらず常にインタプリタクロージャのままだが、
; macroexpand-all/compile-exprの内部実装がlistを直接呼び出すため、global_envへの
; 束縛自体はこのファイルの中で（=lisp_compiler_readyがtrueになる前に）済ませておく
; 必要がある
(defun list args args)

; milestone87: append/reverseは元々非tail再帰(Cコールスタックの深さがリスト長に比例)
; で書かれており、これ自身がコンパイラ・マクロ展開器の内部実装として多用されるため、
; 28個の節を持つandの集約テストのように十分な長さ・入れ子でcompile-expr自身を
; 呼び出すと、この2関数の再帰深さがCスタックを食い尽くしてunbound variable等の
; 破損した状態でpanicする実クラッシュが再現した。milestone87で導入したdo特殊形式
; (Cの再帰を一切使わないwhile(1)ループ)で書き直し、リスト長に関わらずCスタックの
; 深さをO(1)に保つ。このファイル自身はlisp_compiler_readyがまだfalseな間に読み込ま
; れる(ツリーウォークで評価される)ため、ここではwhileマクロ(lisp/stdlib.lisp、
; まだ読み込まれていない)ではなくdoを直接使う。
;
; reverseはappendに依存しない単純な畳み込みなので、先にreverseを定義してから、
; appendはreverseの結果をbへ1要素ずつconsしていく形にする(相互依存を避ける)。
; CLのappendは可変長だが、このスコープでは2引数のみサポートする(明示的な範囲の限定)
(defun reverse (lst)
  (do ((src lst (cdr src))
       (result nil (cons (car src) result)))
      ((null src) result)))

; milestone87追記: 最初の実装はreverseを2回通す(a全体を逆順化してからさらに
; consで積み直す)形で書いたが、これはconsセル生成数が元の再帰実装(|a|個)の2倍
; (|a|×2個)になり、test-compile-expr.lisp(29個のdefun、うち1つは28節のand)の
; ロード中にヒープを圧迫し「Lisp panic: expected a cons cell but got something else」
; という別のクラッシュ(スタックではなくヒープ枯渇由来)を実測で引き起こした。
; rplacdで新規consセルを1個ずつ末尾に連結していく方式(head/tailポインタ手法)に
; 書き直し、生成するconsセル数を元の再帰実装と同じ|a|個に戻した(Cスタックの深さは
; 引き続きO(1)のまま)
(defun append (a b)
  (if (null a)
      b
      (let ((head (cons (car a) nil)))
        (do ((src (cdr a) (cdr src))
             (tail head))
            ((null src) (rplacd tail b) head)
          (let ((cell (cons (car src) nil)))
            (rplacd tail cell)
            (setq tail cell))))))

; fnはローカル変数（仮引数）だが、lisp_evalの関数呼び出し分岐は呼び出し式の演算子位置を
; 常に汎用evalで評価するため、ローカル変数が指すクロージャをそのまま(fn ...)の形で
; 呼び出せる。新しいfuncall/applyプリミティブは不要（milestone29の調査で確認済み）
;
; milestone87: append/reverseと同じ理由でdo化する。macroexpand-all-forms自身がandの
; 28節のように長い節リストに対して直接mapcarを呼ぶため(例:
; (cons op (mapcar macroexpand-all (cdr form)))、'and節)、append/reverseだけを
; do化してもmapcarが非tail再帰のままだとCスタックの深さが節数に比例してしまい、
; 実測でも修正しきれなかった。appendと同じ理由(2倍のconsセル生成によるヒープ圧迫を
; 避ける)で、reverse2回通しではなくrplacdによるhead/tail連結方式にしてある
(defun mapcar (fn lst)
  (if (null lst)
      nil
      (let ((head (cons (fn (car lst)) nil)))
        (do ((src (cdr lst) (cdr src))
             (tail head))
            ((null src) head)
          (let ((cell (cons (fn (car src)) nil)))
            (rplacd tail cell)
            (setq tail cell))))))

; --- コンパイラフロントエンド: マクロ展開 (milestone 40, documents/lisp_vm.md 目標2着手) ---
; macroexpand-all は既存の macroexpand-1 (milestone 21) を式全体に再帰的に適用し、
; 入力S式からマクロ呼び出しを完全に消し去る。quote/quasiquoteの内側はデータであり
; 評価されないため展開しない。各特殊形式は、変数名・タグ名など評価されない位置は
; そのまま保持し、評価される位置のサブフォームだけをmacroexpand-allする
(defun macroexpand-all-let-binding (binding)
  (list (car binding) (macroexpand-all (car (cdr binding)))))

(defun macroexpand-all-let (form)
  (cons (car form)
        (cons (mapcar macroexpand-all-let-binding (car (cdr form)))
              (mapcar macroexpand-all (cdr (cdr form))))))

(defun macroexpand-all-cond-clause (clause)
  (mapcar macroexpand-all clause))

; milestone87: doのbinding-specは(var init step)/(var init)/(var)/varのいずれかを
; 取り得る(let同様の並行束縛+step-formによる並行再束縛)。varの位置は評価されない
; ため保持し、init/step(存在する要素のみ)だけをmacroexpand-allする。形そのものの
; 正規化(2要素・3要素への揃え直し)はcompile-do側の責務とし、ここではmacroexpand-1が
; init/step内のマクロ呼び出しを展開できるようにするだけに留める
(defun macroexpand-all-do-binding (binding)
  (if (atom binding)
      binding
      (cons (car binding) (mapcar macroexpand-all (cdr binding)))))

; (do bindings (end-test . result-forms) . body)。end-test/result-formsはいずれも
; 評価される位置なので、まとめて1つのリストとしてmapcar macroexpand-allで展開できる
; (順序を保ったまま各要素を差し替えるだけなので、end-testとresult-formsを個別に
; 分けて扱う必要は無い)
(defun macroexpand-all-do (form)
  (cons (car form)
        (cons (mapcar macroexpand-all-do-binding (car (cdr form)))
              (cons (mapcar macroexpand-all (car (cdr (cdr form))))
                    (mapcar macroexpand-all (cdr (cdr (cdr form))))))))

; formはこの時点でトップレベルのマクロ呼び出しではないと分かっているconsである。
; いずれの特殊形式にも該当しない場合は関数呼び出しとみなし、演算子位置も含め
; 全要素を展開する（lisp_evalは演算子位置も汎用evalするため、lambda式の直書きなども
; そのままmacroexpand-allに乗る。stdlib.lispのmapcar冒頭の注釈参照）
(defun macroexpand-all-forms (form)
  (let ((op (car form)))
    (cond
      ((eq op 'quote) form)
      ((eq op 'quasiquote) form)
      ((eq op 'defmacro) form)
      ((eq op 'if) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'progn) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'and) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'or) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'when) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'unless) (cons op (mapcar macroexpand-all (cdr form))))
      ((eq op 'setq)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'let) (macroexpand-all-let form))
      ((eq op 'let*) (macroexpand-all-let form))
      ((eq op 'cond) (cons op (mapcar macroexpand-all-cond-clause (cdr form))))
      ((eq op 'block)
       (cons op (cons (car (cdr form)) (mapcar macroexpand-all (cdr (cdr form))))))
      ((eq op 'return-from)
       (if (null (cdr (cdr form)))
           form
           (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form)))))))
      ((eq op 'lambda)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'defvar)
       (if (null (cdr (cdr form)))
           form
           (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form)))))))
      ((eq op 'defparameter)
       (list op (car (cdr form)) (macroexpand-all (car (cdr (cdr form))))))
      ((eq op 'defun)
       (list op (car (cdr form)) (car (cdr (cdr form)))
             (macroexpand-all (car (cdr (cdr (cdr form)))))))
      ((eq op 'do) (macroexpand-all-do form))
      (t (mapcar macroexpand-all form)))))

; macroexpand-1(lisp_macroexpand_1)は呼び出し式の演算子を無条件にlisp_evalで評価して
; マクロかどうかを確認するため、if/let/quoteなど特殊形式のキーワードをそのまま渡すと
; 「unbound variable」でpanicする（特殊形式のキーワードは通常の変数として束縛されておらず、
; lisp_eval側では関数呼び出し分岐に入る前の専用チェックで捕まえているため）。従って特殊形式は
; マクロ呼び出しであり得ないと分かった時点でmacroexpand-1を呼ばずに直接macroexpand-all-forms
; へ渡す。マクロ呼び出しの可能性がある（特殊形式のキーワードではない）formだけがmacroexpand-1
; の対象になる
(defun macroexpand-all-special-form-p (op)
  (or (eq op 'quote) (eq op 'quasiquote) (eq op 'if) (eq op 'progn)
      (eq op 'let) (eq op 'let*) (eq op 'setq) (eq op 'cond)
      (eq op 'and) (eq op 'or) (eq op 'when) (eq op 'unless)
      (eq op 'block) (eq op 'return-from) (eq op 'lambda)
      (eq op 'defvar) (eq op 'defparameter) (eq op 'defun) (eq op 'defmacro)
      (eq op 'do)))

(defun macroexpand-all (form)
  (if (atom form)
      form
      (if (macroexpand-all-special-form-p (car form))
          (macroexpand-all-forms form)
          (let ((expanded (macroexpand-1 form)))
            (if (eq expanded form)
                (macroexpand-all-forms expanded)
                (macroexpand-all expanded))))))

; compileはS式をVM用バイトコードへ変換する入口。まずmacroexpand-allでマクロを
; 全て消し去ってから本番のコード生成に渡す。本番のコード生成(compile-expr、
; milestone42以降)はまだ存在しないため、現時点では展開結果をそのまま返す
(defun compile (form) (macroexpand-all form))

; --- バイトコード中間表現とアセンブラ (milestone 41, documents/lisp_vm.md 目標2) ---
; compile-expr(milestone42以降)はジャンプ先の絶対バイト位置を生成時にはまだ知らない
; (if/letなどのコンパイルでは「後で出てくる位置」へ前方参照でジャンプする必要がある)。
; そこで一旦「ラベル付き命令リスト」という中間表現(IR)へコンパイルし、アセンブラが
; 全命令のバイト長からラベルの絶対位置を確定させてから、最終的なバイト列を組み立てる。
;
; IRは命令のリスト。各要素は次のいずれかの形:
;   (label name)          -- 現在位置にnameというラベルを置く。バイトは出力しない
;   (opcode)               -- オペランド無しの1byte命令(例: (OP_ADD)、(OP_RETURN))
;   (opcode n)              -- オペランドnを2byte(リトルエンディアン)値として使う3byte命令
;                              (例: (OP_CONST 0)、(OP_LOAD_LOCAL 1))
;   (opcode (ref name))    -- オペランドがラベルnameの絶対バイト位置に解決される3byte命令
;                              (OP_JUMP/OP_JUMP_IF_FALSE用)
; オペランドがfixnumかラベル参照(ref name)かは、consかどうか(atom)で判別できるため、
; symbolp/numberpのような型判定ビルトインが無くても実装できる(fixnum/symbolはどちらも
; atomだが、ref参照は常にconsになるよう意図的にIRの形を選んでいる)。
;
; opcode番号はsrc/lisp.hのOP_*マクロと必ず一致させる(C側と手で同期を保つ必要がある)
(defvar *op-const*         0)
(defvar *op-add*           1)
(defvar *op-return*        2)
(defvar *op-jump*           3)
(defvar *op-jump-if-false*  4)
(defvar *op-load-local*     5)
(defvar *op-store-local*    6)
(defvar *op-make-local*     7)
(defvar *op-call*           8)
(defvar *op-make-closure*   9)
(defvar *op-load-upvalue*  10)
(defvar *op-store-upvalue* 11)
(defvar *op-cons*          12)
(defvar *op-car*           13)
(defvar *op-cdr*           14)
(defvar *op-eq*            15)
(defvar *op-global-ref*    16)
(defvar *op-global-set*    17)
(defvar *op-block*         18)
(defvar *op-return-from*   19)
(defvar *op-pop*           20)

(defun asm-label-p (instr) (eq (car instr) 'label))

; ラベル定義は0byte、オペランド無し命令は1byte、オペランド有り命令は3byte
; (milestone60: オペランドは元々1byteだったが、defunを既定でコンパイルするようになった結果
; 実際にジャンプ先/定数indexが255を超える関数が現れ、オペランドを2byte(リトルエンディアン)へ
; 拡張した。opcode 1byte + オペランド2byte = 3byte)
(defun asm-instr-length (instr)
  (if (null (cdr instr)) 1 3))

; オペランド値(fixnum、0〜65535を想定)を2byte(リトルエンディアン)へ分割するための
; 下位byte/上位byteの取り出し。*・/・mod・ashのようなビルトインは存在しないため、
; +/-/<のみで256を引き続けることで実装する(オペランド値はVM_BRIDGE_MAX_BYTECODE程度に
; 収まる範囲なので再帰の深さも小さい)
(defun low-byte (v)
  (if (< v 256) v (low-byte (- v 256))))

(defun high-byte (v)
  (if (< v 256) 0 (+ 1 (high-byte (- v 256)))))

; instrsを先頭から辿り、各labelの出現位置(先頭からの絶対バイトオフセット)を
; ((name . offset) ...)というalistとして集める。ラベル自身はバイトを消費しないため、
; ラベル定義に出会っても現在のoffsetをそのまま次の再帰へ渡す
(defun asm-collect-labels (instrs offset)
  (if (null instrs)
      nil
      (let ((instr (car instrs)))
        (if (asm-label-p instr)
            (cons (cons (car (cdr instr)) offset)
                  (asm-collect-labels (cdr instrs) offset))
            (asm-collect-labels (cdr instrs) (+ offset (asm-instr-length instr)))))))

; 見つからない場合はlabelsがnilになり(car nil)が自然にpanicする(存在しないラベル名は
; コンパイラ側のバグであり、nth等と同様にここでは防御的なチェックを行わない)
(defun asm-label-offset (name labels)
  (if (eq (car (car labels)) name)
      (cdr (car labels))
      (asm-label-offset name (cdr labels))))

(defun asm-resolve-operand (operand labels)
  (if (atom operand)
      operand
      (asm-label-offset (car (cdr operand)) labels)))

; 1命令分のバイト列を返す(ラベル定義はnil、すなわち0byte)。オペランドは2byte
; (リトルエンディアン、下位byte→上位byteの順)へ分割して出力する
(defun asm-emit-instr (instr labels)
  (if (asm-label-p instr)
      nil
      (if (null (cdr instr))
          (list (car instr))
          (let ((value (asm-resolve-operand (car (cdr instr)) labels)))
            (list (car instr) (low-byte value) (high-byte value))))))

(defun asm-emit-all (instrs labels)
  (if (null instrs)
      nil
      (append (asm-emit-instr (car instrs) labels)
              (asm-emit-all (cdr instrs) labels))))

; assembleはラベル付き命令リストIRを受け取り、ラベル参照をすべて絶対バイト位置へ解決した
; 上でフラットなバイト値(fixnum)のリストを返す。まだ生バイト列バッファへの変換ビルトインは
; 存在しないため(milestone46で導入予定の検証用ブリッジまで不要)、ここではLispのリストを
; そのままbytecode表現として使う
(defun assemble (instrs)
  (let ((labels (asm-collect-labels instrs 0)))
    (asm-emit-all instrs labels)))

; --- compile-expr: リテラル・quote・ifのコンパイル (milestone 42, documents/lisp_vm.md 目標2) ---
; compile-exprはマクロ展開済み(macroexpand-all通過後)のS式を1つ受け取り、milestone41の
; IR(ラベル付き命令リスト)を返す。複数のcompile-expr呼び出しをまたいで「定数プール」
; (constantsベクタに載る値の列)を1つに保つ必要があるため、呼び出し側は`ctx`という
; ミュータブルな箱(cons)を最初に1つ作って全呼び出しに引き渡す。ctxのcarは「これまでに
; 登録した定数の個数」、cdrは「登録済みの値を新しい順で並べたリスト」(rplacdでconsする
; だけでO(1)に追加できるようにするため逆順で持ち、確定時にreverseする)
(defun compile-make-ctx () (cons 0 nil))

(defun compile-register-const (ctx value)
  (let ((idx (car ctx)))
    (rplacd ctx (cons value (cdr ctx)))
    (rplaca ctx (+ idx 1))
    idx))

(defun compile-ctx-constants (ctx) (reverse (cdr ctx)))

; if本体のように複数のIR断片(いずれもリスト)を連結する場面が多いため、任意個の
; 断片を受け取れるようにrest-arg(milestone29のlist同様、仮引数リスト全体を1つの
; symbolにする書き方)でappendしていく
(defun compile-concat-all (instr-lists)
  (if (null instr-lists)
      nil
      (append (car instr-lists) (compile-concat-all (cdr instr-lists)))))

(defun compile-concat instr-lists (compile-concat-all instr-lists))

; formがatomの場合の既定の扱い: そのまま評価結果になる自己評価的な値として、
; OP_CONSTで定数プールから読み出すコードにコンパイルする。milestone43以降、
; symbolはまずcompile-variable-refでレキシカル束縛の有無を確認してから
; ここへフォールバックするため、ここに来るのは数値・nil・文字列などの本来の
; リテラルと、tおよびkeyword(いずれも自己評価するのでOP_GLOBAL_REFの対象には
; しない、milestone51)のみになる
(defun compile-literal (form ctx)
  (list (list *op-const* (compile-register-const ctx form))))

; レキシカルスコープ外のsymbol(グローバル変数・グローバル関数)を実行時に
; global_envへ問い合わせるコードにコンパイルする(milestone51)。symbol自身を
; 定数プールへ登録し、OP_GLOBAL_REFにそのindexを持たせる(compile-literalが
; 値そのものを登録するのと同じ仕組みを、symbolの登録に使い回している)
(defun compile-global-ref (form ctx)
  (list (list *op-global-ref* (compile-register-const ctx form))))

; quoteの内側はそもそも評価されないデータなので、car(cdr form)（quoteされた
; S式そのもの）をそのまま定数として登録する(macroexpand-allがquoteの内側を
; 展開しないのと同じ理由)
(defun compile-quote (form ctx)
  (list (list *op-const* (compile-register-const ctx (car (cdr form))))))

; --- コンパイル時レキシカル環境とlet/変数参照/setq (milestone 43, documents/lisp_vm.md 目標2) ---
; envは((symbol . slot) ...)というalist。「この位置で見えているレキシカル変数名→
; フレームスロット番号」の対応を表す。let束縛のスロットは一度確保すると解放しない
; (実行時にOP_MAKE_LOCALで一度ボックス化したフレームスロットを、let本体の評価が
; 終わった後で明示的に「解放」する命令が存在しないため、意図的にスロットを再利用
; しない単純化を採用した)。progn相当の逐次実行フォームがまだ存在しないmilestone43
; 時点では、ifの2つの分岐のように互いに排他的な経路が常に同じ深さから始まる
; ケースしか発生しないため、次に使うスロット番号は単純に「envに現在見えている
; 変数の数」(compile-env-length)で求まる。milestone44以降もこの関数はローカル
; スロット用alistだけでなくupvalue捕捉記録用alistの探索・計数にも再利用する
(defun compile-env-length (env)
  (if (null env)
      0
      (+ 1 (compile-env-length (cdr env)))))

; 見つからなければnilを返す安全な検索。レキシカルに束縛されていないsymbolの扱いは
; 呼び出し元(compile-variable-ref)に委ねる(milestone51以降、グローバル参照
; (OP_GLOBAL_REF)としてコンパイルされる。compile-literalへのフォールバックはtと
; keywordなど本来のリテラルにのみ残る)
(defun compile-env-find (name env)
  (if (null env)
      nil
      (if (eq (car (car env)) name)
          (car env)
          (compile-env-find name (cdr env)))))

; --- compile-expr: lambdaとクロージャ捕捉 (milestone 44, documents/lisp_vm.md 目標2) ---
; milestone43までのenv(単純な(symbol . slot)のalist)を、lambdaの境界をまたいで
; 自由変数を解決できる「スコープ」構造に拡張する。1つのスコープは
;   (locals captures next-slot-box . enclosing)
; の4要素からなる:
;   locals        -- このスコープ(関数)自身のローカル変数alist。milestone43のenvと同じ形
;   captures      -- このスコープがさらに外側から捕捉したupvalueの記録(下記compile-make-captures)
;   next-slot-box -- このスコープが属する関数活性化フレーム全体で「次に割り当てる
;                    物理フレームスロット番号」を保持するミュータブルな箱(cons、
;                    ctxの定数カウンタと同じ仕組み)。OP_MAKE_LOCALは一度確保した
;                    スロットを解放しない(下記compile-let-extend-scopeのコメント参照)
;                    ため、let同士がifの2分岐のように排他的でなく逐次実行される場合
;                    (milestone54のand/or/progn等が生成する入れ子)でもスロット番号が
;                    衝突しないよう、レキシカルな可視性(compile-env-length)ではなく
;                    この関数全体で共有される単調増加カウンタから割り当てる。lambdaの
;                    境界(新しい関数活性化フレーム)でのみ新しい箱を作り直す
;   enclosing     -- 直接囲む関数のスコープ、トップレベル(囲むlambdaが無い)ならnil
; トップレベルのcompile-expr呼び出しもnilではなく実体を持つスコープを渡す
; (compile-make-top-scope)必要がある。これはcompile-resolveがenclosingの有無だけで
; 「もうこれ以上外側を辿れない」を判定し、scopeそのものをnilにはしないという設計の
; ため(scope-locals等をnilに対して呼ぶと(car nil)でpanicしてしまう)
(defun compile-make-scope (locals captures next-slot-box enclosing)
  (cons locals (cons captures (cons next-slot-box enclosing))))

(defun scope-locals (scope) (car scope))
(defun scope-captures (scope) (car (cdr scope)))
(defun scope-next-slot-box (scope) (car (cdr (cdr scope))))
(defun scope-enclosing (scope) (cdr (cdr (cdr scope))))

(defun compile-make-next-slot-box (initial) (cons initial nil))
(defun compile-alloc-slot (box)
  (let ((idx (car box)))
    (rplaca box (+ idx 1))
    idx))

; capturesは「このスコープがまだ捕捉していないupvalueを新規に捕捉した記述子を
; 登録していく場所」。desc-ctxはcompile-make-ctxを再利用した(kind . index)記述子の
; 登録簿(compile-register-const/compile-ctx-constantsで登録・確定する、通常の
; 定数プールと全く同じ「値を渡してindexを得る」操作なので新しい仕組みを作らず流用する)。
; lookup-boxは「symbol名→既に割り当てたupvalue index」の対応をrplacaで書き足していく
; 単純な箱(cons)で、同じ外側変数を本体中で複数回参照しても捕捉記述子が重複登録
; されないようにするためのキャッシュ
(defun compile-make-captures () (cons (compile-make-ctx) (cons nil nil)))
(defun captures-desc-ctx (captures) (car captures))
(defun captures-lookup-box (captures) (cdr captures))
(defun captures-lookup-alist (captures) (car (captures-lookup-box captures)))
(defun captures-lookup-add (captures name idx)
  (rplaca (captures-lookup-box captures)
          (cons (cons name idx) (captures-lookup-alist captures))))

(defun compile-make-top-scope ()
  (compile-make-scope nil (compile-make-captures) (compile-make-next-slot-box 0) nil))

; symbol1つをスコープ連鎖に沿って解決する。戻り値は('local . slot)/('upvalue . idx)/nil
; のいずれか。まず自分自身のlocalsを見て、無ければ自分がすでに捕捉済みのupvalueの
; キャッシュを見て、どちらにも無ければ直接囲むスコープへ再帰的に問い合わせ、そこで
; 見つかった結果を新しいupvalue捕捉としてこのスコープに記録する(compile-resolve-
; capture-from-enclosing)。enclosingがnil(トップレベルまで辿り切った)のに見つから
; なければnilを返し、呼び出し元(compile-variable-ref)はcompile-literalへフォール
; バックする
(defun compile-resolve (name scope)
  (let ((local (compile-env-find name (scope-locals scope))))
    (if local
        (cons 'local (cdr local))
        (compile-resolve-upvalue name scope))))

(defun compile-resolve-upvalue (name scope)
  (let ((cached (compile-env-find name (captures-lookup-alist (scope-captures scope)))))
    (if cached
        (cons 'upvalue (cdr cached))
        (compile-resolve-capture-from-enclosing name scope))))

; 直接囲むスコープでnameを解決し、その結果に応じてkindを決めて新しいupvalue捕捉を
; 記録する。囲むスコープでの解決結果が('local . slot)ならkind=0(囲む関数のフレーム
; スロットを直接捕捉)、('upvalue . idx)ならkind=1(囲む関数自身のupvalues[idx]を
; そのままコピーする)。この2種類だけで何段外側の変数でも1段分の捕捉記述子の連鎖に
; フラット化できるため、OP_LOAD_UPVALUE/OP_STORE_UPVALUEは常に自分自身のupvalues
; ベクタだけを見ればよくなる(documents/lisp_vm.md milestone38のVM側設計と対応する)
(defun compile-resolve-capture-from-enclosing (name scope)
  (if (null (scope-enclosing scope))
      nil
      (let ((outer (compile-resolve name (scope-enclosing scope))))
        (if (null outer)
            nil
            (let ((captures (scope-captures scope)))
              (let ((kind (if (eq (car outer) 'local) 0 1)))
                (let ((idx (compile-register-const (captures-desc-ctx captures)
                                                    (cons kind (cdr outer)))))
                  (captures-lookup-add captures name idx)
                  (cons 'upvalue idx))))))))

; compile-resolveの結果((kind . index)のcons)から対応するload/store命令を選ぶ
(defun compile-load-op (resolved)
  (if (eq (car resolved) 'local) *op-load-local* *op-load-upvalue*))
(defun compile-store-op (resolved)
  (if (eq (car resolved) 'local) *op-store-local* *op-store-upvalue*))

; レキシカルに見つからないsymbol(tとkeywordを除く。両方とも自己評価するため
; compile-literalへ回す)はグローバル参照とみなし、compile-global-refで実行時に
; global_envを解決するコードにコンパイルする(milestone51で、レキシカルスコープ外の
; symbolを裸の定数としてpushしていた誤ったフォールバックを廃止した)
(defun compile-variable-ref (form ctx scope)
  (let ((resolved (compile-resolve form scope)))
    (cond
      (resolved (list (list (compile-load-op resolved) (cdr resolved))))
      ((and (symbolp form) (not (eq form t)) (not (keywordp form)))
       (compile-global-ref form ctx))
      (t (compile-literal form ctx)))))

; letはlet*と異なり、全てのinit-formを束縛前の外側scope(この関数の引数scope、
; まだ拡張していないもの)で評価する(milestone17のlisp_eval同様、束縛同士が
; 互いを参照できない並行束縛)。
;
; is_specialな名前(milestone18のdefvar/defparameterで既に確立済みの動的変数)を
; letで束縛する場合は、milestone57で既知の制限として明記した「真の動的束縛」を
; ここで実現する。is_specialはdefvarが実際に実行された時点で確定する性質だが、
; コンパイラ自身がこの処理系上で動くLisp関数である(コンパイル専用の別ステージが
; 存在しない)ため、compile-let-process-bindingsが今まさにコンパイルしている
; 時点でspecial-variable-pを直接呼んで判定できる(compile-defvarが自分自身の
; establish前後でこの値を実行時にしか判定できないのとは違う場面であることに注意)。
; special名の束縛は、通常のOP_MAKE_LOCALによるレキシカルシャドーイングではなく、
; 「シンボル自身のvalue(sym->value、lisp_env_lookup/lisp_env_setが最優先で見る
; 場所)を退避してから書き換え、let脱出時に復元する」という形にコンパイルする。
;
; 重要な制約: OP_MAKE_LOCALは値スタックの「最上位」だけを箱化(cons化)する命令で、
; 深さを指定して特定の位置を箱化することはできない(pop 1つ→push 1つで同じ位置に
; 置き換えるだけ)。そのため「全bindingのinit-formを先にpushし尽くしてから、まとめて
; 複数回OP_MAKE_LOCALを呼ぶ」という組み方は成立しない(2回目以降のOP_MAKE_LOCALは
; 1回目が箱化した値を再び箱化するだけで、それより下の値には決して届かない)。
; したがってOP_MAKE_LOCALは「値をpushした直後」に必ず1対1で対応させる必要がある。
;
; 一方、letの並行束縛semantics(どのinit-formも他のbindingの新しい値を見てはならない)
; を守るために本当に遅延させなければならないのは、special名の「グローバルの値
; (sym->value)そのものへの書き込み(OP_GLOBAL_SET)」だけである。push直後に
; OP_MAKE_LOCALで名前の無い隠しローカルへ値を箱化しておくだけなら、scope-locals
; にまだ登録していない限り他のどのinit-formからも参照できない(レキシカルには
; 見つからずcompile-global-ref経由でも、そのシンボルのグローバル値自体はまだ
; 書き換えていないので古い値が見える)。よってcompile-let-process-bindingsは
; bindingを先頭から順に1つずつ処理し、各bindingについて次を隣接して行う:
;   非special名: init-formのコード(元のscope、未拡張)→OP_MAKE_LOCAL
;                (このslotをlocals-additionsとして記録し、後でscope-localsへ足す)
;   special名: [OP_GLOBAL_REFで現在値をpush]→OP_MAKE_LOCAL(退避用の隠しslotへ)、
;              続けてinit-formのコード(元のscope)→OP_MAKE_LOCAL(新しい値用の
;              隠しslotへ)。この時点ではまだOP_GLOBAL_SETを呼ばないため、後続の
;              bindingのinit-formが誤って新しい値を見ることはない。
; 全bindingのpush+箱化が終わった後(compile-letが)、special名の「新しい値」slotを
; まとめてOP_LOAD_LOCAL→OP_GLOBAL_SETし、ここで初めて実際にグローバルへ反映する
; (この時点で全init-formの評価は完了済みなので、他のbindingを巻き込む余地は無い)。
; bodyの実行後、同じ要領で退避slotから読み出してOP_GLOBAL_SETし元の値へ戻す。
;
; ただしreturn-fromでbodyから早期脱出した場合はこの復元コードを素通りしてしまう
; (VMにunwind-protect相当の機構が無いため)。milestone57の既知の制限のうち
; return-from安全性に関する部分は本milestoneでも意図的に対象外のままとする。
(defun compile-let-restore-one (entry ctx)
  (list (list *op-load-local* (cdr entry))
        (list *op-global-set* (compile-register-const ctx (car entry)))))

(defun compile-let-restore-code (restores ctx)
  (if (null restores)
      nil
      (compile-concat (compile-let-restore-one (car restores) ctx)
                      (compile-let-restore-code (cdr restores) ctx))))

; push-irが値を1つpushするコードである前提で、直後にOP_MAKE_LOCAL slotを連結し新しい
; slotを割り当てる。milestone83/84でOP_MAKE_LOCALはFP相対indexオペランドを取り、
; 呼び出し時に確保済みの固定スロットへ直接書き込むようになった(スタックへpushし直さない)。
; (cons ir-with-make-local slot)を返す
(defun compile-let-push-and-box (push-ir scope)
  (let ((slot (compile-alloc-slot (scope-next-slot-box scope))))
    (cons (compile-concat push-ir (list (list *op-make-local* slot))) slot)))

; bindingsを先頭から順に処理し、(ir locals-additions commits restores)の
; 4要素リストを返す。ir中のOP_MAKE_LOCALは全てpush直後に隣接しており、
; special名のOP_GLOBAL_SETは一切含まない(commits/restoresとして呼び出し側へ
; 返すのみで、実際の発行はcompile-letが全binding処理後にまとめて行う)
(defun compile-let-process-bindings (bindings ctx scope)
  (if (null bindings)
      (list nil nil nil nil)
      (let ((sym (car (car bindings)))
            (init-form (car (cdr (car bindings)))))
        (if (special-variable-p sym)
            (let ((sym-idx (compile-register-const ctx sym)))
              (let ((saved (compile-let-push-and-box
                             (list (list *op-global-ref* sym-idx)) scope)))
                (let ((newval (compile-let-push-and-box
                               (compile-expr init-form ctx scope) scope)))
                  (let ((rest (compile-let-process-bindings (cdr bindings) ctx scope)))
                    (list
                      (compile-concat (car saved) (car newval) (car rest))
                      (car (cdr rest))
                      (cons (cons sym (cdr newval)) (car (cdr (cdr rest))))
                      (cons (cons sym (cdr saved)) (car (cdr (cdr (cdr rest))))))))))
            (let ((boxed (compile-let-push-and-box (compile-expr init-form ctx scope) scope)))
              (let ((rest (compile-let-process-bindings (cdr bindings) ctx scope)))
                (list
                  (compile-concat (car boxed) (car rest))
                  (cons (cons sym (cdr boxed)) (car (cdr rest)))
                  (car (cdr (cdr rest)))
                  (car (cdr (cdr (cdr rest)))))))))))

; locals-additions(通常束縛の(sym . slot))を先頭から順にscope-localsへ足していく。
; special束縛はlocals-additionsに含まれないため、bodyの中でその名前を参照すると
; レキシカルには見つからずcompile-global-ref(is_special対応)経由で正しく解決される
(defun compile-let-add-locals (locals-additions scope)
  (if (null locals-additions)
      scope
      (compile-let-add-locals
        (cdr locals-additions)
        (compile-make-scope
          (cons (car locals-additions) (scope-locals scope))
          (scope-captures scope)
          (scope-next-slot-box scope)
          (scope-enclosing scope)))))

; milestone87: 修正前は(car (cdr (cdr form)))で先頭のbody-formしか取らず、2個目以降の
; body-formはコンパイルすら行われず完全に消えていた(単に値が捨てられるのではなく
; バイトコードに一切現れない)。do/whileの本体をletで包み「ループ+その後で結果を返す」
; という2-form以上のbodyを書く典型的な命令型パターンが軒並み壊れるバグだった
; (test-while.lispで実測発見)。compile-let-star(841行目)が元からbodyを
; (cons 'progn body)で包んでいたのと同じ方式にし、compile-progn-body(既存の
; OP_POPによる非末尾値の破棄ロジック)へ委譲する
;
; 既知の制限(milestone78で発見、根本修正はmilestone83/84として未着手): letがtail
; 位置以外(関数呼び出しの引数のような非tail位置の兄弟式)で使われた場合、
; OP_MAKE_LOCALが確保したスロットがスタック上に残り続け、後続の計算を壊すことがある
(defun compile-let (form ctx scope)
  (let ((bindings (car (cdr form)))
        (body (cons 'progn (cdr (cdr form)))))
    (let ((processed (compile-let-process-bindings bindings ctx scope)))
      (let ((bindings-ir (car processed))
            (locals-additions (car (cdr processed)))
            (commits (car (cdr (cdr processed))))
            (restores (car (cdr (cdr (cdr processed))))))
        (compile-concat bindings-ir
                        (compile-let-restore-code commits ctx)
                        (compile-expr body ctx (compile-let-add-locals locals-additions scope))
                        (compile-let-restore-code restores ctx))))))

; setqは代入した値をそのまま式の値として返す(Common Lisp/既存lisp_evalと同じ)。
; OP_STORE_LOCAL/OP_STORE_UPVALUEはストアと同時にスタック最上位をpopしてしまう
; (値を残さない)ため、ストア後に対応するload命令で同じスロット/upvalue indexを
; 読み直し、「常に1つの値をスタックに残す」というcompile-exprの不変条件を既存の
; 命令だけで満たす。resolvedがnil(setqの対象がどのスコープにも見つからない、
; つまりグローバル変数へのsetq)の場合はOP_GLOBAL_SET/OP_GLOBAL_REFの組で同じ
; 「store後にreload」の形にコンパイルする(milestone51。対象のsymbol自身を定数
; プールへ登録し、OP_GLOBAL_REFと共有するため一度だけ登録する)
(defun compile-setq (form ctx scope)
  (let ((target (car (cdr form)))
        (value-code (compile-expr (car (cdr (cdr form))) ctx scope)))
    (let ((resolved (compile-resolve target scope)))
      (if resolved
          (compile-concat value-code
                          (list (list (compile-store-op resolved) (cdr resolved)))
                          (list (list (compile-load-op resolved) (cdr resolved))))
          (let ((idx (compile-register-const ctx target)))
            (compile-concat value-code
                            (list (list *op-global-set* idx))
                            (list (list *op-global-ref* idx))))))))

; elseが省略された(if cond then)の場合は、既存のlisp_evalのif実装と同じく
; elseの値をnilとして扱う
(defun compile-if-else-code (form ctx scope)
  (if (null (cdr (cdr (cdr form))))
      (compile-expr nil ctx scope)
      (compile-expr (car (cdr (cdr (cdr form)))) ctx scope)))

; ifは次の形にコンパイルする:
;   <cond-code>
;   OP_JUMP_IF_FALSE else-label
;   <then-code>
;   OP_JUMP end-label
;   label else-label
;   <else-code>
;   label end-label
; else/endのラベル名はgensym(milestone20、intern済みシンボル表に登録されない
; ため衝突しない)で毎回新しく作るため、ifを何重に入れ子にしてもラベル名が
; 衝突することはない
(defun compile-if (form ctx scope)
  (let ((cond-code (compile-expr (car (cdr form)) ctx scope))
        (then-code (compile-expr (car (cdr (cdr form))) ctx scope))
        (else-code (compile-if-else-code form ctx scope))
        (else-label (gensym))
        (end-label (gensym)))
    (compile-concat cond-code
                    (list (list *op-jump-if-false* (list 'ref else-label)))
                    then-code
                    (list (list *op-jump* (list 'ref end-label)))
                    (list (list 'label else-label))
                    else-code
                    (list (list 'label end-label)))))

; milestone89: &optional/&rest/&key/&aux/&allow-other-keysのいずれか
(defun lambda-list-keyword-p (sym)
  (or (eq sym '&optional)
      (eq sym '&rest)
      (eq sym '&key)
      (eq sym '&aux)
      (eq sym '&allow-other-keys)))

; milestone89: paramsに上記5keywordのいずれかが1つでも含まれるか。
; bare-symbol rest-arg形式(milestone29)ではparamsがconsではないため自然にnilになる
(defun lambda-list-contains-keyword-p (params)
  (if (atom params)
      nil
      (if (lambda-list-keyword-p (car params))
          t
          (lambda-list-contains-keyword-p (cdr params)))))

; lambdaの各仮引数に、OP_CALLの呼び出し規約(引数はフレーム先頭からnargs個の
; スロットにその場でボックス化される、documents/lisp_vm.md milestone37)と一致する
; 順序でスロット0, 1, 2...を割り当てる
(defun compile-lambda-param-locals (params idx)
  (if (null params)
      nil
      (cons (cons (car params) idx)
            (compile-lambda-param-locals (cdr params) (+ idx 1)))))

; lambdaは新しい関数境界を作るため、本体は独立した定数プール(inner-ctx)と
; 独立したスコープ(locals=仮引数、captures=まだ空、enclosing=このlambdaを
; 直接囲むscope)でコンパイルする。本体コンパイル中にcompile-resolveが
; enclosingを辿って外側変数を捕捉するたびcaptures-desc-ctxへ記述子が積まれて
; いくので、本体コンパイルが終わった時点でinner-scopeのcaptures-desc-ctxを
; 確定すればそれがそのままupvalue_descsになる。
;
; compile-exprはIR(ラベル付き命令リスト)を返すだけの関数なので、lambdaの
; コンパイル結果を実際のLispClosureとして直接構築することはできない(milestone35
; 以降のCブリッジ builtinがまだ存在しない)。そのためmilestone41のbytecode変換
; (assemble)と同じ考え方で、「このlambdaを実体化するのに必要な情報一式」を
; (closure-template nargs bytecode constants upvalue-descs)という素のLispリストに
; まとめ、それをひとつの値として外側ctxの定数プールに登録するにとどめる。先頭に
; 'closure-templateという印を置くのは、milestone46のvm-materialize-constantsが
; 「定数プール中のどの値がまだ実体化していないlambdaテンプレートか」を、
; (quote (a b))のような単なるデータのリストと区別するために必要(データのリストが
; 偶然この形に一致する可能性は、この検証専用ブリッジの対象範囲では無視できる)。
; この入れ子リストを実際のLispClosureへ組み立てる処理はmilestone46のvm-make-closure
; ビルトインに委ねる(milestone41の「バイト列への変換はここでは行わない」という
; 制約と同じ理由)。
;
; compile-exprは常に「スタックに値を1つpushするコード」を返すだけで、OP_RETURNは
; 発行しない(milestone42-45はバイトコードIRの構造検証止まりで実際にlisp_vm_execへ
; 渡していなかったため、この欠落はmilestone46でOP_RETURNが無いまま関数末尾を素通り
; してbytecode配列の外側の未定義バイトを命令として読んでしまう(unknown VM opcode
; panic)まで発覚しなかった)。そのため本体コードの末尾に明示的にOP_RETURNを1つ
; 追加し、milestone35以降の手動バイトコード同様「最後は必ずOP_RETURNで終わる」
; 規約に合わせる
; milestone89: paramsが&optional/&rest/&key/&aux/&allow-other-keysのいずれかを含む
; lambdaは、ここでそのまま(未対応のまま)コンパイルすると&optional等の語がそのまま
; 1個目の仮引数名として扱われる危険な黒魔術(サイレントな誤動作)になる。トップレベル
; のdefunはlisp_eval_toplevel(src/lisp.c、lisp_defun_params_needs_interpreter)が
; 個別にツリーウォークへフォールバックさせるためここには来ないが、すでにコンパイル
; されている関数の内側に直接書かれたネストしたlambda式はこの分岐を通るため、
; 明示的にpanicする(documents/lisp_lambda_list_keywords.mdのスコープ外として明記済み)
(defun compile-lambda (form ctx scope)
  (let ((params (car (cdr form)))
        (body (car (cdr (cdr form)))))
    (if (lambda-list-contains-keyword-p params)
        (%panic-compiled-lambda-list-keyword)
        (let ((inner-ctx (compile-make-ctx)))
          (let ((inner-scope (compile-make-scope (compile-lambda-param-locals params 0)
                                                  (compile-make-captures)
                                                  (compile-make-next-slot-box (compile-env-length params))
                                                  scope)))
            (let ((body-code (assemble (compile-concat (compile-expr body inner-ctx inner-scope)
                                                        (list (list *op-return*))))))
              ; milestone83/84: scope-next-slot-boxはparams分のindexから始まり、letが束縛
              ; されるたびに単調増加するだけ(再利用・減少しない)なので、本体コンパイル完了後の
              ; 最終値がそのままこの関数フレームが必要とするローカルスロット総数(max-locals)になる
              (let ((max-locals (car (scope-next-slot-box inner-scope))))
                (let ((package (list 'closure-template
                                      (compile-env-length params)
                                      max-locals
                                      body-code
                                      (compile-ctx-constants inner-ctx)
                                      (compile-ctx-constants (captures-desc-ctx (scope-captures inner-scope))))))
                  (list (list *op-make-closure* (compile-register-const ctx package)))))))))))

; --- compile-expr: 関数呼び出し・プリミティブ呼び出し (milestone 45, documents/lisp_vm.md 目標2) ---
; OP_CALL <nargs>(src/lisp.c)の呼び出し規約は「先にnargs個の生の引数値をスタックに積み、
; その上に呼び出し対象の関数値を積んでから発行する」(OP_CALLは最初にfn_objをpopし、
; 残りのnargs個をそのままフレームとしてボックス化する)。そのためcompile-callは
; 引数を先頭から順にコンパイルしてから、最後に関数式(car form。symbolの場合も
; あればlambda式そのものである場合もあり、いずれもcompile-expr自身に委ねられる)
; をコンパイルする
(defun compile-call-args (args ctx scope)
  (if (null args)
      nil
      (compile-concat (compile-expr (car args) ctx scope)
                      (compile-call-args (cdr args) ctx scope))))

(defun compile-call (form ctx scope)
  (compile-concat (compile-call-args (cdr form) ctx scope)
                  (compile-expr (car form) ctx scope)
                  (list (list *op-call* (compile-env-length (cdr form))))))

; milestone39でVMにインライン化された4つのプリミティブ(OP_CONS/OP_CAR/OP_CDR/OP_EQ)は
; 汎用のOP_CALLを経由せず、対応する専用命令へ直接コンパイルする。OP_CONSは
; cdr_valを先にpop・car_valを後にpopする実装(src/lisp.c)なので、carになる式を
; 先に、cdrになる式を後にコンパイルして積む順序を合わせる
(defun compile-cons (form ctx scope)
  (compile-concat (compile-expr (car (cdr form)) ctx scope)
                  (compile-expr (car (cdr (cdr form))) ctx scope)
                  (list (list *op-cons*))))

(defun compile-car (form ctx scope)
  (compile-concat (compile-expr (car (cdr form)) ctx scope)
                  (list (list *op-car*))))

(defun compile-cdr (form ctx scope)
  (compile-concat (compile-expr (car (cdr form)) ctx scope)
                  (list (list *op-cdr*))))

(defun compile-eq (form ctx scope)
  (compile-concat (compile-expr (car (cdr form)) ctx scope)
                  (compile-expr (car (cdr (cdr form))) ctx scope)
                  (list (list *op-eq*))))

; OP_ADDはmilestone35からVMに存在する基本命令だが、milestone39のインライン化対象
; (OP_CONS/OP_CAR/OP_CDR/OP_EQ)には含まれていなかったため、+を関数呼び出しとして
; 書いてもこれまでのcompile-exprでは呼び出す先が存在しなかった(未束縛symbolの
; 自己評価フォールバックでsymbol '+自体がOP_CALLに渡り、実行時にpanicする)。
; milestone46の統合検証で算術を含むケースを再現するために、cons/car/cdr/eqと
; 同じ形でOP_ADDへの直接コンパイルを追加する。加算は可換なのでpop順自体に
; 意味はないが、他のプリミティブと同じ「左の式を先にコンパイルする(先にpushする)」
; 規則に合わせる
;
; 修正 (milestone 58): OP_ADDは2引数分しかpopしないため、当初のcompile-addは
; 引数を常に2個(第1・第2引数)だけコンパイルし、3個以上の引数を黒く無視していた
; (例: (+ 3 3 3)がpanic無しに6を返す)。+はlisp_builtin_add(src/lisp.c)側では
; 可変長引数なので、ビルトイン呼び出し経由(OP_CALL)ならこの問題は起きないが、
; compile-addは直接OP_ADDへインライン化するため独自にn引数対応が必要だった。
; milestone57まではcompile-and-runを明示的に呼ぶテストコードでしか(+ a b)の
; 2引数の形しか使われず露見しなかったが、milestone58でトップレベルformが既定で
; コンパイル経路を通るようになった結果、通常の3引数以上の+呼び出しで初めて
; 発覚した。引数無し(結果0)・1個(そのまま)・2個以上(右結合でOP_ADDを繰り返す)の
; 全ての引数個数を再帰的に処理する
(defun compile-add (form ctx scope)
  (compile-add-args (cdr form) ctx scope))

(defun compile-add-args (args ctx scope)
  (cond
    ((null args) (compile-expr 0 ctx scope))
    ((null (cdr args)) (compile-expr (car args) ctx scope))
    (t (compile-concat (compile-expr (car args) ctx scope)
                        (compile-add-args (cdr args) ctx scope)
                        (list (list *op-add*))))))

; --- compile-expr: progn/let*/cond/and/or/when/unless (milestone 54, documents/lisp_vm_integration.md) ---
; この7種はいずれもVMに新しい命令を追加せず、既存のif/let(とジャンプ命令)の組み合わせに
; 脱糖(desugar)する。具体的には「新しいS式を組み立てて、それをcompile-exprへ再度渡す」
; という形で実装する(生のIRを自分で組み立てない)。この方法を選んだ理由は、let束縛や
; ifの分岐にまつわるスコープ拡張・ラベル発行・ジャンプ先解決を、既に検証済みのcompile-let/
; compile-ifにそのまま委譲できるため。ここで組み立てるif/let/progn/cond/and/or
; はいずれもmacroexpand-allの対象外の特殊形式(macroexpand-all-special-form-p)であり、
; compile-exprに渡す前に呼び出し元(compile-and-run等)が既にmacroexpand-allを1回
; 通しているため、ここで組み立てるサブフォーム自体を再度macroexpand-allする必要はない
; (もともとのformの一部を再利用しているだけで、新しいマクロ呼び出しを持ち込んでは
; いないため)

; 修正 (milestone 78): 当初は(progn e1 e2...) => (let ((gensym e1)) (progn e2...))
; とletのOP_MAKE_LOCAL経由でe1の値を捨てていたが、OP_MAKE_LOCALはボックス化した値を
; スタック上に永続的に残す(対応する解放命令が無い、let束縛スロットの前提そのもの)。
; これはprogn/let全体が関数のtail位置にある場合はOP_RETURNがフレーム全体を捨てるため
; 無害だが、(eq (progn a b) (progn c d))のように関数呼び出しの引数(non-tail位置)として
; 使われた場合、捨てられなかったボックスがスタックに残り続け、後続の引数やOP_CALLの
; nargsが期待するスタック深さを壊してcompile-callの「1つのサブ式は必ず1つの値だけを
; スタックに残す」という前提全体を崩壊させる重大バグだった(defpackageの展開結果を
; eqの引数として直接比較するテストで発覚)。let(OP_MAKE_LOCAL)を経由せず、e1を評価して
; 値を残さず捨てるだけの専用命令OP_POPを新設し、それに置き換えて解決する
(defun compile-progn-body (forms ctx scope)
  (cond
    ((null forms) (compile-expr nil ctx scope))
    ((null (cdr forms)) (compile-expr (car forms) ctx scope))
    (t (compile-concat (compile-expr (car forms) ctx scope)
                        (list (list *op-pop*))
                        (compile-progn-body (cdr forms) ctx scope)))))

(defun compile-progn (form ctx scope) (compile-progn-body (cdr form) ctx scope))

; (let* () . body) => (progn . body)、(let* ((v1 i1) rest...) . body) =>
; (let ((v1 i1)) (let* (rest...) . body)) という入れ子のletへ1束縛ずつ帰着させる。
; 各再帰段でbindingsが1つ減るごとにletを1つ被せるので、v2の初期化式はv1が既に
; 束縛された内側のletの中でコンパイルされ、let*の逐次束縛(milestone17のlisp_eval
; と同じ、後の束縛が前の束縛を参照できる)が自然に再現される
(defun compile-let-star-bindings (bindings body ctx scope)
  (if (null bindings)
      (compile-expr (cons 'progn body) ctx scope)
      (compile-expr (list 'let (list (car bindings))
                          (cons 'let* (cons (cdr bindings) body)))
                    ctx scope)))

(defun compile-let-star (form ctx scope)
  (compile-let-star-bindings (car (cdr form)) (cdr (cdr form)) ctx scope))

; (and) => t、(and e) => e、(and e1 e2...) => (if e1 (and e2...) nil)。
; 短絡評価はifのジャンプそのものであり、新しい仕組みは何も要らない
(defun compile-and-forms (forms ctx scope)
  (cond
    ((null forms) (compile-expr t ctx scope))
    ((null (cdr forms)) (compile-expr (car forms) ctx scope))
    (t (compile-expr (list 'if (car forms) (cons 'and (cdr forms)) nil) ctx scope))))

(defun compile-and (form ctx scope) (compile-and-forms (cdr form) ctx scope))

; (or) => nil、(or e) => e、(or e1 e2...) => (let ((g e1)) (if g g (or e2...)))。
; orはandと違い分岐の結果として「テストした値そのもの」を返す必要があるため、
; テスト結果を1回だけletで束縛してから、その束縛済みの値をifの条件とthen節の
; 両方で参照する(テスト式を2回評価しないため)
(defun compile-or-forms (forms ctx scope)
  (cond
    ((null forms) (compile-expr nil ctx scope))
    ((null (cdr forms)) (compile-expr (car forms) ctx scope))
    (t (let ((g (gensym)))
         (compile-expr (list 'let (list (list g (car forms)))
                             (list 'if g g (cons 'or (cdr forms))))
                       ctx scope)))))

(defun compile-or (form ctx scope) (compile-or-forms (cdr form) ctx scope))

; condの各クローズは(test . body)。bodyがあれば(if test (progn . body) (cond . rest))、
; bodyが省略された「テストの値そのものを返す」CLの慣用句(test-onlyクローズ、
; test/lisp/test-special-forms.lispの(cond (5))が5を返すケースが既存の回帰テスト)は
; orの1ステップと全く同じ構造なので、同じletで束縛してから使い回す形にする
(defun compile-cond-clause (clause rest ctx scope)
  (let ((test (car clause))
        (body (cdr clause)))
    (if (null body)
        (let ((g (gensym)))
          (compile-expr (list 'let (list (list g test))
                              (list 'if g g (cons 'cond rest)))
                        ctx scope))
        (compile-expr (list 'if test (cons 'progn body) (cons 'cond rest)) ctx scope))))

(defun compile-cond-clauses (clauses ctx scope)
  (if (null clauses)
      (compile-expr nil ctx scope)
      (compile-cond-clause (car clauses) (cdr clauses) ctx scope)))

(defun compile-cond (form ctx scope) (compile-cond-clauses (cdr form) ctx scope))

; (when test . body) => (if test (progn . body) nil)
(defun compile-when (form ctx scope)
  (compile-expr (list 'if (car (cdr form)) (cons 'progn (cdr (cdr form))) nil) ctx scope))

; (unless test . body) => (if test nil (progn . body))
(defun compile-unless (form ctx scope)
  (compile-expr (list 'if (car (cdr form)) nil (cons 'progn (cdr (cdr form)))) ctx scope))

; --- compile-expr: block/return-from (milestone 55, documents/lisp_vm_integration.md) ---
; if/let/lambdaへの脱糖では表現できない非局所脱出のため、milestone54の7形式とは異なり
; 新規opcode(OP_BLOCK/OP_RETURN_FROM、src/lisp.h)を使う。blockの本体は引数無しのlambdaとして
; コンパイルすることで、upvalue捕捉(自由変数参照)をcompile-lambdaの既存機構に丸ごと委譲できる
; (block自身にレキシカルスコープを拡張する仕組みは要らない――bodyは外側のscopeをそのまま
; 見えるlambdaとして包むだけでよい)。tagは評価されない生のsymbolなので、compile-literalと
; 同じ「定数プールへそのまま登録する」方式(compile-register-const)で登録する
(defun compile-block (form ctx scope)
  (let ((tag (car (cdr form)))
        (body (cdr (cdr form))))
    (compile-concat (compile-lambda (list 'lambda nil (cons 'progn body)) ctx scope)
                    (list (list *op-block* (compile-register-const ctx tag))))))

; return-fromの値部分は通常のcompile-exprで評価してからpopされる値としてOP_RETURN_FROMへ渡す。
; 値省略時は(return-from tag)がnilを返す既存インタプリタ(milestone19)の挙動に合わせてnilを使う
(defun compile-return-from (form ctx scope)
  (let ((tag (car (cdr form)))
        (value-forms (cdr (cdr form))))
    (compile-concat (compile-expr (if (null value-forms) nil (car value-forms)) ctx scope)
                    (list (list *op-return-from* (compile-register-const ctx tag))))))

; --- compile-expr: quasiquote対応 (milestone 56, documents/lisp_vm_integration.md) ---
; 既存インタプリタのlisp_qq_expand(src/lisp.c、milestone9台)と全く同じ再帰構造を、
; 「実行時にconsを積むS式を組み立ててcompile-exprへ再度渡す」という脱糖として
; コンパイル時に行う(milestone54のprogn等と同じ「新しいS式を組み立てて再帰する」
; 技法)。lisp_qq_expandはformを直接env上でwalkして即座に値を作るが、compile-exprは
; コード生成しかできないため、対応するcons/append呼び出しのS式を組み立てておき、
; 実際の値の構築は生成されたコードの実行時に行わせる。
; - formがコンスでなければ(quote form)として自己クオートするデータのまま残す
;   (symbol等をcompile-variable-refに渡すと変数参照として誤解釈されてしまうため)
; - formが(unquote x)そのものならxをそのまま返し、通常の式として(呼び出し元の
;   compile-exprで)評価させる(lisp_qq_expandがlisp_eval(x, env)する箇所に対応)
; - リスト先頭要素が(unquote-splicing x)なら(append x <残りを再帰>)を組み立てる
;   (lisp_qq_expandのlisp_append(spliced, rest)に対応。appendはlisp/stdlib.lisp
;   本体で定義済みの通常のインタプリタクロージャで、OP_CALLのlisp_apply委譲
;   (milestone52)経由で呼ばれる)
; - それ以外のコンスは(cons <carを再帰> <cdrを再帰>)を組み立てる
; ネストしたquasiquote自体は特別扱いしない点もlisp_qq_expandの既存の単純化を
; そのまま継承する(内側のquasiquoteのunquote/unquote-splicingが外側と同じ深さで
; 展開される)。また、macroexpand-allはquasiquoteの内側を展開しない(既存の
; macroexpand-all-forms、opがquasiquoteならformをそのまま返す)ため、unquote内で
; マクロを呼んでいる場合はツリーウォークと異なりコンパイル時に展開されない
; (ツリーウォークはlisp_evalが呼び出し式ごとに毎回マクロ判定するため展開されるが、
; VMにその仕組みは無い)。この非対称性は既存のmacroexpand-allの制約であり、
; 本milestoneではunquote内でマクロを呼ばない前提で扱う
(defun compile-qq-desugar (form)
  (cond
    ((atom form) (list 'quote form))
    ((eq (car form) 'unquote) (car (cdr form)))
    ((and (not (atom (car form))) (eq (car (car form)) 'unquote-splicing))
     (list 'append (car (cdr (car form))) (compile-qq-desugar (cdr form))))
    (t (list 'cons (compile-qq-desugar (car form)) (compile-qq-desugar (cdr form))))))

(defun compile-quasiquote (form ctx scope)
  (compile-expr (compile-qq-desugar (car (cdr form))) ctx scope))

; --- compile-expr: defvar/defparameter対応 (milestone 57, documents/lisp_vm_integration.md) ---
; is_specialはdefvarが実際に実行されるまで真にならない実行時の可変プロパティであり、
; コンパイル時に静的解決できない。そのため新opcodeは追加せず、is_special/valueへの
; 最小限の直接アクセスをC側にspecial-variable-p/establish-specialという通常のビルトイン
; として公開し(src/lisp.c)、既存のOP_CALL(milestone56がappendを再利用したのと同じ考え方)
; 経由で呼び出す。
; defvarは「既にis_specialなら値を評価も上書きもしない」という非対称な条件分岐を持つ
; (lisp_evalのdefvar特殊形式、src/lisp.c、milestone18と同じ挙動)。これをVMのif/progn
; へそのまま脱糖する(milestone54のprogn等と同じ「新しいS式を組み立てて再帰する」技法):
;   (defvar sym value) => (if (special-variable-p 'sym) 'sym (establish-special 'sym value))
;   (defparameter sym value) => (establish-special 'sym value)
; value-formの評価中にreturn-fromが発生した場合、establish-specialへのOP_CALL自体が
; バイトコード上まだ実行されていない位置で早期returnするため、is_specialは立たない
; (lisp_evalのif (lisp_return_tag != LISP_NIL) return; と同じ安全性が、OP_CALLの
; 引数評価が単純に後続命令へ進めなくなるというVMの構造から自然に得られる)。
;
; 注意: compile-let/compile-let*はis_specialを一切考慮しない。既存の特殊変数名をletで
; 束縛すると、真の動的束縛(letを抜ける際の復元、return-fromで中断されても復元される保証)
; ではなく通常のレキシカルシャドーイングとして扱われる。これを安全に実現するにはVM側に
; unwind-protect相当の機構(return-fromでフレームが早期returnしても後続の「復元」命令を
; 必ず実行する仕組み)が必要だが、これは本milestoneの一行の主旨(defvar/defparameter
; フォーム自体のコンパイル対応)を超える範囲であり、明確に対象外とする
(defun compile-defvar (form ctx scope)
  (let ((sym (car (cdr form)))
        (value-form (car (cdr (cdr form)))))
    (compile-expr (list 'if (list 'special-variable-p (list 'quote sym))
                             (list 'quote sym)
                             (list 'establish-special (list 'quote sym) value-form))
                  ctx scope)))

(defun compile-defparameter (form ctx scope)
  (let ((sym (car (cdr form)))
        (value-form (car (cdr (cdr form)))))
    (compile-expr (list 'establish-special (list 'quote sym) value-form) ctx scope)))

; milestone 60: defun本体はlambdaと全く同じ規約(bodyは単一formのみ、milestone21の
; gotchaを継承)でコンパイルできるため、(defun name params body) =>
; (establish-global-function (quote name) (lambda params body))という新しいS式を
; 組み立ててcompile-exprへ再度渡す(milestone54のprogn等と同じ「S式レベルの脱糖」技法)。
; establish-global-function(src/lisp.c)はツリーウォークのdefun特殊形式と全く同じ
; lisp_env_extendでglobal_envへ束縛する新規Cビルトインで、既存のOP_CALL経由で呼ぶ。
; rest-arg形式(paramsがbare symbol、milestone29)のdefunは、コンパイル済みクロージャの
; 呼び出し規約(closure->nargsの厳密一致、OP_CALL/lisp_apply)がrest-argを一切サポート
; しない(documents/lisp_vm_integration.mdのスコープ外として明記済み)ため、この分岐には
; 来ない: lisp_eval_toplevel(src/lisp.c)がlisp_defun_params_is_restargで判定して
; そのケースだけ既存のツリーウォークへ振り分ける
(defun compile-defun (form ctx scope)
  (let ((name (car (cdr form)))
        (params (car (cdr (cdr form))))
        (body (car (cdr (cdr (cdr form))))))
    (compile-expr (list 'establish-global-function (list 'quote name)
                                                     (list 'lambda params body))
                  ctx scope)))

; --- compile-expr: do (milestone 87, documents/lisp_package_system.md) ---
; このLisp処理系にはどの経路にも末尾呼び出し最適化が無く(documents/lisp_vm.mdの
; 非ゴール)、Cコールスタックの深さがそのままイテレーション回数に比例してしまう
; ため、doは新規opcodeを追加せずifと同じジャンプ/ラベルの組み方(cond-code→
; jump-if-false else-label→then-code→jump end-label→label else-label→else-code→
; label end-label)を流用し、「else側」(end-testがfalseだった続行パス)の末尾で
; ループ先頭のラベルへ戻るジャンプを1つ追加するだけでC呼び出しを一切増やさない
; ループを組む。イテレーション回数に関わらずCスタックはO(1)のまま(milestone87の
; 目的そのもの)。
;
; bindingのスロットはlet同様OP_MAKE_LOCALで1度だけボックス化し、以後は解放も
; 再確保もしない(let/letの既存の単純化をそのまま継承)。初期化フェーズは
; let(の並行束縛semantics)と全く同じ処理で済むため、compile-let-process-bindings
; をそのまま再利用する(is_specialの退避/確定/復元もletと同型)。ただしletの
; binding-specは常に(var init)の2要素だが、CLのdo binding-specはvar単体/(var)/
; (var init)/(var init step)のいずれも許すため、compile-let-process-bindingsへ渡す
; 前に(var init nil-step)の3要素へ正規化しておく(init省略時はnil)
(defun compile-do-normalize-binding (binding)
  (cond
    ((atom binding) (list binding nil nil))
    ((null (cdr binding)) (list (car binding) nil nil))
    ((null (cdr (cdr binding))) (list (car binding) (car (cdr binding)) nil))
    (t binding)))

(defun compile-do-normalize-bindings (bindings)
  (if (null bindings)
      nil
      (cons (compile-do-normalize-binding (car bindings))
            (compile-do-normalize-bindings (cdr bindings)))))

(defun compile-do-binding-var (binding) (car binding))
(defun compile-do-binding-step (binding) (car (cdr (cdr binding))))
(defun compile-do-has-step-p (binding) (not (null (compile-do-binding-step binding))))

; 各イテレーションのstep-formは、letの並行束縛と同じく「どのstep-formも他の
; step-formが書き込んだ後の新しい値を見てはならない」。OP_STORE_LOCAL/OP_GLOBAL_SET
; はどちらも値をpushし直さない(値をpopするだけ)ため、「step-formを持つ全bindingの
; 値を先に(旧い値のまま)push-し尽くし、その後pushした順と逆順でstoreする」ことで
; 並行評価を実現する(store命令1つがちょうど1つの値をpopするので、逆順にすれば
; 各storeは自分に対応する値を正しく受け取る)。step-formを持たないbindingは
; push/storeのどちらも生成しない(スロットの値がそのまま次のイテレーションへ
; 持ち越される)
(defun compile-do-step-push (bindings ctx scope)
  (cond
    ((null bindings) nil)
    ((compile-do-has-step-p (car bindings))
     (compile-concat (compile-expr (compile-do-binding-step (car bindings)) ctx scope)
                     (compile-do-step-push (cdr bindings) ctx scope)))
    (t (compile-do-step-push (cdr bindings) ctx scope))))

(defun compile-do-store-one (binding ctx scope)
  (let ((sym (compile-do-binding-var binding)))
    (if (special-variable-p sym)
        (list (list *op-global-set* (compile-register-const ctx sym)))
        (list (list *op-store-local* (cdr (compile-env-find sym (scope-locals scope))))))))

(defun compile-do-step-store (bindings-reversed ctx scope)
  (cond
    ((null bindings-reversed) nil)
    ((compile-do-has-step-p (car bindings-reversed))
     (compile-concat (compile-do-store-one (car bindings-reversed) ctx scope)
                     (compile-do-step-store (cdr bindings-reversed) ctx scope)))
    (t (compile-do-step-store (cdr bindings-reversed) ctx scope))))

(defun compile-do-step-code (bindings ctx scope)
  (compile-concat (compile-do-step-push bindings ctx scope)
                  (compile-do-step-store (reverse bindings) ctx scope)))

; body-formはいずれも非tail位置の副作用目的の式(結果はresult-formsのみが返す)なので、
; progn-bodyと違い最後のformの値も含め全て評価直後にOP_POPで捨てる(1個評価→1個破棄
; が1組ずつ隣接するので、bodyが何個あってもこの往復1組あたりのスタック効果は0になり、
; ループ1周のスタック深さがloop-startへ戻るたびに常に同じ値へ揃う)
(defun compile-do-body-forms (forms ctx scope)
  (if (null forms)
      nil
      (compile-concat (compile-expr (car forms) ctx scope)
                      (list (list *op-pop*))
                      (compile-do-body-forms (cdr forms) ctx scope))))

; end-test/result-formsは(end-test . result-forms)という1つのリスト。result-formsは
; progn-bodyと同じ「最後の値だけ残す」semantics(CLのdoのresult-formsは複数書けて
; 最後の値がdo全体の値になる、result-forms省略時はnil)なのでcompile-progn-bodyを
; そのまま再利用する
(defun compile-do (form ctx scope)
  (let ((bindings (compile-do-normalize-bindings (car (cdr form))))
        (end-test (car (car (cdr (cdr form)))))
        (result-forms (cdr (car (cdr (cdr form)))))
        (body (cdr (cdr (cdr form)))))
    (let ((processed (compile-let-process-bindings bindings ctx scope)))
      (let ((bindings-ir (car processed))
            (locals-additions (car (cdr processed)))
            (commits (car (cdr (cdr processed))))
            (restores (car (cdr (cdr (cdr processed))))))
        (let ((new-scope (compile-let-add-locals locals-additions scope))
              (loop-start (gensym))
              (body-label (gensym))
              (loop-end (gensym)))
          (compile-concat
            bindings-ir
            (compile-let-restore-code commits ctx)
            (list (list 'label loop-start))
            (compile-expr end-test ctx new-scope)
            (list (list *op-jump-if-false* (list 'ref body-label)))
            (compile-progn-body result-forms ctx new-scope)
            (list (list *op-jump* (list 'ref loop-end)))
            (list (list 'label body-label))
            (compile-do-body-forms body ctx new-scope)
            (compile-do-step-code bindings ctx new-scope)
            (list (list *op-jump* (list 'ref loop-start)))
            (list (list 'label loop-end))
            (compile-let-restore-code restores ctx)))))))

; atomは(既にレキシカル束縛の有無を確認する)compile-variable-refに委ねる。
; cons/car/cdr/eq/+はインライン化された専用命令へ、それ以外の形式(carがsymbol
; でもlambda式でも構わない)はすべて汎用の関数呼び出しcompile-callへコンパイル
; する。これにより「未対応の形式」は無くなり、milestone42のt節に残っていた
; 明示的なnilフォールバックは不要になった
(defun compile-expr (form ctx scope)
  (cond
    ((atom form) (compile-variable-ref form ctx scope))
    ((eq (car form) 'quote) (compile-quote form ctx))
    ((eq (car form) 'quasiquote) (compile-quasiquote form ctx scope))
    ((eq (car form) 'if) (compile-if form ctx scope))
    ((eq (car form) 'let) (compile-let form ctx scope))
    ((eq (car form) 'let*) (compile-let-star form ctx scope))
    ((eq (car form) 'progn) (compile-progn form ctx scope))
    ((eq (car form) 'cond) (compile-cond form ctx scope))
    ((eq (car form) 'and) (compile-and form ctx scope))
    ((eq (car form) 'or) (compile-or form ctx scope))
    ((eq (car form) 'when) (compile-when form ctx scope))
    ((eq (car form) 'unless) (compile-unless form ctx scope))
    ((eq (car form) 'block) (compile-block form ctx scope))
    ((eq (car form) 'return-from) (compile-return-from form ctx scope))
    ((eq (car form) 'defvar) (compile-defvar form ctx scope))
    ((eq (car form) 'defparameter) (compile-defparameter form ctx scope))
    ((eq (car form) 'defun) (compile-defun form ctx scope))
    ((eq (car form) 'setq) (compile-setq form ctx scope))
    ((eq (car form) 'lambda) (compile-lambda form ctx scope))
    ((eq (car form) '+) (compile-add form ctx scope))
    ((eq (car form) 'cons) (compile-cons form ctx scope))
    ((eq (car form) 'car) (compile-car form ctx scope))
    ((eq (car form) 'cdr) (compile-cdr form ctx scope))
    ((eq (car form) 'eq) (compile-eq form ctx scope))
    ((eq (car form) 'do) (compile-do form ctx scope))
    (t (compile-call form ctx scope))))

; --- 統合検証: compile-and-run (milestone 46, documents/lisp_vm.md 目標2完了) ---
; compile-expr(Lisp側)が返すのはIR/データ(バイトコードは整数のリスト、定数プールは
; Lispの値のリストで、その中に未実体化のlambdaテンプレート(closure-template)が
; 混在しうる)であり、実際のLispClosureはまだ1つも構築されていない。C側に新設した
; vm-make-closure/vm-execの2ビルトイン(milestone35以降で用意済みのlisp_make_compiled/
; lisp_vm_exec/lisp_make_upvalue_descs/lisp_compiled_set_upvalue_descsの薄いラッパー)
; を使い、定数プールの中身を末端から順に実体化してからvm-make-closureで組み立てる

; consはatomの否定と同じ(atomはconsとnilの両方でtを返すため、nilはclosure-template-pの
; 対象にならないよう先にnilでないことを確認する必要はない: (car nil)を呼ぶ前に
; (not (atom c))がnilでconsであることを保証してから短絡評価するので安全)
(defun closure-template-p (c)
  (and (not (atom c)) (eq (car c) 'closure-template)))

; constants-list中の各要素を実体化する。closure-templateならvm-materialize-templateで
; 再帰的に実際のLispClosureへ、それ以外の値(数値・symbol・cons・文字列等)はそのまま返す
(defun vm-materialize-constants (constants)
  (if (null constants)
      nil
      (cons (vm-materialize-constant (car constants))
            (vm-materialize-constants (cdr constants)))))

(defun vm-materialize-constant (c)
  (if (closure-template-p c)
      (vm-materialize-template c)
      c))

; (closure-template nargs max-locals bytecode constants upvalue-descs)の構造から、
; constantsを先に(再帰的に)実体化してからvm-make-closureへ渡し、実際のLispClosureを1つ作る
(defun vm-materialize-template (template)
  (let ((nargs (car (cdr template)))
        (max-locals (car (cdr (cdr template))))
        (bytecode (car (cdr (cdr (cdr template)))))
        (constants (car (cdr (cdr (cdr (cdr template))))))
        (upvalue-descs (car (cdr (cdr (cdr (cdr (cdr template))))))))
    (vm-make-closure nargs max-locals bytecode (vm-materialize-constants constants) upvalue-descs)))

; exprをcompile-expr+assembleでトップレベル(nargs=0、upvalue無し)のbytecode/constants
; へコンパイルし、定数プールを実体化してvm-make-closureで実際のLispClosureを組み立て、
; vm-execで実行した結果を返す。目標1(milestone34-39)で手動バイトコードとして検証した
; ケース群を、compile-expr経由の同じS式から生成したバイトコードで再現するための
; 検証専用の橋渡し。compile-lambdaの本体コンパイルと同様、compile-expr自体は
; OP_RETURNを発行しないため、末尾に明示的に1つ追加する。
; compile-exprはマクロを一切知らないため、渡す前にmacroexpand-allでマクロを全て
; 展開しておく必要がある(milestone 50: これが抜けていた配線漏れの修正)
(defun compile-and-run (expr)
  (let ((ctx (compile-make-ctx))
        (scope (compile-make-top-scope)))
    (let ((bytecode (assemble (compile-concat (compile-expr (macroexpand-all expr) ctx scope)
                                               (list (list *op-return*))))))
      (vm-exec (vm-make-closure 0 (car (scope-next-slot-box scope)) bytecode
                                 (vm-materialize-constants (compile-ctx-constants ctx)) nil)))))

; milestone 58: 統一トップレベル評価ドライバの導入。stdlib.lisp/compiler.lispは
; （milestone63でファイルを分割した後も）compile-expr（このファイル内で上で定義済み）
; より前に置かれたdefvar形式のopcode定数などを含む。lisp_eval_toplevel（src/lisp.c）が
; コンパイル経路を使い始めるのはstdlib.lisp・このファイルの読み込みが両方とも最後まで
; 終わった後でなければならないため、このファイルの本当に最後のフォームとしてここで
; フラグを立てる。これより後にこのファイルへ新しいdefunを追加してはならない
; （追加する場合はこのフォームより前に置くこと）
(mark-compiler-ready)
