# CommonLispラムダリストキーワード導入マイルストーン

## 目的

現在のLispインタプリタの仮引数リストは完全に位置ベースで、実引数の個数が仮引数の個数と厳密に
一致しない限り`lisp_panic`する（`lisp_env_bind_params`、コンパイル済みクロージャの`OP_CALL`/
`lisp_apply`も同様の厳密一致チェック、`lisp_vm_integration.md`マイルストーン37）。唯一の可変長引数の
書き方は、`bare_metal_lisp.md`マイルストーン29で導入された「仮引数リスト全体が単一のbare symbol」
という書き方（`(defun list args args)`）のみだった。

本ドキュメントは、CommonLispの`&optional`（デフォルト値+supplied-p変数）・`&rest`・`&key`
（`&optional`と同様にデフォルト値+supplied-p変数）・`&aux`・`&allow-other-keys`と同等のラムダリスト
キーワードを実装するマイルストーンを記録する。

マイルストーン番号は既存ロードマップの最新完了マイルストーン87に続く**89**とする（`src/main.c`に
誤って残っていた「(milestone 88)」というコメントは実際には登録されていない非公式な記述であり、
ユーザーの判断により88番との混同を避けて89番から開始する）。

## 設計方針

### 実装アプローチ: ツリーウォークインタプリタへの拡張に限定する

現在のコードベースには「コンパイラが対応できない可変長引数はツリーウォークへ丸ごとフォール
バックする」という確立された前例（milestone29のrest-arg、`lisp_defun_params_is_restarg`）がある。
本マイルストーンもこのパターンを踏襲し、**VM/バイトコード/`OP_CALL`には一切手を入れない**。

- `&optional`/`&key`のデフォルト値は「呼び出し時に、それまでに束縛済みの仮引数を参照できる環境で
  評価する」というCL標準の逐次評価が必要で、これは既存の`lisp_env_bind_params`（envを1引数ずつ
  拡張していく既存の実装）に自然に乗る。
- ツリーウォークの`lisp_apply`はコンパイル済みクロージャ・ビルトイン・インタプリタクロージャの
  いずれも同じ関数から呼べる（`OP_CALL`の非コンパイル済みフォールバックが既に`lisp_apply`に
  委譲している、`lisp_vm_integration.md`マイルストーン52/53）。したがって、新しいキーワードを使う
  `defun`/`lambda`がインタプリタクロージャのままであっても、コンパイル済みコードから問題なく
  呼び出せる（既存の`list`/`append`等がまさにこの経路で動いている）。
- VM側に新オペコードや可変長`OP_CALL`規約を追加する実装は、教育目的の本プロジェクトのスコープに
  対して過大であり、既存の「非対応形式はインタプリタへ逃がす」という設計判断と一致しない。

### 明記するスコープ外: コンパイル済みコードに直接ネストした`lambda`

トップレベルの`defun`がこれらのキーワードを使う場合は、`lisp_eval_toplevel`の判定
（`lisp_defun_params_needs_interpreter`、milestone29/60の`lisp_defun_params_is_restarg`を一般化）
を拡張し、丸ごとツリーウォークへフォールバックさせる。`defmacro`は元から常にツリーウォーク経由
なので変更不要。

しかし、**すでにコンパイルされている関数の内側に直接書かれた`(lambda (&optional x) ...)`のような
ネストした`lambda`式**は、`compile-lambda`（`lisp/compiler.lisp`）がコンパイルするためこの設計の
対象外になる。ここで単に無視すると`&optional`という語がそのまま1個目の仮引数名として扱われてしまう
危険な黒魔術（サイレントな誤動作）になるため、`compile-lambda`に「paramsに新キーワードシンボルが
1つでも含まれていたら`lisp_panic`する」という安全策を追加した（milestone29のrest-arg同様、この
制約自体をここに明記する）。

## 新しいラムダリスト文法

```
lambda-list    ::= required* [&optional opt-spec*] [&rest var]
                   [&key key-spec* [&allow-other-keys]] [&aux aux-spec*]
opt-spec       ::= var | (var) | (var default-form) | (var default-form supplied-p-var)
key-spec       ::= var | (var) | (var default-form) | (var default-form supplied-p-var)
                   ; キーワード名はvarの名前から:varに固定する(CLの((:kw var) ...)形の
                   ; リネームは対象外)
aux-spec       ::= var | (var init-form)
```

- 出現順序は上記の並びに固定し、順序違反・キーワードの重複は`lisp_panic`する（CLの緩い順序規則
  より厳格）。
- `default-form`/`init-form`はそれまでに束縛済みの引数を参照できる環境で逐次評価する。
- `&rest var`は`&optional`消費後に残っている実引数を丸ごとリストとして束縛する（実引数を消費
  しない。`&key`が同じ残り引数をさらにキーワード/値ペアとして解釈する対象になる、CLと同じ）。
- `&key`パースは残り引数を2個ずつ`:var`キーワードと値のペアとして走査し、未知のキーワードは
  `&allow-other-keys`が無ければ`lisp_panic`する。ペア数が奇数（値の欠落）も`lisp_panic`する。
- `&rest`も`&key`も無い状態で実引数が余れば、既存どおり`too many arguments`で`lisp_panic`する。

## ファイル構成

新規ソースファイルは追加しない。既存の3ファイル構成（`src/uefi.h`／`src/lisp.h`+`src/lisp.c`／
`src/main.c`）と`lisp/compiler.lisp`・`lisp/stdlib.lisp`をそのまま使う。検証用フィクスチャは新規の
`test/lisp/test-lambda-list-keywords.lisp`を追加する。

## マイルストーン一覧

| # | マイルストーン | 状態 | 主な内容 |
|---|---|---|---|
| 89 | `&optional`/`&rest`/`&key`/`&aux`/`&allow-other-keys`の実装 | 完了 | `src/lisp.c`に5キーワード用のシンボルを他の特殊形式シンボルと同様に`lisp_symbols_init`で1度だけinternしキャッシュし、`lisp_env_bind_params`を上記文法全体を解釈するよう書き直した（`lisp_env_bind_optional`/`lisp_env_bind_rest`/`lisp_env_bind_key`/`lisp_env_bind_aux`/`lisp_lambda_parse_var_spec`の5つの静的ヘルパーに分割）。`lisp_defun_params_is_restarg`を`lisp_defun_params_needs_interpreter`へ一般化し、paramsが「bare symbol」または「5キーワードのいずれかを含む」場合に真を返すようにした（`lisp_eval_toplevel`はこの判定を使い続ける）。`lisp/compiler.lisp`の`compile-lambda`に、paramsが5キーワードのいずれかを含む場合に専用ビルトイン`%panic-compiled-lambda-list-keyword`を呼んで`lisp_panic`する安全策を追加した。新規`test/lisp/test-lambda-list-keywords.lisp`（10項目: `&optional`のデフォルト値・supplied-p、`&rest`単独、`&rest`と`&optional`の併用、`&key`のデフォルト値・supplied-p、`&rest`と`&key`の併用、`&aux`の逐次初期化、`&allow-other-keys`での未知キーワード許容、コンパイル済みコードから新キーワード付き関数を呼び出せることの回帰確認）を追加し、`make build`・`make test`（全23フィクスチャ）で回帰が無いことを確認した。異常終了系（`&key`の後に`&optional`という順序違反・必須引数不足・`&allow-other-keys`無しでの未知キーワード・コンパイル済みコード内ネストlambdaでのpanic、計5シナリオ）は`make test`では検出できないため、個別の対話REPLセッション（QEMUシリアル経由）でいずれも意図したメッセージで`lisp_panic`し、panic後もREPLが正常復帰し以降の評価に影響しないことを確認した。 |

## スコープ外として明記する項目

以下は本マイルストーンの範囲を超えるため対象外とする:

- すでにコンパイルされている関数の内側に直接書かれたネストした`lambda`式での新キーワードの使用
  （`compile-lambda`が検出時に`lisp_panic`する。トップレベルの`defun`はツリーウォークへフォール
  バックするため対象外にならない）。
- `&key`のCL標準の`((:kw var) ...)`リネーム構文（キーワード名はvarの名前から`:var`に固定）。
- VM/バイトコード側での新オペコード追加・可変長`OP_CALL`呼び出し規約の拡張。新キーワードを使う
  関数は常にツリーウォークインタプリタのクロージャのまま扱う。
- CL標準のより緩いラムダリストキーワード出現順序規則。本実装は
  `required* &optional &rest &key[&allow-other-keys] &aux`の固定順序のみを認め、順序違反は
  `lisp_panic`する。

## 検証方針

`make build`でクロスコンパイルが通ることと、`make test`（`test/lisp/`配下の全フィクスチャを
QEMU/OVMFヘッドレスで実行するハーネス）に既存回帰が無いことを確認する。加えて、`make test`単体
では検出できない異常終了系シナリオ（順序違反・必須引数不足・未知キーワード・コンパイル済みコード
内ネストlambdaでのpanic）は、標準の方針どおり個別に対話REPLセッションでQEMU実行し、panic後も
REPLが正常復帰することを確認してから完了とする。
