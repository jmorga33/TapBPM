#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

#include "bpm_engine.h"
#include "../res/resource.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"TapBPMWindowClass";
constexpr wchar_t kWindowTitle[] = L"TapBPM";

constexpr int kBaseClientW = 320;
constexpr int kBaseClientH = 224;

constexpr UINT_PTR kTimerAnimId = 1;
constexpr UINT_PTR kTimerHoldId = 2;
constexpr UINT_PTR kTimerCopiedId = 3;

constexpr UINT kAnimIntervalMs = 16;
constexpr UINT kHoldTimeoutMs = 2550;
constexpr UINT kCopiedDurationMs = 950;

constexpr double kPulseDurationSec = 0.22;
constexpr double kCopiedDurationSec = 0.95;

constexpr UINT kWmDpiChanged = 0x02E0;

enum class HitZone {
    None,
    Pad,
    BtnTop,
    BtnCopy,
    BtnReset
};

struct AppState {
    BpmEngine engine;
    LARGE_INTEGER qpc_freq = {};

    bool always_on_top = true;
    UINT dpi = 96;

    double pulse_start_sec = -10.0;
    float pulse_alpha = 0.0f;
    double copied_until_sec = -10.0;

    HitZone hover = HitZone::None;
    bool mouse_tracked = false;

    int client_w = kBaseClientW;
    int client_h = kBaseClientH;

    RECT rect_pad = {};
    RECT rect_top = {};
    RECT rect_copy = {};
    RECT rect_reset = {};

    HDC mem_dc = nullptr;
    HBITMAP mem_bmp = nullptr;
    HBITMAP old_bmp = nullptr;
    int buf_w = 0;
    int buf_h = 0;
};

inline float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline int ScalePx(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

inline float ScaleF(float dip, UINT dpi) {
    return dip * (static_cast<float>(dpi) / 96.0f);
}

inline Gdiplus::RectF ToRectF(const RECT& r) {
    return Gdiplus::RectF(
        static_cast<float>(r.left),
        static_cast<float>(r.top),
        static_cast<float>(r.right - r.left),
        static_cast<float>(r.bottom - r.top));
}

inline Gdiplus::Color BlendColor(Gdiplus::Color a, Gdiplus::Color b, float t) {
    t = ClampF(t, 0.0f, 1.0f);
    const BYTE r = static_cast<BYTE>(a.GetR() + (b.GetR() - a.GetR()) * t + 0.5f);
    const BYTE g = static_cast<BYTE>(a.GetG() + (b.GetG() - a.GetG()) * t + 0.5f);
    const BYTE bl = static_cast<BYTE>(a.GetB() + (b.GetB() - a.GetB()) * t + 0.5f);
    const BYTE al = static_cast<BYTE>(a.GetA() + (b.GetA() - a.GetA()) * t + 0.5f);
    return Gdiplus::Color(al, r, g, bl);
}

double NowSec(const AppState* state) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(state->qpc_freq.QuadPart);
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
        using SetAwareFn = BOOL(WINAPI*)();
        auto fallback = reinterpret_cast<SetAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
        if (fallback) {
            fallback();
        }
    }
}

UINT QueryWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<GetDpiFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (fn) {
            const UINT dpi = fn(hwnd);
            if (dpi >= 96) {
                return dpi;
            }
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = 96;
    if (hdc) {
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hwnd, hdc);
    }
    return dpi >= 96 ? dpi : 96;
}

void AdjustWindowRectDpi(RECT* rc, DWORD style, DWORD ex_style, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using AdjDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto fn = reinterpret_cast<AdjDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (fn && fn(rc, style, FALSE, ex_style, dpi)) {
            return;
        }
    }
    AdjustWindowRectEx(rc, style, FALSE, ex_style);
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    HRESULT hr = DwmSetWindowAttribute(
        hwnd, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(
            hwnd, static_cast<DWMWINDOWATTRIBUTE>(19), &dark, sizeof(dark));
    }
    COLORREF caption = RGB(18, 19, 22);
    DwmSetWindowAttribute(
        hwnd, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption));
}

void ComputeLayout(AppState* state) {
    const UINT dpi = state->dpi;
    const int margin = ScalePx(14, dpi);
    const int w = state->client_w;
    const int h = state->client_h;

    const int pill_y = ScalePx(11, dpi);
    const int pill_h = ScalePx(22, dpi);
    const int gap = ScalePx(5, dpi);

    const int rst_w = ScalePx(38, dpi);
    const int copy_w = ScalePx(44, dpi);
    const int top_w = ScalePx(40, dpi);

    const int rst_right = w - margin;
    state->rect_reset = {rst_right - rst_w, pill_y, rst_right, pill_y + pill_h};

    const int copy_right = state->rect_reset.left - gap;
    state->rect_copy = {copy_right - copy_w, pill_y, copy_right, pill_y + pill_h};

    const int top_right = state->rect_copy.left - gap;
    state->rect_top = {top_right - top_w, pill_y, top_right, pill_y + pill_h};

    const int pad_top = ScalePx(44, dpi);
    const int pad_bottom = h - ScalePx(38, dpi);
    state->rect_pad = {margin, pad_top, w - margin, pad_bottom};
}

HitZone HitTest(const AppState* state, POINT pt, bool has_bpm) {
    if (PtInRect(&state->rect_top, pt)) return HitZone::BtnTop;
    if (PtInRect(&state->rect_copy, pt)) return has_bpm ? HitZone::BtnCopy : HitZone::None;
    if (PtInRect(&state->rect_reset, pt)) return HitZone::BtnReset;
    if (PtInRect(&state->rect_pad, pt)) return HitZone::Pad;
    return HitZone::None;
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float max_r = (r.Width < r.Height ? r.Width : r.Height) * 0.5f;
    const float rad = ClampF(radius, 0.0f, max_r);
    if (rad <= 0.01f) {
        path.AddRectangle(r);
        return;
    }
    const float d = rad * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawPill(
    Gdiplus::Graphics& g,
    const RECT& rc,
    const wchar_t* label,
    const Gdiplus::Font& font,
    const Gdiplus::StringFormat& fmt,
    Gdiplus::Color fill,
    Gdiplus::Color border,
    Gdiplus::Color text_color,
    float radius) {
    const Gdiplus::RectF rf = ToRectF(rc);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, rf, radius);

    Gdiplus::SolidBrush fill_brush(fill);
    Gdiplus::Pen border_pen(border, 1.0f);
    g.FillPath(&fill_brush, &path);
    g.DrawPath(&border_pen, &path);

    Gdiplus::SolidBrush text_brush(text_color);
    g.DrawString(label, -1, &font, rf, &fmt, &text_brush);
}

void EnsureBackbuffer(AppState* state, HDC target_dc) {
    if (state->mem_dc && state->buf_w == state->client_w && state->buf_h == state->client_h) {
        return;
    }
    if (state->mem_dc) {
        SelectObject(state->mem_dc, state->old_bmp);
        DeleteObject(state->mem_bmp);
        DeleteDC(state->mem_dc);
        state->mem_dc = nullptr;
        state->mem_bmp = nullptr;
        state->old_bmp = nullptr;
    }
    if (state->client_w <= 0 || state->client_h <= 0) {
        return;
    }
    state->mem_dc = CreateCompatibleDC(target_dc);
    state->mem_bmp = CreateCompatibleBitmap(target_dc, state->client_w, state->client_h);
    state->old_bmp = static_cast<HBITMAP>(SelectObject(state->mem_dc, state->mem_bmp));
    state->buf_w = state->client_w;
    state->buf_h = state->client_h;
}

void FreeBackbuffer(AppState* state) {
    if (state->mem_dc) {
        SelectObject(state->mem_dc, state->old_bmp);
        DeleteObject(state->mem_bmp);
        DeleteDC(state->mem_dc);
        state->mem_dc = nullptr;
        state->mem_bmp = nullptr;
        state->old_bmp = nullptr;
    }
    state->buf_w = 0;
    state->buf_h = 0;
}

void UpdateAnimationState(AppState* state, double now) {
    const double elapsed = now - state->pulse_start_sec;
    if (elapsed >= 0.0 && elapsed < kPulseDurationSec) {
        const float t = static_cast<float>(1.0 - elapsed / kPulseDurationSec);
        state->pulse_alpha = t * t;
    } else {
        state->pulse_alpha = 0.0f;
    }
}

void Render(HWND hwnd, HDC target_dc, AppState* state) {
    EnsureBackbuffer(state, target_dc);
    if (!state->mem_dc) {
        return;
    }

    const double now = NowSec(state);
    UpdateAnimationState(state, now);
    const BpmSnapshot snap = state->engine.GetSnapshot(now);
    const bool show_copied = (now < state->copied_until_sec);

    const UINT dpi = state->dpi;
    const float s = static_cast<float>(dpi) / 96.0f;

    const Gdiplus::Color col_bg(255, 18, 19, 22);
    const Gdiplus::Color col_card(255, 25, 28, 34);
    const Gdiplus::Color col_border(255, 40, 45, 56);
    const Gdiplus::Color col_mint(255, 0, 229, 160);
    const Gdiplus::Color col_amber(255, 255, 159, 67);
    const Gdiplus::Color col_blue(255, 88, 166, 255);
    const Gdiplus::Color col_red(255, 248, 113, 113);
    const Gdiplus::Color col_text(255, 248, 250, 252);
    const Gdiplus::Color col_muted(255, 139, 148, 166);
    const Gdiplus::Color col_dim(255, 86, 94, 112);

    Gdiplus::Color status_accent = col_dim;
    if (show_copied) {
        status_accent = col_mint;
    } else if (snap.has_bpm) {
        if (snap.is_held) {
            status_accent = col_blue;
        } else if (snap.confidence >= 0.45) {
            status_accent = col_mint;
        } else {
            status_accent = col_amber;
        }
    } else if (snap.tap_count == 1) {
        status_accent = col_amber;
    }

    const Gdiplus::Color pulse_accent =
        (snap.has_bpm && snap.confidence >= 0.45) ? col_mint : col_amber;

    {
        Gdiplus::Graphics g(state->mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        g.Clear(col_bg);

        Gdiplus::FontFamily ui_family(L"Segoe UI");
        Gdiplus::Font font_status(&ui_family, 10.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font font_pill(&ui_family, 10.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font font_micro(&ui_family, 10.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font font_hero(&ui_family, 50.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font font_sub(&ui_family, 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font font_hint(&ui_family, 10.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::StringFormat fmt_left;
        fmt_left.SetAlignment(Gdiplus::StringAlignmentNear);
        fmt_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt_left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::StringFormat fmt_center;
        fmt_center.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt_center.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        // Top-left status indicator
        const float led_r = 3.5f * s;
        const float led_cx = 14.0f * s + led_r;
        const float led_cy = 22.0f * s;
        Gdiplus::SolidBrush led_brush(status_accent);
        g.FillEllipse(&led_brush, led_cx - led_r, led_cy - led_r, led_r * 2.0f, led_r * 2.0f);

        wchar_t status_buf[64] = {};
        if (show_copied) {
            std::swprintf(status_buf, 64, L"COPIED TO CLIPBOARD");
        } else if (snap.has_bpm) {
            std::swprintf(
                status_buf, 64, L"%s \u00B7 %d %s",
                snap.is_held ? L"HELD" : L"LIVE",
                snap.tap_count,
                snap.tap_count == 1 ? L"TAP" : L"TAPS");
        } else if (snap.tap_count == 1) {
            std::swprintf(status_buf, 64, L"TAP AGAIN...");
        } else {
            std::swprintf(status_buf, 64, L"READY");
        }

        const Gdiplus::RectF status_rect(
            26.0f * s, 11.0f * s,
            static_cast<float>(state->rect_top.left) - 30.0f * s, 22.0f * s);
        Gdiplus::SolidBrush status_text_brush(show_copied ? col_mint : col_muted);
        g.DrawString(status_buf, -1, &font_status, status_rect, &fmt_left, &status_text_brush);

        // Top-right pills: TOP, COPY, RST
        const float pill_rad = 6.0f * s;
        {
            Gdiplus::Color fill(255, 24, 27, 34);
            Gdiplus::Color border = col_border;
            Gdiplus::Color txt = col_muted;
            if (state->always_on_top) {
                fill = Gdiplus::Color(42, 0, 229, 160);
                border = Gdiplus::Color(150, 0, 229, 160);
                txt = col_mint;
            }
            if (state->hover == HitZone::BtnTop) {
                fill = BlendColor(fill, Gdiplus::Color(255, 255, 255, 255), 0.08f);
            }
            DrawPill(g, state->rect_top, L"TOP", font_pill, fmt_center, fill, border, txt, pill_rad);
        }
        {
            Gdiplus::Color fill(255, 24, 27, 34);
            Gdiplus::Color border = col_border;
            Gdiplus::Color txt = snap.has_bpm ? col_text : col_dim;
            if (show_copied) {
                fill = Gdiplus::Color(42, 0, 229, 160);
                border = Gdiplus::Color(150, 0, 229, 160);
                txt = col_mint;
            } else if (snap.has_bpm && state->hover == HitZone::BtnCopy) {
                fill = Gdiplus::Color(255, 34, 39, 49);
                border = Gdiplus::Color(255, 68, 76, 92);
            }
            DrawPill(g, state->rect_copy, L"COPY", font_pill, fmt_center, fill, border, txt, pill_rad);
        }
        {
            Gdiplus::Color fill(255, 24, 27, 34);
            Gdiplus::Color border = col_border;
            Gdiplus::Color txt = (snap.tap_count > 0) ? col_muted : col_dim;
            if (state->hover == HitZone::BtnReset) {
                fill = Gdiplus::Color(38, 248, 113, 113);
                border = Gdiplus::Color(145, 248, 113, 113);
                txt = col_red;
            }
            DrawPill(g, state->rect_reset, L"RST", font_pill, fmt_center, fill, border, txt, pill_rad);
        }

        // Hero tap pad card
        const Gdiplus::RectF pad_rf = ToRectF(state->rect_pad);
        const float pad_rad = 12.0f * s;
        Gdiplus::Color card_fill = col_card;
        if (state->hover == HitZone::Pad) {
            card_fill = BlendColor(card_fill, Gdiplus::Color(255, 42, 48, 60), 0.25f);
        }
        card_fill = BlendColor(card_fill, pulse_accent, state->pulse_alpha * 0.14f);

        Gdiplus::Color card_border = col_border;
        if (state->hover == HitZone::Pad) {
            card_border = Gdiplus::Color(255, 56, 63, 78);
        }
        card_border = BlendColor(card_border, pulse_accent, state->pulse_alpha * 0.85f);

        Gdiplus::GraphicsPath pad_path;
        AddRoundedRect(pad_path, pad_rf, pad_rad);
        Gdiplus::SolidBrush pad_brush(card_fill);
        Gdiplus::Pen pad_pen(card_border, 1.25f * s);
        g.FillPath(&pad_brush, &pad_path);
        g.DrawPath(&pad_pen, &pad_path);

        // Micro label inside card
        const Gdiplus::RectF micro_rf(pad_rf.X + 14.0f * s, pad_rf.Y + 10.0f * s, 90.0f * s, 16.0f * s);
        Gdiplus::SolidBrush micro_brush(col_dim);
        g.DrawString(L"BPM", -1, &font_micro, micro_rf, &fmt_left, &micro_brush);

        // Beat pulse LED top-right of card
        const float beat_cx = pad_rf.X + pad_rf.Width - 18.0f * s;
        const float beat_cy = pad_rf.Y + 18.0f * s;
        const float beat_r = 4.2f * s;
        if (state->pulse_alpha > 0.01f) {
            const float ring_r = beat_r + (1.0f - state->pulse_alpha) * 7.5f * s;
            const BYTE ring_a = static_cast<BYTE>(state->pulse_alpha * 190.0f);
            Gdiplus::Pen ring_pen(
                Gdiplus::Color(ring_a, pulse_accent.GetR(), pulse_accent.GetG(), pulse_accent.GetB()),
                1.5f * s);
            g.DrawEllipse(&ring_pen, beat_cx - ring_r, beat_cy - ring_r, ring_r * 2.0f, ring_r * 2.0f);
        }
        Gdiplus::Color beat_dot = (state->pulse_alpha > 0.01f)
            ? BlendColor(Gdiplus::Color(255, 45, 51, 64), pulse_accent, state->pulse_alpha)
            : (snap.has_bpm ? BlendColor(Gdiplus::Color(255, 45, 51, 64), status_accent, 0.45f)
                            : Gdiplus::Color(255, 45, 51, 64));
        Gdiplus::SolidBrush beat_brush(beat_dot);
        g.FillEllipse(&beat_brush, beat_cx - beat_r, beat_cy - beat_r, beat_r * 2.0f, beat_r * 2.0f);

        // Hero BPM number
        wchar_t hero_buf[32] = {};
        Gdiplus::Color hero_color = Gdiplus::Color(255, 74, 81, 98);
        if (snap.has_bpm) {
            std::swprintf(hero_buf, 32, L"%.1f", snap.bpm);
            hero_color = col_text;
        } else if (snap.tap_count == 1) {
            std::swprintf(hero_buf, 32, L"TAP...");
            hero_color = col_amber;
        } else {
            std::swprintf(hero_buf, 32, L"---.-");
        }
        const Gdiplus::RectF hero_rf(
            pad_rf.X + 8.0f * s,
            pad_rf.Y + 22.0f * s,
            pad_rf.Width - 16.0f * s,
            68.0f * s);
        Gdiplus::SolidBrush hero_brush(hero_color);
        g.DrawString(hero_buf, -1, &font_hero, hero_rf, &fmt_center, &hero_brush);

        // Sub-readout: half / double / jitter
        wchar_t sub_buf[96] = {};
        Gdiplus::Color sub_color = col_muted;
        if (snap.has_bpm) {
            if (snap.window_count >= 3) {
                std::swprintf(
                    sub_buf, 96,
                    L"\u00BD\u00D7 %.1f   \u00B7   2\u00D7 %.1f   \u00B7   \u00B1%.0f ms",
                    snap.half_bpm, snap.double_bpm, snap.jitter_ms);
            } else {
                std::swprintf(
                    sub_buf, 96,
                    L"\u00BD\u00D7 %.1f   \u00B7   2\u00D7 %.1f",
                    snap.half_bpm, snap.double_bpm);
            }
        } else if (snap.tap_count == 1) {
            std::swprintf(sub_buf, 96, L"Keep tapping on the beat...");
            sub_color = col_muted;
        } else {
            std::swprintf(sub_buf, 96, L"Press SPACE or click here to the beat");
            sub_color = col_dim;
        }
        const Gdiplus::RectF sub_rf(
            pad_rf.X + 8.0f * s,
            pad_rf.Y + 94.0f * s,
            pad_rf.Width - 16.0f * s,
            22.0f * s);
        Gdiplus::SolidBrush sub_brush(sub_color);
        g.DrawString(sub_buf, -1, &font_sub, sub_rf, &fmt_center, &sub_brush);

        // Confidence bar at bottom of card
        const float bar_pad_x = 14.0f * s;
        const float bar_w = pad_rf.Width - bar_pad_x * 2.0f;
        const float bar_h = 4.0f * s;
        const float bar_x = pad_rf.X + bar_pad_x;
        const float bar_y = pad_rf.Y + pad_rf.Height - 14.0f * s;
        const Gdiplus::RectF track_rf(bar_x, bar_y, bar_w, bar_h);

        Gdiplus::GraphicsPath track_path;
        AddRoundedRect(track_path, track_rf, bar_h * 0.5f);
        Gdiplus::SolidBrush track_brush(Gdiplus::Color(255, 35, 40, 52));
        g.FillPath(&track_brush, &track_path);

        if (snap.has_bpm && snap.confidence > 0.01) {
            const float fill_w = ClampF(
                bar_w * static_cast<float>(snap.confidence),
                bar_h, bar_w);
            const Gdiplus::RectF fill_rf(bar_x, bar_y, fill_w, bar_h);
            Gdiplus::GraphicsPath fill_path;
            AddRoundedRect(fill_path, fill_rf, bar_h * 0.5f);
            Gdiplus::Color bar_color = snap.is_held
                ? col_blue
                : (snap.confidence >= 0.45 ? col_mint : col_amber);
            Gdiplus::SolidBrush fill_brush(bar_color);
            g.FillPath(&fill_brush, &fill_path);
        }

        // Bottom shortcut hint bar
        const Gdiplus::RectF hint_rf(
            14.0f * s,
            static_cast<float>(state->rect_pad.bottom) + 6.0f * s,
            static_cast<float>(state->client_w) - 28.0f * s,
            22.0f * s);
        Gdiplus::SolidBrush hint_brush(col_dim);
        g.DrawString(
            L"SPACE Tap   \u00B7   R Reset   \u00B7   T On Top   \u00B7   C Copy",
            -1, &font_hint, hint_rf, &fmt_center, &hint_brush);
    }

    BitBlt(target_dc, 0, 0, state->client_w, state->client_h, state->mem_dc, 0, 0, SRCCOPY);
    (void)hwnd;
}

void OnTap(HWND hwnd, AppState* state) {
    const double now = NowSec(state);
    if (!state->engine.RecordTap(now)) {
        return;
    }
    state->pulse_start_sec = now;
    state->pulse_alpha = 1.0f;
    SetTimer(hwnd, kTimerAnimId, kAnimIntervalMs, nullptr);
    SetTimer(hwnd, kTimerHoldId, kHoldTimeoutMs, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnReset(HWND hwnd, AppState* state) {
    state->engine.Reset();
    state->pulse_alpha = 0.0f;
    state->pulse_start_sec = -10.0;
    KillTimer(hwnd, kTimerAnimId);
    KillTimer(hwnd, kTimerHoldId);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnToggleTopmost(HWND hwnd, AppState* state) {
    state->always_on_top = !state->always_on_top;
    SetWindowPos(
        hwnd,
        state->always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnCopyBpm(HWND hwnd, AppState* state) {
    const double now = NowSec(state);
    const BpmSnapshot snap = state->engine.GetSnapshot(now);
    if (!snap.has_bpm) {
        return;
    }

    wchar_t text[32] = {};
    const int len = std::swprintf(text, 32, L"%.1f", snap.bpm);
    if (len <= 0) {
        return;
    }

    const SIZE_T bytes = static_cast<SIZE_T>(len + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) {
        return;
    }

    void* ptr = GlobalLock(hmem);
    if (!ptr) {
        GlobalFree(hmem);
        return;
    }
    std::wmemcpy(static_cast<wchar_t*>(ptr), text, static_cast<size_t>(len + 1));
    GlobalUnlock(hmem);

    if (!OpenClipboard(hwnd)) {
        GlobalFree(hmem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return;
    }
    CloseClipboard();

    state->copied_until_sec = now + kCopiedDurationSec;
    SetTimer(hwnd, kTimerCopiedId, kCopiedDurationMs, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* init_state = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(init_state));
        break;
    }

    case WM_CREATE: {
        if (state) {
            state->dpi = QueryWindowDpi(hwnd);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            state->client_w = rc.right - rc.left;
            state->client_h = rc.bottom - rc.top;
            ComputeLayout(state);
            ApplyDarkTitleBar(hwnd);
        }
        return 0;
    }

    case WM_SIZE: {
        if (state) {
            state->client_w = LOWORD(lParam);
            state->client_h = HIWORD(lParam);
            ComputeLayout(state);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (state) {
            Render(hwnd, hdc, state);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!state || (lParam & 0x40000000L) != 0) {
            return 0;
        }
        switch (wParam) {
        case VK_SPACE:
            OnTap(hwnd, state);
            return 0;
        case 'R':
        case VK_ESCAPE:
        case VK_BACK:
            OnReset(hwnd, state);
            return 0;
        case 'T':
            OnToggleTopmost(hwnd, state);
            return 0;
        case 'C':
            OnCopyBpm(hwnd, state);
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        if (!state) {
            return 0;
        }
        SetFocus(hwnd);
        const POINT pt = {
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        const BpmSnapshot snap = state->engine.GetSnapshot(NowSec(state));
        const HitZone zone = HitTest(state, pt, snap.has_bpm);
        switch (zone) {
        case HitZone::BtnTop:
            OnToggleTopmost(hwnd, state);
            break;
        case HitZone::BtnCopy:
            OnCopyBpm(hwnd, state);
            break;
        case HitZone::BtnReset:
            OnReset(hwnd, state);
            break;
        case HitZone::Pad:
            OnTap(hwnd, state);
            break;
        case HitZone::None:
            break;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        if (!state->mouse_tracked) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) {
                state->mouse_tracked = true;
            }
        }
        const POINT pt = {
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        const BpmSnapshot snap = state->engine.GetSnapshot(NowSec(state));
        const HitZone new_hover = HitTest(state, pt, snap.has_bpm);
        if (new_hover != state->hover) {
            state->hover = new_hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (state) {
            state->mouse_tracked = false;
            if (state->hover != HitZone::None) {
                state->hover = HitZone::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (state && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const BpmSnapshot snap = state->engine.GetSnapshot(NowSec(state));
            const HitZone zone = HitTest(state, pt, snap.has_bpm);
            SetCursor(LoadCursorW(nullptr, zone != HitZone::None ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }

    case WM_TIMER: {
        if (!state) {
            return 0;
        }
        if (wParam == kTimerAnimId) {
            const double now = NowSec(state);
            UpdateAnimationState(state, now);
            if (state->pulse_alpha <= 0.0f) {
                KillTimer(hwnd, kTimerAnimId);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == kTimerHoldId) {
            KillTimer(hwnd, kTimerHoldId);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == kTimerCopiedId) {
            KillTimer(hwnd, kTimerCopiedId);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case kWmDpiChanged: {
        if (state) {
            state->dpi = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            ComputeLayout(state);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, kTimerAnimId);
        KillTimer(hwnd, kTimerHoldId);
        KillTimer(hwnd, kTimerCopiedId);
        if (state) {
            FreeBackbuffer(state);
        }
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    EnableDpiAwareness();

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        return 1;
    }

    int exit_code = 0;
    {
        AppState state{};
        QueryPerformanceFrequency(&state.qpc_freq);

        HICON icon_big = static_cast<HICON>(LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR));
        HICON icon_small = static_cast<HICON>(LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.hIcon = icon_big;
        wc.hIconSm = icon_small;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClassName;

        if (!RegisterClassExW(&wc)) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return 1;
        }

        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        const DWORD ex_style = WS_EX_TOPMOST;

        // Determine primary work-area DPI via desktop or fallback 96, then refine once created
        UINT init_dpi = 96;
        HDC screen_dc = GetDC(nullptr);
        if (screen_dc) {
            init_dpi = static_cast<UINT>(GetDeviceCaps(screen_dc, LOGPIXELSX));
            ReleaseDC(nullptr, screen_dc);
        }
        if (init_dpi < 96) {
            init_dpi = 96;
        }

        RECT wr = {0, 0, ScalePx(kBaseClientW, init_dpi), ScalePx(kBaseClientH, init_dpi)};
        AdjustWindowRectDpi(&wr, style, ex_style, init_dpi);
        const int win_w = wr.right - wr.left;
        const int win_h = wr.bottom - wr.top;

        RECT work_area = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
        const int pos_x = work_area.left + ((work_area.right - work_area.left) - win_w) / 2;
        const int pos_y = work_area.top + ((work_area.bottom - work_area.top) - win_h) / 2;

        HWND hwnd = CreateWindowExW(
            ex_style,
            kWindowClassName,
            kWindowTitle,
            style,
            pos_x,
            pos_y,
            win_w,
            win_h,
            nullptr,
            nullptr,
            hInstance,
            &state);

        if (!hwnd) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return 1;
        }

        // Re-adjust if actual window DPI differs from desktop DPI (multi-monitor setups)
        const UINT actual_dpi = QueryWindowDpi(hwnd);
        if (actual_dpi != init_dpi) {
            state.dpi = actual_dpi;
            RECT r2 = {0, 0, ScalePx(kBaseClientW, actual_dpi), ScalePx(kBaseClientH, actual_dpi)};
            AdjustWindowRectDpi(&r2, style, ex_style, actual_dpi);
            SetWindowPos(
                hwnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exit_code = static_cast<int>(msg.wParam);
    }

    Gdiplus::GdiplusShutdown(gdiplus_token);
    return exit_code;
}
