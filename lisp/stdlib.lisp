; milestone 29: 標準ライブラリの自己ホスティング化。
; cons/car/cdr・型判定・算術の核（+/-/<）・IOなど「本当に低レベルな操作」だけをC側の
; 組み込みに残し、それ以外はここでLisp自身で定義する。EfiMainが起動時に自動でこの
; ファイルをloadする（documents/bare_metal_lisp.md参照）。

(defun not (x) (eq x nil))
(defun null (x) (eq x nil))

; 仮引数リスト全体を単一のsymbolにする書き方(milestone29でlisp_env_bind_paramsに
; 追加したrest-arg機構)で可変長引数を受け取る。評価済みの実引数がそのままリストとして
; argsに束縛されるので、それをそのまま返すだけでCLのlistと同じ挙動になる
(defun list args args)

; CLのappendは可変長だが、このスコープでは2引数のみサポートする(明示的な範囲の限定)
(defun append (a b)
  (if (null a)
      b
      (cons (car a) (append (cdr a) b))))

(defun reverse (lst)
  (if (null lst)
      nil
      (append (reverse (cdr lst)) (cons (car lst) nil))))

(defun 1+ (x) (+ x 1))
(defun 1- (x) (- x 1))

; C側の算術の核として組み込んだ<だけを使い、比較演算子群の残りをすべてLispで導出する。
; CLと異なり2引数のみサポートする(明示的な範囲の限定)
(defun > (a b) (< b a))
(defun = (a b) (and (not (< a b)) (not (< b a))))
(defun <= (a b) (not (< b a)))
(defun >= (a b) (not (< a b)))
(defun /= (a b) (not (= a b)))

(defun zerop (n) (= n 0))
(defun plusp (n) (< 0 n))
(defun minusp (n) (< n 0))

(defun nth (n lst)
  (if (zerop n)
      (car lst)
      (nth (1- n) (cdr lst))))

; fnはローカル変数（仮引数）だが、lisp_evalの関数呼び出し分岐は呼び出し式の演算子位置を
; 常に汎用evalで評価するため、ローカル変数が指すクロージャをそのまま(fn ...)の形で
; 呼び出せる。新しいfuncall/applyプリミティブは不要（milestone29の調査で確認済み）
(defun mapcar (fn lst)
  (if (null lst)
      nil
      (cons (fn (car lst)) (mapcar fn (cdr lst)))))

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
      (eq op 'defvar) (eq op 'defparameter) (eq op 'defun) (eq op 'defmacro)))

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
;   (opcode n)              -- オペランドnをそのままバイト値として使う2byte命令
;                              (例: (OP_CONST 0)、(OP_LOAD_LOCAL 1))
;   (opcode (ref name))    -- オペランドがラベルnameの絶対バイト位置に解決される2byte命令
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

(defun asm-label-p (instr) (eq (car instr) 'label))

; ラベル定義は0byte、オペランド無し命令は1byte、オペランド有り命令は2byte
(defun asm-instr-length (instr)
  (if (null (cdr instr)) 1 2))

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

; 1命令分のバイト列を返す(ラベル定義はnil、すなわち0byte)
(defun asm-emit-instr (instr labels)
  (if (asm-label-p instr)
      nil
      (if (null (cdr instr))
          (list (car instr))
          (list (car instr) (asm-resolve-operand (car (cdr instr)) labels)))))

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
; ここへフォールバックするため、ここに来るのは「レキシカルに束縛されていない
; symbol(グローバル/動的変数の参照は本ロードマップのスコープ外)」と
; 数値・nil・t・文字列などの本来のリテラルのみになる
(defun compile-literal (form ctx)
  (list (list *op-const* (compile-register-const ctx form))))

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
; 変数の数」(compile-env-length)で求まる
(defun compile-env-length (env)
  (if (null env)
      0
      (+ 1 (compile-env-length (cdr env)))))

; 見つからなければnilを返す安全な検索。レキシカルに束縛されていないsymbolは
; compile-literalへフォールバックしてそのまま自己評価的な定数として扱う
; (milestone42で残していた既知の制約はレキシカル変数についてはここで解消される。
; グローバル/動的変数への正規の対応は依然スコープ外)
(defun compile-env-find (name env)
  (if (null env)
      nil
      (if (eq (car (car env)) name)
          (car env)
          (compile-env-find name (cdr env)))))

; setqの対象は必ずレキシカルに束縛されている前提のため、見つからない場合は
; asm-label-offset(milestone41)と同じ流儀で(car nil)へ落ちて自然にpanicする
; (グローバル変数へのsetqは未対応であることを示すため、防御的なチェックを
; 追加しない)
(defun compile-env-slot (name env)
  (if (eq (car (car env)) name)
      (cdr (car env))
      (compile-env-slot name (cdr env))))

(defun compile-variable-ref (form ctx env)
  (let ((binding (compile-env-find form env)))
    (if binding
        (list (list *op-load-local* (cdr binding)))
        (compile-literal form ctx))))

; letはlet*と異なり、全てのinit-formを束縛前の外側env(この関数の引数env、
; まだ拡張していないもの)で評価する(milestone17のlisp_eval同様、束縛同士が
; 互いを参照できない並行束縛)。各init-formのコードの直後にOP_MAKE_LOCALを
; 1つ発行することで、その結果をそのままボックス化して並行に確保していく
(defun compile-let-bindings (bindings ctx env)
  (if (null bindings)
      nil
      (compile-concat (compile-expr (car (cdr (car bindings))) ctx env)
                      (list (list *op-make-local*))
                      (compile-let-bindings (cdr bindings) ctx env))))

; bindingsの各変数名に、外側envの長さから始まる連番のスロット番号を割り当てた
; 新しいenvを返す(compile-let-bindingsがOP_MAKE_LOCALを発行する順序と一致させる
; ため、先頭の束縛から順に外側envの長さ、+1、+2...という番号を振る)
(defun compile-let-extend-env (bindings env)
  (if (null bindings)
      env
      (compile-let-extend-env
        (cdr bindings)
        (cons (cons (car (car bindings)) (compile-env-length env)) env))))

(defun compile-let (form ctx env)
  (let ((bindings (car (cdr form)))
        (body (car (cdr (cdr form)))))
    (compile-concat (compile-let-bindings bindings ctx env)
                    (compile-expr body ctx (compile-let-extend-env bindings env)))))

; setqは代入した値をそのまま式の値として返す(Common Lisp/既存lisp_evalと同じ)。
; OP_STORE_LOCALはストアと同時にスタック最上位をpopしてしまう(値を残さない)ため、
; ストア後にOP_LOAD_LOCALで同じスロットを読み直し、「常に1つの値をスタックに
; 残す」というcompile-exprの不変条件を既存の命令だけで満たす
(defun compile-setq (form ctx env)
  (let ((slot (compile-env-slot (car (cdr form)) env)))
    (compile-concat (compile-expr (car (cdr (cdr form))) ctx env)
                    (list (list *op-store-local* slot))
                    (list (list *op-load-local* slot)))))

; elseが省略された(if cond then)の場合は、既存のlisp_evalのif実装と同じく
; elseの値をnilとして扱う
(defun compile-if-else-code (form ctx env)
  (if (null (cdr (cdr (cdr form))))
      (compile-expr nil ctx env)
      (compile-expr (car (cdr (cdr (cdr form)))) ctx env)))

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
(defun compile-if (form ctx env)
  (let ((cond-code (compile-expr (car (cdr form)) ctx env))
        (then-code (compile-expr (car (cdr (cdr form))) ctx env))
        (else-code (compile-if-else-code form ctx env))
        (else-label (gensym))
        (end-label (gensym)))
    (compile-concat cond-code
                    (list (list *op-jump-if-false* (list 'ref else-label)))
                    then-code
                    (list (list *op-jump* (list 'ref end-label)))
                    (list (list 'label else-label))
                    else-code
                    (list (list 'label end-label)))))

; atomは(既にレキシカル束縛の有無を確認する)compile-variable-refに委ねる。
; quote/if/let/setq以外の形式(関数呼び出し・lambdaなど)はmilestone44以降で
; 対応するため、現時点では未対応であることを明示するためにt節でnilを返す
(defun compile-expr (form ctx env)
  (cond
    ((atom form) (compile-variable-ref form ctx env))
    ((eq (car form) 'quote) (compile-quote form ctx))
    ((eq (car form) 'if) (compile-if form ctx env))
    ((eq (car form) 'let) (compile-let form ctx env))
    ((eq (car form) 'setq) (compile-setq form ctx env))
    (t nil)))
