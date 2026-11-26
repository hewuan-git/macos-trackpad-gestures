#!/bin/bash
# 安装 trackpad-media 为登录自启的 LaunchAgent
# 用法: ./install.sh [闸门] [切歌方式] [横滑行程] [几指] [轻点]
#   闸门: audio(默认，有进程在送音频才生效) | media(只认 MediaRemote) | none(不判定)
#   切歌: key(默认，模拟硬件媒体键) | mr(只发 MediaRemote 命令) | both(两个都发)
#   行程: 0.04~0.9，越小越轻就能切歌，默认 0.10（0.22 是很用力划一整下）
#   几指: 2~5，默认 3；系统的"N 指轻扫"是 N 指及以上都算，要和系统错开
#   轻点: on(默认，N 指轻点=播放/暂停) | off
set -euo pipefail

GATE="${1:-audio}"
TRACK="${2:-key}"
SWIPE="${3:-0.10}"
FINGERS="${4:-3}"
TAP="${5:-on}"
case "$GATE" in media|audio|none) ;; *) echo "闸门只能是 media|audio|none"; exit 2 ;; esac
case "$TRACK" in mr|key|both) ;; *) echo "切歌只能是 mr|key|both"; exit 2 ;; esac
case "$SWIPE" in *.*) ;; *) echo "行程要写成小数，比如 0.08"; exit 2 ;; esac
case "$FINGERS" in 2|3|4|5) ;; *) echo "几指只能是 2|3|4|5"; exit 2 ;; esac
case "$TAP" in on|off) ;; *) echo "轻点只能是 on|off"; exit 2 ;; esac

# 系统的"N 指轻扫"把 N 指及以上都算进去，先看看这一档还被系统占着没
case "$FINGERS" in 3) W=Three ;; 4) W=Four ;; *) W="" ;; esac
if [ -n "$W" ]; then
    keys="${W}FingerHorizSwipeGesture ${W}FingerVertSwipeGesture"
    if [ "$W" = Three ]; then
        keys="$keys TrackpadThreeFingerDrag"
        if [ "$TAP" = on ]; then keys="$keys TrackpadThreeFingerTapGesture"; fi
    fi
    for k in $keys; do
        v="$(defaults read com.apple.AppleMultitouchTrackpad "$k" 2>/dev/null || echo 0)"
        if [ "$v" != "0" ]; then echo "警告：系统里 $k = $v，$FINGERS 指这一档还被系统占着，会和手势撞车"; fi
    done
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$HOME/Library/Application Support/trackpad-media"

if ! command -v clang >/dev/null 2>&1; then
    echo "找不到 clang。先装命令行工具：xcode-select --install"
    exit 1
fi
APP="$DEST_DIR/TrackpadMedia.app"
BIN="$APP/Contents/MacOS/trackpad-media"
LABEL="com.user.trackpad-media"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$DEST_DIR/run.log"

mkdir -p "$APP/Contents/MacOS" "$HOME/Library/LaunchAgents"

# 源码没变就别重新编译：ad-hoc 签名绑的是二进制内容，重编一次，用户加的授权就默默作废了
STAMP="$DEST_DIR/build.stamp"
SUM="$(shasum -a 256 "$HERE/main.c" | cut -d' ' -f1)"
if [ -x "$BIN" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$SUM" ]; then
    echo "源码没变，沿用已签名的二进制（授权不会失效）"
else
    clang -O2 -fblocks -Wall -Wno-deprecated-declarations -o "$BIN" "$HERE/main.c" \
          -framework CoreFoundation -framework CoreAudio -framework IOKit -ldl

    cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>trackpad-media</string>
    <key>CFBundleIdentifier</key><string>com.local.trackpad-media</string>
    <key>CFBundleName</key><string>Trackpad Media</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.1</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF
    codesign -s - --force "$APP"
    echo "$SUM" > "$STAMP"
fi

# 早期版本留下的裸二进制，改名而不是删掉
[ -e "$DEST_DIR/trackpad-media" ] && mv "$DEST_DIR/trackpad-media" "$DEST_DIR/trackpad-media.old" 2>/dev/null || true

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>--run</string>
        <string>--gate</string>
        <string>$GATE</string>
        <string>--track</string>
        <string>$TRACK</string>
        <string>--swipe</string>
        <string>$SWIPE</string>
        <string>--fingers</string>
        <string>$FINGERS</string>
        <string>--tap</string>
        <string>$TAP</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
EOF

# bootout 之后紧接着 bootstrap 偶尔会报 error 5，重试几次
i=0
until launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null; do
    i=$((i+1))
    [ "$i" -ge 5 ] && { echo "启动任务装不上（launchctl bootstrap 连续失败），手动试: launchctl bootstrap gui/$(id -u) '$PLIST'"; exit 1; }
    sleep 2
done
sleep 2
PID="$(launchctl print "gui/$(id -u)/$LABEL" 2>/dev/null | sed -n 's/.*pid = \([0-9]*\).*/\1/p' | head -1)"
echo "--- 已安装：闸门=$GATE 切歌=$TRACK 横滑行程=$SWIPE 用=$FINGERS 指 轻点=$TAP  进程号=${PID:-未启动} ---"
echo
echo "正在用媒体键自检（会短暂降一格音量再还原）……"
if [ -n "$PID" ]; then
    kill -USR1 "$PID"
    sleep 6
    tail -6 "$LOG"
    echo
    echo "如果上面说“媒体键授权未通过”："
    echo "  打开 系统设置 → 隐私与安全性 → 设备控制和数据访问（旧系统叫辅助功能），找到 Trackpad Media 把开关打开；"
    echo "  也可以直接跳到这一页: open \"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility\""
    echo "  列表里没有就用 + 号添加（在文件框里按 ⇧⌘G 粘贴）：$APP"
    echo "  拨完开关要重启一次进程才认：launchctl kickstart -k gui/\$(id -u)/$LABEL"
    echo "  然后重跑自检：kill -USR1 \$(pgrep -f TrackpadMedia.app) && tail -4 $LOG"
fi
echo
echo "日志: $LOG"
echo "停止: ./stop.sh    重启: launchctl kickstart -k gui/\$(id -u)/$LABEL"
