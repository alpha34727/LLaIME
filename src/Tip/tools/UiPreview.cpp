// Renders the IME's windows and saves them as PNG files, for checking the UI without
// installing the TIP. Usage: LLaIME.UiPreview.exe <output-dir>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Globals.h"
#include "ui/CandidateWindow.h"
#include "ui/PopupMenu.h"
#include "ui/TextIcon.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;
using namespace llaime;

namespace {

void Pump(int ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  MSG msg;
  while (std::chrono::steady_clock::now() < end) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(10);
  }
}

bool SavePixels(void* bits, int w, int h, const std::wstring& path);

bool SavePng(HWND hwnd, const std::wstring& path) {
  RECT rc;
  GetWindowRect(hwnd, &rc);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  HGDIOBJ old = SelectObject(dc, bmp);
  PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
  SelectObject(dc, old);
  DeleteDC(dc);
  bool ok = SavePixels(bits, w, h, path);
  DeleteObject(bmp);
  return ok;
}

bool SavePixels(void* bits, int w, int h, const std::wstring& path) {

  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapFrameEncode> frame;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
  // PrintWindow leaves alpha at 0; make the pixels opaque for a BGRA PNG.
  auto* px = static_cast<BYTE*>(bits);
  for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;
  if (ok) {
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = SUCCEEDED(frame->SetPixelFormat(&format)) &&
         IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
         SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, static_cast<BYTE*>(bits))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  }
  return ok;
}

void PreviewCandidates(const std::wstring& dir, const wchar_t* suffix) {
  CandidateWindow window({});
  std::vector<std::wstring> items = {
      L"意義", L"異議", L"意譯", L"泄泄", L"洩洩", L"意", L"一", L"億", L"義", L"亦",
      L"益",   L"易",   L"逸",   L"溢",   L"奕",   L"疫", L"藝", L"裔", L"翼", L"詣",
      L"議",   L"異",   L"毅",   L"翊",   L"懿",   L"羿", L"誼", L"邑", L"弈", L"抑",
      L"鎰",   L"驛",   L"悒",   L"熠",   L"👌",   L"😀", L"👍", L"🙂", L"役", L"憶"};
  for (bool expanded : {false, true}) {
    window.SetExpanded(expanded);
    window.Show(items, RECT{300, 300, 320, 320}, true);
    window.SetProbabilities({0.80, 0.12, 0.04, 0.02, 0.01, 0.004, 0.003, 0.001});
    Pump(400);
    HWND hwnd = FindWindowW(L"LLaIME.CandidateWindow", nullptr);
    std::wstring name = expanded ? L"/candidates_expanded_" : L"/candidates_";
    if (hwnd) SavePng(hwnd, dir + name + suffix + L".png");
    window.Hide();
  }
}

void PreviewMenu(const std::wstring& dir, const wchar_t* suffix) {
  std::wstring path = dir + L"\\menu_" + suffix + L".png";
  // The menu runs a modal loop; capture it from another thread, then dismiss it by
  // clicking (posting a button-up on) its first item.
  std::thread capture([path] {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Sleep(500);
    HWND hwnd = FindWindowW(L"LLaIME.PopupMenu", nullptr);
    if (!hwnd) return;
    SavePng(hwnd, path);
    UINT dpi = GetDpiForWindow(hwnd);
    int y = MulDiv(4 + 16, dpi, 96);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(60, dpi, 96), y));
    CoUninitialize();
  });
  ShowPopupMenu({
      {1, L"中文", true},
      {2, L"英數", false},
      {0, L"", false, true},
      {3, L"半形", true},
      {4, L"全形", false},
      {0, L"", false, true},
      {5, L"暫停學習", false},
  });
  capture.join();
}

// Draws the taskbar 中/英 icons (current Windows theme) on a taskbar-colored strip.
void PreviewTaskbarIcons(const std::wstring& dir) {
  const int size = LangBarIconSize();
  const int w = size * 9, h = size * 2;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  HGDIOBJ old = SelectObject(dc, bmp);
  RECT rc{0, 0, w, h};
  HBRUSH bg = CreateSolidBrush(SystemUsesLightTheme() ? RGB(238, 238, 238) : RGB(28, 28, 28));
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  int x = size;
  for (int i = 0; i < 4; ++i) {
    HICON icon = CreateTaskbarTextIcon(i % 2 ? L"英" : L"中", size, i >= 2);
    DrawIconEx(dc, x, size / 2, icon, size, size, 0, nullptr, DI_NORMAL);
    DestroyIcon(icon);
    x += size * 2;
  }
  SelectObject(dc, old);
  DeleteDC(dc);
  SavePixels(bits, w, h, dir + L"/taskbar_icons.png");
  DeleteObject(bmp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_hInst = hInst;
  std::wstring dir = cmdLine && *cmdLine ? cmdLine : L".";
  for (int dark : {0, 1}) {
    Theme::OverrideForPreview(dark);
    const wchar_t* suffix = dark ? L"dark" : L"light";
    PreviewCandidates(dir, suffix);
    PreviewMenu(dir, suffix);
  }
  PreviewTaskbarIcons(dir);
  CoUninitialize();
  return 0;
}
