#!/bin/bash
# 停止并取消自启。不删除任何文件——程序、plist、日志都保留。
# 恢复只需: launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.user.trackpad-media.plist
set -uo pipefail

LABEL="com.user.trackpad-media"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null && echo "已停止 $LABEL" || echo "$LABEL 本来就没在跑"

if [ -f "$PLIST" ]; then
    mv "$PLIST" "$PLIST.disabled"
    echo "自启已关闭（plist 改名为 $PLIST.disabled，没有删除）"
fi

echo
echo "重新启用:"
echo "  mv '$PLIST.disabled' '$PLIST' && launchctl bootstrap gui/\$(id -u) '$PLIST'"
