// [vendor] sn-lookandfeel 9593173 sha256:9a0fc4d28b0e4bc1
// この行より下は配られたコピー。直すなら sn-lookandfeel(正)を直して配り直す
#pragma once
// SnLookAndFeel.h — つまみとフェーダーの共通デザイン(SN 音源アプリ共通)
//
// 出どころ: Artificial Reggae Machine の ArgmLookAndFeel.h(2026-08-06)。
// 実地判定=合格(ユーザー「つまみのデザインはよし」)を受けて、
// 2026-08-07 に共通資産として切り出した。
//
// 方針(ユーザー指示 2026-08-06):
//   ・**値は目盛りで読む**。だから、どのつまみにも目盛りとガイド線を必ず描く。
//     点(ビット)がどこかに浮いているだけの表示は、位置を判断できない。
//   ・**直線移動は三角**。三角の頂点が目盛りを指す。頂点=読み取り点。
//   ・**回転は回転体のノブ**。ノブ本体に印(刻み線)が付いていて、
//     その印を周囲の目盛りに合わせて読む。
//
// 目盛りは「大目盛り(長い・明るい)」と「小目盛り(短い・暗い)」の2段。
// 既定は 4分割(大)×4(小)= 端と中央と四分点が読める粒度。
//
// ── 使い方 ────────────────────────────────────────────────────────────
//
// 独自 LookAndFeel を持っていないアプリ:
//     #include "vendor/SnLookAndFeel.h"
//     sn::KnobLookAndFeel laf;                  // エディタのメンバに置く
//     setLookAndFeel (&laf);                    // コンストラクタ
//     ~Editor() { setLookAndFeel (nullptr); }   // ★これを忘れると落ちる
//
// 既に独自 LookAndFeel を持っているアプリ:
//     class MyLookAndFeel : public sn::KnobLookAndFeel   // ← 継承元だけ差し替え
//     …そして自前の drawRotarySlider / drawLinearSlider を消す。
//     ボタン・コンボ・配色など、それ以外の細工はそのまま残る。
//
// ── 色について ────────────────────────────────────────────────────────
//
// アクセント(ガイド弧・ノブの印・フェーダーの通過部)は
// `juce::Slider::thumbColourId` から拾う。**アプリ側の色をそのまま使う**ので、
// このファイルを入れてもアプリのアクセント色は変わらない。
//
// ノブ本体・目盛り・溝の色は `Palette` で差し替えられる。
// **形は共通・色はアプリのもの**が方針。気に入られたのは
// 「目盛りで読む/ノブに印/直線は三角」という**読み方の設計**であって
// 灰色そのものではないので、絵を持つアプリ(Matryoshka Guitar の木のボディ、
// oxide の DS-1 など)は色だけ自分のものに差し替える:
//
//     sn::KnobLookAndFeel::Palette p;
//     p.bodyTop = juce::Colour (0xffb97745);   // 飴色
//     p.bodyBottom = juce::Colour (0xff5a2d14);
//     laf.setPalette (p);
//
// ★このファイルは**配られたコピー**。直すときは正(sn-lookandfeel)を直して
//   配り直すこと。ここで直すと、次の配布で黙って消える。

// ★JuceHeader.h を作らない作りの repo がある(Heptode はモジュールを直接
//   include している)。どちらでも通るようにする
#if __has_include(<JuceHeader.h>)
 #include <JuceHeader.h>
#else
 #include <juce_gui_basics/juce_gui_basics.h>
#endif
#include <algorithm>
#include <cmath>

namespace sn
{

class KnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /** 色。**形は共通・色はアプリのもの**。
        ユーザーが気に入ったのは「目盛りで読む/ノブに印/直線は三角」という
        **読み方の設計**であって、灰色そのものではない。木のボディ絵に灰色の
        つまみを被せれば絵と喧嘩するだけなので、絵を持つアプリは色だけ差し替える
        (Matryoshka Guitar の飴色、oxide の DS-1 など)。 */
    struct Palette
    {
        juce::Colour bodyTop    { 0xff525f6c };  ///< ノブ本体の上(光が当たる側)
        juce::Colour bodyBottom { 0xff2a323b };  ///< ノブ本体の下
        juce::Colour rim        { 0xff1b2027 };  ///< ノブの縁
        juce::Colour tickMajor  { 0xffc3cedb };  ///< 大目盛り
        juce::Colour tickMinor  { 0xff6b7986 };  ///< 小目盛り
        juce::Colour track      { 0xff2a323b };  ///< フェーダーの溝
        juce::Colour thumbFill  { 0xffdfe7ee };  ///< 三角つまみの面
        juce::Colour thumbEdge  { 0xff11151a };  ///< 三角つまみの縁
    };

    void setPalette (const Palette& p) { pal = p; }
    const Palette& palette() const { return pal; }

    KnobLookAndFeel()
    {
        setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffdfe7ee));
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff232a33));
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a4552));
    }

    // ── 回転つまみ ──────────────────────────────────────────────────────
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float cx = area.getCentreX(), cy = area.getCentreY();
        const float outer = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 1.0f;
        // 目盛り・ガイド弧・ノブ本体が重ならないよう、半径をはっきり分ける。
        // 詰めると本体が目盛りを潰して、結局どこを指しているか読めなくなる
        const float tickOuter = outer;
        const float tickInner = outer - 6.0f;
        const float bodyR = outer - 15.0f;
        if (bodyR < 6.0f)
        {
            // ★小さい枠では目盛りが入らない。**何も描かないのは事故**なので、
            //   本体と印だけの簡略形へ落とす(ペダル系の小さいノブがこれ)
            drawCompact (g, cx, cy, juce::jmax (3.0f, outer - 2.0f), pos,
                         startAngle, endAngle, s);
            return;
        }

        const auto accent = s.findColour (juce::Slider::thumbColourId);

        // 目盛り。大目盛り5本(0/25/50/75/100%)の間に小目盛り3本ずつ
        for (int i = 0; i <= 16; ++i)
        {
            const bool major = (i % 4) == 0;
            const float a = startAngle + (endAngle - startAngle) * (float) i / 16.0f;
            const float sinA = std::sin (a), cosA = std::cos (a);
            const float r0 = major ? tickInner - 3.0f : tickInner + 1.0f;
            g.setColour (major ? pal.tickMajor : pal.tickMinor);
            g.drawLine (cx + sinA * r0, cy - cosA * r0,
                        cx + sinA * tickOuter, cy - cosA * tickOuter, major ? 2.0f : 1.1f);
        }

        // 現在値までのガイド弧(どこまで来ているかを弧でも示す)
        {
            juce::Path arc;
            arc.addCentredArc (cx, cy, bodyR + 5.5f, bodyR + 5.5f, 0.0f,
                               startAngle, startAngle + (endAngle - startAngle) * pos, true);
            g.setColour (accent.withAlpha (0.85f));
            g.strokePath (arc, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        drawBody (g, cx, cy, bodyR);
        drawPointer (g, cx, cy, bodyR, pos, startAngle, endAngle, accent);
    }

    // ── 直線つまみ ──────────────────────────────────────────────────────
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           const juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        const auto accent = s.findColour (juce::Slider::thumbColourId);
        const bool vertical = (style == juce::Slider::LinearVertical
                               || style == juce::Slider::LinearBarVertical);

        if (vertical)
        {
            const float trackX = (float) x + (float) width * 0.62f;
            const float top = (float) y + 6.0f, bottom = (float) y + (float) height - 6.0f;

            // 目盛りは左側に並べる。三角の頂点がここを指す
            for (int i = 0; i <= 16; ++i)
            {
                const bool major = (i % 4) == 0;
                const float ty = juce::jmap ((float) i / 16.0f, bottom, top);
                const float len = major ? 14.0f : 7.0f;
                g.setColour (major ? pal.tickMajor : pal.tickMinor);
                g.drawLine (trackX - 10.0f - len, ty, trackX - 10.0f, ty, major ? 2.0f : 1.1f);
            }

            // 溝と、下から現在値までのガイド
            g.setColour (pal.track);
            g.fillRoundedRectangle (trackX - 2.5f, top, 5.0f, bottom - top, 2.5f);
            g.setColour (accent.withAlpha (0.85f));
            g.fillRoundedRectangle (trackX - 2.5f, sliderPos, 5.0f, bottom - sliderPos, 2.5f);

            // ★三角のつまみ。頂点(左)が目盛りを指す
            juce::Path tri;
            tri.startNewSubPath (trackX - 10.0f, sliderPos);           // 頂点=読み取り点
            tri.lineTo (trackX + 9.0f, sliderPos - 7.0f);
            tri.lineTo (trackX + 9.0f, sliderPos + 7.0f);
            tri.closeSubPath();
            g.setColour (pal.thumbFill);
            g.fillPath (tri);
            g.setColour (pal.thumbEdge);
            g.strokePath (tri, juce::PathStrokeType (1.0f));
        }
        else
        {
            const float trackY = (float) y + (float) height * 0.38f;
            const float left = (float) x + 6.0f, right = (float) x + (float) width - 6.0f;

            for (int i = 0; i <= 16; ++i)
            {
                const bool major = (i % 4) == 0;
                const float tx = juce::jmap ((float) i / 16.0f, left, right);
                const float len = major ? 14.0f : 7.0f;
                g.setColour (major ? pal.tickMajor : pal.tickMinor);
                g.drawLine (tx, trackY + 10.0f, tx, trackY + 10.0f + len, major ? 2.0f : 1.1f);
            }

            g.setColour (pal.track);
            g.fillRoundedRectangle (left, trackY - 2.5f, right - left, 5.0f, 2.5f);
            g.setColour (accent.withAlpha (0.85f));
            g.fillRoundedRectangle (left, trackY - 2.5f, sliderPos - left, 5.0f, 2.5f);

            juce::Path tri;
            tri.startNewSubPath (sliderPos, trackY + 9.0f);            // 頂点=読み取り点
            tri.lineTo (sliderPos - 7.0f, trackY - 9.0f);
            tri.lineTo (sliderPos + 7.0f, trackY - 9.0f);
            tri.closeSubPath();
            g.setColour (pal.thumbFill);
            g.fillPath (tri);
            g.setColour (pal.thumbEdge);
            g.strokePath (tri, juce::PathStrokeType (1.0f));
        }
    }

private:
    /** ノブ本体(回転体)。上から光が当たっている見え方にして、面の向きを出す。 */
    void drawBody (juce::Graphics& g, float cx, float cy, float r) const
    {
        juce::ColourGradient grad (pal.bodyTop, cx, cy - r,
                                   pal.bodyBottom, cx, cy + r, false);
        g.setGradientFill (grad);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.setColour (pal.rim);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
    }

    /** ★ノブに付いた印。これを目盛りに合わせて読む。 */
    void drawPointer (juce::Graphics& g, float cx, float cy, float r, float pos,
                             float startAngle, float endAngle, juce::Colour accent) const
    {
        const float a = startAngle + (endAngle - startAngle) * pos;
        const float sinA = std::sin (a), cosA = std::cos (a);
        g.setColour (accent);
        g.drawLine (cx + sinA * (r * 0.30f), cy - cosA * (r * 0.30f),
                    cx + sinA * (r * 0.94f), cy - cosA * (r * 0.94f),
                    juce::jmax (2.0f, r * 0.13f));
    }

    /** 目盛りが物理的に入らない小さい枠向け。ガイド弧と印だけは必ず残す。 */
    void drawCompact (juce::Graphics& g, float cx, float cy, float r, float pos,
                             float startAngle, float endAngle, juce::Slider& s) const
    {
        const auto accent = s.findColour (juce::Slider::thumbColourId);
        juce::Path arc;
        arc.addCentredArc (cx, cy, r, r, 0.0f,
                           startAngle, startAngle + (endAngle - startAngle) * pos, true);
        g.setColour (accent.withAlpha (0.85f));
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
        const float bodyR = juce::jmax (3.0f, r - 4.0f);
        drawBody (g, cx, cy, bodyR);
        drawPointer (g, cx, cy, bodyR, pos, startAngle, endAngle, accent);
    }

    Palette pal;
};

} // namespace sn
