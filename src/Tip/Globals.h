#pragma once
#include <windows.h>
#include <msctf.h>

namespace llaime {

extern HINSTANCE g_hInst;
extern LONG g_dllRefCount;

void DllAddRef();
void DllRelease();

// {5B72F657-F087-4472-A827-7FD8EB608DE6}
inline constexpr CLSID CLSID_TextService = {
    0x5b72f657, 0xf087, 0x4472, {0xa8, 0x27, 0x7f, 0xd8, 0xeb, 0x60, 0x8d, 0xe6}};

// {671F61EB-E460-42B0-8B3A-C90CB1488716}
inline constexpr GUID GUID_ProfileBopomofo = {
    0x671f61eb, 0xe460, 0x42b0, {0x8b, 0x3a, 0xc9, 0x0c, 0xb1, 0x48, 0x87, 0x16}};

// {28D85CA4-8A21-4090-A922-1C200BBB3CDB}
inline constexpr GUID GUID_DisplayAttributeInput = {
    0x28d85ca4, 0x8a21, 0x4090, {0xa9, 0x22, 0x1c, 0x20, 0x0b, 0xbb, 0x3c, 0xdb}};

// {677D3CAB-EB1B-44BB-AABB-EFAD22834C8E}
inline constexpr GUID GUID_DisplayAttributeConverted = {
    0x677d3cab, 0xeb1b, 0x44bb, {0xaa, 0xbb, 0xef, 0xad, 0x22, 0x83, 0x4c, 0x8e}};

inline constexpr LANGID kLangIdZhTw = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);

}  // namespace llaime
