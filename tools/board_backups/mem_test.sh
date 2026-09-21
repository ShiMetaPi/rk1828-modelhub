#!/bin/sh
# 模型内存/性能测试（参数化版）
# 用法: sh mem_test.sh <rknn> <weight> <tokenizer> <embed> [prompt]
# 流程与必记字段说明见 D:\hsx_workspace\rk1828\docs\README.md
OUT=/tmp/demo_run.log
DIR=/root/rknn_MiniCPM5_2B_demo   # demo 二进制 + lib 目录（固定）

RKNN=${1:?用法: sh mem_test.sh <rknn> <weight> <tokenizer> <embed> [prompt]}
WEIGHT=${2:?缺 weight 路径}
TOK=${3:?缺 tokenizer 路径}
EMB=${4:?缺 embedding 路径}
PROMPT=${5:-用两百字介绍一下长城}

devmem() { rknn-smi info 2>/dev/null | awk -F'|' '/RK1828/{split($5,a,"/"); gsub(/ /,"",a[1]); print a[1]}'; }

echo "===== 0. 参数 ====="
echo "rknn = $RKNN ($(du -m "$RKNN" 2>/dev/null | cut -f1)MB)"
echo "weight = $WEIGHT ($(du -m "$WEIGHT" 2>/dev/null | cut -f1)MB)"
echo "prompt = $PROMPT"

echo ""
echo "===== 1. 基线 ====="
free -m | awk 'NR==2{print "主机内存: used="$3"MB avail="$7"MB"}'
echo "NPU设备内存: $(devmem) / 5120 MB"

echo ""
echo "===== 2. 运行 demo（每2秒采样: t | 主机RSS(KB) | HWM(KB) | NPU(MB)）====="
cd $DIR || exit 1
LD_LIBRARY_PATH=./lib ./rknn_minicpm5_2b_demo \
  "$RKNN" "$WEIGHT" "$TOK" "$EMB" 0xff "$PROMPT" > $OUT 2>&1 &
PID=$!
echo "pid=$PID"

peak_rss=0; peak_dev=0; peak_hwm=0; t=0
while kill -0 $PID 2>/dev/null && [ $t -lt 240 ]; do
  sleep 2; t=$((t+2))
  rss=$(awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null); rss=${rss:-0}
  hwm=$(awk '/VmHWM/{print $2}' /proc/$PID/status 2>/dev/null); hwm=${hwm:-0}
  dev=$(devmem); dev=${dev:-0}
  [ "$rss" -gt "$peak_rss" ] 2>/dev/null && peak_rss=$rss
  [ "$hwm" -gt "$peak_hwm" ] 2>/dev/null && peak_hwm=$hwm
  [ "$dev" -gt "$peak_dev" ] 2>/dev/null && peak_dev=$dev
  echo "t=${t}s | RSS=$rss | HWM=$hwm | NPU=$dev"
done

echo ""
echo "===== 3. 进程退出后（内存应回落）====="
sleep 2
free -m | awk 'NR==2{print "主机内存: used="$3"MB avail="$7"MB"}'
echo "NPU设备内存: $(devmem) / 5120 MB"
echo "峰值汇总: 主机RSS峰值=${peak_rss}KB (VmHWM=${peak_hwm}KB)  NPU峰值=${peak_dev}MB"

echo ""
echo "===== 4. demo 运行日志关键行 ====="
grep -E 'vocab_info|Weight sync|Device Memory|Node [0-9]|Finished|Stop|Prefill|Generate|Error|error' $OUT | head -20
