# BR2 互換性：静的判定と実測

2026-11-10 に TAB-A03-BR2 を実機接続して測った。以前の版は「実機が無いので記録値と突き合わせる」内容だったが、それが実測に置き換わっている。測定していない項目だけを未検証として残す。

## 実測した BR2 の値

```sh
getprop ro.product.model     TAB-A03-BR2
getprop ro.build.display.id  02.04.000 release-keys
getprop ro.hardware          pxa1928
getprop ro.serialno          0C8R236091
getprop ro.debuggable        0
getenforce                   Permissive（素の状態では Enforcing）
/proc/version                Linux version 3.10.33 (builduser@BuildPC)
                             (gcc 4.9.x-google 20140827) #2 SMP PREEMPT
                             Mon Nov 25 22:24:06 JST 2019
/proc/iomem                  System RAM 01000000-7fffffff
```

BS との差はビルド番号とカーネルのビルド日時だけ。`System RAM` の範囲は完全に一致した。

## コードが固定している値

`charepa-apk/jni/charepa.c` から抽出した。右列は BR2 で実測した結果。

| 依存 | 値 | BR2 での結果 |
| --- | --- | --- |
| `PAGE_OFFSET_V` | `0xffffffc000000000` | cred が見つかった（合致） |
| `MEMSTART` | `0x01000000` | `read_iomem()` が実行時に読む。BR2 も同じ値 |
| `CRED_STEP` | 192 | cred を 192 バイト単位で歩いて当たった |
| `ALPHABET` | 192 (`0xc0`) | ゲートが `word=192` で rc=0（合致） |
| `ALPHA_FD` | 192 | fd 表に収まった |
| `CLASS_FD` | 4 | `validate` で dma-buf が fd 4 になった |
| `ION_SLOT` | 3000 | `/dev/ion` を 3000 に退避できた |
| cred オフセット | `+4, +40, +48, +56, +64` | `cap_effective` への書き込みが効いた |

`struct cred` の配置はカーネル版とビルド設定で決まる。BR2 は `3.10.33 #2` で BS と版が同じなので一致した。これは推定ではなく、cred を見つけて実際に root が取れたことによる裏づけ。

## `validate` の実測

副作用のない 4 バイト書き込みの確認が、BR2 でそのまま通った。

```
ion fd=3000
iomem: System RAM 0x01000000..0x7fffffff
dma-buf fd=4 iova=0x80000000
gate(word=0)   rc=-1 errno=14  want -1
gate(word=192) rc=0 errno=0  want 0
word at +4: 0x80000000 (want 0x80000000)
VALIDATE PASS
```

## 初回 root の実測

以前の実行が残した `calibinit.sh` を `calibinit.sh.parked` に退避して再起動し、`Enforcing`・`/proc/myroot` なし・モジュール未ロードの状態から始めた。attempt 6 で通った。

```
cred scan probes=10106880 hits=123 granted=15 cred=0xffffffc07547e540
AVC win phys=0x73599540 off=1280 probes=280277 stores=209
after avc: module_errno=8 (8 = capability check passed)
init_module rc=0 errno=0 (156576 bytes)
hook: target=/system/bin/debuggerd (real=/system/bin/debuggerd.real)
hook: /system/bin/debuggerd.real relabelled to u:object_r:debuggerd_exec:s0
hook: /system/bin/debuggerd relabelled to u:object_r:system_file:s0
RESULT ROOT uid=0 (persistent)
```

`stores=209` は BS の実測（392〜2415）より少なく、AVC ノードが早く見つかった。

## 永続化の実測

再起動 2 回とも、payload が init ドメインで走って root を戻した。

```
CHAREPA_BOOT: context=u:r:init:s0
CHAREPA_BOOT: init_module rc=0 errno=0 (Success) bytes=156576
CHAREPA_BOOT: write /proc/myroot rc=1 errno=0 uid now 0
```

ラベルも維持される。

```
debuggerd       u:object_r:system_file:s0      713240
debuggerd.real  u:object_r:debuggerd_exec:s0    25820
```

アプリを削除しても戻る。`pm uninstall com.charepa` の後に再起動して `uid=0(root)`。

## モジュールが要求するシンボル

```
commit_creds  prepare_kernel_cred  selinux_enforcing
kallsyms_lookup_name  printk  proc_create_data  remove_proc_entry
```

すべて `kallsyms_lookup_name()` 経由で解決する。ハードコードされたアドレスは無い。BR2 で `init_module rc=0` が返ったので、7 件とも解決できたことが動作から分かる。`become_root()` が `-ENOENT` を返せば `myroot_init()` が失敗してロードが拒否されるため、ロード成功は解決成功の証拠になる。

## vermagic

```
vermagic=3.10.33 SMP preempt mod_unload aarch64
```

ビルド日時を含まない。BR2 のカーネルは 2019-11-25 ビルドだが、版・SMP・preempt・アンロード可否・アーキが同じなので一致した。`__versions` セクションを使っていないので CRC 照合も不要。

## 実行時に解決している箇所

| 対象 | 解決方法 | BR2 での結果 |
| --- | --- | --- |
| RAM 範囲 | `/proc/iomem` の `System RAM` 行 | BS と同じ |
| system ブロックデバイス | `soc.1/by-name/system` と `by-name/system` を順に試す | `soc.1/by-name/system` で開けた |
| フック先 | `/init.rc` と `/init.${ro.hardware}.rc` を読み、`class main` のサービスを選ぶ | `debuggerd` を選んだ |

## 実測で判明した差分

`calibinit` のサービス定義は `/init.rc` ではなく **`/init.pxa1928.rc`** にあった。

```
service calibinit /system/bin/calibinit.sh
    class main
    oneshot
```

`/init.rc` は `import /init.${ro.hardware}.rc` でこれを読む。以前のコードは `/init.rc` だけを読んでいたので、この個体が実際に使っていたフックを見落としていた。現在は両方を読む。BR2 では `debuggerd` も `class main` なので、この版では `debuggerd` が選ばれた。

## 未検証のまま残るもの

- `avc_node` の位置 —— 起動ごとに動く。BR2 では attempt 6 で当たったが、再起動ごとに当たり待ちが必要
- 日本語版 BR2（型番・ビルド番号が違う個体）—— 未接続
- `calibinit.sh` を使う経路 —— 退避したため、この版では未使用。BS の `init.rc` に `calibinit` が無いので、この経路は実機で一度も走っていない
- アプリ削除後に `pm install` し直さずに再 root する経路 —— 未実施（フックは `/system` に残るので不要）

## 結論

**BR2 で root が取れ、再起動を越えて永続化することを実測した。** BS と同じ APK 1 個で通る。`validate` も同じく PASS。

`charepa-apk/tools/br2_verify.sh` は 5 段階を実測する。最初の `validate` は副作用が無く、ここで落ちれば固定値の前提が崩れていると分かる。
