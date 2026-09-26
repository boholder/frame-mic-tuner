// 画面に出す文言の表の中身。
#include "i18n.h"

#include <cstdlib>
#include <fstream>

namespace {

/**
 * Steam の言語設定を読む。~/.steam/registry.vdf の最初の "language" の値（"japanese" など）。
 * @return 値。読めなければ空
 */
std::string steamLanguage() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    std::ifstream file(std::string(home) + "/.steam/registry.vdf");
    std::string line;
    while (std::getline(file, line)) {
        // 形式: <タブ>"language"<タブ>"japanese"
        const std::string key = "\"language\"";
        const size_t at = line.find(key);
        if (at == std::string::npos) continue;
        const size_t open = line.find('"', at + key.size());
        const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) return "";
        return line.substr(open + 1, close - open - 1);
    }
    return "";
}

/**
 * ロケールの環境変数（LC_ALL → LC_MESSAGES → LANG の順で最初に空でないもの）が日本語か。
 * @return 日本語なら true
 */
bool localeIsJapanese() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] != '\0') return std::string(value).rfind("ja", 0) == 0;
    }
    return false;
}

/**
 * システム言語を調べる（systemLanguage の本体）。
 * @return 言語
 */
Language detectSystemLanguage() {
    const std::string steam = steamLanguage();
    if (!steam.empty()) return steam == "japanese" ? Language::Ja : Language::En;
    return localeIsJapanese() ? Language::Ja : Language::En;
}

const UiText kJapanese = {
    "マイク", "使用中", "未使用", "読み込み中…",
    "イヤホン", "スピーカー",
    "小さな音まで届く", "スピーカーの音を消す",
    "エコー除去", "スピーカーの音を消す",
    "ノイズ除去", "口の音など小さい音も消える",
    "オン", "オフ",
    "つながり", "マイク", "音質補正", "エコー除去", "ノイズ除去", "アプリへ",
    "使っていないので処理はお休み中", "つながりを読めません",
    "声のチェック", "アプリに届く音を録って聞き比べ",
    "録音", "停止", "録音中", "秒", "%.1f 秒・残り %.1f 秒",
    "「録音」を押すと、最大 10 秒録ります", "まだ録音はありません",
    "＋ノイズ除去", "設定不明",
    "言語", "SteamVR と一緒に起動",
    "自動起動は準備されていません（./install.sh を実行してください）", "自動起動の状態を読めません",
    "終了", "もう一度押すと終了",
    "押すとすぐ切り替わり、再起動後も残ります。録音はメモリの中だけで、終了すると消えます",
    "読み取りに失敗（wpctl）", "切り替えの仕組みが入っていません（./install.sh のあと再起動）",
    "つながりの読み取りに失敗（pw-link）", "切り替えに失敗（wpctl）", "自動起動の切り替えに失敗（systemctl）",
    "録音を始められません（PipeWire）", "再生できません（PipeWire）",
    "判定の厳しさ", "低いほど声以外の音も通る", "余韻", "声のあと音を通す時間",
    "オフの間は効きません", "標準に戻す",
    "ノイズ除去の強さを変えられません（pw-cli）",
    "どこで音を聞いてる？", "選ぶとおすすめの設定になります",
    "イヤホン・ヘッドホン", "Frame のスピーカー",
    "エコー除去 オフ", "エコー除去 オン",
    "細かく調整",
    "今は細かく調整した設定です", "ノイズ除去 オフ",
    "エコー除去の切り替えに失敗（wpctl）", "ノイズ除去の切り替えに失敗（wpctl）",
    "かんたん", "細かく調整を見る →", "標準は 23%・500ms（SteamOS の値）",
};

const UiText kEnglish = {
    "Mic", "In use", "Not in use", "Loading…",
    "Earphones", "Speaker",
    "Quiet sounds come through", "Keeps speaker sound out of the mic",
    "Echo cancel", "Removes speaker sound",
    "Noise filter", "Also cuts quiet mouth sounds",
    "On", "Off",
    "Signal path", "Mic", "EQ", "Echo cancel", "Noise filter", "To apps",
    "Not in use, filters are resting", "Can't read the path",
    "Voice check", "Record what apps hear, then compare",
    "Record", "Stop", "Recording", "s", "%.1f s · %.1f s left",
    "Press Record to capture up to 10 s", "No recordings yet",
    " + noise filter", "Unknown setting",
    "Language", "Start with SteamVR",
    "Autostart is not installed (run ./install.sh)", "Can't read the autostart state",
    "Quit", "Press again to quit",
    "Applies instantly and survives restarts. Recordings stay in memory and vanish on quit",
    "Read failed (wpctl)", "Mic switch is not installed (run ./install.sh, then reboot)",
    "Path read failed (pw-link)", "Switch failed (wpctl)", "Autostart change failed (systemctl)",
    "Can't start recording (PipeWire)", "Can't play (PipeWire)",
    "Strictness", "Lower lets more through", "Hold", "Sound kept after speech",
    "No effect while off", "Default",
    "Can't change the noise filter (pw-cli)",
    "How are you listening?", "Sets the recommended settings",
    "Earphones / headphones", "Frame speakers",
    "Echo cancel: off", "Echo cancel: on",
    "Fine-tune",
    "Using fine-tuned settings", "Noise filter: off",
    "Echo cancel switch failed (wpctl)", "Noise filter switch failed (wpctl)",
    "Quick", "Open Fine-tune →", "Default: 23% · 500 ms (SteamOS)",
};

}  // namespace

const UiText& uiText(Language language) {
    return language == Language::En ? kEnglish : kJapanese;
}

Language systemLanguage() {
    static const Language cached = detectSystemLanguage();
    return cached;
}

const char* languageCode(Language language) {
    return language == Language::En ? "en" : "ja";
}

bool parseLanguage(const std::string& code, Language& language) {
    if (code == "ja") {
        language = Language::Ja;
        return true;
    }
    if (code == "en") {
        language = Language::En;
        return true;
    }
    return false;
}
