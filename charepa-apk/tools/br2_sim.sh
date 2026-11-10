#!/usr/bin/env bash
# 偽の adb で BR2 ランブックの制御フローを通しで確かめる。
#
# これはシミュレーションであり、実機の動作を保証しない。確かめるのは
#   1. 機種判定で TAB-A03-BR2 だけが通るか
#   2. validate → 初回 root ループ → フック確認 → 再起動 の順に進むか
#   3. ループが成功条件（端末上の .real の存在）で止まるか
# だけ。ハード側の事実は何も動かさない。
set -u

mkdir -p /tmp/br2sim/bin /tmp/br2sim/state
cp "$(dirname "$0")/fake_adb.sh" /tmp/br2sim/bin/adb
chmod +x /tmp/br2sim/bin/adb

rm -rf /tmp/br2sim/state && mkdir -p /tmp/br2sim/state

sed -e 's/sudo -n adb/adb/g' -e "s|APK=.*|APK=$(dirname "$0")/../charepa.apk|" \
  $(dirname "$0")/br2_verify.sh > /tmp/br2sim/br2_sim.sh
chmod +x /tmp/br2sim/br2_sim.sh

PATH=/tmp/br2sim/bin:$PATH bash /tmp/br2sim/br2_sim.sh > /tmp/sim3.txt 2>&1
rc=$?
echo "runbook exit=$rc"
echo
grep -aE 'model=|hook installed|RESULT|persistence|hook state' /tmp/sim3.txt | head -10
echo
echo "attempts by the fake device: $(cat /tmp/br2sim/state/attempts 2>/dev/null)"
echo "hook flag afterwards:        $(cat /tmp/br2sim/state/hook 2>/dev/null)"
echo
echo '== did it stop at the success condition, not at the 40 cap? =='
last=$(grep -aoE '^[0-9]+:' /tmp/sim3.txt | tail -1 | tr -d ':')
echo "last attempt line: $last (the cap is 40)"
