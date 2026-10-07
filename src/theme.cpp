// 色の定義とコントラスト比の計算の実装。
#include "theme.h"

#include <cmath>
#include <cstdio>

namespace {

/**
 * sRGB の 1 チャンネルを線形の値にする（WCAG 2.x の式）。
 * @param c 0〜1
 * @return 線形の値
 */
double linearChannel(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/**
 * 色を #rrggbb にする（表示用）。
 * @param c 色
 * @return 文字列
 */
std::string hexText(Color c) {
    char text[16];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", static_cast<int>(std::lround(c.r * 255)),
                  static_cast<int>(std::lround(c.g * 255)), static_cast<int>(std::lround(c.b * 255)));
    return text;
}

/**
 * 種類の名前（表示用）。
 * @param kind 種類
 * @return 名前
 */
const char* kindName(ContrastKind kind) {
    switch (kind) {
        case ContrastKind::Text: return "文字";
        case ContrastKind::LargeText: return "较大文字";
        case ContrastKind::Ui: return "控件";
        case ContrastKind::Disabled: return "禁用（参考）";
    }
    return "";
}

}  // namespace

double relativeLuminance(Color c) {
    return 0.2126 * linearChannel(c.r) + 0.7152 * linearChannel(c.g) + 0.0722 * linearChannel(c.b);
}

double contrastRatio(Color a, Color b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double light = la > lb ? la : lb;
    const double dark = la > lb ? lb : la;
    return (light + 0.05) / (dark + 0.05);
}

double requiredRatio(ContrastKind kind) {
    return kind == ContrastKind::Text ? 4.5 : 3.0;
}

const std::vector<ContrastPair>& contrastPairs() {
    // 描画で使っている組み合わせをすべて並べる（文字の大きさによらず、文字はすべて 4.5:1 で確かめる）
    static const std::vector<ContrastPair> pairs = {
        // 地・カードの上の文字
        {"标题·正文（面板底色）", kText, kBg, ContrastKind::Text},
        {"正文（卡片）", kText, kCard, ContrastKind::Text},
        {"辅助说明文字（面板底色）", kTextMuted, kBg, ContrastKind::Text},
        {"辅助说明文字（卡片）", kTextMuted, kCard, ContrastKind::Text},
        // ボタン・ピル
        {"按钮文字", kText, kControl, ContrastKind::Text},
        {"按钮的辅助说明文字", kTextMuted, kControl, ContrastKind::Text},
        {"按钮文字（悬停·按下）", kText, kControlHover, ContrastKind::Text},
        {"已选中文字（强调色填充）", kOnAccent, kAccent, ContrastKind::Text},
        {"已选中文字（按下时）", kOnAccent, kAccentPressed, ContrastKind::Text},
        {"无法按下的按钮文字", kTextDisabled, kControl, ContrastKind::Disabled},
        // アクセント色の文字
        {"强调色文字（面板底色）", kAccent, kBg, ContrastKind::Text},
        {"强调色文字（卡片）", kAccent, kCard, ContrastKind::Text},
        {"已接通段的文字（强调色浅填充）", kText, kAccentTint, ContrastKind::Text},
        // 状態
        {"“使用中”徽标", kSuccess, kSuccessTint, ContrastKind::Text},
        {"“未使用”徽标", kTextMuted, kControl, ContrastKind::Text},
        {"失败·“录音中”文字（卡片）", kDanger, kCard, ContrastKind::Text},
        {"失败文字（面板底色）", kDanger, kBg, ContrastKind::Text},
        {"录音中的停止按钮文字", kText, kDangerTint, ContrastKind::Text},
        {"退出按钮文字", kText, kQuitFill, ContrastKind::Text},
        {"退出确认中的文字（红色填充）", kOnAccent, kDanger, ContrastKind::Text},
        // 部品の見分け（WCAG 1.4.11）
        {"按钮·胶囊的边框（卡片）", kBorder, kCard, ContrastKind::Ui},
        {"按钮·胶囊的边框（面板底色）", kBorder, kBg, ContrastKind::Ui},
        {"已选中的填充（卡片）", kAccent, kCard, ContrastKind::Ui},
        {"已选中的填充（胶囊底色）", kAccent, kControl, ContrastKind::Ui},
        {"已选中的填充（面板底色）", kAccent, kBg, ContrastKind::Ui},
        {"录音的 ● 与停止按钮的边框", kDanger, kControl, ContrastKind::Ui},
        {"录音中的停止按钮边框（卡片）", kDanger, kCard, ContrastKind::Ui},
        {"“使用中”的 ●（徽标填充）", kSuccess, kSuccessTint, ContrastKind::Ui},
        {"“未使用”的 ○（徽标填充）", kTextMuted, kControl, ContrastKind::Ui},
        {"退出按钮的边框（面板底色）", kDanger, kBg, ContrastKind::Ui},
        {"未选中卡片的边框（面板底色）", kBorder, kBg, ContrastKind::Ui},
        {"按下未选中卡片时的边框（面板底色）", kAccent, kBg, ContrastKind::Ui},
        {"录音按钮的边框（卡片）", kDanger, kCard, ContrastKind::Ui},
        {"信号链路的连线（已接通）", kAccent, kCard, ContrastKind::Ui},
        {"信号链路的连线·段边框（未接通，虚线）", kBorder, kCard, ContrastKind::Ui},
        {"音量表的填充（音量表底色）", kAccent, kBg, ContrastKind::Ui},
        {"波形（卡片）", kTextMuted, kCard, ContrastKind::Ui},
        {"波形的已播放·播放位置线（卡片）", kAccent, kCard, ContrastKind::Ui},
        {"播放按钮的 ▶（按钮底色）", kText, kControl, ContrastKind::Ui},
        // イヤホン / スピーカーのカード（プリセット）のチップ: 地の色のピルに文字
        {"标签文字（未选中卡片）", kText, kBg, ContrastKind::Text},
        {"标签文字（已选中卡片上）", kAccent, kBg, ContrastKind::Text},
        {"标签边框（未选中卡片）", kBorder, kCard, ContrastKind::Ui},
        {"标签边框（悬停未选中卡片时）", kBorder, kControl, ContrastKind::Ui},
        {"标签底色（已选中卡片上）", kBg, kAccent, ContrastKind::Ui},
        {"预设的标题·精细调整的标题", kTextMuted, kBg, ContrastKind::Text},
        // 左の列のタブ（かんたん / 細かく調整。ピル型の切り替え）と、かんたんのタブの「細かく調整を見る →」
        {"标签页文字（未选中）", kText, kControl, ContrastKind::Text},
        {"标签页文字（已选中，强调色填充）", kOnAccent, kAccent, ContrastKind::Text},
        {"标签页边框（面板底色）", kBorder, kBg, ContrastKind::Ui},
        {"标签页已选中的填充（标签页底色）", kAccent, kControl, ContrastKind::Ui},
        {"“查看精细调整 →”的文字", kText, kControl, ContrastKind::Text},
        {"“查看精细调整 →”的文字（悬停时）", kText, kControlHover, ContrastKind::Text},
        {"“默认为 23%·500ms”的说明", kTextMuted, kBg, ContrastKind::Text},
        // どちらのプリセットとも一致しないときのグレーのカード（押せるまま。非活性の例外にせず、3:1 以上で読めるように）
        {"灰色卡片的文字·图标·标签文字（底色填充）", kTextDisabled, kBg, ContrastKind::Disabled},
        {"灰色卡片的文字（悬停时）", kTextDisabled, kControl, ContrastKind::Disabled},
        {"灰色卡片的虚线边框·标签边框", kBorder, kBg, ContrastKind::Ui},
        {"灰色卡片的标签边框（悬停时）", kBorder, kControl, ContrastKind::Ui},
        {"“当前是精细调整后的设置”", kText, kBg, ContrastKind::Text},
        // ノイズ除去の強さのバー（パネルの地の上）
        {"滑块的数值文字", kText, kBg, ContrastKind::Text},
        {"滑块的填充（到当前值）", kAccent, kBg, ContrastKind::Ui},
        {"滑块的轨道边框", kBorder, kBg, ContrastKind::Ui},
        {"滑块柄", kText, kBg, ContrastKind::Ui},
        {"滑块柄的边缘（在填充上区分滑块柄）", kBg, kAccent, ContrastKind::Ui},
        {"噪声抑制关闭时的滑块填充·滑块柄·数值", kTextDisabled, kBg, ContrastKind::Disabled},
        {"− / ＋ / 恢复默认的文字", kText, kControl, ContrastKind::Text},
        {"播放中的 ■（强调色填充）", kOnAccent, kAccent, ContrastKind::Ui},
        // 更新の帯（カード。枠は外がパネルの地、内がカード）
        {"更新提示条的文本（最新·检查中·更新中·检查）", kText, kCard, ContrastKind::Text},
        {"更新提示条的辅助说明（仅版本·第 2 行）", kTextMuted, kCard, ContrastKind::Text},
        {"更新提示条的新版本·替换完成的文本", kAccent, kCard, ContrastKind::Text},
        {"更新提示条的失败文本", kDanger, kCard, ContrastKind::Text},
        {"更新提示条的强调色边框（面板底色）", kAccent, kBg, ContrastKind::Ui},
        {"更新提示条的强调色边框（卡片）", kAccent, kCard, ContrastKind::Ui},
        {"更新提示条的红色边框（面板底色）", kDanger, kBg, ContrastKind::Ui},
        {"更新提示条的红色边框（卡片）", kDanger, kCard, ContrastKind::Ui},
        {"更新提示条的按钮文字", kText, kControl, ContrastKind::Text},
        {"更新提示条的按钮文字（悬停·按下）", kText, kControlHover, ContrastKind::Text},
        {"更新提示条的按钮边框（卡片）", kBorder, kCard, ContrastKind::Ui},
        {"更新提示条的“更新”文字（强调色填充）", kOnAccent, kAccent, ContrastKind::Text},
        {"更新提示条的“更新”文字（按下时）", kOnAccent, kAccentPressed, ContrastKind::Text},
        {"更新提示条的“更新”填充（卡片）", kAccent, kCard, ContrastKind::Ui},
    };
    return pairs;
}

int printContrastReport() {
    int failures = 0;
    double lowest = 100.0;
    const ContrastPair* lowestPair = nullptr;
    std::printf("%-6s %-7s %-7s %6s %5s  %s\n", "结果", "文字颜色", "背景", "对比度", "要求值", "用在哪里");
    for (const ContrastPair& pair : contrastPairs()) {
        const double ratio = contrastRatio(pair.fg, pair.bg);
        const double need = requiredRatio(pair.kind);
        const bool ok = ratio >= need;
        if (!ok) ++failures;
        if (ratio < lowest) {
            lowest = ratio;
            lowestPair = &pair;
        }
        std::printf("%-6s %s %s %6.2f %5.1f  %s（%s）\n", ok ? "通过" : "未通过", hexText(pair.fg).c_str(),
                    hexText(pair.bg).c_str(), ratio, need, pair.what, kindName(pair.kind));
    }
    if (lowestPair != nullptr) {
        std::printf("最低对比度: %.2f（%s: %s / %s）\n", lowest, lowestPair->what, hexText(lowestPair->fg).c_str(),
                    hexText(lowestPair->bg).c_str());
    }
    std::printf("%zu 组中有 %d 组未通过\n", contrastPairs().size(), failures);
    return failures == 0 ? 0 : 1;
}
