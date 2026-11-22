# Swipe to switch tracks on macOS — the native Windows trackpad gestures, reimplemented

**English** · [简体中文](README.zh-CN.md)

Looking for a **trackpad gesture to switch songs on macOS** — skip to the next / previous track, turn the volume up and down, play/pause — without lifting your fingers off the pad? That's what this does: multi-finger sideways swipe = previous / next track, vertical swipe = volume, tap = play / pause. The gestures Windows Precision Touchpads have out of the box, on a Mac.

One C file, zero dependencies. All you need is the `clang` that ships with the Command Line Tools (`xcode-select --install`) — no full Xcode, no GUI app left running in the menu bar. It builds into a 70 KB binary that registers itself as a LaunchAgent and starts at login. Two ways in: clone the repo and run one `install.sh`, or let Homebrew own the binary with `brew install`.

The repo is named `macos-trackpad-gestures` because GitHub only accepts ASCII in repo names and the English keywords are what people actually search for; the program, the command and the binary are all still called `trackpad-media`. The Homebrew formula is `trackpad-media` too. The program's log and its own messages are in Chinese — this file is the English version of [README.zh-CN.md](README.zh-CN.md).

## Quick start

```sh
brew tap hewuan-git/tap && brew install trackpad-media
trackpad-media-setup none key 0.10 3 on     # gate / track / travel / fingers / tap — see "Install" below
```

No Homebrew? Clone instead:

```sh
git clone https://github.com/hewuan-git/macos-trackpad-gestures.git && cd trackpad-media
./install.sh none key 0.10 3 on
```

Then grant the permission once, by hand:

```sh
open "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"
```

That last step only opens the settings page; you still have to flip the switch yourself — Apple does not let any installer pre-grant media-key access (see "One permission you must grant by hand"). **Volume needs no permission at all**, so even before granting, up/down swipes already work; only sideways swipes and taps are dead.

Read "Move the system gestures out of the way first" before you try anything — it is the easiest thing to get wrong.

## Why write this at all

On Windows these gestures come with the machine: Precision Touchpad and the ASUS / Lenovo vendor drivers ship multi-finger sideways swipe for track switching, vertical swipe for volume, and a tap for play/pause (the finger count and directions differ by vendor). Coming back to macOS feels clumsy: the system only offers window gestures — "three/four-finger swipe to switch full-screen apps, Mission Control" — and media control is left to third-party tools. The ones I tried were either paid, or insisted on keeping a GUI app running, or needed a Lua runtime. None of them fit, so I wrote this.

### How it differs from what's already out there

| Option | What you'd have to install | Why I didn't use it |
|---|---|---|
| Built-in macOS trackpad gestures | nothing | Only window gestures (Mission Control, switch full-screen apps); there is no media gesture and no way to remap one |
| Paid gesture apps (BetterTouchTool and friends) | a GUI app that stays running | Too heavy a thing to keep around just for skipping tracks and setting volume |
| Scripting tools (Hammerspoon and friends) | a Lua runtime + an app that stays running | Doable, but installing a whole runtime for three fingers didn't feel worth it |
| **This project** | only the `clang` in the Command Line Tools | A 70 KB binary and one LaunchAgent; nothing with a window |

## Gestures

Three fingers by default (2 to 5 can be selected):

| Motion | Effect | Path used |
|---|---|---|
| Swipe left | Previous track | Emulated hardware media key |
| Swipe right | Next track | Emulated hardware media key |
| Swipe up | Volume up | CoreAudio, sets master volume directly |
| Swipe down | Volume down | CoreAudio, sets master volume directly |
| Tap | Play / pause | Emulated hardware media key |

### Move the system gestures out of the way first

macOS treats "N-finger swipe" as **N fingers or more**: if Mission Control is bound to "three-finger swipe up", a four-finger swipe up still opens Mission Control. So the finger count this program uses must be completely disjoint from whatever the system slot uses, or one swipe fires both.

The default is three fingers, which means all three-finger system gestures must be free:

- System Settings → Trackpad → More Gestures → **Swipe between full-screen apps** and **Mission Control**: neither should be set to "three fingers" — pick "four fingers" or "off".
- **Three-finger drag** (under Accessibility → Pointer Control, the key is `TrackpadThreeFingerDrag`): better off, otherwise holding and dragging with three fingers gets eaten by the window manager.

`install.sh` reads the slot you picked from `com.apple.AppleMultitouchTrackpad` (the built-in trackpad's domain) and prints a warning if the system still occupies it. Magic Trackpad settings live in a separate domain, `com.apple.driver.AppleBluetoothMultitouch.trackpad`, which you have to check yourself with the command below. These keys are only drop-downs in the UI; from the command line, moving the two three-finger entries over to four fingers looks like this:

```sh
for d in com.apple.AppleMultitouchTrackpad com.apple.driver.AppleBluetoothMultitouch.trackpad; do
    defaults write $d TrackpadThreeFingerHorizSwipeGesture -int 0
    defaults write $d TrackpadThreeFingerVertSwipeGesture  -int 0
    defaults write $d TrackpadFourFingerHorizSwipeGesture  -int 2
    defaults write $d TrackpadFourFingerVertSwipeGesture   -int 2
done
killall Dock
```

`0` means off; `2` means that row's default action (vertical = Mission Control, horizontal = switch full-screen apps). You must `killall Dock` afterwards for it to take effect.

## Install

### With Homebrew

```sh
brew tap hewuan-git/tap
brew install trackpad-media
trackpad-media-setup none key 0.10 3 on
```

The formula compiles `main.c` into a `TrackpadMedia.app` under Homebrew's prefix, ad-hoc signs it, and `trackpad-media-setup` writes the same LaunchAgent from there. Same program, same five arguments, same log file. Stop it with `trackpad-media-setup --off`.

Two things that are specific to the Homebrew route:

- `brew upgrade` recompiles, and an ad-hoc signature is bound to the binary's bytes, so the permission entry dies. After an upgrade, delete the entry and add it again (`tccutil reset Accessibility com.local.trackpad-media` clears it).
- The formula pins a tagged release, so `brew` installs a released version rather than whatever the last commit was. Tap source: [hewuan-git/homebrew-tap](https://github.com/hewuan-git/homebrew-tap).

### From source

```sh
git clone https://github.com/hewuan-git/macos-trackpad-gestures.git
cd trackpad-media
./install.sh none key 0.10 3 on
```

The five positional arguments, in order:

| Argument | Values | Default | Meaning |
|---|---|---|---|
| gate | `none` / `audio` / `media` | `audio` | When gestures are allowed to act. `none` = always; `audio` = only while some process is actually pushing audio out; `media` = trust only the playback state the system has registered |
| track | `key` / `mr` / `both` | `key` | How track commands are delivered: emulated hardware media key / MediaRemote command / both |
| travel | `0.04`~`0.9` | `0.10` | How far a sideways swipe must travel to switch tracks (normalized to the full trackpad width; 0.10 is about 1 cm) |
| fingers | `2`~`5` | `3` | How many fingers to use — must not collide with the system's slot |
| tap | `on` / `off` | `on` | Whether an N-finger tap means play/pause |

Tuning the feel means re-running `install.sh` — or `trackpad-media-setup`, if you installed with Homebrew. **If the source hasn't changed, `install.sh` neither recompiles nor re-signs**, so changing parameters cannot break the permission you already granted.

With no arguments you get the defaults in the table above (gate `audio`, three fingers). Every flag of the binary itself, plus the debug modes `--check` / `--watch` / `--test`, is listed by `./trackpad-media --help`.

## One permission you must grant by hand

Track switching and play/pause synthesize hardware key presses, which need the "Device and Data Access" permission (called Accessibility on older systems). Apple forbids any installer from obtaining it in advance, and a background process cannot even raise the prompt, so it must be added manually on every machine:

1. Open System Settings → Privacy & Security → Device and Data Access (or run `open "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"`).
2. The program registers itself with the system on every launch, so the list usually already shows **Trackpad Media** (with a space, with an app icon) — just flip it on. If it isn't there, click **+**, press **⇧⌘G** in the file panel and paste `~/Library/Application Support/trackpad-media/TrackpadMedia.app` — or, if you installed with Homebrew, `$(brew --prefix trackpad-media)/TrackpadMedia.app`. If the list also contains an all-lowercase `trackpad-media`, that is a leftover from an early version's bare binary — switching it does nothing, select it and press **−** to remove it.
3. **After flipping the switch, restart the process once**, otherwise the program keeps judging by the old state. Measured: checking immediately after flipping it still reports "status 1 (denied)"; after `launchctl kickstart -k gui/$(id -u)/com.user.trackpad-media` it works right away.
4. Verify: wait two seconds, then `kill -USR1 <pid>` (get the pid with `pgrep -f TrackpadMedia.app`). The log line `自检通过：音量 x -> y（已还原）` ("self-test passed: volume restored") means you are done.

Volume control needs no permission — it works as soon as you install.

**This permission is bound to the binary's contents** (ad-hoc signature → cdhash). After one rebuild, the switch in the list still looks "on" but is actually invalid: you must delete the entry and re-add it. So finish your code changes first, then grant — not the other way around.

## Before you move to another Mac

Raw trackpad data comes from the private `MultitouchSupport.framework`, which has no public headers. The field offsets of the finger struct (96 bytes per finger, x at +32, y at +36, normalized 0..1, **the y axis grows upwards**) were reverse-engineered from measurements on a MacBook Air M1. Most machines agree, but don't assume it works out of the box — run watch mode first. It prints the displacement you actually drew and performs no action:

```sh
# compile only, don't install it as a persistent service
clang -O2 -fblocks -Wall -Wno-deprecated-declarations -o trackpad-media main.c \
      -framework CoreFoundation -framework CoreAudio -framework IOKit -ldl

./trackpad-media --check         # can the framework load, how many trackpads, is the media-key path there
./trackpad-media --watch 30      # do a few gestures within 30 s, check the sign and size of dx/dy
```

If watch mode shows left/right or up/down inverted, fix the direction checks in `endGesture()` / `onContact()` — don't touch the offsets.

## Troubleshooting

- **Log**: `~/Library/Application Support/trackpad-media/run.log`. Every gesture, every block by the gate, every missing permission is written there. 78 bytes per line (61 KB over 805 lines on my machine), so a few hundred gestures a day is a dozen KB — a year of it is a few MB, no rotation needed. To clear it, stop first: launchd holds the file, and truncating it in place leaves a hole:

  ```sh
  launchctl bootout gui/$(id -u)/com.user.trackpad-media
  rm ~/Library/Application\ Support/trackpad-media/run.log
  launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.user.trackpad-media.plist
  ```
- **Remote self-test**: `kill -USR1 <pid>` sends one "volume down" and restores it, which is an objective way to confirm the media key actually goes out; `kill -USR2 <pid>` rotates the gate.
- **NetEase Cloud Music ignores MediaRemote**: while it is making sound, the system's `NowPlaying` is still empty — hence the default of emulated hardware media keys (`--track key`). With `--track mr` this kind of app simply won't react.
- **How `--gate audio` decides "making sound"**: it reads CoreAudio's process object list and checks, per process, whether audio is being pushed to the output device. Don't use `kAudioDevicePropertyDeviceIsRunningSomewhere` — that returns true for anything merely holding the output device, idle included.

## Stop / uninstall

```sh
./stop.sh                      # if you installed from source
trackpad-media-setup --off     # if you installed with Homebrew
```

This only stops the service and disables autostart; the program, the plist and the log are all kept (the plist is renamed to `.disabled`). The script prints a one-line restore command. With Homebrew, run `brew uninstall trackpad-media` afterwards to drop the binary — stop the service first, or launchd keeps running a deleted file. To remove everything (source install):

```sh
./stop.sh
rm -rf ~/Library/Application Support/trackpad-media ~/Library/LaunchAgents/com.user.trackpad-media.plist.disabled
tccutil reset Accessibility com.local.trackpad-media
```

## Known limitations

- Only meaningful on a Mac with a trackpad; for an external Magic Trackpad you should verify the data yourself with `--watch`.
- There is no animation between multi-finger gestures; detection relies on a displacement threshold and axis locking, so swiping very slowly reads as "held still" and gets ignored.
- The code uses `kIOMainPortDefault` and `kAudioObjectPropertyElementMain`, both macOS 12+, so **macOS 12 is the floor**. It is verified on macOS 27.0.1 with trackpad family `0x6a`; 12 through 26 are untested (those `MultitouchSupport` symbols haven't changed in years, but don't treat that as a guarantee).
