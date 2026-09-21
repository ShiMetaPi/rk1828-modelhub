#!/bin/sh
# vl_demo 引擎服务（Qwen2.5-VL 真后端）内存/性能测试 —— mem_test_vl.sh 的引擎变体。
# 前提：sh /root/vl_demo/start.sh（real 模式）已启动，模型加载完毕。
# 流程：基线采样 → 连发 3 次多轮 /ask（每次带新摄像头帧）→ 期间 2s 采样
#       引擎进程 RSS/HWM + NPU 设备内存 → 峰值汇总 + 每次回答的 stats。
OUT=/tmp/engine_mem_ask.log

devmem() { rknn-smi info 2>/dev/null | awk -F'|' '/RK1828/{split($5,a,"/"); gsub(/ /,"",a[1]); print a[1]}'; }

echo "===== 0. 前提检查 ====="
PID=$(pgrep -f "[v]l_demo/vl_engine" | head -1)
[ -z "$PID" ] && { echo "引擎未运行，先 sh /root/vl_demo/start.sh"; exit 1; }
echo "engine pid=$PID"

echo ""
echo "===== 1. 基线（空闲服务，模型已加载）====="
free -m | awk 'NR==2{print "主机内存: used="$3"MB avail="$7"MB"}'
echo "NPU设备内存: $(devmem) / 5120 MB"

echo ""
echo "===== 2. 连续 3 次多轮 /ask（每2秒采样: t | 主机RSS(KB) | HWM(KB) | NPU(MB)）====="
peak_rss=0; peak_dev=0; peak_hwm=0; t=0
(
  while :; do
    rss=$(awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null); rss=${rss:-0}
    hwm=$(awk '/VmHWM/{print $2}' /proc/$PID/status 2>/dev/null); hwm=${hwm:-0}
    dev=$(devmem); dev=${dev:-0}
    echo "t=${t}s | RSS=$rss | HWM=$hwm | NPU=$dev" >> $OUT.sampling
    sleep 2; t=$((t+2))
  done
) &
SAMPLER=$!
: > $OUT.sampling
: > $OUT
Q1="What do you see in the camera?"
Q2="What color is the main object?"
Q3="Summarize the scene in one sentence."
for i in 1 2 3; do
  eval Q=\$Q$i
  echo "--- ask $i: $Q ---" >> $OUT
  curl -s -m 90 -X POST http://127.0.0.1:8080/ask -d "$Q" >> $OUT 2>&1
  echo "" >> $OUT
done
sleep 2
kill $SAMPLER 2>/dev/null
sleep 1
tail -5 $OUT.sampling
echo ""
echo "===== 3. 峰值汇总 ====="
peak_rss=$(sed 's/.*RSS=//;s/ .*//' $OUT.sampling | sort -n | tail -1)
peak_hwm=$(sed 's/.*HWM=//;s/ .*//' $OUT.sampling | sort -n | tail -1)
peak_dev=$(sed 's/.*NPU=//;s/ .*//' $OUT.sampling | sort -n | tail -1)
echo "主机RSS峰值=${peak_rss}KB (VmHWM=${peak_hwm}KB)  NPU峰值=${peak_dev}MB"
free -m | awk 'NR==2{print "主机内存: used="$3"MB avail="$7"MB"}'
echo "NPU设备内存: $(devmem) / 5120 MB"

echo ""
echo "===== 4. 三次回答与 stats ====="
grep -E '--- ask|@@STATS' $OUT
