# Mac 手势滑动切歌（实现 Windows 触控板原生手势功能）

**简体中文** · [English](README.md)

在 macOS 上用触摸板多指手势控制音乐：左右滑切歌（上一首 / 下一首）、上下滑调音量、轻点播放/暂停。也就是"Windows 触摸板默认就会、Mac 上要找第三方工具"的那套动作。

一个 C 文件，零依赖：只需要命令行工具里的 clang（`xcode-select --install`），不需要完整 Xcode、不需要任何常驻 GUI App。编译出来是个 70 KB 的裸二进制，注册成登录自启的 LaunchAgent。装法两条：clone 下来跑一条 `install.sh`，或者交给 Homebrew 用 `brew install`。

仓库名是 `macos-trackpad-gestures`（GitHub 的仓库名只收 ASCII，所以用英文关键词，方便别人搜到）；程序、命令、二进制和 Homebrew 的 formula 都仍叫 `trackpad-media`。

## 三步跑起来

```sh
brew tap hewuan-git/tap && brew install trackpad-media
trackpad-media-setup none key 0.10 3 on     # 闸门 / 切歌 / 行程 / 几指 / 轻点，含义见下面「安装」
```

不想装 Homebrew 就 clone：

```sh
git clone https://github.com/hewuan-git/macos-trackpad-gestures.git && cd trackpad-media
./install.sh none key 0.10 3 on
```

最后手动授权一次（这一步只是把设置页打开）：

```sh
open "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"
```

开关还得你自己拨——Apple 不允许任何安装器预先拿到媒体键权限（详见下文「必须手动授权一次」）。**调音量不需要授权**，所以就算还没授权，上下滑已经能用了，只是左右滑和轻点没反应。

上手前请先看「一定要先把系统手势挪开」，这是最容易踩的坑。

**请用 v1.1 或更新。** v1.0 睡一觉之后触摸板订阅会失效：进程还在、授权也仍然显示"已允许"，但再也收不到手指数据，所以用几天手势就静默失灵，而且哪里都不报错。升级就好（`brew upgrade trackpad-media`，或重跑 `./install.sh`）；[v1.1 的 Release 说明](https://github.com/hewuan-git/macos-trackpad-gestures/releases/tag/v1.1)写了这件事是怎么排查出来的。只要重新编译，授权就会作废一次，需要重新添加——见下面「必须手动授权一次」。

## 为什么自己写

Windows 上这套手势是现成的——精确式触摸板和 ASUS / Lenovo 这类厂商驱动里，多指左右滑切歌、上下滑调音量、点一下播放暂停都默认就有（各家默认指头数和方向不完全一样），用惯了回到 Mac 上就很别扭。Mac 端系统只提供"三指/四指轻扫切换全屏 App、调度中心"这类窗口手势，媒体控制得靠第三方；试过的几个要么收费、要么得挂一个带界面的 App、要么要装 Lua 运行时，都不合用，就自己写了一个。

### 和现成方案的区别

| 方案 | 要额外装什么 | 我没选它的原因 |
|---|---|---|
| macOS 自带的触摸板手势 | 无 | 只有窗口类手势（切换全屏 App、调度中心），没有媒体类手势，也改不成媒体 |
| 收费手势工具（BetterTouchTool 这类） | 一个常驻的图形界面 App | 只为切歌调音量挂一个带界面的 App，太重 |
| 脚本类工具（Hammerspoon 这类） | Lua 运行时 + 常驻 App | 能写，但为了三根手指装一整套运行时不划算 |
| **本项目** | 只有命令行里的 clang | 70 KB 裸二进制 + 一个 LaunchAgent，没有界面、没有常驻进程以外的东西 |

## 手势

默认三指（可以改成二到五指）：

| 动作 | 效果 | 走哪条通路 |
|---|---|---|
| 向左滑 | 上一首 | 模拟硬件媒体键 |
| 向右滑 | 下一首 | 模拟硬件媒体键 |
| 向上滑 | 音量增大 | CoreAudio 直接改 master 音量 |
| 向下滑 | 音量减小 | CoreAudio 直接改 master 音量 |
| 轻点 | 播放/暂停 | 模拟硬件媒体键 |

### 一定要先把系统手势挪开

macOS 的"N 指轻扫"是按 **N 指及以上**算的：调度中心设成"三指向上轻扫"时，你四指往上滑照样会弹调度中心。所以本项目用的指头数必须和系统那一档**完全错开**，否则一次滑动两边都触发。

默认用三指，那就要求系统的三指全空：

- 系统设置 → 触控板 → 更多手势 → **轻扫切换全屏显示应用程序**、**调度中心**：都别选"三指"，选"四指"或"关"。
- **三指拖移**（在 辅助功能 → 指针控制 里，对应 `TrackpadThreeFingerDrag`）：建议关掉，否则三指按住拖动会被系统拿去移窗口。

`install.sh` 会读 `com.apple.AppleMultitouchTrackpad`（内置触摸板那份）里你选的这一档，占用就打印警告；外接 Magic Trackpad 的设置存在 `com.apple.driver.AppleBluetoothMultitouch.trackpad`，那份得自己照下面的命令查一遍。这些键在界面上只是下拉框，用命令行改是这样（把三指的两条挪到四指）：

```sh
for d in com.apple.AppleMultitouchTrackpad com.apple.driver.AppleBluetoothMultitouch.trackpad; do
    defaults write $d TrackpadThreeFingerHorizSwipeGesture -int 0
    defaults write $d TrackpadThreeFingerVertSwipeGesture  -int 0
    defaults write $d TrackpadFourFingerHorizSwipeGesture  -int 2
    defaults write $d TrackpadFourFingerVertSwipeGesture   -int 2
done
killall Dock
```

`0` 是关，`2` 是这一栏的默认动作（竖向=调度中心，横向=切换全屏 App）。改完必须 `killall Dock` 才生效。

## 安装

### 用 Homebrew

```sh
brew tap hewuan-git/tap
brew install trackpad-media
trackpad-media-setup none key 0.10 3 on
```

formula 把 `main.c` 编成 Homebrew 目录下的 `TrackpadMedia.app`、ad-hoc 签好名，`trackpad-media-setup` 再写同一个 LaunchAgent。程序、五个参数、日志位置全都一样；`trackpad-media-setup --off` 停掉。

走 Homebrew 有两点要留意：

- `brew upgrade` 会重新编译，ad-hoc 签名绑的是二进制内容，所以升级之后原来那条授权就失效了：删掉旧条目重新加一次（`tccutil reset Accessibility com.local.trackpad-media` 可以清掉）。
- formula 钉在打 tag 的版本上，`brew` 装的是发布版，不是最新一次提交。tap 的源码在 [hewuan-git/homebrew-tap](https://github.com/hewuan-git/homebrew-tap)。

### 从源码

```sh
git clone https://github.com/hewuan-git/macos-trackpad-gestures.git
cd trackpad-media
./install.sh none key 0.10 3 on
```

五个位置参数依次是：

| 参数 | 取值 | 默认 | 含义 |
|---|---|---|---|
| 闸门 | `none` / `audio` / `media` | `audio` | 什么时候允许手势生效。`none` 一直生效；`audio` 只在有进程正在往外送音频时生效；`media` 只认系统登记的播放状态 |
| 切歌 | `key` / `mr` / `both` | `key` | 发媒体命令的方式：模拟硬件媒体键 / MediaRemote 命令 / 两个都发 |
| 行程 | `0.04`~`0.9` | `0.10` | 横向切歌需要划过多远（归一化到整块触摸板宽度，0.10 大约 1 cm） |
| 几指 | `2`~`5` | `3` | 用几根手指，要和系统手势错开 |
| 轻点 | `on` / `off` | `on` | 多指轻点是否等于播放/暂停 |

调手感就是重跑一次 `install.sh`（用 Homebrew 装的就重跑 `trackpad-media-setup`）。**源码没变时 `install.sh` 不重新编译、不重新签名**，所以调参数不会弄坏你已经给过的授权。

不传参数时用的是上表的默认值（闸门 `audio`、三指）。二进制自己的完整参数和 `--check` / `--watch` / `--test` 这些调试模式，`./trackpad-media --help` 都能查到。

## 必须手动授权一次

切歌和播放/暂停要模拟硬件按键，需要「设备控制和数据访问」（旧系统叫辅助功能）授权。Apple 不允许任何安装器预先拿到它，后台进程连授权弹窗都弹不出来，所以每台机器都要手动加一次：

1. 打开这一页：系统设置 → 隐私与安全性 → 设备控制和数据访问（或者 `open "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"`）
2. 程序每次启动都会向系统登记一次，列表里一般能直接看到 **Trackpad Media**（带空格、有 App 图标），把开关拨到开就行；看不到就点 **+**，在文件选择框里按 **⇧⌘G**，粘贴 `~/Library/Application Support/trackpad-media/TrackpadMedia.app`（用 Homebrew 装的话是 `$(brew --prefix trackpad-media)/TrackpadMedia.app`）。要是列表里还有一个全小写的 `trackpad-media`，那是早期版本留下的裸二进制，拨它没用，选中按 **−** 删掉。
3. **拨完开关要重启一次进程**，否则程序仍然按旧状态判定：实测开关拨开之后立刻查，报的还是"状态 1（拒绝）"，`launchctl kickstart -k gui/$(id -u)/com.user.trackpad-media` 之后马上就好了。
4. 验证：等两秒，`kill -USR1 <pid>`（pid 用 `pgrep -f TrackpadMedia.app` 拿），日志里出现 `自检通过：音量 x -> y（已还原）` 就成了

调音量不需要任何授权，装完就能用。

**这个授权绑的是二进制内容**（ad-hoc 签名 → cdhash）。重新编译一次，列表里的开关看着还是"开"，实际已经失效，必须删掉重新添加。所以：改完代码再授权，别边改边授权。

## 换一台机器之前

触摸板原始数据来自私有框架 `MultitouchSupport.framework`，没有公开头文件，手指结构体的字段偏移（每根手指跨距 96 字节、x 在 +32、y 在 +36，归一化 0..1，**y 轴向上增大**）是从 MacBook Air M1 上实测反推的。大多数机器一致，但别默认能直接用——先跑一次观察模式，它会打印你实际划出来的位移而不做任何动作：

```sh
# 只编译、不装成常驻服务
clang -O2 -fblocks -Wall -Wno-deprecated-declarations -o trackpad-media main.c \
      -framework CoreFoundation -framework CoreAudio -framework IOKit -ldl

./trackpad-media --check         # 框架能不能打开、有几块触摸板、媒体键通路
./trackpad-media --watch 30      # 30 秒内做几次手势，看 dx/dy 的正负和大小对不对
```

`--watch` 里如果左右/上下的正负和预期相反，改 `endGesture()` / `onContact()` 里的方向判断即可，别改偏移。

## 排查

- **日志**：`~/Library/Application Support/trackpad-media/run.log`。每次手势、每次被闸门拦下、每次缺权限都写在这里。一条 78 字节（本机跑了 805 行、61 KB），一天用几百次也就十几 KB，攒一年几 MB，不用专门轮转。想清空要先停再删——文件是被 launchd 持有的，直接截断会留下空洞：

  ```sh
  launchctl bootout gui/$(id -u)/com.user.trackpad-media
  rm ~/Library/Application\ Support/trackpad-media/run.log
  launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.user.trackpad-media.plist
  ```
- **远程自检**：`kill -USR1 <pid>` 发一次"音量减"再自动还原，用来客观确认媒体键真的能发出去；`kill -USR2 <pid>` 轮换闸门方式；`kill -WINCH <pid>` 不重启进程就重新订阅一次触摸板（也不会把授权弄没）。
- **睡一觉回来手势就不灵了**：这是长时间常驻真会踩到的坑。以前进程还活着、授权也还在，但睡眠唤醒后触摸板订阅成了空壳，划什么都没反应，还不报错。v1.1 起会监听 `IOPMrootDomain` 的 `kIOMessageSystemHasPoweredOn`，收到就重新枚举、重新订阅，每秒试一次直到又收到手指数据。老版本遇到这件事一条命令就能救活、且不会作废授权：
  ```sh
  launchctl kickstart -k gui/$(id -u)/com.user.trackpad-media
  ```
  判断到底是哪一层坏了就看日志：`tail ~/Library/Application\ Support/trackpad-media/run.log`，手势只要被识别就一定有一行；**划了却一行都不出**就是订阅断了，不是授权的问题。
- **网易云音乐不响应 MediaRemote**：它出声的时候系统查 `NowPlaying` 依然是空的，所以默认用模拟硬件媒体键（`--track key`）。用 `--track mr` 在这类 App 上会没反应。
- **`--gate audio` 判"正在出声"**：读 CoreAudio 的进程对象列表，逐个看是否正在往输出设备送数据。不要用 `kAudioDevicePropertyDeviceIsRunningSomewhere`，那个只要有程序占用输出设备就返回真，闲置也算。

## 停止 / 卸载

```sh
./stop.sh                      # 从源码装的
trackpad-media-setup --off     # 用 Homebrew 装的
```

只停止并取消自启，程序、plist、日志都保留（plist 改名为 `.disabled`）。脚本会打印一行恢复命令。Homebrew 装的还要 `brew uninstall trackpad-media` 才会把二进制拿掉——先停服务再卸载，否则 launchd 会继续抱着一个已经删掉的文件跑。想彻底删（源码版）：

```sh
./stop.sh
rm -rf ~/Library/Application Support/trackpad-media ~/Library/LaunchAgents/com.user.trackpad-media.plist.disabled
tccutil reset Accessibility com.local.trackpad-media
```

## 已知限制

- 只在有触摸板的 Mac 上有意义；外接 Magic Trackpad 需要自己用 `--watch` 验一遍数据。
- 多指手势之间没有过渡动画，判定靠位移阈值和方向锁定，划太慢会被当成"按住没动"而忽略。
- 代码用了 `kIOMainPortDefault` 和 `kAudioObjectPropertyElementMain`，这两个都是 macOS 12 才有的，所以**最低 macOS 12**；在 macOS 27.0.1 / 触摸板 family `0x6a` 上实测通过，12~26 没验证过（`MultitouchSupport` 这几个符号很多年没变，但别当成保证）。
