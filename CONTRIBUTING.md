# 文書とコードの保守

このリポジトリは 2 種類の成果物を持つ。**文書**（`docs/`）と**コード**（`src/`）。
片方を変えたら、もう片方との整合を確認する。

## 1. コードを変えたとき

1. `src/tools/*.c` を直す
2. `make tools` で全 16 本をビルドし、`file build/<名前>` が
   `ELF 64-bit LSB executable, ARM aarch64, statically linked` になることを確認する
3. `docs/charepa-root-source-code.md` を作り直す（`src/` の内容を埋め込む）
4. 本文（`docs/charepa-root-full-story.md`）が該当ファイルの挙動を説明していれば、
   その記述も直す

ソース付録は生成物なので、手で編集しない。`src/` が正で、付録は写しである。

## 2. モジュールを変えたとき

1. `src/module/myroot.c` を直す
2. ベンダーツリーを用意して `make module KDIR="$KDIR"`
3. `strings -a src/module/myroot.ko | grep vermagic` が
   `vermagic=3.10.33 SMP preempt mod_unload aarch64` を返すことを確認する
4. `aarch64-linux-gnu-nm -u src/module/myroot.ko` の未定義シンボルが
   `kallsyms_lookup_name` / `printk` / `proc_create_data` / `remove_proc_entry` の
   4 つだけであることを確認する
5. 実機でロードし、`/proc/myroot` と `enforce=0` を確認する
6. `src/prebuilt/myroot.ko` を差し替える

**差し替えを忘れると、配布物とソースが食い違う。** リポジトリの `src/prebuilt/` は
「本セッションで実際に動かしたもの」を意味するので、ビルドし直したら必ず更新する。

## 3. 文書を変えたとき

1. `docs/charepa-root-full-story.md` を直す
2. 章番号を変えたら、本文中の `第 N 章` の参照を全部追う
3. 撤回を追加したら、第 15 章の表に 1 行足し、冒頭の件数（45 件）も直す
4. 実測値を足したら、第 17 章にも反映する
5. コードフェンスの数を数え、偶数であることを確認する（閉じ忘れの検出）

```sh
grep -c '^```' docs/charepa-root-full-story.md   # 偶数なら閉じている
```

## 4. 公開してはいけないものを入れない

- 端末のシリアル（本文では `ADB-SERIAL`、スクリプトは `ADB_SERIAL` 環境変数で受け取る）
- ホストの接続先・IP・ユーザー名（手順は `$KDIR` などのプレースホルダで書く）
- 端末から吸い出したカーネルメモリ（`leak.bin`、パニックダンプ）
- 他者の公開物から取得した調査メモ（`research/`）

`.gitignore` が `archive/`、`experiments/`、`research/`、`*.log`、`*.bin` を除外する。
`git status` にこれらが出てきたら、`.gitignore` が効いていない。

## 5. 記述の約束

- 数値は 16 進または実測単位のまま書く。丸めない。
- 推測は「推定」と明記する。実測とソース確認と混ぜない。
- 撤回した主張は消さず、第 15 章に残す。**失敗の記録の方が再利用価値が高い。**
- 戻り値を状態の証拠として書かない。カーネルが書いた側のオブジェクトで判定する。
