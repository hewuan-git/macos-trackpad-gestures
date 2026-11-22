/*
 * trackpad-media — 触摸板手势控制媒体
 *
 * N 指向左滑  -> 上一首      N 指向右滑 -> 下一首
 *              （模拟硬件媒体键，网易云这类不向系统注册播放状态的 App 也吃这一套）
 * N 指向上滑  -> 音量增大    N 指向下滑 -> 音量减小
 *              （CoreAudio 直接改，不需要任何授权）
 * N 指轻点    -> 播放/暂停   （模拟硬件播放键，--tap off 可关）
 * N 由 --fingers 决定（默认 3）。系统的"N 指轻扫"是 N 指及以上都算，
 * 所以这个数必须和系统手势用的指头数错开，否则一起触发。
 * 默认只在"有进程正在送音频"时生效。
 *
 * macOS 27 上 MultitouchSupport.framework 没有公开头文件，
 * 下面 Finger 结构体的偏移是用实测数据反推出来的（见 --check）。
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <math.h>
#include <objc/objc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- 实测得到的触摸板数据结构 ---------- */
#define FINGER_STRIDE 96
#define FINGER_X_OFF  32
#define FINGER_Y_OFF  36

/* ---------- 手势参数（归一化坐标，0..1 覆盖整块触摸板） ---------- */
#define AXIS_LOCK         0.035   /* 移动多少算确定方向 */
#define SWIPE_RATIO       1.6     /* 横向要明显强于纵向 */
#define VOL_TRIGGER       0.10    /* 纵向每移动这么多 = 一格音量 */
#define VOL_STEP          0.0625  /* 一格，和硬件音量键的 1/16 对齐 */

/* 横向切歌需要的位移，用 --swipe 调，改这个值不必重新编译 */
static double swipeMin = 0.10;
/* 用几根手指，用 --fingers 调。系统手势是"N 指及以上都算"，所以只能和系统错开。
   默认三指：四指那一档通常还被系统的切换全屏 App / 调度中心占着 */
static int wantFingers = 3;
/* N 指轻点 = 播放/暂停，用 --tap off 关掉 */
static int tapEnabled = 1;
#define TAP_MOVE_MAX   0.018   /* 轻点时手指最多允许漂这么远 */
#define TAP_TIME_MAX   0.45    /* 按下到抬起不超过这么久才算轻点 */

static int verbose = 1;
static int dryRun = 0;

static void logmsg(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&ts.tv_sec));
    fprintf(stderr, "[%s] ", stamp);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* CFString -> C 字符串；用轮换缓冲，允许一次 printf 里调用多次 */
static const char *cf2c(const void *v) {
    static char bufs[4][512];
    static int slot = 0;
    slot = (slot + 3) & 3;
    if (!v || CFGetTypeID((CFTypeRef)v) != CFStringGetTypeID()) return "(非字符串)";
    if (!CFStringGetCString((CFStringRef)v, bufs[slot], sizeof bufs[slot], kCFStringEncodingUTF8))
        return "(编码失败)";
    return bufs[slot];
}

/* ================= 音量：CoreAudio 直接读写 ================= */

static AudioDeviceID defaultOutput(void) {
    AudioDeviceID d = 0;
    UInt32 sz = sizeof d;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &d) != noErr) return 0;
    return d;
}

static int volIsRunningSomewhere(void) {
    AudioDeviceID d = defaultOutput();
    if (!d) return 0;
    UInt32 r = 0;
    UInt32 sz = sizeof r;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceIsRunningSomewhere,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &r) != noErr) return 0;
    return r != 0;
}

static int caFlag(AudioObjectID o, AudioObjectPropertySelector p, AudioObjectPropertyScope sc) {
    UInt32 v = 0xFFFF;
    UInt32 sz = sizeof v;
    AudioObjectPropertyAddress a = {p, sc, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &v) != noErr) return -1;
    return (int)v;
}

static char *procName(AudioObjectID proc, char *out, size_t cap) {
    CFTypeRef bid = NULL;
    UInt32 sz = sizeof bid;
    out[0] = 0;
    AudioObjectPropertyAddress a = {kAudioProcessPropertyBundleID,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(proc, &a, 0, NULL, &sz, &bid) != noErr || !bid) return out;
    CFStringRef s = NULL;
    if (CFGetTypeID(bid) == CFStringGetTypeID()) s = (CFStringRef)bid;
    else if (CFGetTypeID(bid) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef)bid) > 0)
        s = (CFStringRef)CFArrayGetValueAtIndex((CFArrayRef)bid, 0);
    if (s && CFGetTypeID(s) == CFStringGetTypeID())
        CFStringGetCString(s, out, cap, kCFStringEncodingUTF8);
    CFRelease(bid);
    return out;
}

/* 有几个进程正在往外送音频；names 里带回最多 4 个 bundle id。
   实测本机：网易云放歌时为 1，afplay 起停时进程数在 0.5 秒内跟着变。 */
static int audioProducers(char *names, size_t cap) {
    AudioObjectID list[128];
    UInt32 sz = sizeof list;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyProcessObjectList,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    names[0] = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, list) != noErr)
        return -1;
    int total = (int)(sz / sizeof(AudioObjectID));
    int cnt = 0;
    for (int i = 0; i < total; i++) {
        if (caFlag(list[i], kAudioProcessPropertyIsRunningOutput,
                   kAudioObjectPropertyScopeOutput) != 1) continue;
        cnt++;
        if (cnt <= 4) {
            char nm[72];
            procName(list[i], nm, sizeof nm);
            strncat(names, nm[0] ? nm : "(无标识)", cap - strlen(names) - 1);
            strncat(names, " ", cap - strlen(names) - 1);
        }
    }
    return cnt;
}

static float volGet(AudioDeviceID d) {
    Float32 v = -1;
    UInt32 sz = sizeof v;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyVolumeScalar,
                                    kAudioDevicePropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &v) != noErr) return -1;
    return v;
}

static int volSet(AudioDeviceID d, float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    Float32 nv = v;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyVolumeScalar,
                                    kAudioDevicePropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof nv, &nv) == noErr ? 0 : -1;
}

static void changeVolume(int dir) {
    AudioDeviceID d = defaultOutput();
    if (!d) { logmsg("没有默认输出设备"); return; }
    float cur = volGet(d);
    if (cur < 0) { logmsg("读不到音量"); return; }
    float next = cur + dir * VOL_STEP;
    if (dryRun) { logmsg("音量 %.2f -> %.2f (dry-run 未生效)", cur, next); return; }
    if (volSet(d, next) == 0)
        logmsg("音量 %.2f -> %.2f", cur, fmaxf(0, fminf(1, next)));
    else
        logmsg("设置音量失败");
}

/* ============ 硬件媒体键（相当于替你按 F8/F10/F11/F12） ============
 * 走 IOHIDPostEvent。这条路需要"辅助功能"授权；没授权时事件会被系统丢掉。
 * macOS 27 已经把 IOHIDPostEvent 标了废弃，但没有等价的公开 API，
 * 事件内容按 compound（系统定义事件）的原始布局填写。 */

static io_connect_t hidConn;

static int hidReady(void) {
    if (hidConn) return 1;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("IOHIDSystem"));
    if (!svc) return 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kIOHIDParamConnectType, &hidConn);
    IOObjectRelease(svc);
    return (kr == KERN_SUCCESS && hidConn) ? 1 : 0;
}

static int postMediaKey(int keycode) {
    if (!hidReady()) { logmsg("打不开 IOHIDSystem，媒体键发不出去"); return -1; }
    int acc = IOHIDCheckAccess(kIOHIDRequestTypePostEvent);
    if (acc != kIOHIDAccessTypeGranted) {
        logmsg("缺权限：媒体键授权未通过（当前状态 %d，0=允许 1=拒绝 2=未决定），"
               "请在 系统设置→隐私与安全性→设备控制和数据访问（旧系统叫辅助功能）里勾选本程序", acc);
        return -2;
    }
    for (int pass = 0; pass < 2; pass++) {
        NXEventData ev;
        memset(&ev, 0, sizeof ev);
        ev.compound.subType = NX_SUBTYPE_AUX_CONTROL_BUTTONS;
        ev.compound.misc.L[0] = (keycode << 16) | ((pass ? NX_KEYUP : NX_KEYDOWN) << 8);
        IOGPoint loc = {0, 0};
        kern_return_t kr = IOHIDPostEvent(hidConn, NX_SYSDEFINED, loc, &ev,
                                         kNXEventDataVersion, 0, 0);
        if (kr) { logmsg("IOHIDPostEvent 失败 kr=0x%x", (unsigned)kr); return -1; }
        if (!pass) usleep(60 * 1000);
    }
    return 0;
}

/* 0=只用 MediaRemote 1=只用媒体键 2=两个都发 */
static int trackVia = 1;

/* 没授权时向系统登记一次（会让本程序出现在"辅助功能"列表里，也可能直接弹窗） */
static void requestPostAccess(void) {
    int a = (int)IOHIDCheckAccess(kIOHIDRequestTypePostEvent);
    if (a == kIOHIDAccessTypeGranted) { logmsg("辅助功能：已授权"); return; }
    logmsg("辅助功能：状态 %d，向系统登记请求……", a);
    bool ok = IOHIDRequestAccess(kIOHIDRequestTypePostEvent);
    logmsg("辅助功能：请求返回 %s，现在状态 %d（0=允许 1=拒绝 2=未决定）",
           ok ? "已允许" : "未允许", (int)IOHIDCheckAccess(kIOHIDRequestTypePostEvent));
}

/* 发一次"音量减"再读回音量，用来远程验证媒体键通路是否真的通 */
static void mediaKeySelfTest(void) {
    AudioDeviceID d = defaultOutput();
    float before = volGet(d);
    if (!d || before < 0) { logmsg("自检：读不到音量，跳过"); return; }
    if (dryRun) { logmsg("自检：dry-run 不真发按键"); return; }
    requestPostAccess();
    logmsg("自检开始：当前音量 %.3f，准备发一次硬件『音量减』", before);
    if (postMediaKey(NX_KEYTYPE_SOUND_DOWN) != 0) { logmsg("自检：按键没发出去"); return; }
    for (int i = 0; i < 14; i++) {
        usleep(150 * 1000);
        float now = volGet(d);
        if (now >= 0 && now != before) {
            volSet(d, before);
            logmsg("自检通过：音量 %.3f -> %.3f，媒体键通路可用（已还原到 %.3f）",
                   before, now, before);
            return;
        }
    }
    logmsg("自检结果：权限没问题但音量没变，说明按键编码没被系统认——需要换编码再试");
}

/* ================= MediaRemote：切歌 + 是否在播放 ================= */

enum { kMRPlay = 0, kMRPause = 1, kMRTogglePlayPause = 2, kMRNextTrack = 4, kMRPreviousTrack = 5 };

typedef void (*MRSendFn)(int, CFDictionaryRef);
typedef void (*MRGetPlayingFn)(dispatch_queue_t, void (^)(BOOL));
typedef void (*MRGetInfoFn)(dispatch_queue_t, void (^)(CFDictionaryRef));
typedef void (*MRGetClientFn)(dispatch_queue_t, void (^)(CFTypeRef));
typedef void (*MRRegisterFn)(dispatch_queue_t);

static MRSendFn pSend;
static MRGetPlayingFn pGetPlaying;
static MRGetInfoFn pGetInfo;
static MRGetClientFn pGetClient;
static volatile bool g_isPlaying = false;
static CFDictionaryRef g_info = NULL;
static CFTypeRef g_client = NULL;

static void loadMediaRemote(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_NOW);
    if (!h) { logmsg("MediaRemote 打不开: %s", dlerror()); return; }
    pSend = (MRSendFn)dlsym(h, "MRMediaRemoteSendCommand");
    pGetPlaying = (MRGetPlayingFn)dlsym(h, "MRMediaRemoteGetNowPlayingApplicationIsPlaying");
    pGetInfo = (MRGetInfoFn)dlsym(h, "MRMediaRemoteGetNowPlayingInfo");
    pGetClient = (MRGetClientFn)dlsym(h, "MRMediaRemoteGetNowPlayingClient");
    MRRegisterFn reg = (MRRegisterFn)dlsym(h, "MRMediaRemoteRegisterForNowPlayingNotifications");
    if (reg) reg(dispatch_get_main_queue());
    if (!pSend || !pGetPlaying) logmsg("MediaRemote 符号缺失");
}

static void refreshPlaying(void) {
    if (!pGetPlaying) return;
    pGetPlaying(dispatch_get_main_queue(), ^(BOOL playing) { g_isPlaying = (bool)playing; });
}

static void refreshInfo(void) {
    if (!pGetInfo) return;
    pGetInfo(dispatch_get_main_queue(), ^(CFDictionaryRef info) {
        if (g_info) CFRelease(g_info);
        g_info = info ? CFRetain(info) : NULL;
    });
}

/* 系统里到底有没有"当前播放客户端"——与调用方是不是 .app 无关，用来做交叉验证 */
static void refreshClient(void) {
    if (!pGetClient) return;
    pGetClient(dispatch_get_main_queue(), ^(CFTypeRef client) {
        if (g_client) CFRelease(g_client);
        g_client = client ? CFRetain(client) : NULL;
    });
}

/* "媒体正在播放"的判定方式：0=MediaRemote 上报 1=有进程正在送音频 2=不判定
   本机网易云不向 MediaRemote 注册，所以默认用 1。 */
static int gateMode = 1;
static char gateWho[224];

static const char *gateLabel(void) {
    return gateMode == 0 ? "MediaRemote 正在播放"
                         : gateMode == 1 ? "有进程正在送音频" : "不判定";
}

static bool gateOpen(void) {
    switch (gateMode) {
        case 1: {
            int n = audioProducers(gateWho, sizeof gateWho);
            return n > 0;
        }
        case 2: return true;
        default: return g_isPlaying;
    }
}

static void sendTrack(int dir) {
    const char *what = dir > 0 ? "下一首" : "上一首";
    if (dryRun) { logmsg("%s（dry-run 未发送）", what); return; }
    if (trackVia != 1 && pSend) {
        logmsg("%s：走 MediaRemote 命令", what);
        pSend(dir > 0 ? kMRNextTrack : kMRPreviousTrack, NULL);
    }
    if (trackVia != 0) {
        int key = dir > 0 ? NX_KEYTYPE_NEXT : NX_KEYTYPE_PREVIOUS;
        logmsg("%s：模拟硬件媒体键（keycode %d）", what, key);
        postMediaKey(key);
    }
}

/* N 指点按 = 播放/暂停。硬件播放键本身就是切换，网易云吃这一套 */
static void sendToggle(void) {
    if (dryRun) { logmsg("播放/暂停（dry-run 未发送）"); return; }
    if (trackVia != 1 && pSend) {
        logmsg("播放/暂停：走 MediaRemote 命令");
        pSend(kMRTogglePlayPause, NULL);
    }
    if (trackVia != 0) {
        logmsg("播放/暂停：模拟硬件媒体键（keycode %d）", NX_KEYTYPE_PLAY);
        postMediaKey(NX_KEYTYPE_PLAY);
    }
}

/* ================= 触摸板手势 ================= */

typedef void *MTDeviceRef;
typedef void (*MTContactCb)(MTDeviceRef, void *, int, double, int);
typedef MTDeviceRef (*MTCreateListFn)(void);
typedef void (*MTRegisterFn)(MTDeviceRef, MTContactCb);
typedef void (*MTStartFn)(MTDeviceRef, int);
typedef void (*MTStopFn)(MTDeviceRef);

/* 轻点判定用自带时钟，不信任回调给的时间戳单位 */
static double nowSec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct {
    int active;
    int targetHit;        /* 这一轮凑够过目标手指数 */
    int seqActive;        /* 这一轮有手指按下 */
    int maxFingers;       /* 这一轮实际达到的最大手指数 */
    int blockLogged;      /* 一次手势只抱怨一次 */
    int tooMany;          /* 多出来的手指是留给系统手势的，这一轮我不出手 */
    double sx, sy;      /* 达到目标手指数时的起点 */
    double lx, ly;      /* 上一帧质心 */
    double maxDx, maxDy;
    int axis;           /* 0 未定 1 横 2 纵 */
    double volAcc;
    double t0, tEnd;    /* 凑够手指数的时刻、最后一次还凑够的时刻 */
} g;

static void blockedNote(const char *what) {
    if (g.blockLogged) return;
    g.blockLogged = 1;
    if (gateMode == 1)
        logmsg("%s：闸门关（此刻没有进程在送音频）。想让手势一直生效就用 --gate none", what);
    else
        logmsg("%s：闸门关（判定方式：%s）", what, gateLabel());
}

static int readCentroid(void *fingers, int n, double *cx, double *cy) {
    if (n > 11) n = 11;
    double ax = 0, ay = 0;
    for (int i = 0; i < n; i++) {
        char *p = (char *)fingers + (size_t)i * FINGER_STRIDE;
        float x, y;
        memcpy(&x, p + FINGER_X_OFF, sizeof x);
        memcpy(&y, p + FINGER_Y_OFF, sizeof y);
        if (!(x >= -0.2f && x <= 1.2f) || !(y >= -0.2f && y <= 1.2f)) return 0;
        ax += x;
        ay += y;
    }
    *cx = ax / n;
    *cy = ay / n;
    return 1;
}

static void endGesture(void) {
    if (!g.active || g.tooMany) return;
    if (dryRun)
        logmsg("手势结束 axis=%d dx=%+.3f dy=%+.3f 闸门(%s)=%s", g.axis, g.maxDx, g.maxDy,
               gateLabel(), gateOpen() ? "开" : "关");
    int isSwipe = g.axis == 1 || (g.axis == 0 && fabs(g.maxDx) >= swipeMin &&
                                  fabs(g.maxDx) >= SWIPE_RATIO * fabs(g.maxDy));
    if (isSwipe && fabs(g.maxDx) >= swipeMin) {
        if (!gateOpen()) blockedNote("横滑");
        else sendTrack(g.maxDx < 0 ? -1 : +1);   /* 左滑=上一首，右滑=下一首 */
    } else if (g.axis == 2) {
        /* 纵向已在滑动过程中逐格处理过了 */
    } else if (g.axis == 0) {
        double moved = fabs(g.maxDx) > fabs(g.maxDy) ? fabs(g.maxDx) : fabs(g.maxDy);
        double held = g.tEnd - g.t0;
        if (tapEnabled && moved <= TAP_MOVE_MAX && held > 0 && held <= TAP_TIME_MAX) {
            if (!gateOpen()) blockedNote("轻点");
            else sendToggle();
        } else if (dryRun) {
            logmsg("%d 指按下但既没到滑动阈值也不像轻点 (dx=%.3f dy=%.3f 按住 %.2fs)",
                   wantFingers, g.maxDx, g.maxDy, held);
        }
    }
}

static void onContact(MTDeviceRef dev, void *fingers, int n, double ts, int frame) {
    (void)dev; (void)ts; (void)frame;
    double cx, cy;

    if (n > 0) {
        if (!g.seqActive) {
            memset(&g, 0, sizeof g);
            g.seqActive = 1;
        }
        if (n > g.maxFingers) g.maxFingers = n;
        if (n > wantFingers && !g.tooMany) {
            g.tooMany = 1;
            if (g.targetHit) logmsg("这一轮到了 %d 根手指，比我要的 %d 根多，这一下交给系统", n, wantFingers);
        }
    }

    if (n >= wantFingers && !g.tooMany) {
        if (!readCentroid(fingers, n, &cx, &cy)) return;
        if (!g.targetHit) {
            g.targetHit = 1;
            g.active = 1;
            g.t0 = g.tEnd = nowSec();
            g.sx = g.lx = cx;
            g.sy = g.ly = cy;
            g.maxDx = g.maxDy = 0;
            g.axis = 0;
            g.volAcc = 0;
            return;
        }
        double dy = cy - g.ly;
        g.lx = cx;
        g.ly = cy;
        g.tEnd = nowSec();
        double tdx = cx - g.sx, tdy = cy - g.sy;
        if (fabs(tdx) > fabs(g.maxDx)) g.maxDx = tdx;
        if (fabs(tdy) > fabs(g.maxDy)) g.maxDy = tdy;

        if (g.axis == 0) {
            if (fabs(tdx) >= AXIS_LOCK && fabs(tdx) >= fabs(tdy)) g.axis = 1;
            else if (fabs(tdy) >= AXIS_LOCK && fabs(tdy) > fabs(tdx)) g.axis = 2;
        }
        if (g.axis == 2) {
            g.volAcc += dy;
            while (fabs(g.volAcc) >= VOL_TRIGGER) {
                int dir = g.volAcc > 0 ? +1 : -1;   /* 实测上滑时 volAcc 为正（y 轴朝上增大）→ 上滑变响 */
                if (!gateOpen()) { blockedNote("竖滑"); g.volAcc = 0; break; }
                changeVolume(dir);
                g.volAcc -= (g.volAcc > 0 ? VOL_TRIGGER : -VOL_TRIGGER);
            }
        }
    } else if (n == 0) {
        if (!g.targetHit && g.maxFingers >= wantFingers)
            logmsg("这一轮最多 %d 根手指，没凑够 %d 根，忽略", g.maxFingers, wantFingers);
        endGesture();
        memset(&g, 0, sizeof g);
    } else if (g.targetHit && n <= 1) {
        endGesture();
        memset(&g, 0, sizeof g);
    }
}

/* ================= 主循环 ================= */

static MTDeviceRef gDevice;
static MTStopFn pStop;
/* MTDeviceCreateList 是 Create 规则，数组持有设备对象；提前释放会让 gDevice 变成野指针，
   所以这份只活一次的分配故意不回收 */
static CFArrayRef gDeviceList;

/* 用信号远程操作正在运行的守护进程，这样测试是在它自己的授权身份下做的：
 *   SIGUSR1 = 跑一次媒体键自检（会短暂降一格音量再还原）
 *   SIGUSR2 = 轮换闸门判定方式 */
static volatile sig_atomic_t wantSelfTest;
static volatile sig_atomic_t wantGateCycle;

static void onUsr(int sig) {
    if (sig == SIGUSR1) wantSelfTest = 1;
    else wantGateCycle = 1;
}

static void onTimer(CFRunLoopTimerRef t, void *ctx) {
    (void)t; (void)ctx;
    refreshPlaying();
    refreshClient();
    if (wantSelfTest) { wantSelfTest = 0; mediaKeySelfTest(); }
    if (wantGateCycle) {
        wantGateCycle = 0;
        gateMode = (gateMode + 1) % 3;
        logmsg("闸门切到：%s", gateLabel());
    }
}

static void cleanup(int sig) {
    (void)sig;
    if (gDevice && pStop) pStop(gDevice);
    _exit(0);
}

static int startDaemon(int secs) {
    void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!h) { logmsg("MultitouchSupport 打不开: %s", dlerror()); return 0; }
    MTCreateListFn createList = (MTCreateListFn)dlsym(h, "MTDeviceCreateList");
    MTRegisterFn reg = (MTRegisterFn)dlsym(h, "MTRegisterContactFrameCallback");
    MTStartFn start = (MTStartFn)dlsym(h, "MTDeviceStart");
    pStop = (MTStopFn)dlsym(h, "MTDeviceStop");
    if (!createList || !reg || !start) { logmsg("MultitouchSupport 符号缺失，这个系统版本用不了"); return 0; }

    gDeviceList = (CFArrayRef)createList();
    if (!gDeviceList || CFArrayGetCount(gDeviceList) == 0) {
        logmsg("找不到触摸板设备。没有内置触摸板又想让手势生效，接一个 Magic Trackpad 再重新安装");
        return 0;
    }
    gDevice = (MTDeviceRef)CFArrayGetValueAtIndex(gDeviceList, 0);
    reg(gDevice, onContact);
    loadMediaRemote();
    if (!dryRun && trackVia != 0) requestPostAccess();
    if (wantFingers <= 2) logmsg("注意：用 %d 根手指会和系统的双指滚动、缩放完全重叠，大概率不是你想要的", wantFingers);

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    signal(SIGUSR1, onUsr);
    signal(SIGUSR2, onUsr);

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(), 1.0, 0, 0, onTimer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    logmsg("trackpad-media 已启动 pid=%d：%d 指左=上一首 右=下一首（横滑行程 %.2f），%d 指上=音量增大 下=减小，%d 指轻点=%s；"
           "闸门=%s，切歌=%s",
           getpid(), wantFingers, swipeMin, wantFingers, wantFingers,
           tapEnabled ? "播放/暂停" : "不管（--tap off）", gateLabel(),
           trackVia == 0 ? "只用 MediaRemote" : trackVia == 2 ? "两者都发" : "只用硬件媒体键");
    start(gDevice, 0);
    if (secs > 0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);
    else CFRunLoopRun();
    pStop(gDevice);
    return 0;
}

static void usage(FILE *f) {
    fprintf(f, "用法: trackpad-media [--check|--watch [秒]|--test next|prev|volup|voldown|toggle|selftest|--run|-q]"
               " [--gate media|audio|none] [--track mr|key|both] [--swipe 0.04~0.9] [--fingers 2~5] [--tap on|off]\n"
               "  --check  打印触摸板、音量、媒体键通路的状态，不做任何动作\n"
               "  --watch [秒]  观察模式：只打印手势判定的中间量，不发消息\n"
               "  --test X  立刻发一次 X 然后退出，用来单独验证某条通路\n"
               "  --run     常驻运行（LaunchAgent 用的就是这个）\n"
               "运行中还可用信号远程操作：kill -USR1 <pid> 跑媒体键自检，kill -USR2 <pid> 换闸门\n");
}

int main(int argc, char **argv) {
    const char *mode = "run", *sub = NULL;
    int afterMode = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--gate") || !strcmp(a, "--track") || !strcmp(a, "--swipe")
            || !strcmp(a, "--fingers") || !strcmp(a, "--tap")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s 需要一个值\n", a); return 2; }
            const char *v = argv[++i];
            afterMode = 0;
            if (!strcmp(a, "--tap")) {
                if (!strcmp(v, "on")) tapEnabled = 1;
                else if (!strcmp(v, "off")) tapEnabled = 0;
                else { fprintf(stderr, "--tap 只接受 on|off\n"); return 2; }
            } else if (!strcmp(a, "--swipe")) {
                double d = atof(v);
                if (d < 0.04 || d > 0.9) { fprintf(stderr, "--swipe 要在 0.04~0.9 之间\n"); return 2; }
                swipeMin = d;
            } else if (!strcmp(a, "--fingers")) {
                int f = atoi(v);
                if (f < 2 || f > 5) { fprintf(stderr, "--fingers 只接受 2~5\n"); return 2; }
                wantFingers = f;
            } else if (!strcmp(a, "--gate")) {
                if (!strcmp(v, "media")) gateMode = 0;
                else if (!strcmp(v, "audio")) gateMode = 1;
                else if (!strcmp(v, "none")) gateMode = 2;
                else { fprintf(stderr, "--gate 只接受 media|audio|none\n"); return 2; }
            } else {
                if (!strcmp(v, "mr")) trackVia = 0;
                else if (!strcmp(v, "key")) trackVia = 1;
                else if (!strcmp(v, "both")) trackVia = 2;
                else { fprintf(stderr, "--track 只接受 mr|key|both\n"); return 2; }
            }
            continue;
        }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        if (!strcmp(a, "--check") || !strcmp(a, "--watch") || !strcmp(a, "--test")
            || !strcmp(a, "--run") || !strcmp(a, "-q")) {
            mode = a;
            afterMode = 1;
            continue;
        }
        if (a[0] != '-' && afterMode) { sub = a; afterMode = 0; continue; }
        fprintf(stderr, "不认识的参数：%s\n", a);
        usage(stderr);
        return 2;
    }

    if (!strcmp(mode, "--check")) {
        void *mt = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
        printf("MultitouchSupport : %s\n", mt ? "可用" : "不可用");
        if (mt) {
            MTCreateListFn cl = (MTCreateListFn)dlsym(mt, "MTDeviceCreateList");
            CFArrayRef l = cl ? (CFArrayRef)cl() : NULL;
            printf("触摸板设备数     : %ld\n", l ? (long)CFArrayGetCount(l) : -1L);
        }
        loadMediaRemote();
        printf("MediaRemote 发送 : %s\n", pSend ? "可用" : "不可用");
        printf("MediaRemote 状态 : %s\n", pGetPlaying ? "可用" : "不可用");
        AudioDeviceID d = defaultOutput();
        printf("默认输出设备     : %u  音量 %.2f\n", (unsigned)d, d ? volGet(d) : -1.f);
        refreshPlaying();
        refreshInfo();
        refreshClient();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.5, false);
        printf("正在播放        : %s\n", g_isPlaying ? "是" : "否");
        printf("播放客户端对象  : %s\n", g_client ? "有（系统已注册播放器）" : "无（没有任何 App 注册为当前播放者）");
        printf("音频设备在用    : %s（这个太松，只作参考）\n", volIsRunningSomewhere() ? "是" : "否");
        printf("送音频的进程    : %d 个 %s\n", audioProducers(gateWho, sizeof gateWho), gateWho);
        printf("媒体键通路      : %s，辅助功能状态 %d（0=允许 1=拒绝 2=未决定）\n",
               hidReady() ? "IOHIDSystem 已连上" : "打不开 IOHIDSystem",
               (int)IOHIDCheckAccess(kIOHIDRequestTypePostEvent));
        printf("当前闸门方式    : %s -> %s\n", gateLabel(), gateOpen() ? "开（手势会生效）" : "关（手势被忽略）");
        if (g_info) {
            printf("NowPlaying 字段 :\n");
            CFIndex cnt = CFDictionaryGetCount(g_info);
            const void *keys[64];
            const void *vals[64];
            if (cnt > 64) cnt = 64;
            CFDictionaryGetKeysAndValues(g_info, keys, vals);
            for (CFIndex i = 0; i < cnt; i++) {
                const void *v = vals[i];
                if (CFGetTypeID(v) == CFStringGetTypeID())
                    printf("  %-36s = %s\n", cf2c(keys[i]), cf2c(v));
                else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
                    double d = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d);
                    printf("  %-36s = %g\n", cf2c(keys[i]), d);
                } else
                    printf("  %-36s = <%s>\n", cf2c(keys[i]),
                           CFGetTypeID(v) == CFDataGetTypeID() ? "Data" : "其他");
            }
        } else {
            printf("NowPlaying 字段 : 空（没有 App 向系统注册播放状态）\n");
        }
        return 0;
    }
    if (!strcmp(mode, "--test")) {
        loadMediaRemote();
        const char *what = sub ? sub : "next";
        if (!strcmp(what, "next")) sendTrack(+1);
        else if (!strcmp(what, "prev")) sendTrack(-1);
        else if (!strcmp(what, "volup")) changeVolume(+1);
        else if (!strcmp(what, "voldown")) changeVolume(-1);
        else if (!strcmp(what, "toggle")) sendToggle();
        else if (!strcmp(what, "selftest")) mediaKeySelfTest();
        else { fprintf(stderr, "--test 只接受 next|prev|volup|voldown|toggle|selftest\n"); return 2; }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.6, false);
        return 0;
    }
    if (!strcmp(mode, "--watch")) {
        dryRun = 1;
        int secs = 30;
        if (sub) {
            secs = atoi(sub);
            if (secs < 1 || secs > 600) { fprintf(stderr, "--watch 的秒数要在 1~600，按 30 秒来\n"); secs = 30; }
        }
        return startDaemon(secs);
    }
    if (!strcmp(mode, "--run"))  { return startDaemon(-1); }
    if (!strcmp(mode, "-q"))     { verbose = 0; return startDaemon(-1); }

    usage(stderr);
    return 2;
}
