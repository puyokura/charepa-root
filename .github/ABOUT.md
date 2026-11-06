# このリポジトリの位置づけ

利用者が所有する実機 1 台（チャレンジタッチ / チャレンジパッド2 **TAB-A03-BR2**、
Android 5.1、kernel 3.10.33、Marvell PXA1928）に対して、UART を使わず
ソフトウェアだけで永続 root を取得した記録と、そのときに実際に動かしたコード。

対象は自分が所有する端末で、作業中に端末は何度もカーネルパニックを起こしたが、
そのたびに物理操作で復旧し、復旧経路（recovery が permissive + root adb、
`misc` への書き込み）は事前に実測して確保してあった。

## 何を含めているか

| パス | 内容 |
| --- | --- |
| `docs/charepa-root-full-story.md` | 全過程記録。19 章。判断・実測・失敗・撤回（45 件）・教訓・付録 |
| `docs/charepa-root-source-code.md` | 本文が参照するソースの全文。`src/` の内容を埋め込み、SHA-256 付き |
| `docs/README.md` | 文書の読み方、章立て、記述の約束 |
| `src/tools/` | 実機・QEMU で使ったツール 16 本 |
| `src/module/` | root 化カーネルモジュールのソースと Makefile |
| `src/prebuilt/myroot.ko` | 本セッションでビルドしたモジュール（156,576 バイト） |
| `Makefile` | `make tools` / `make module KDIR=...` |
| `LICENSE` | MIT |

含めていないもの: 実験の生データ（`k1`〜`k178` の 168 本、`b*` の 81 本、
ログ、33MB のカーネルリーク、パニックダンプ）、他者の公開物から取得した調査メモ。
これらはローカルにだけ置いてある。

## 対象端末の防御状態（実測）

| 項目 | 値 |
| --- | --- |
| ビルド | `user` / `release-keys`、`ro.secure=1`、`ro.debuggable=0` |
| SELinux | Enforcing（`u:r:shell:s0`） |
| 初期権限 | uid 2000（`shell`）、`CapEff=0` |
| KASLR | 無効（`CONFIG_RANDOMIZE_BASE` が存在しない） |
| モジュール署名 | 無し（`CONFIG_MODULE_SIG` が存在しない） |
| 検証ブート | 無し（dm-verity も AVB も無い） |
| `panic_on_oops` / `panic` | 1 / 1 |
| オンライン CPU | cpu0 のみ |

## 到達点（2 回連続の再起動で実測）

```text
uid=0 euid=0 gid=0
Uid:    0 0 0 0
CapEff: 0000001fffffffff
enforce = 0
myroot 2330 0 - Live
  /proc/kmsg                             OPEN
  /dev/block/mmcblk0                     OPEN
  /data/misc/keystore/user_0             OPEN
  mount tmpfs: OK
  chown to root: OK
```

## 再現に必要なもの

- aarch64 クロスコンパイラ（`aarch64-linux-gnu-gcc`、`-static` でリンク）
- ベンダーの GPL カーネルツリー（3.10.33、`.config` / `Module.symvers` を含む）— モジュール用
- 対象端末への adb 接続
- 復旧のための物理アクセス（電源ボタン）。UART は不要

## しないこと

- 第三者が管理する端末への適用を想定していない。対象は自分の実機 1 台だけ。
- `k138`（ゲートが書き込みを伴うことの自己証明）は端末を落とす前提の計器なので、
  読むだけで実行しない。
- `broot3` は QEMU 用。実機では `readv` の再確保が安定しない。
