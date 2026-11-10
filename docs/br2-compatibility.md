# BR2 互換性の静的判定

実機が無いため、コードが BR2 で通用するかを「依存の一覧」と「BR2 の記録値」で突き合わせる。断定はしない。実測できないものは未検証と書く。

## コードが固定している値

`charepa-apk/jni/charepa.c` から抽出した。

| 依存 | 値 | 由来 |
| --- | --- | --- |
| `PAGE_OFFSET_V` | `0xffffffc000000000` | arm64 の線形マップ開始。カーネル版に依存せず、両機種で同じ |
| `MEMSTART` | `0x01000000` | BS の `System RAM` 開始。`read_iomem()` が実行時に読む |
| `CRED_STEP` | 192 | `struct cred` の確保単位（kmalloc-192） |
| `ALPHABET` | 192 (`0xc0`) | 無印プロセスの `cap_bset.cap[0]` |
| `ALPHA_FD` | 192 | 手がかり値を fd に置くための番号 |
| `CLASS_FD` | 4 | `SECCLASS_CAPABILITY` |
| `ION_SLOT` | 3000 | `/dev/ion` を退避する fd |
| cred オフセット | `+4, +40, +48, +56, +64, +68` | 3.10 の `struct cred` 配置 |

## cred オフセットが BR2 で変わる条件

`struct cred` はカーネル版とビルド設定で決まる。3.10.33 のまま `#2` を再ビルドしても配置は変わらない。変わるのはカーネル版が上がった場合か、`CONFIG_` で `struct cred` に条件付きフィールドが増減した場合。

BR2 の記録は `3.10.33 #2 SMP PREEMPT`、ビルド日 2019-11-25。BS は同じ `3.10.33 #2`、2019-05-31。**カーネル版が同じなら `struct cred` の配置も同じ**というのが arm64 3.10 の一般的な性質で、これは推定であって実測ではない。

## モジュールが要求するシンボル

```
commit_creds  prepare_kernel_cred  selinux_enforcing
kallsyms_lookup_name  printk  proc_create_data  remove_proc_entry
```

すべて `kallsyms_lookup_name()` 経由で解決する。ハードコードされたアドレスは無い。`CONFIG_KALLSYMS_ALL=y` が BR2 でも有効なら解決できる。

BS で輸出を実測したのは 4 件（`proc_create_data`、`kallsyms_lookup_name`、`printk`、`remove_proc_entry`）。残る 3 件（`commit_creds`、`prepare_kernel_cred`、`selinux_enforcing`）は `kallsyms_lookup_name()` に渡す文字列で、**輸出の有無は BS でも未測定**。ただし BS でモジュールが実際にロードされ `init_module rc=0` を返しているので、この 3 件が解決できたことは動作から間接的に分かる。解決に失敗すれば `become_root()` が `-ENOENT` を返し、モジュールの `myroot_init()` が失敗してロード自体が拒否される。

## vermagic

```
vermagic=3.10.33 SMP preempt mod_unload aarch64
```

ビルド日時を含まない。BR2 が 2019-11-25 ビルドでも、カーネル版・SMP・preempt・モジュールアンロード可否・アーキが同じなら一致する。バージョン CRC（`__versions` セクション）を使っていないので、シンボルの CRC 照合も不要。

## 実行時に解決している箇所

| 対象 | 解決方法 |
| --- | --- |
| RAM 範囲 | `/proc/iomem` の `System RAM` 行 |
| system ブロックデバイス | `soc.1/by-name/system` と `by-name/system` を順に試す |
| フック先 | `/system/bin/debuggerd`（`init.rc` で `class main`、root 起動） |

## 未検証のまま残るもの

- BR2 の `ro.hardware` —— `pxa1928` なら `init.pxa1928.rc` が同じ。未測定
- BR2 の `/proc/iomem` の `System RAM` 範囲 —— 未測定
- BR2 の `by-name` パス —— 未測定
- BR2 の `init.rc` の `debuggerd` stanza —— 未測定
- **フック先が BR2 では別物だった可能性。** 以前の BR2 の記録では永続化に
  `/system/bin/calibinit.sh`（`class main`、seclabel なし、`file_contexts` にエントリなし）
  を使っており、成功している。現在の実装は `/system/bin/debuggerd` を差し替える。
  BS の `init.rc` に `calibinit` は 1 件も無く（grep 0 件）、`debuggerd` が `class main` で
  動いていることを実測したためこの選択だが、**BR2 で `debuggerd` が同じ位置にあるかは
  未測定**。BR2 で `validate` とロードまで通ってフックだけ入らない場合は、まずここを見る。
- `commit_creds` / `prepare_kernel_cred` / `selinux_enforcing` の輸出 —— BS では間接的に確認（モジュールがロードされた）。BR2 では未測定
- `avc_node` の位置 —— 起動ごとに動く。BR2 では別途当たり待ちが必要

## 結論

**BR2 で通用する見込みは高いが、実測していない。** 固定値のうち実行時に解決していないのは `PAGE_OFFSET_V`、`CRED_STEP`、cred オフセット、fd 番号だけで、いずれもカーネル版とビルド設定に依存する。`0bb4:4e16`、PXA1928、Android 5.1、`3.10.33 #2 SMP PREEMPT`、`ro.debuggable=0` が BR2 の記録と BS の実測で一致しているため、これらが揃う限り通る。

`charepa-apk/tools/br2_verify.sh` が 5 段階を実測する。最初の `validate` は副作用が無く、ここで落ちれば固定値の前提が崩れていると分かる。
