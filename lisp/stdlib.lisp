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
; 変数の数」(compile-env-length)で求まる。milestone44以降もこの関数はローカル
; スロット用alistだけでなくupvalue捕捉記録用alistの探索・計数にも再利用する
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

; --- compile-expr: lambdaとクロージャ捕捉 (milestone 44, documents/lisp_vm.md 目標2) ---
; milestone43までのenv(単純な(symbol . slot)のalist)を、lambdaの境界をまたいで
; 自由変数を解決できる「スコープ」構造に拡張する。1つのスコープは
;   (locals captures . enclosing)
; の3要素からなる:
;   locals    -- このスコープ(関数)自身のローカル変数alist。milestone43のenvと同じ形
;   captures  -- このスコープがさらに外側から捕捉したupvalueの記録(下記compile-make-captures)
;   enclosing -- 直接囲む関数のスコープ、トップレベル(囲むlambdaが無い)ならnil
; トップレベルのcompile-expr呼び出しもnilではなく実体を持つスコープを渡す
; (compile-make-top-scope)必要がある。これはcompile-resolveがenclosingの有無だけで
; 「もうこれ以上外側を辿れない」を判定し、scopeそのものをnilにはしないという設計の
; ため(scope-locals等をnilに対して呼ぶと(car nil)でpanicしてしまう)
(defun compile-make-scope (locals captures enclosing)
  (cons locals (cons captures enclosing)))

(defun scope-locals (scope) (car scope))
(defun scope-captures (scope) (car (cdr scope)))
(defun scope-enclosing (scope) (cdr (cdr scope)))

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

(defun compile-make-top-scope () (compile-make-scope nil (compile-make-captures) nil))

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

(defun compile-variable-ref (form ctx scope)
  (let ((resolved (compile-resolve form scope)))
    (if resolved
        (list (list (compile-load-op resolved) (cdr resolved)))
        (compile-literal form ctx))))

; letはlet*と異なり、全てのinit-formを束縛前の外側scope(この関数の引数scope、
; まだ拡張していないもの)で評価する(milestone17のlisp_eval同様、束縛同士が
; 互いを参照できない並行束縛)。各init-formのコードの直後にOP_MAKE_LOCALを
; 1つ発行することで、その結果をそのままボックス化して並行に確保していく
(defun compile-let-bindings (bindings ctx scope)
  (if (null bindings)
      nil
      (compile-concat (compile-expr (car (cdr (car bindings))) ctx scope)
                      (list (list *op-make-local*))
                      (compile-let-bindings (cdr bindings) ctx scope))))

; bindingsの各変数名に、外側scopeのlocalsの長さから始まる連番のスロット番号を
; 割り当てた新しいscopeを返す(compile-let-bindingsがOP_MAKE_LOCALを発行する順序と
; 一致させるため、先頭の束縛から順に外側localsの長さ、+1、+2...という番号を振る)。
; letは関数境界をまたがないため、captures/enclosingは元のscopeのものをそのまま
; 引き継ぎ、locals部分だけを拡張する
(defun compile-let-extend-scope (bindings scope)
  (if (null bindings)
      scope
      (compile-let-extend-scope
        (cdr bindings)
        (compile-make-scope
          (cons (cons (car (car bindings)) (compile-env-length (scope-locals scope)))
                (scope-locals scope))
          (scope-captures scope)
          (scope-enclosing scope)))))

(defun compile-let (form ctx scope)
  (let ((bindings (car (cdr form)))
        (body (car (cdr (cdr form)))))
    (compile-concat (compile-let-bindings bindings ctx scope)
                    (compile-expr body ctx (compile-let-extend-scope bindings scope)))))

; setqは代入した値をそのまま式の値として返す(Common Lisp/既存lisp_evalと同じ)。
; OP_STORE_LOCAL/OP_STORE_UPVALUEはストアと同時にスタック最上位をpopしてしまう
; (値を残さない)ため、ストア後に対応するload命令で同じスロット/upvalue indexを
; 読み直し、「常に1つの値をスタックに残す」というcompile-exprの不変条件を既存の
; 命令だけで満たす。resolvedがnil(setqの対象がどのスコープにも見つからない、
; つまり未対応のグローバル変数へのsetq)の場合はcompile-store-opの(car nil)で
; 自然にpanicする(milestone43のcompile-env-slotと同じ「防御的チェックを追加せず
; let it panicする」方針を維持する)
(defun compile-setq (form ctx scope)
  (let ((resolved (compile-resolve (car (cdr form)) scope)))
    (compile-concat (compile-expr (car (cdr (cdr form))) ctx scope)
                    (list (list (compile-store-op resolved) (cdr resolved)))
                    (list (list (compile-load-op resolved) (cdr resolved))))))

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
; (nargs bytecode constants upvalue-descs)という素のLispリストにまとめ、それを
; ひとつの値として外側ctxの定数プールに登録するにとどめる。この入れ子リストを
; 実際のLispClosureテンプレートへ組み立てる処理はmilestone46の検証用ブリッジに
; 委ねる(milestone41の「バイト列への変換はここでは行わない」という制約と同じ理由)
(defun compile-lambda (form ctx scope)
  (let ((params (car (cdr form)))
        (body (car (cdr (cdr form)))))
    (let ((inner-ctx (compile-make-ctx)))
      (let ((inner-scope (compile-make-scope (compile-lambda-param-locals params 0)
                                              (compile-make-captures)
                                              scope)))
        (let ((body-code (assemble (compile-expr body inner-ctx inner-scope))))
          (let ((package (list (compile-env-length params)
                                body-code
                                (compile-ctx-constants inner-ctx)
                                (compile-ctx-constants (captures-desc-ctx (scope-captures inner-scope))))))
            (list (list *op-make-closure* (compile-register-const ctx package)))))))))

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

; atomは(既にレキシカル束縛の有無を確認する)compile-variable-refに委ねる。
; cons/car/cdr/eqはインライン化された専用命令へ、それ以外の形式(carがsymbol
; でもlambda式でも構わない)はすべて汎用の関数呼び出しcompile-callへコンパイル
; する。これにより「未対応の形式」は無くなり、milestone42のt節に残っていた
; 明示的なnilフォールバックは不要になった
(defun compile-expr (form ctx scope)
  (cond
    ((atom form) (compile-variable-ref form ctx scope))
    ((eq (car form) 'quote) (compile-quote form ctx))
    ((eq (car form) 'if) (compile-if form ctx scope))
    ((eq (car form) 'let) (compile-let form ctx scope))
    ((eq (car form) 'setq) (compile-setq form ctx scope))
    ((eq (car form) 'lambda) (compile-lambda form ctx scope))
    ((eq (car form) 'cons) (compile-cons form ctx scope))
    ((eq (car form) 'car) (compile-car form ctx scope))
    ((eq (car form) 'cdr) (compile-cdr form ctx scope))
    ((eq (car form) 'eq) (compile-eq form ctx scope))
    (t (compile-call form ctx scope))))
