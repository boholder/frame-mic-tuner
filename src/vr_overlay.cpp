// OpenVR との接続とオーバーレイの実装。
#include "vr_overlay.h"

#include "openvr.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

constexpr const char* kDashboardKey = "sasaken.frame-mic-tuner";
constexpr const char* kDashboardName = "Mic";
// 1200px を 2.8m に（1px あたりの大きさは v2 の 1024px / 2.4m とほぼ同じ。高さは約 1.52m）
constexpr float kDashboardWidthM = 2.8f;
// 終了時、オーバーレイを消してから VR_Shutdown まで待つ時間（90Hz で約 36 フレーム）
constexpr int kShutdownWaitMs = 400;

/**
 * オーバーレイのエラー名を返す。
 * @param error エラー
 * @return 名前（例: VROverlayError_None）
 */
const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

/**
 * オーバーレイのエラーを、成功以外なら標準エラーに出す。
 * @param what 何をしたときか
 * @param error エラー
 * @return 成功なら true
 */
bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[VR] %s 失败: %s\n", what, overlayErrorName(error));
    return false;
}

/**
 * 終了処理の 1 手順の結果を、成功でも失敗でもログに出す（あとで順番と結果を確かめるため）。
 * @param what 何をしたか
 * @param error 戻り値
 */
void logShutdownStep(const char* what, vr::EVROverlayError error) {
    std::fprintf(stderr, "[VR] 退出处理 %s -> %s\n", what, overlayErrorName(error));
}

}  // namespace

OverlayHealth judgeOverlay(bool findOk, uint64_t found, uint64_t own) {
    // 見つからない（UnknownOverlay など）か、自分がハンドルを持っていない = 消えている
    if (!findOk || own == 0) return OverlayHealth::Missing;
    return found == own ? OverlayHealth::Alive : OverlayHealth::Replaced;
}

VrOverlay::VrOverlay() = default;

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int width, int height, std::string& message) {
    if (connected_) return ConnectResult::Ok;

    // 1) Background 型で「SteamVR が動いているか」だけ確かめる（動いていなければ起動させない）
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();

    // 2) オーバーレイ型でつなぎ直す
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;

    // 3) Vulkan（OpenVR が要求する拡張と、HMD の GPU で作る）
    std::string vkMessage;
    if (!vulkan_.init(vkMessage)) {
        message = "Vulkan 准备失败: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    // 4) ダッシュボードのパネルとサムネイル
    panelWidth_ = width;
    panelHeight_ = height;
    if (!createDashboardOverlay(message)) {
        shutdown();
        return ConnectResult::Error;
    }

    if (!panelTexture_.create(vulkan_, width, height, vkMessage)) {
        message = "无法创建面板纹理: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    vrserverPid_ = findVrserverPid();
    lastPanelError_.clear();
    return ConnectResult::Ok;
}

bool VrOverlay::createDashboardOverlay(std::string& message) {
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError createError =
        overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay(%s) -> %s\n", kDashboardKey, overlayErrorName(createError));
    if (createError != vr::VROverlayError_None) {
        message = std::string("CreateDashboardOverlay: ") + overlayErrorName(createError);
        return false;
    }
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;

    checkOverlay("SetOverlayWidthInMeters", overlay->SetOverlayWidthInMeters(main, kDashboardWidthM));
    checkOverlay("SetOverlayInputMethod", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    // マウス座標を画像の px にそろえる
    const vr::HmdVector2_t scale = {{static_cast<float>(panelWidth_), static_cast<float>(panelHeight_)}};
    checkOverlay("SetOverlayMouseScale", overlay->SetOverlayMouseScale(main, &scale));
    // ダッシュボードの下のアイコンにホバーしたとき「閉じる」を出す。押されると VREvent_OverlayClosed が届く
    const vr::EVROverlayError closeError = overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true);
    bool closeEnabled = false;
    const vr::EVROverlayError readError =
        overlay->GetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, &closeEnabled);
    std::fprintf(stderr, "[VR] SetOverlayFlag(EnableControlBarClose) -> %s（回读: %s, %s）\n",
                 overlayErrorName(closeError), overlayErrorName(readError), closeEnabled ? "true" : "false");
    return true;
}

OverlayRepair VrOverlay::ensureOverlay() {
    if (!connected_) return OverlayRepair::Ok;
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t found = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError findError = overlay->FindOverlay(kDashboardKey, &found);
    const OverlayHealth health = judgeOverlay(findError == vr::VROverlayError_None, found, dashboardHandle_);
    if (health == OverlayHealth::Alive) {
        if (repairFailing_) std::fprintf(stderr, "[VR] 自修复: 覆盖层已恢复\n");
        repairFailing_ = false;
        return OverlayRepair::Ok;
    }

    // 消えている（または別のハンドルになっている）。理由をログに出して作り直す
    char reason[160];
    if (health == OverlayHealth::Missing) {
        std::snprintf(reason, sizeof(reason), "FindOverlay(%s) -> %s", kDashboardKey, overlayErrorName(findError));
    } else {
        std::snprintf(reason, sizeof(reason), "FindOverlay(%s) 的句柄不同（自己 %llu，当前 %llu）", kDashboardKey,
                      static_cast<unsigned long long>(dashboardHandle_), static_cast<unsigned long long>(found));
    }
    if (!repairFailing_) {
        std::fprintf(stderr, "[VR] 自修复: 仪表盘的覆盖层已从 SteamVR 消失（%s）。将重新创建\n",
                     reason);
    }
    // 古いハンドル（自分のもの）を片付ける。消えている前提なので、エラーは無視する。
    // 別のハンドルがキーを持っていても、それには触らない（自分のハンドルだけを消す）
    if (dashboardHandle_ != 0) {
        overlay->ClearOverlayTexture(dashboardHandle_);
        if (thumbnailHandle_ != 0) overlay->ClearOverlayTexture(thumbnailHandle_);
        overlay->DestroyOverlay(dashboardHandle_);
    }
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;

    std::string message;
    if (!createDashboardOverlay(message)) {
        // KeyInUse（別のプロセスが同じキーを持っている）などは、相手を消さずに次の確かめで再試行する
        if (!repairFailing_ || message != lastRepairMessage_) {
            std::fprintf(stderr, "[VR] 自修复: 无法重新创建（%s）。将在下次检查时重试\n", message.c_str());
        }
        repairFailing_ = true;
        lastRepairMessage_ = message;
        return OverlayRepair::Failed;
    }
    repairFailing_ = false;
    lastRepairMessage_.clear();
    lastPanelError_.clear();
    std::fprintf(stderr, "[VR] 自修复: 已重新创建仪表盘的覆盖层\n");
    return OverlayRepair::Repaired;
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] 开始退出处理\n");

    // 1) テクスチャを外す（コンポジタがこちらの画像を参照しないようにする）
    if (dashboardHandle_ != 0) {
        logShutdownStep("ClearOverlayTexture(面板)", overlay->ClearOverlayTexture(dashboardHandle_));
        logShutdownStep("ClearOverlayTexture(缩略图)", overlay->ClearOverlayTexture(thumbnailHandle_));
    }
    // 2) オーバーレイを消す（サムネイルはパネルと一緒に消える）
    if (dashboardHandle_ != 0) logShutdownStep("DestroyOverlay(面板)", overlay->DestroyOverlay(dashboardHandle_));
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;

    // 3) コンポジタが数フレーム回って、外したテクスチャを手放すのを待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    std::fprintf(stderr, "[VR] 退出处理已等待 %dms\n", kShutdownWaitMs);

    // 4) OpenVR を閉じる（Vulkan の画像を壊すのはこの後、という OpenVR の決まり）
    vr::VR_Shutdown();
    connected_ = false;
    std::fprintf(stderr, "[VR] 退出处理已完成 VR_Shutdown\n");

    // 5) Vulkan の画像とデバイスを壊す
    thumbnailTexture_.destroy();
    panelTexture_.destroy();
    vulkan_.destroy();
    std::fprintf(stderr, "[VR] 退出处理已清理 Vulkan\n");
}

VrEvents VrOverlay::pollEvents() {
    VrEvents result;
    if (!connected_) return result;
    vr::VREvent_t event {};
    while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) result.quit = true;
    }
    if (dashboardHandle_ != 0) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // マウス座標は左下が原点なので、上が原点になるよう反転する
            const double x = event.data.mouse.x;
            const double y = panelHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Down, x, y});
                    }
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    }
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                // ダッシュボードのアイコンにホバーしたときの「閉じる」（VROverlayFlags_EnableControlBarClose）。
                // SteamVR 自体の終了（VRSystem 側の VREvent_Quit）とは別のイベント
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] 仪表盘的“关闭”被按下\n");
                    result.closeRequested = true;
                    break;
                case vr::VREvent_OverlayShown: std::fprintf(stderr, "[VR] 面板已打开\n"); break;
                case vr::VREvent_OverlayHidden: std::fprintf(stderr, "[VR] 面板已关闭\n"); break;
                default: break;
            }
        }
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] 收到来自 SteamVR 的退出通知\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    return result;
}

bool VrOverlay::steamVrAlive() const {
    if (vrserverPid_ <= 0) return true;  // 確かめられないときは生きている扱い
    return ::kill(vrserverPid_, 0) == 0 || errno == EPERM;
}

bool VrOverlay::panelVisible() const {
    return connected_ && dashboardHandle_ != 0 && vr::VROverlay()->IsOverlayVisible(dashboardHandle_);
}

void VrOverlay::showPanel() {
    if (!connected_ || dashboardHandle_ == 0) return;
    // 戻り値は無い。開けたかは、あとの IsOverlayVisible / VREvent_OverlayShown で分かる
    vr::VROverlay()->ShowDashboard(kDashboardKey);
    std::fprintf(stderr, "[VR] 已调用 ShowDashboard(%s)\n", kDashboardKey);
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[Vulkan] 无法创建缩略图纹理: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] 无法发送缩略图: %s\n", message.c_str());
        return false;
    }
    return true;
}

bool VrOverlay::submitPanel(const uint8_t* rgba) {
    if (!connected_ || dashboardHandle_ == 0 || !panelTexture_.ready()) return false;
    std::string message;
    const bool ok = panelTexture_.update(dashboardHandle_, rgba, message);
    // 同じエラーを毎回出さない
    if (message != lastPanelError_) {
        if (!ok) std::fprintf(stderr, "[VR] 无法发送面板: %s\n", message.c_str());
        lastPanelError_ = message;
    }
    return ok;
}

void VrOverlay::logOverlayState(const char* when) const {
    if (!connected_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t found = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError findError = overlay->FindOverlay(kDashboardKey, &found);
    uint32_t w = 0;
    uint32_t h = 0;
    const vr::EVROverlayError sizeError = overlay->GetOverlayTextureSize(dashboardHandle_, &w, &h);
    uint32_t tw = 0;
    uint32_t th = 0;
    const vr::EVROverlayError thumbError = overlay->GetOverlayTextureSize(thumbnailHandle_, &tw, &th);
    std::fprintf(stderr,
                 "[VR] 检查（%s）: FindOverlay(%s) -> %s（同一句柄: %s） 面板图像 %ux%u（%s） "
                 "缩略图图像 %ux%u（%s） 面板显示中: %s 仪表盘: %s\n",
                 when, kDashboardKey, overlayErrorName(findError), found == dashboardHandle_ ? "是" : "否", w, h,
                 overlayErrorName(sizeError), tw, th, overlayErrorName(thumbError),
                 overlay->IsOverlayVisible(dashboardHandle_) ? "是" : "否",
                 overlay->IsDashboardVisible() ? "开" : "关");
}

int VrOverlay::probe() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        std::printf("无法连接 SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    int code = 1;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError findError = overlay ? overlay->FindOverlay(kDashboardKey, &handle)
                                                  : vr::VROverlayError_RequestFailed;
    std::printf("FindOverlay(%s) -> %s\n", kDashboardKey, overlay ? overlayErrorName(findError) : "没有 IVROverlay");
    if (overlay != nullptr && findError == vr::VROverlayError_None) {
        code = 0;
        char name[128] = {};
        overlay->GetOverlayName(handle, name, sizeof(name), nullptr);
        bool closeFlag = false;
        overlay->GetOverlayFlag(handle, vr::VROverlayFlags_EnableControlBarClose, &closeFlag);
        float widthM = 0.0f;
        overlay->GetOverlayWidthInMeters(handle, &widthM);
        std::printf("  名称: %s  宽度: %.2fm  关闭按钮: %s  面板显示中: %s  仪表盘: %s\n", name, widthM,
                    closeFlag ? "有" : "无", overlay->IsOverlayVisible(handle) ? "是" : "否",
                    overlay->IsDashboardVisible() ? "开" : "关");
        uint32_t w = 0;
        uint32_t h = 0;
        const vr::EVROverlayError sizeError = overlay->GetOverlayTextureSize(handle, &w, &h);
        std::printf("  GetOverlayTextureSize -> %s（%ux%u）\n", overlayErrorName(sizeError), w, h);
        // GetOverlayImageData は使わない: 2026-09-27 に試すと、Vulkan のテクスチャが入ったオーバーレイに対して
        // 呼んだ側（このプロセス）が SIGSEGV で落ちた（SteamVR 側は無事）。画像が入ったことは大きさで確かめる
    }
    vr::VR_Shutdown();
    return code;
}

int VrOverlay::switchAway(double seconds) {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        std::printf("无法连接 SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::VR_Shutdown();
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        std::printf("无法以覆盖层模式连接: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    constexpr const char* kAwayKey = "sasaken.frame-mic-tuner.probe-away";
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError createError = overlay->CreateDashboardOverlay(kAwayKey, "Probe", &main, &thumbnail);
    std::printf("CreateDashboardOverlay(%s) -> %s\n", kAwayKey, overlayErrorName(createError));
    int code = 1;
    if (createError == vr::VROverlayError_None) {
        vr::VROverlayHandle_t mic = vr::k_ulOverlayHandleInvalid;
        overlay->FindOverlay(kDashboardKey, &mic);
        const auto micVisible = [&]() {
            return mic != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(mic) ? "是" : "否";
        };
        std::printf("切换前: Mic 面板显示中: %s\n", micVisible());
        // 画像の無いオーバーレイにはダッシュボードが切り替わらなかったので、アイコンの PNG を入れておく
        // （SetOverlayRaw の共有メモリは使わない。ファイルから読ませる）
        char exe[4096] = {};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        std::string icon = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string();
        icon = icon.substr(0, icon.find_last_of('/')) + "/../contrib/icons/frame-mic-tuner-256.png";
        char resolved[4096] = {};
        if (::realpath(icon.c_str(), resolved) != nullptr) {
            std::printf("SetOverlayFromFile(%s) -> %s\n", resolved,
                        overlayErrorName(overlay->SetOverlayFromFile(main, resolved)));
        }
        overlay->SetOverlayWidthInMeters(main, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        overlay->ShowDashboard(kAwayKey);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        std::printf("切换后: Mic 面板显示中: %s  临时覆盖层显示中: %s\n", micVisible(),
                    overlay->IsOverlayVisible(main) ? "是" : "否");
        std::printf("DestroyOverlay -> %s\n", overlayErrorName(overlay->DestroyOverlay(main)));
        code = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    vr::VR_Shutdown();
    return code;
}
