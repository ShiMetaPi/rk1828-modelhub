#!/bin/sh
# Xorg 段错误看门狗（2026-09-21）
# 板子 modesetting 驱动偶发 crtc flip timeout → Xorg 段错误 → lightdm 弹回 greeter。
# 本脚本：greeter 在场且无用户会话连续 20 秒 → restart lightdm（autologin=root）自动回桌面。
# 停用：systemctl disable --now session-watchdog
CNT=/tmp/wd_greeter_cnt
echo 0 > $CNT 2>/dev/null
while true; do
  sleep 10
  if pgrep -f lightdm-gtk-greeter >/dev/null 2>&1 && ! pgrep -f xfce4-session >/dev/null 2>&1; then
    N=$(($(cat $CNT 2>/dev/null || echo 0) + 1))
    echo $N > $CNT
    if [ "$N" -ge 2 ]; then
      echo 0 > $CNT
      logger -t session-watchdog "greeter 挂场且无会话，restart lightdm 自动回桌面"
      systemctl restart lightdm
    fi
  else
    echo 0 > $CNT
  fi
done
