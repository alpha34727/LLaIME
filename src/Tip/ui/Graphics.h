#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <string>

namespace llaime {

// Process-wide Direct2D / DirectWrite factories shared by the IME's windows.
ID2D1Factory* D2DFactory();
IDWriteFactory* DWriteFactory();

// Draws text with color glyphs enabled, so emoji render in color (Segoe UI Emoji fallback).
void DrawColorText(ID2D1RenderTarget* target, const std::wstring& text, IDWriteTextFormat* format,
                   const D2D1_RECT_F& rect, ID2D1Brush* brush);

float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format);

// Applies Windows 11 popup chrome: rounded corners and a border matching the theme.
void ApplyPopupChrome(HWND hwnd, bool dark);

}  // namespace llaime
