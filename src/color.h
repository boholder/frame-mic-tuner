// 描画で使う RGB の色。cairo に依存しないので、色の定義（theme.h）とコントラスト比の計算だけを
// cairo 抜きでビルド・テストできる。
#pragma once

/** RGB の色（0〜1）。 */
struct Color {
    double r, g, b;
};
