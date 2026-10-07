// Frame Mic Tuner: Steam Frame のマイクのエコー除去・ノイズ除去を、SteamVR のダッシュボードから切り替えるパネル。
// 切り替えは「wpctl settings --save」だけで行う（PipeWire・WirePlumber の再起動・amixer の書き換えはしない）。
#include "command.h"
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "mic_panel.h"
#include "mic_state.h"
#include "mic_worker.h"
#include "theme.h"
#include "voice_check.h"
#include "vr_overlay.h"

#include "update_check.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef FRAME_MIC_TUNER_VERSION
#define FRAME_MIC_TUNER_VERSION "unknown"  // CMake を通さずにビルドしたとき
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gShowRequested = 0;  ///< SIGUSR1（2 つ目の起動から）でパネルを開く

/** 「終了」「閉じる」で終わったときの終了コード（systemd の RestartPreventExitStatus= に書く）。 */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;         ///< ダッシュボードのサムネイルの一辺（px）
constexpr double kPanelPollSec = 0.033;     ///< パネルが見えている間のイベント確認の間隔
constexpr double kClosedPollSec = 0.25;     ///< パネルが見えていない間のイベント確認の間隔
constexpr double kVoiceFrameSec = 1.0 / 15; ///< 録音中・再生中の描き直しの間隔
constexpr double kOverlayCheckSec = 3.0;    ///< 自己修復: 自分のオーバーレイがまだあるかを確かめる間隔（閉じている間も）

// 新しい版の確認・更新（vendor/frame-updater）。frame-update.sh は install.sh が置く場所を読む
constexpr const char* kUpdateAppName = "frame-mic-tuner";
constexpr const char* kUpdateRepo = "sasaken1102r/frame-mic-tuner";
constexpr const char* kUpdateAssetPattern = "frame-mic-tuner-{version}.tar.gz";

/** コマンドラインの内容。 */
struct Options {
    enum class Mode { Overlay, Print, DumpPng, Probe, SwitchAway, TestRecord, ContrastReport, SelfTest, Version, Help };
    Mode mode = Mode::Overlay;
    std::string configPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    int thumbnailSize = 256;       ///< --thumbnail-png で書き出す一辺（px）
    std::string language;          ///< 空でなければ PNG の書き出しで設定の言語の代わりに使う（ja / en）
    bool previewQuit = false;      ///< 「もう一度押すと終了」の状態で描く
    std::vector<MicCommand> sets;  ///< --set-echo などの書き込み（--print の前に実行する）
    // 見た目の確認用のダミーの状態（どれか 1 つでも付けると、実際の値を読まずにダミーで描く）
    bool fake = false;
    bool fakeEcho = true;
    bool fakeNs = false;
    bool fakeIdle = false;
    bool fakeLoading = false;
    Autostart fakeAutostart = Autostart::Disabled;
    MicError fakeError = MicError::None;
    // 声のチェックのダミー（--dump-png 用）
    bool fakeRecording = false;
    int fakeHistory = 0;       ///< 履歴の件数（0〜5）
    int fakePlaying = -1;      ///< 再生中の行（新しい順の何件目か。-1 でなし）
    VoiceError fakeVoiceError = VoiceError::None;
    std::string previewPressed;  ///< 押している見た目にするボタン（earphone / speaker / record）
    double testRecordSec = 3.0;  ///< --test-record の秒数
    bool debugRecordOnOpen = false;  ///< 確認用: パネルが開いたら自動で録音を始める
    double switchAwaySec = 3.0;      ///< --probe-switch-away で切り替えたままにする秒数
    // ノイズ除去の強さ（--set-ns-vad / --set-ns-grace。その場でかけるだけで、設定ファイルには保存しない）
    bool setNsVad = false;
    double nsVad = 0.0;
    bool setNsGrace = false;
    double nsGrace = 0.0;
    // ノイズ除去の強さのダミーと、バーをドラッグしている見た目（--dump-png 用）
    double fakeNsVad = kNsVadDefault;
    double fakeNsGrace = kNsGraceDefault;
    PanelAction previewDrag = PanelAction::None;
    double previewDragValue = 0.0;
    std::string tab;  ///< 空でなければ PNG の書き出しで設定のタブの代わりに使う（quick / fine）
    // 版の行のダミー（--dump-png 用）
    std::string fakeUpdate;         ///< unknown/uptodate/checking/available/manual/installing/installed/checkfailed/installfailed
    bool previewUpdateConfirm = false;  ///< 「更新する」の確認の表示にする（available と組み合わせる）
};

/**
 * SIGTERM / SIGINT を受けたら止める印をつける。
 * @param signal 受けたシグナル（使わない）
 */
void onSignal(int /*signal*/) {
    gStopRequested = 1;
}

/**
 * SIGUSR1 を受けたら、パネルを開く印をつける（2 つ目の起動が送ってくる）。
 * @param signal 受けたシグナル（使わない）
 */
void onShowSignal(int /*signal*/) {
    gShowRequested = 1;
}

/**
 * SIGTERM / SIGINT で行儀よく終われるようにし、SIGUSR1 でパネルを開けるようにする。
 */
void installSignalHandlers() {
    struct sigaction show {};
    show.sa_handler = onShowSignal;
    sigemptyset(&show.sa_mask);
    sigaction(SIGUSR1, &show, nullptr);

    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * 止める印・パネルを開く印がつくまで、または指定時間が経つまで待つ。
 * @param seconds 待つ秒数
 */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested && !gShowRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::fmin(left, 0.5)));
    }
}

/** 使い方を表示する。 */
void printUsage() {
    std::printf(
        "用法: frame-mic-tuner [选项]\n"
        "  （无）                在 SteamVR 仪表盘中显示面板并常驻（没有 SteamVR 时等待）\n"
        "                        如果已在常驻，则打开那边的面板后退出\n"
        "  --print               不启动 OpenVR，显示当前值、麦克风是否使用中、信号链路后退出\n"
        "  --set-echo on|off     切换回声消除后，显示与 --print 相同的内容\n"
        "  --set-ns on|off       切换噪声抑制后显示\n"
        "  --set-autostart on|off  切换是否随 SteamVR 一起启动（systemctl --user enable/disable）后显示\n"
        "  --set-ns-vad N        当场应用噪声抑制的判定严格度（0～99%%）后显示（不保存）\n"
        "  --set-ns-grace N      当场应用噪声抑制的保持时间（0～1000ms）后显示（不保存）\n"
        "  --dump-png PATH       不启动 OpenVR，把面板图像写入 PNG 后退出（按当前值绘制）\n"
        "  --thumbnail-png PATH  把仪表盘的缩略图（与＋图标相同的图）写入 PNG\n"
        "      --thumbnail-size N  该缩略图的边长（默认 256）\n"
        "      --language ja|en|zh  用该语言绘制，代替设置中的语言\n"
        "      --preview-quit    按“再按一次即退出”的状态绘制\n"
        "      --tab quick|fine  用该标签页（简单 / 精细调整）绘制，代替设置中的标签页\n"
        "      --fake            不读取实际值，按虚拟状态（扬声器·使用中）绘制。后面的 --fake-* 也一样\n"
        "      --fake-echo on|off / --fake-ns on|off   虚拟的回声消除·噪声抑制\n"
        "      --fake-idle       麦克风未使用\n"
        "      --fake-loading    尚未读取的状态\n"
        "      --fake-autostart on|off|missing|unknown  自启动状态（missing = 没有单元文件）\n"
        "      --fake-error read|not-installed|links|write|write-echo|write-ns|autostart  红色失败提示\n"
        "      --fake-recording  录音中（音量表·经过时间）的外观\n"
        "      --fake-history N  虚拟历史 N 条（0～5）\n"
        "      --fake-playing I  把历史第 I 条（0 为最新）设为播放中\n"
        "      --fake-voice-error record|play  声音检查的失败提示\n"
        "      --fake-update STATE  版本行的虚拟值（unknown/uptodate/checking/available/manual/installing/\n"
        "                        installed/checkfailed/installfailed）\n"
        "      --preview-update-confirm  把“更新”变成确认显示（与 --fake-update available 组合使用）\n"
        "      --preview-pressed earphone|speaker|record  按下时的外观\n"
        "      --fake-ns-vad N / --fake-ns-grace N  虚拟的噪声抑制强度（默认 23 / 500）\n"
        "      --preview-drag-vad N / --preview-drag-grace N  把该滑块拖到 N 的外观\n"
        "  --test-record [S]     不启动 OpenVR，从默认输入录制 S 秒（默认 3）→ 显示峰值和长度 → 用默认输出播放\n"
        "                        （录音中和之后也显示 pw-metadata -n filters。音频只存在于内存中）\n"
        "  --contrast-report     按画面的文字颜色·部件颜色与背景的每种组合，给出 WCAG 对比度与是否通过\n"
        "  --self-test           不启动 OpenVR，用判定函数（自修复的覆盖层判定·输出读取）试算并输出结果\n"
        "  --probe-switch-away [S]  确认用：切换到临时仪表盘覆盖层 S 秒（默认 3），让 Mic 面板保持关闭状态\n"
        "  --debug-record-on-open   确认用（常驻）：面板打开后自动开始录音（用于确认关闭时是否停止的日志）\n"
        "  --probe               诊断用：以 Background 类型连接 SteamVR，寻找常驻的面板并输出状态\n"
        "  --version             显示版本后退出\n"
        "  --config PATH         配置文件（默认 ~/.config/frame-mic-tuner/config.json）\n");
}

/**
 * on / off を読む。
 * @param text 引数
 * @param value 読めたときの書き込み先
 * @return 読めたら true
 */
bool parseOnOff(const std::string& text, bool& value) {
    if (text == "on" || text == "true") {
        value = true;
        return true;
    }
    if (text == "off" || text == "false") {
        value = false;
        return true;
    }
    std::fprintf(stderr, "请用 on 或 off 指定：%s\n", text.c_str());
    return false;
}

/**
 * コマンドラインを読む。
 * @param argc 引数の数
 * @param argv 引数
 * @param options 書き込み先
 * @return 正しく読めたら true
 */
bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        bool value = false;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if ((arg == "--set-echo" || arg == "--set-ns" || arg == "--set-autostart") && hasNext) {
            if (!parseOnOff(argv[++i], value)) return false;
            const MicCommand::Kind kind = arg == "--set-echo" ? MicCommand::Kind::SetEcho
                                          : arg == "--set-ns" ? MicCommand::Kind::SetNs
                                                              : MicCommand::Kind::SetAutostart;
            options.sets.push_back({kind, value});
            options.mode = Options::Mode::Print;
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--language" && hasNext) {
            options.language = argv[++i];
            Language check;
            if (!parseLanguage(options.language, check)) {
                std::fprintf(stderr, "--language 只能是 ja、en 或 zh：%s\n", options.language.c_str());
                return false;
            }
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--tab" && hasNext) {
            options.tab = argv[++i];
            if (options.tab != "quick" && options.tab != "fine") {
                std::fprintf(stderr, "--tab 只能是 quick 或 fine：%s\n", options.tab.c_str());
                return false;
            }
        } else if (arg == "--set-ns-vad" && hasNext) {
            options.setNsVad = true;
            options.nsVad = clampNsVad(std::atof(argv[++i]));
            options.mode = Options::Mode::Print;
        } else if (arg == "--set-ns-grace" && hasNext) {
            options.setNsGrace = true;
            options.nsGrace = clampNsGrace(std::atof(argv[++i]));
            options.mode = Options::Mode::Print;
        } else if (arg == "--fake-ns-vad" && hasNext) {
            options.fakeNsVad = clampNsVad(std::atof(argv[++i]));
            options.fake = true;
        } else if (arg == "--fake-ns-grace" && hasNext) {
            options.fakeNsGrace = clampNsGrace(std::atof(argv[++i]));
            options.fake = true;
        } else if ((arg == "--preview-drag-vad" || arg == "--preview-drag-grace") && hasNext) {
            options.previewDrag = arg == "--preview-drag-vad" ? PanelAction::NsVadSlider : PanelAction::NsGraceSlider;
            options.previewDragValue = std::atof(argv[++i]);
        } else if (arg == "--fake") {
            options.fake = true;
        } else if (arg == "--fake-echo" && hasNext) {
            if (!parseOnOff(argv[++i], options.fakeEcho)) return false;
            options.fake = true;
        } else if (arg == "--fake-ns" && hasNext) {
            if (!parseOnOff(argv[++i], options.fakeNs)) return false;
            options.fake = true;
        } else if (arg == "--fake-idle") {
            options.fakeIdle = true;
            options.fake = true;
        } else if (arg == "--fake-loading") {
            options.fakeLoading = true;
            options.fake = true;
        } else if (arg == "--fake-autostart" && hasNext) {
            const std::string state = argv[++i];
            if (state == "on") {
                options.fakeAutostart = Autostart::Enabled;
            } else if (state == "off") {
                options.fakeAutostart = Autostart::Disabled;
            } else if (state == "missing") {
                options.fakeAutostart = Autostart::Missing;
            } else if (state == "unknown") {
                options.fakeAutostart = Autostart::Unknown;
            } else {
                std::fprintf(stderr, "--fake-autostart 只能是 on / off / missing / unknown：%s\n", state.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-error" && hasNext) {
            const std::string kind = argv[++i];
            if (kind == "read") {
                options.fakeError = MicError::ReadSettings;
            } else if (kind == "not-installed") {
                options.fakeError = MicError::NotInstalled;
            } else if (kind == "links") {
                options.fakeError = MicError::ReadLinks;
            } else if (kind == "write") {
                options.fakeError = MicError::WriteSettings;
            } else if (kind == "write-echo") {
                options.fakeError = MicError::WriteEcho;
            } else if (kind == "write-ns") {
                options.fakeError = MicError::WriteNs;
            } else if (kind == "autostart") {
                options.fakeError = MicError::WriteAutostart;
            } else {
                std::fprintf(stderr, "--fake-error 只能是 read / not-installed / links / write / write-echo / write-ns / autostart：%s\n",
                             kind.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-recording") {
            options.fakeRecording = true;
        } else if (arg == "--fake-history" && hasNext) {
            options.fakeHistory = std::max(0, std::min(static_cast<int>(kVoiceHistory), std::atoi(argv[++i])));
        } else if (arg == "--fake-playing" && hasNext) {
            options.fakePlaying = std::atoi(argv[++i]);
        } else if (arg == "--fake-voice-error" && hasNext) {
            const std::string kind = argv[++i];
            if (kind != "record" && kind != "play") {
                std::fprintf(stderr, "--fake-voice-error 只能是 record / play：%s\n", kind.c_str());
                return false;
            }
            options.fakeVoiceError = kind == "record" ? VoiceError::Record : VoiceError::Play;
        } else if (arg == "--fake-update" && hasNext) {
            options.fakeUpdate = argv[++i];
            static const char* kKnown[] = {"unknown",  "uptodate",     "checking",     "available",
                                           "manual",   "installing",   "installed",    "checkfailed",
                                           "installfailed"};
            bool known = false;
            for (const char* name : kKnown) known |= options.fakeUpdate == name;
            if (!known) {
                std::fprintf(stderr,
                             "--fake-update 只能是 unknown/uptodate/checking/available/manual/installing/installed/"
                             "checkfailed/installfailed：%s\n",
                             options.fakeUpdate.c_str());
                return false;
            }
        } else if (arg == "--preview-update-confirm") {
            options.previewUpdateConfirm = true;
        } else if (arg == "--preview-pressed" && hasNext) {
            options.previewPressed = argv[++i];
        } else if (arg == "--test-record") {
            options.mode = Options::Mode::TestRecord;
            if (hasNext && argv[i + 1][0] != '-') {
                options.testRecordSec = std::max(0.5, std::min(kVoiceMaxSec, std::atof(argv[++i])));
            }
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--self-test") {
            options.mode = Options::Mode::SelfTest;
        } else if (arg == "--probe-switch-away") {
            options.mode = Options::Mode::SwitchAway;
            if (hasNext && argv[i + 1][0] != '-') options.switchAwaySec = std::max(0.5, std::atof(argv[++i]));
        } else if (arg == "--debug-record-on-open") {
            options.debugRecordOnOpen = true;
        } else if (arg == "--probe") {
            options.mode = Options::Mode::Probe;
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "未知的参数：%s\n", arg.c_str());
            return false;
        }
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    return true;
}

/**
 * 自動起動の状態を日本語にする（--print 用）。
 * @param autostart 状態
 * @return 文言
 */
const char* autostartName(Autostart autostart) {
    switch (autostart) {
        case Autostart::Enabled: return "开（enabled）";
        case Autostart::Disabled: return "关（disabled）";
        case Autostart::Missing: return "没有单元文件（not-found）";
        case Autostart::Unknown: break;
    }
    return "无法读取";
}

/**
 * ノイズ除去の強さを短い日本語にする（ログ・--print 用）。
 * @param ns 値
 * @return 例:「判定の厳しさ 23%・余韻 500ms（ns_capture の id 53）」
 */
std::string describeNsParams(const NsParams& ns) {
    if (!ns.nodeKnown) return std::string(kNsNodeName) + " 未找到";
    char text[160];
    std::snprintf(text, sizeof(text), "判定严格度 %s·保持时间 %s（%s 的 id %d）",
                  ns.vadKnown ? (std::to_string(static_cast<int>(std::lround(ns.vad))) + "%").c_str() : "?",
                  ns.graceKnown ? (std::to_string(static_cast<int>(std::lround(ns.grace))) + "ms").c_str() : "?",
                  kNsNodeName, ns.nodeId);
    return text;
}

/**
 * 表示する状態が変わったときに、ログへ 1 行出す（外から変えられたときの追従をあとで確かめるため）。
 * @param state 状態
 */
void logState(const MicState& state) {
    const auto onOff = [](bool known, bool value) { return known ? (value ? "开" : "关") : "?"; };
    const UiText& text = uiText(Language::Zh);
    const MicError error = state.writeError != MicError::None ? state.writeError : state.readError;
    const NsParams& ns = state.nsParams;
    std::fprintf(stderr, "[麦克风] 更新显示：回声消除 %s·噪声抑制 %s（%s）·%s·%s·自启动 %s%s%s\n",
                 onOff(state.echoKnown, state.echo), onOff(state.nsKnown, state.ns), describeNsParams(ns).c_str(),
                 !state.linksKnown ? "使用状态未知" : (state.inUse ? "使用中" : "未使用"), describeChain(state).c_str(),
                 autostartName(state.autostart), error != MicError::None ? "·失败：" : "",
                 errorText(error, text).c_str());
}

/**
 * 状態を端末向けに書き出す（--print 用）。
 * @param state 状態
 */
void printState(const MicState& state) {
    const auto onOff = [](bool known, bool value) { return known ? (value ? "开（true）" : "关（false）") : "无法读取"; };
    std::printf("回声消除 (%s)：%s\n", kEchoCancelKey, onOff(state.echoKnown, state.echo));
    std::printf("噪声抑制 (%s)：%s\n", kNoiseSuppressionKey, onOff(state.nsKnown, state.ns));
    std::printf("噪声抑制强度：%s\n", describeNsParams(state.nsParams).c_str());
    if (state.echoKnown && state.nsKnown) {
        // プリセット: イヤホン = エコー除去オフ・ノイズ除去オフ、スピーカー = エコー除去オン・ノイズ除去オフ
        const char* preset = state.ns ? "精细调整的设置（与两个预设都不同）"
                                      : (state.echo ? "扬声器" : "耳机");
        std::printf("预设：%s\n", preset);
    }
    std::printf("麦克风：%s\n", !state.linksKnown ? "无法读取" : (state.inUse ? "使用中" : "未使用"));
    std::printf("信号链路：%s\n", describeChain(state).c_str());
    if (!state.users.empty()) {
        std::printf("正在从麦克风取音的节点：");
        for (const auto& user : state.users) std::printf(" %s", user.c_str());
        std::printf("\n");
    }
    std::printf("随 SteamVR 一起启动 (%s)：%s\n", kServiceName, autostartName(state.autostart));
    const UiText& text = uiText(Language::Zh);
    if (state.readError != MicError::None) std::printf("失败：%s\n", errorText(state.readError, text).c_str());
    if (state.writeError != MicError::None) std::printf("失败：%s\n", errorText(state.writeError, text).c_str());
    std::printf("-- pw-link -l 中麦克风的通路 --\n%s", state.rawLinks.c_str());
}

/**
 * --print（と --set-*）: OpenVR なしで、書き込みをしてから今の状態を表示する。
 * @param options コマンドライン
 * @return 終了コード（書き込みか読み取りに失敗したら 1）
 */
int runPrint(const Options& options) {
    MicError writeError = MicError::None;
    for (const MicCommand& command : options.sets) {
        bool ok = false;
        switch (command.kind) {
            case MicCommand::Kind::SetEcho: ok = writeSetting(kEchoCancelKey, command.value); break;
            case MicCommand::Kind::SetNs: ok = writeSetting(kNoiseSuppressionKey, command.value); break;
            case MicCommand::Kind::SetAutostart: ok = writeAutostart(command.value); break;
            case MicCommand::Kind::SetNsParams: break;  // 下でまとめて扱う
            case MicCommand::Kind::SetPreset: break;    // --print からは使わない
        }
        if (!ok) {
            writeError = command.kind == MicCommand::Kind::SetAutostart ? MicError::WriteAutostart
                                                                         : MicError::WriteSettings;
        }
    }
    if (options.setNsVad || options.setNsGrace) {
        // 指定しなかったほうは今の値のまま。その場でかけるだけで、設定ファイルには保存しない
        const NsParams now = readNsParams();
        const double vad = options.setNsVad ? options.nsVad : (now.vadKnown ? now.vad : kNsVadDefault);
        const double grace = options.setNsGrace ? options.nsGrace : (now.graceKnown ? now.grace : kNsGraceDefault);
        if (!now.nodeKnown || !writeNsParams(now.nodeId, vad, grace)) writeError = MicError::WriteNsParams;
    }
    MicState state = readMicState();
    state.writeError = writeError;
    printState(state);
    return (state.readError != MicError::None || writeError != MicError::None) ? 1 : 0;
}

/**
 * 見た目の確認用のダミーの状態を作る。
 * @param options コマンドライン（--fake-*）
 * @return 状態
 */
MicState fakeState(const Options& options) {
    MicState state;
    if (options.fakeLoading) return state;
    state.loaded = true;
    state.echoKnown = true;
    state.echo = options.fakeEcho;
    state.nsKnown = true;
    state.ns = options.fakeNs;
    state.linksKnown = true;
    state.inUse = !options.fakeIdle;
    if (state.inUse) {
        // tracker と同じ: 使用中は EQ が必ず入り、エコー除去・ノイズ除去は設定しだい
        state.chain.push_back({ChainStage::Kind::Eq, "eq"});
        if (state.echo) state.chain.push_back({ChainStage::Kind::EchoCancel, "echo_cancel"});
        if (state.ns) state.chain.push_back({ChainStage::Kind::NoiseSuppression, "ns"});
    }
    state.autostart = options.fakeAutostart;
    state.nsParams.nodeKnown = true;
    state.nsParams.nodeId = 53;
    state.nsParams.vadKnown = true;
    state.nsParams.vad = options.fakeNsVad;
    state.nsParams.graceKnown = true;
    state.nsParams.grace = options.fakeNsGrace;
    const MicError error = options.fakeError;
    if (error == MicError::WriteSettings || error == MicError::WriteAutostart || error == MicError::WriteEcho ||
        error == MicError::WriteNs) {
        state.writeError = error;
    } else if (error != MicError::None) {
        state.readError = error;
        if (error == MicError::ReadSettings || error == MicError::NotInstalled) {
            state.echoKnown = false;
            state.nsKnown = false;
        }
        if (error == MicError::ReadLinks) {
            state.linksKnown = false;
            state.inUse = false;
            state.chain.clear();
        }
    }
    return state;
}

/**
 * 見た目の確認用のダミーの声のチェックの状態を作る。
 * @param options コマンドライン（--fake-recording・--fake-history など）
 * @return 状態（履歴の音声は無音、波形だけそれらしく作る）
 */
VoiceView fakeVoiceView(const Options& options) {
    VoiceView view;
    view.recording = options.fakeRecording;
    view.recordSec = 3.4;
    view.levelDb = -14.2f;
    view.error = options.fakeVoiceError;
    // 新しい順。録ったときの設定をいろいろにして、聞き比べの様子にする
    const double lengths[] = {3.2, 5.0, 2.4, 8.1, 10.0};
    const bool echoes[] = {true, false, true, false, true};
    const bool nss[] = {false, false, true, true, false};
    const std::time_t now = std::time(nullptr);
    for (int i = 0; i < options.fakeHistory; ++i) {
        auto clip = std::make_shared<VoiceClip>();
        clip->id = static_cast<uint64_t>(100 - i);
        clip->recordedAt = now - 90 * (i + 1);
        clip->echoKnown = clip->nsKnown = true;
        clip->echo = echoes[i];
        clip->ns = nss[i];
        clip->nsParamsKnown = true;
        clip->nsVad = i == 2 ? 10.0 : kNsVadDefault;      // 3 件目は強さを変えて録った例
        clip->nsGrace = i == 2 ? 800.0 : kNsGraceDefault;
        clip->samples.assign(static_cast<size_t>(lengths[i] * kVoiceRate), 0);
        clip->wave.resize(kWaveBins);
        for (int b = 0; b < kWaveBins; ++b) {
            // 話し声らしく、ふくらみと切れ目のある包絡にする
            const double t = static_cast<double>(b) / kWaveBins;
            const double envelope = std::fabs(std::sin(t * (9 + i * 2))) * (0.35 + 0.65 * std::fabs(std::sin(t * 3.1 + i)));
            clip->wave[b] = static_cast<float>(0.02 + 0.5 * envelope * envelope);
        }
        clip->peakDb = -6.0f - i;
        view.clips.push_back(clip);
    }
    if (options.fakePlaying >= 0 && options.fakePlaying < static_cast<int>(view.clips.size())) {
        view.playing = true;
        view.playingId = view.clips[options.fakePlaying]->id;
        view.playSec = view.clips[options.fakePlaying]->seconds() * 0.4;
    }
    return view;
}

/**
 * 見た目の確認用のダミーの更新の状態を作る（--dump-png 用）。
 * @param options コマンドライン（--fake-update）
 * @return 状態
 */
frame_updater::UpdateStatus fakeUpdateStatus(const Options& options) {
    using frame_updater::UpdateState;
    frame_updater::UpdateStatus status;
    status.current = FRAME_MIC_TUNER_VERSION;
    const std::string& state = options.fakeUpdate;
    if (state == "uptodate") {
        status.state = UpdateState::UpToDate;
    } else if (state == "checking") {
        status.state = UpdateState::UpToDate;
        status.checking = true;
    } else if (state == "available") {
        status.state = UpdateState::Available;
        status.latest = "9.9.9";
        status.url = "https://github.com/" + std::string(kUpdateRepo) + "/releases/tag/v9.9.9";
        status.installable = true;
    } else if (state == "manual") {
        status.state = UpdateState::Available;
        status.latest = "9.9.9";
        status.url = "https://github.com/" + std::string(kUpdateRepo) + "/releases/tag/v9.9.9";
        status.installable = false;
        status.reason = "no-checksums";
    } else if (state == "installing") {
        status.state = UpdateState::Installing;
        status.step = "download";
        status.version = "9.9.9";
    } else if (state == "installed") {
        status.state = UpdateState::Installed;
        status.version = "9.9.9";
    } else if (state == "checkfailed") {
        status.state = UpdateState::CheckFailed;
        status.error = "network";
    } else if (state == "installfailed") {
        status.state = UpdateState::InstallFailed;
        status.error = "checksum-mismatch";
    } else {
        status.state = UpdateState::Unknown;  // "unknown" か、指定なし
    }
    return status;
}

/**
 * --dump-png / --thumbnail-png: OpenVR なしでパネル（とサムネイル）を描いて PNG に書き出す。
 * @param options コマンドライン
 * @return 終了コード
 */
int runDumpPng(const Options& options) {
    Config config = loadConfigOrDefault(options.configPath);
    if (!options.language.empty()) parseLanguage(options.language, config.language);
    if (!options.tab.empty()) config.tab = options.tab == "fine" ? PanelTab::Fine : PanelTab::Quick;
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    if (!options.pngPath.empty()) {
        const MicState state = options.fake ? fakeState(options) : readMicState();
        MicPanel panel(fonts);
        if (options.previewQuit) panel.armQuitForPreview();
        if (!options.previewPressed.empty()) {
            PanelHit hit;
            if (options.previewPressed == "earphone") hit.action = PanelAction::Earphone;
            if (options.previewPressed == "speaker") hit.action = PanelAction::Speaker;
            if (options.previewPressed == "record") hit.action = PanelAction::Record;
            panel.setPointerForPreview(hit, hit);
        }
        if (options.previewDrag != PanelAction::None) panel.setDragForPreview(options.previewDrag, options.previewDragValue);
        if (options.previewUpdateConfirm) panel.armUpdateForPreview();
        panel.render(config, state, fakeVoiceView(options), fakeUpdateStatus(options));
        if (!panel.writePng(options.pngPath)) {
            std::fprintf(stderr, "无法写出 PNG：%s\n", options.pngPath.c_str());
            return 1;
        }
        std::printf("已写出 PNG：%s（%dx%d）\n", options.pngPath.c_str(), panel.width(), panel.height());
    }
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("已写出缩略图：%s（%dx%d）\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * pw-metadata -n filters の filter.smart.disabled を 1 行にまとめて表示する（--test-record 用）。
 * @param when いつの表示か
 */
void printFilterMetadata(const char* when) {
    const CommandResult result = runCommand({"pw-metadata", "-n", "filters"});
    std::string line;
    size_t pos = 0;
    while ((pos = result.out.find("id:", pos)) != std::string::npos) {
        const size_t end = result.out.find('\n', pos);
        const std::string entry = result.out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const size_t id = entry.find(' ');
        const size_t value = entry.find("value:'");
        if (id != std::string::npos && value != std::string::npos) {
            line += " " + entry.substr(0, id) + "=" + entry.substr(value + 7, entry.find('\'', value + 7) - value - 7);
        }
        pos = end == std::string::npos ? result.out.size() : end;
    }
    if (line.empty()) line = " （无法读取：" + describeCommand({"pw-metadata", "-n", "filters"}, result) + "）";
    std::printf("  [%s] pw-metadata -n filters 的 filter.smart.disabled:%s\n", when, line.c_str());
}

/**
 * --test-record: OpenVR なしで、既定の入力から録音 → ピークと長さを表示 → メモリから既定の出力へ再生。
 * 音声はメモリの中だけで、ファイルにもログにも書かない。
 * @param options コマンドライン
 * @return 終了コード
 */
int runTestRecord(const Options& options) {
    const MicState settings = readMicState(false);
    std::printf("录制时的设置：%s\n", describeChain(settings).c_str());
    printFilterMetadata("录音前");
    VoiceCheck voice;
    if (!voice.startRecording(settings)) {
        std::printf("无法开始录音\n");
        return 1;
    }
    const double start = nowSeconds();
    bool checked = false;
    float loudest = -120.0f;
    int ticks = 0;
    while (!gStopRequested && voice.recording() && nowSeconds() - start < options.testRecordSec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        voice.update();
        const VoiceView view = voice.view();
        loudest = std::max(loudest, view.levelDb);
        if (++ticks % 5 == 0) std::printf("  %.1f 秒：音量表 %.1f dBFS\n", view.recordSec, view.levelDb);
        if (!checked && nowSeconds() - start > 1.2) {
            checked = true;
            printFilterMetadata("录音中");
            const MicState during = readMicState(false);
            std::printf("  [录音中] 信号链路：%s\n  [录音中] 正在从麦克风取音的节点：", describeChain(during).c_str());
            for (const auto& user : during.users) std::printf(" %s", user.c_str());
            std::printf("\n");
        }
    }
    voice.stopRecording();
    if (voice.clips().empty()) {
        std::printf("没有录到声音（太短或失败）\n");
        voice.shutdown();
        return 1;
    }
    const VoiceClip& clip = *voice.clips().front();
    std::printf("录音：%.2f 秒（%zu 个采样·%d Hz·mono int16）·整体峰值 %.1f dBFS·音量表最大值 %.1f dBFS\n",
                clip.seconds(), clip.samples.size(), kVoiceRate, clip.peakDb, loudest);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    printFilterMetadata("录音后 1.5 秒");

    if (!voice.play(clip.id)) {
        std::printf("无法开始播放\n");
        voice.shutdown();
        return 1;
    }
    const double playStart = nowSeconds();
    double lastPos = 0.0;
    while (!gStopRequested && voice.playingId() != 0 && nowSeconds() - playStart < clip.seconds() + 5.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        lastPos = std::max(lastPos, voice.view().playSec);
        voice.update();
    }
    std::printf("播放：用了 %.2f 秒结束（传入位置 %.2f 秒）\n", nowSeconds() - playStart, lastPos);
    voice.shutdown();
    return 0;
}

/**
 * バーのドラッグを終わらせ、最後の値を返す（離したとき・タブを切り替えたとき・パネルを閉じたとき）。
 * 値を送って保存するのは呼び出し側。
 * @param panel パネル
 * @param state 今のマイクの状態（ドラッグしていないほうのバーの値に使う）
 * @param vad 判定の厳しさ（%）の書き込み先
 * @param grace 余韻（ms）の書き込み先
 * @return ドラッグしていたら true
 */
bool finishDrag(MicPanel& panel, const MicState& state, double& vad, double& grace) {
    if (!panel.dragging()) return false;
    panel.displayedNsValues(state, vad, grace);  // ドラッグ中のバーはその値、もう 1 本は今の値
    panel.pointerLeave();                         // ドラッグと押している見た目を終わらせる
    return true;
}

/**
 * --self-test: OpenVR も外部コマンドも使わずに、判定の関数を決まった入力で試す。
 * 自己修復のオーバーレイの判定（FindOverlay の結果の読み方）と、wpctl・pw-link・systemctl・pw-dump の出力の読み取り。
 * @return すべて合えば 0、1 つでも違えば 1
 */
int runSelfTest() {
    int failures = 0;
    int total = 0;
    /**
     * 1 つの確かめの結果を出す。
     * @param what 何を確かめたか
     * @param ok 合っていれば true
     */
    const auto expect = [&](const char* what, bool ok) {
        ++total;
        if (!ok) ++failures;
        std::printf("%s  %s\n", ok ? "通过  " : "未通过", what);
    };

    // 自己修復: FindOverlay の結果から、自分のオーバーレイがまだあるか
    expect("覆盖层：找到了，且是自己的句柄 → 存在",
           judgeOverlay(true, 0x1234, 0x1234) == OverlayHealth::Alive);
    expect("覆盖层：未找到（UnknownOverlay）→ 已消失",
           judgeOverlay(false, 0, 0x1234) == OverlayHealth::Missing);
    expect("覆盖层：找到了但是别人的句柄 → 键被别的东西持有",
           judgeOverlay(true, 0x9999, 0x1234) == OverlayHealth::Replaced);
    expect("覆盖层：自己没有句柄（上次重建失败）→ 按已消失处理并重建",
           judgeOverlay(true, 0x9999, 0) == OverlayHealth::Missing);
    expect("覆盖层：未找到，也没有句柄 → 已消失", judgeOverlay(false, 0, 0) == OverlayHealth::Missing);

    // wpctl settings の出力
    bool value = false;
    expect("wpctl: “Value: true (Saved: true)”→ true",
           parseSettingValue("Value: true (Saved: true)\n", value) && value);
    expect("wpctl: “Setting '...' not found”→ 无法读取",
           !parseSettingValue("Setting 'frame-mic.echo-cancel' not found\n", value));
    const std::string list =
        "Settings:\n\n- Id: frame-mic.echo-cancel\n  Value: false\t[Saved: false]\n\n"
        "- Id: frame-mic.noise-suppression\n  Default: false\n  Value: true\t[Saved: true]\n";
    bool echo = true;
    bool ns = false;
    expect("wpctl: 从列表中读取 回声消除 = false·噪声抑制 = true",
           parseSettingFromList(list, kEchoCancelKey, echo) && !echo &&
               parseSettingFromList(list, kNoiseSuppressionKey, ns) && ns);

    // systemctl --user is-enabled の出力
    expect("systemctl: not-found → 没有单元文件", parseAutostart("not-found\n") == Autostart::Missing);
    expect("systemctl: enabled → 已启用", parseAutostart("enabled\n") == Autostart::Enabled);

    // pw-link -l の出力（イヤホン: EQ → 出力。フィルターが入っているのでマイク使用中）
    const std::string links =
        "alsa_input.platform-sound.HiFi__Mic__source:capture_FL\n"
        "  |-> eq_capture:input_FL\n"
        "eq_source:capture_MONO\n"
        "  |-> alsa_loopback_stream.alsa_input.platform-sound.HiFi__Mic__source:input_MONO\n";
    MicState state;
    parseLinks(links, state);
    expect("pw-link: 麦克风 → EQ → 输出、使用中",
           state.linksKnown && state.inUse && state.chain.size() == 1 &&
               state.chain[0].kind == ChainStage::Kind::Eq);

    // pw-dump ns_capture の出力（ノードの id と、判定の厳しさ・余韻）
    // param 名に「)"」が入るので、区切り付きの生の文字列にする
    const std::string dump = R"json([{"id": 53, "type": "PipeWire:Interface:Node", "info": {
        "props": {"node.name": "ns_capture"},
        "params": {"Props": [{"volume": 1.0},
            {"params": ["noise_suppressor_mono:VAD Threshold (%)", 23.0,
                        "noise_suppressor_mono:VAD Grace Period (ms)", 500.0]}]}}}])json";
    const NsParams params = parseNsDump(dump);
    expect("pw-dump: ns_capture 的 id 53·23%·500ms",
           params.nodeKnown && params.nodeId == 53 && params.vadKnown && params.vad == 23.0 && params.graceKnown &&
               params.grace == 500.0);
    expect("噪声抑制强度：把超出范围的值收敛（150% → 99%、-5ms → 0ms、512ms → 510ms）",
           clampNsVad(150) == 99.0 && clampNsGrace(-5) == 0.0 && clampNsGrace(512) == 510.0);

    // 設定ファイル: タブ（と言語・ノイズ除去の強さ）を保存して読み直すと同じになる（一時ファイルで試して消す）
    {
        char path[] = "/tmp/frame-mic-tuner-selftest-XXXXXX";
        const int fd = ::mkstemp(path);
        bool ok = fd >= 0;
        if (fd >= 0) ::close(fd);
        Config saved;
        saved.language = Language::En;
        saved.tab = PanelTab::Fine;
        saved.hasNsParams = true;
        saved.nsVad = 30;
        saved.nsGrace = 600;
        std::string error;
        std::vector<std::string> warnings;
        Config loaded;
        ok = ok && saveConfig(path, saved, error) && loadConfig(path, loaded, warnings, error);
        expect("配置文件：保存标签页“精细调整”·English·30%/600ms 再读取后相同",
               ok && loaded.tab == PanelTab::Fine && loaded.language == Language::En && loaded.hasNsParams &&
                   loaded.nsVad == 30 && loaded.nsGrace == 600 && warnings.empty());
        saved.tab = PanelTab::Quick;
        ok = saveConfig(path, saved, error) && loadConfig(path, loaded, warnings, error);
        expect("配置文件：保存标签页“简单”再读取后相同", ok && loaded.tab == PanelTab::Quick);
        ::unlink(path);
        Config fresh;
        expect("配置文件：没有标签页时为“简单”", fresh.tab == PanelTab::Quick);
    }

    // バーのドラッグ: 細かく調整のタブで判定の厳しさのバーをつかんで動かし、タブを切り替える（= ドラッグを終わらせる）と、
    // 最後の値が返り、ドラッグは終わっている
    {
        FontSet fonts;
        fonts.load(kFontPath, kBoldFontPath);
        MicPanel panel(fonts);
        Config config;
        config.tab = PanelTab::Fine;
        MicState state;
        state.loaded = state.echoKnown = state.nsKnown = state.ns = true;
        state.nsParams.nodeKnown = state.nsParams.vadKnown = state.nsParams.graceKnown = true;
        state.nsParams.nodeId = 53;
        state.nsParams.vad = kNsVadDefault;
        state.nsParams.grace = kNsGraceDefault;
        panel.render(config, state, VoiceView());
        double x = 0.0;
        double y = 0.0;
        const bool found = panel.trackCenter(PanelAction::NsVadSlider, x, y);
        const PanelHit hit = panel.pointerDown(x, y, 0.0);  // 溝の真ん中を押す → 50% 前後へ飛ぶ
        panel.pointerMove(x + 60, y);                        // 右へドラッグ
        const bool draggingBefore = panel.dragging();
        double vad = 0.0;
        double grace = 0.0;
        const bool finished = finishDrag(panel, state, vad, grace);
        expect("拖动：按下轨道即开始拖动", found && hit.action == PanelAction::NsVadSlider && draggingBefore);
        expect("拖动：用切换标签页结束拖动时，返回最后的值（向右移动后的 60% 以上）和当前的保持时间",
               finished && vad >= 60 && vad <= kNsVadMax && grace == kNsGraceDefault);
        expect("拖动：结束之后不再处于拖动状态", !panel.dragging());
        double again = 0.0;
        expect("拖动：没有在拖动时什么都不返回", !finishDrag(panel, state, again, again));
        config.tab = PanelTab::Quick;
        panel.render(config, state, VoiceView());
        double qx = 0.0;
        double qy = 0.0;
        expect("标签页：简单标签页下不绘制滑块", !panel.trackCenter(PanelAction::NsVadSlider, qx, qy));
    }

    // 更新の帯: 「更新する」の 1 回目は確認の表示（「やめる」が出る）だけ、「やめる」で元に戻る。確認中の 2 回目で更新する
    {
        FontSet fonts;
        fonts.load(kFontPath, kBoldFontPath);
        MicPanel panel(fonts);
        const Config config;
        const MicState state;
        frame_updater::UpdateStatus update;
        update.state = frame_updater::UpdateState::Available;
        update.current = "0.2.0";
        update.latest = "9.9.9";
        update.installable = true;
        double x = 0.0;
        double y = 0.0;
        panel.render(config, state, VoiceView(), update);
        const bool noCancelFirst = !panel.buttonCenter(PanelAction::UpdateCancel, x, y);
        panel.buttonCenter(PanelAction::UpdateInstall, x, y);
        const PanelHit first = panel.pointerDown(x, y, 0.0);
        panel.pointerUp();
        panel.render(config, state, VoiceView(), update);
        const bool cancelShown = panel.buttonCenter(PanelAction::UpdateCancel, x, y);
        const PanelHit cancel = panel.pointerDown(x, y, 0.1);
        panel.pointerUp();
        panel.render(config, state, VoiceView(), update);
        const bool cancelGone = !panel.buttonCenter(PanelAction::UpdateCancel, x, y);
        expect("更新提示条：“更新”第一次不返回任何操作，只显示“取消”",
               noCancelFirst && first.action == PanelAction::None && cancelShown);
        expect("更新提示条：按“取消”后确认消失", cancel.action == PanelAction::UpdateCancel && cancelGone);
        panel.buttonCenter(PanelAction::UpdateInstall, x, y);
        panel.pointerDown(x, y, 0.2);
        panel.pointerUp();
        panel.render(config, state, VoiceView(), update);
        panel.buttonCenter(PanelAction::UpdateInstall, x, y);
        const PanelHit second = panel.pointerDown(x, y, 0.3);
        expect("更新提示条：确认中再按一次“更新”即开始更新", second.action == PanelAction::UpdateInstall);
    }

    std::printf("%d 项中有 %d 项未通过\n", total, failures);
    return failures == 0 ? 0 : 1;
}

/**
 * install.sh が frame-update.sh を置いた場所（$XDG_DATA_HOME か ~/.local/share の下）。
 * @return パス
 */
std::string updateScriptPath() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    return base + "/" + kUpdateAppName + "/frame-update.sh";
}

/**
 * 常駐のロックファイルのパス（$XDG_RUNTIME_DIR の下）。
 * @return パス
 */
std::string lockFilePath() {
    // Steam から・systemd から・SSH から起動しても同じ場所になるよう、XDG_RUNTIME_DIR が無ければ /run/user/<uid> を使う
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return std::string(runtime) + "/frame-mic-tuner.lock";
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime + "/frame-mic-tuner.lock";
    return "/tmp/frame-mic-tuner-" + std::to_string(::getuid()) + ".lock";
}

/**
 * 常駐のロックを取る。取れたら自分の PID を書いて、ファイルを開いたままにする（終われば OS がロックを外す）。
 * 取れなければ、ロックを持っている常駐側の PID を返す。
 * @param lockFd 取れたときのファイル（開いたままにする）の書き込み先
 * @param holderPid 取れなかったときの、常駐側の PID の書き込み先（読めなければ 0）
 * @return ロックを取れた（またはロックを使えないので、そのまま起動してよい）なら true
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[启动] 无法打开锁文件 %s，因此不检查重复启动，直接启动\n", path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[启动] 无法把 PID 写入锁文件\n");
        }
        lockFd = fd;
        return true;
    }
    // 常駐側がロックを取った直後で、まだ PID を書いていないことがあるので少し待って読み直す
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * このプロセスが systemd の frame-mic-tuner.service として起動されたか。
 * INVOCATION_ID だけでは決めない（Frame の Steam は steam.service で動いていて、＋から起動した子にも
 * INVOCATION_ID が引き継がれる）。自分の cgroup がこのサービスのものかで確かめる。
 * @return サービスとして起動されたなら true
 */
bool startedByOwnService() {
    const char* invocation = std::getenv("INVOCATION_ID");
    if (invocation == nullptr || invocation[0] == '\0') return false;
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    while (std::getline(cgroup, line)) {
        if (line.size() >= std::strlen("/frame-mic-tuner.service") &&
            line.compare(line.size() - std::strlen("/frame-mic-tuner.service"), std::string::npos,
                         "/frame-mic-tuner.service") == 0) {
            return true;
        }
    }
    return false;
}

/**
 * パネルのボタンの操作を実行する（マイク・自動起動はワーカーに頼み、言語はここで保存し、録音・再生は声のチェックへ）。
 * @param hit 押されたボタン
 * @param config 今の設定（書き換える）
 * @param configPath 設定ファイルのパス
 * @param worker ワーカー
 * @param voice 声のチェック
 * @param state 今のマイクの状態（録り始めたときの設定として履歴に残す）
 * @param view 最後に描いた声のチェックの状態（履歴の何件目かを id に直す）
 */
void handleAction(PanelHit hit, Config& config, const std::string& configPath, MicWorker& worker, VoiceCheck& voice,
                  const MicState& state, const VoiceView& view) {
    const PanelAction action = hit.action;
    switch (action) {
        case PanelAction::Earphone:  // プリセットは呼び出し側で扱う（書き込みが終わるまでカードの見た目を保つため）
        case PanelAction::Speaker:
        case PanelAction::TabQuick:  // タブも呼び出し側で扱う（ドラッグを終わらせてから切り替えるため）
        case PanelAction::TabFine: return;
        case PanelAction::EchoOn:
        case PanelAction::EchoOff:
            std::fprintf(stderr, "[操作] 回声消除 %s\n", action == PanelAction::EchoOn ? "开" : "关");
            worker.request({MicCommand::Kind::SetEcho, action == PanelAction::EchoOn});
            return;
        case PanelAction::NsOn:
        case PanelAction::NsOff:
            std::fprintf(stderr, "[操作] 噪声抑制 %s\n", action == PanelAction::NsOn ? "开" : "关");
            worker.request({MicCommand::Kind::SetNs, action == PanelAction::NsOn});
            return;
        case PanelAction::AutostartOn:
        case PanelAction::AutostartOff:
            std::fprintf(stderr, "[操作] 随 SteamVR 一起启动 %s\n", action == PanelAction::AutostartOn ? "开" : "关");
            worker.request({MicCommand::Kind::SetAutostart, action == PanelAction::AutostartOn});
            return;
        case PanelAction::LanguageJa:
        case PanelAction::LanguageEn:
        case PanelAction::LanguageZh: {
            const Language language = action == PanelAction::LanguageJa
                                          ? Language::Ja
                                          : (action == PanelAction::LanguageEn ? Language::En : Language::Zh);
            if (language == config.language) return;
            config.language = language;
            std::string error;
            if (!saveConfig(configPath, config, error)) std::fprintf(stderr, "[设置] 保存失败：%s\n", error.c_str());
            return;
        }
        case PanelAction::Record:
            if (voice.recording()) {
                std::fprintf(stderr, "[操作] 停止录音\n");
                voice.stopRecording();
            } else {
                std::fprintf(stderr, "[操作] 录音\n");
                voice.startRecording(state);
            }
            return;
        case PanelAction::Play: {
            if (hit.index < 0 || hit.index >= static_cast<int>(view.clips.size())) return;
            const uint64_t id = view.clips[hit.index]->id;
            if (voice.playingId() == id) {
                std::fprintf(stderr, "[操作] 停止播放\n");
                voice.stopPlayback();
            } else {
                std::fprintf(stderr, "[操作] 播放第 %d 条\n", hit.index + 1);
                voice.play(id);
            }
            return;
        }
        case PanelAction::NsVadSlider:  // ノイズ除去の強さは呼び出し側で扱う（ドラッグと間引きがあるため）
        case PanelAction::NsGraceSlider:
        case PanelAction::NsVadMinus:
        case PanelAction::NsVadPlus:
        case PanelAction::NsGraceMinus:
        case PanelAction::NsGracePlus:
        case PanelAction::NsReset:
        case PanelAction::Quit:            // 終了は呼び出し側で扱う
        case PanelAction::UpdateCheckNow:  // 更新の操作も呼び出し側で扱う（UpdateChecker を持っているため）
        case PanelAction::UpdateInstall:
        case PanelAction::UpdateRetry:
        case PanelAction::UpdateDismiss:
        case PanelAction::UpdateCancel:    // 「やめる」はパネルの中で確認を取り消すだけ
        case PanelAction::None: break;
    }
}

/**
 * オーバーレイとして常駐する。SteamVR が無ければ数秒おきに待ち、終了の知らせで静かに終わる。
 * すでに常駐していれば、そちらにパネルを開くよう知らせてすぐ終わる（VR_Init はしない）。
 * @param options コマンドライン
 * @return 終了コード
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    if (!acquireInstanceLock(lockFd, holderPid)) {
        // systemd（Restart=always）から起動されたのに常駐がいるときは、5 秒ごとにパネルが開き続けないよう
        // 知らせを送らずに、起動し直されない終了コードで静かに終わる
        if (startedByOwnService()) {
            std::fprintf(stderr, "[启动] 已经在常驻（PID %d）。本次是从服务启动，所以什么都不做直接退出\n",
                         static_cast<int>(holderPid));
            return kExitCodeUserQuit;
        }
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[启动] 已经在常驻（PID %d）。打开面板后退出\n",
                         static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[启动] 似乎已经在常驻，但没能发送通知（PID %d）\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    Config config = loadConfigOrDefault(options.configPath);
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    MicPanel panel(fonts);
    MicWorker worker;
    worker.start();
    // 保存したノイズ除去の強さがあれば、ノードが見つかりしだいかける（SteamOS は PipeWire の起動のたびに既定値に戻す）
    if (config.hasNsParams) worker.setDesiredNsParams(config.nsVad, config.nsGrace);
    VoiceCheck voice;
    VrOverlay vr;

    // 新しい版の確認・更新（vendor/frame-updater）。古い版から来て install-args が無いときの既定はオプションなし
    frame_updater::UpdaterConfig updaterConfig;
    updaterConfig.script = updateScriptPath();
    updaterConfig.app = kUpdateAppName;
    updaterConfig.repo = kUpdateRepo;
    updaterConfig.currentVersion = FRAME_MIC_TUNER_VERSION;
    updaterConfig.assetPattern = kUpdateAssetPattern;
    frame_updater::UpdateChecker updater(updaterConfig);
    uint64_t drawnUpdateRevision = updater.revision();
    /**
     * 版の行のボタンを扱う（UpdateChecker を持っているのでここで扱う）。
     * @param action 押されたボタン
     */
    const auto handleUpdateAction = [&](PanelAction action) {
        switch (action) {
            case PanelAction::UpdateCheckNow:
                std::fprintf(stderr, "[更新] 开始检查\n");
                updater.checkNow();
                return;
            case PanelAction::UpdateInstall:
            case PanelAction::UpdateRetry:
                std::fprintf(stderr, "[更新] 开始更新\n");
                if (!updater.install()) std::fprintf(stderr, "[更新] 无法开始\n");
                return;
            case PanelAction::UpdateDismiss:
                updater.dismiss();
                return;
            default:
                return;
        }
    };

    // SteamVR を待つ
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result = vr.connect(panel.width(), panel.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR 尚未启动，因此等待（每 3 秒重试）\n");
            } else {
                std::fprintf(stderr, "[VR] 连接失败：%s（3 秒后重试）\n", message.c_str());
            }
            lastMessage = message;
        }
        sleepInterruptible(3.0);
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[启动] 收到了打开面板的通知，但尚未连接到 SteamVR\n");
        }
    }
    if (gStopRequested) {
        worker.stop();
        if (lockFd >= 0) ::close(lockFd);
        return 0;
    }
    std::fprintf(stderr, "[VR] 已连接到 SteamVR\n");
    MicState state;
    uint64_t drawnVersion = worker.snapshot(state);
    {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
        // パネルにも最初の 1 枚（読み込み中）を入れておく（初めて選ばれたとき、画像が無い瞬間を作らない）
        panel.render(config, state, VoiceView(), updater.status());
        vr.submitPanel(panel.toRgba().data());
        vr.logOverlayState("刚连接后");
    }

    VoiceView voiceView;
    double lastVoiceFrame = 0.0;
    // ノイズ除去の強さ: バーのドラッグ中は 100ms おきに最後の値だけ送り、離したときに必ず 1 回送って保存する
    double lastNsSendAt = -1.0;
    double sentVad = -1.0;
    double sentGrace = -1.0;
    // プリセット: 書き込みと読み直しが終わるまで（受付番号まで終わるまで）、押したカードを選択中の見た目で保つ
    uint64_t presetTicket = 0;
    /**
     * プリセットを書き込む。イヤホン = エコー除去オフ・ノイズ除去オフ、スピーカー = エコー除去オン・ノイズ除去オフ。
     * ワーカーが 2 つ書いてから 1 回だけ読み直す。ノイズ除去のバーの値は変えない。
     * @param action Earphone か Speaker
     */
    const auto applyPreset = [&](PanelAction action) {
        const bool speaker = action == PanelAction::Speaker;
        std::fprintf(stderr, "[操作] %s\n", speaker ? "扬声器（回声消除开·噪声抑制关）"
                                                     : "耳机（回声消除关·噪声抑制关）");
        presetTicket = worker.request({MicCommand::Kind::SetPreset, speaker});
        panel.holdPreset(action, nowSeconds() + 8.0);  // 書き込みが詰まっても、8 秒で実際の値の表示に戻す
    };
    /**
     * ノイズ除去の強さをワーカーに頼み（前に送った値と同じなら頼まない）、読み直しが追いつくまで表示を保つ。
     * @param vad 判定の厳しさ（%）
     * @param grace 余韻（ms）
     * @param save 設定ファイルにも保存するか（離したとき・− / ＋・標準に戻す）
     */
    const auto sendNsParams = [&](double vad, double grace, bool save) {
        vad = clampNsVad(vad);
        grace = clampNsGrace(grace);
        if (vad != sentVad || grace != sentGrace) {
            MicCommand command {MicCommand::Kind::SetNsParams};
            command.vad = vad;
            command.grace = grace;
            worker.request(command);
            sentVad = vad;
            sentGrace = grace;
            lastNsSendAt = nowSeconds();
        }
        panel.holdNsValues(vad, grace, nowSeconds() + 1.5);
        if (!save) return;
        config.hasNsParams = true;
        config.nsVad = vad;
        config.nsGrace = grace;
        std::string error;
        if (saveConfig(options.configPath, config, error)) {
            std::fprintf(stderr, "[噪声抑制] 已保存判定严格度 %.0f%%·保持时间 %.0fms\n", vad, grace);
        } else {
            std::fprintf(stderr, "[设置] 保存失败：%s\n", error.c_str());
        }
    };
    /**
     * ノイズ除去の強さのボタン・バーの押下を扱う。
     * @param action 押されたもの
     * @return 扱ったら true（ほかのボタンなら false）
     */
    const auto handleNsAction = [&](PanelAction action) {
        double vad = 0.0;
        double grace = 0.0;
        panel.displayedNsValues(state, vad, grace);  // バーを押した直後は、押したところの値
        switch (action) {
            case PanelAction::NsVadSlider:
            case PanelAction::NsGraceSlider: sendNsParams(vad, grace, false); return true;
            case PanelAction::NsVadMinus: sendNsParams(vad - kNsVadStep, grace, true); return true;
            case PanelAction::NsVadPlus: sendNsParams(vad + kNsVadStep, grace, true); return true;
            case PanelAction::NsGraceMinus: sendNsParams(vad, grace - kNsGraceStep, true); return true;
            case PanelAction::NsGracePlus: sendNsParams(vad, grace + kNsGraceStep, true); return true;
            case PanelAction::NsReset:
                std::fprintf(stderr, "[操作] 把噪声抑制强度恢复默认\n");
                sendNsParams(kNsVadDefault, kNsGraceDefault, true);
                return true;
            default: return false;
        }
    };
    /**
     * ポインターを離した（パネルから外れた）とき。バーをドラッグしていたら、最後の値を必ず送って保存する。
     * @param leave パネルから外れたなら true
     * @return 描き直しが要るなら true
     */
    const auto releasePointer = [&](bool leave) {
        double vad = 0.0;
        double grace = 0.0;
        const bool wasDragging = finishDrag(panel, state, vad, grace);
        const bool changed = leave ? panel.pointerLeave() : panel.pointerUp();
        if (wasDragging) sendNsParams(vad, grace, true);
        return changed || wasDragging;
    };
    /**
     * タブを切り替えて保存する。バーをドラッグしていたら、先に最後の値を送って保存する。
     * @param tab 切り替え先
     */
    const auto switchTab = [&](PanelTab tab) {
        double vad = 0.0;
        double grace = 0.0;
        if (finishDrag(panel, state, vad, grace)) sendNsParams(vad, grace, true);
        if (config.tab == tab) return;
        config.tab = tab;
        std::fprintf(stderr, "[操作] 标签页：%s\n", tab == PanelTab::Quick ? "简单" : "精细调整");
        std::string error;
        if (!saveConfig(options.configPath, config, error)) std::fprintf(stderr, "[设置] 保存失败：%s\n", error.c_str());
    };
    bool dirty = true;
    bool wasVisible = false;
    bool firstSubmit = true;
    bool userQuit = false;
    double nextOverlayCheck = nowSeconds() + kOverlayCheckSec;
    /**
     * 自己修復: 自分のダッシュボードのオーバーレイがまだ SteamVR にあるかを確かめ（FindOverlay 1 回）、
     * 消えていれば作り直して、サムネイルとパネルの画像を送り直す。作り直せなければ次の確かめ（3 秒後）で再試行する。
     */
    const auto checkOverlayNow = [&]() {
        nextOverlayCheck = nowSeconds() + kOverlayCheckSec;
        if (vr.ensureOverlay() != OverlayRepair::Repaired) return;
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
        drawnVersion = worker.snapshot(state);
        drawnUpdateRevision = updater.revision();
        panel.render(config, state, voiceView, updater.status());
        vr.submitPanel(panel.toRgba().data());
        vr.logOverlayState("重建之后");
        dirty = true;
    };
    while (!gStopRequested && !userQuit) {
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[启动] 收到第二次启动的通知，打开面板\n");
            checkOverlayNow();  // 開く前に、オーバーレイが消えていないか確かめる（消えていれば作り直してから開く）
            vr.showPanel();
        }
        // 自己修復: 閉じている間も 3 秒おきに、自分のオーバーレイがまだ SteamVR にあるかを確かめる
        if (nowSeconds() >= nextOverlayCheck) checkOverlayNow();
        const VrEvents events = vr.pollEvents();
        // SteamVR 自体の終了（VREvent_Quit）は終了コード 0。ダッシュボードのアイコンの「閉じる」
        // （VREvent_OverlayClosed）はユーザーの終了なので、「終了」と同じく終了コード 3
        if (events.quit) break;
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] 因仪表盘的“关闭”而退出\n");
            userQuit = true;
            break;
        }
        if (!vr.steamVrAlive()) {
            std::fprintf(stderr, "[VR] vrserver 已消失，因此退出\n");
            break;
        }

        // 新しい版の確認・更新の状態を進める（見えていない間も。GitHub に行くのはスクリプトのキャッシュが切れたときだけ）
        updater.tick(config.updateCheck);
        if (updater.revision() != drawnUpdateRevision) dirty = true;

        // パネルが見えている間だけ、ワーカーが 1 秒ごとに読み直す（見えていない間は何も実行しない）
        const bool visible = vr.panelVisible();
        worker.setActive(visible);
        // パネルが閉じたら、録音はすぐ止める（見えないところで録らない）。再生も止めて、PipeWire のストリームを片付ける
        if (!visible && wasVisible) {
            if (voice.busy()) std::fprintf(stderr, "[声音] 面板已关闭，因此停止录音和播放\n");
            voice.shutdown();
            // バーをドラッグしたまま閉じたときも、最後の値を送って保存する
            double vad = 0.0;
            double grace = 0.0;
            if (finishDrag(panel, state, vad, grace)) sendNsParams(vad, grace, true);
        }
        if (options.debugRecordOnOpen && visible && !wasVisible) {
            std::fprintf(stderr, "[声音] 确认用：面板已打开，因此开始录音\n");
            voice.startRecording(state);
        }
        dirty |= voice.update();

        for (const PointerInput& input : events.pointer) {
            switch (input.type) {
                case PointerInput::Type::Move: dirty |= panel.pointerMove(input.x, input.y); break;
                case PointerInput::Type::Down: {
                    const PanelHit hit = panel.pointerDown(input.x, input.y, nowSeconds());
                    if (hit.action == PanelAction::Quit) {
                        std::fprintf(stderr, "[VR] 因面板的“退出”而退出\n");
                        userQuit = true;
                    } else if (hit.action == PanelAction::Earphone || hit.action == PanelAction::Speaker) {
                        applyPreset(hit.action);
                    } else if (hit.action == PanelAction::TabQuick || hit.action == PanelAction::TabFine) {
                        switchTab(hit.action == PanelAction::TabQuick ? PanelTab::Quick : PanelTab::Fine);
                    } else if (hit.action == PanelAction::UpdateCheckNow || hit.action == PanelAction::UpdateInstall ||
                               hit.action == PanelAction::UpdateRetry || hit.action == PanelAction::UpdateDismiss) {
                        handleUpdateAction(hit.action);
                    } else if (!handleNsAction(hit.action)) {
                        handleAction(hit, config, options.configPath, worker, voice, state, voiceView);
                    }
                    dirty = true;
                    break;
                }
                case PointerInput::Type::Up: dirty |= releasePointer(false); break;
                case PointerInput::Type::Leave: dirty |= releasePointer(true); break;
            }
        }
        if (userQuit) break;
        // プリセットの書き込みと読み直しが終わったら、カードの見た目を実際の値に戻す
        if (panel.presetHeld() && worker.completed() >= presetTicket) {
            panel.clearPresetHold();
            dirty = true;
        }
        // バーのドラッグ中は、値が変わっていれば 100ms おきに送る（連打しない）
        if (panel.dragging() && nowSeconds() - lastNsSendAt >= 0.1) {
            double vad = 0.0;
            double grace = 0.0;
            panel.displayedNsValues(state, vad, grace);
            sendNsParams(vad, grace, false);
        }
        dirty |= panel.tick(nowSeconds());  // 「もう一度押すと終了」の期限切れ
        if (worker.version() != drawnVersion) dirty = true;
        // 録音中・再生中はメーターと再生位置を動かすため、1 秒に 15 回描き直す
        if (voice.busy() && nowSeconds() - lastVoiceFrame >= kVoiceFrameSec) dirty = true;

        // パネルは見えているときだけ、変化があったときだけ描く
        if (visible && (dirty || !wasVisible)) {
            const uint64_t version = worker.snapshot(state);
            if (version != drawnVersion) logState(state);
            drawnVersion = version;
            voiceView = voice.view();
            lastVoiceFrame = nowSeconds();
            drawnUpdateRevision = updater.revision();
            panel.render(config, state, voiceView, updater.status());
            vr.submitPanel(panel.toRgba().data());
            dirty = false;
            if (firstSubmit) {
                firstSubmit = false;
                vr.logOverlayState("首次绘制面板之后");
            }
        }
        wasVisible = visible;

        // 待つ: パネルが見えている間はポインターに素早く応えるため短く
        sleepInterruptible(visible ? kPanelPollSec : kClosedPollSec);
    }

    // SIGTERM / SIGINT・SteamVR の終了・vrserver の消滅・「終了」のどれでも同じ終了処理を通す
    voice.shutdown();  // 録音・再生を止める（録った音はメモリごと消える）
    vr.shutdown();
    worker.stop();
    std::fprintf(stderr, "[VR] 已退出\n");
    if (lockFd >= 0) ::close(lockFd);
    // ユーザーが終了したときは、systemd（Restart=always）に起動し直させないよう決まった終了コードにする
    return userQuit ? kExitCodeUserQuit : 0;
}

}  // namespace

/**
 * エントリーポイント。
 * @param argc 引数の数
 * @param argv 引数
 * @return 終了コード
 */
int main(int argc, char** argv) {
    // journald でも行ごとにすぐ出るようにする
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    installSignalHandlers();

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("frame-mic-tuner %s\n", FRAME_MIC_TUNER_VERSION); return 0;
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::Probe: return VrOverlay::probe();
        case Options::Mode::SwitchAway: return VrOverlay::switchAway(options.switchAwaySec);
        case Options::Mode::TestRecord: return runTestRecord(options);
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::SelfTest: return runSelfTest();
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
