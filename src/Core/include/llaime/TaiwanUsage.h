#pragma once
#include <string>
#include <string_view>

namespace llaime {

// Words whose spelling differs between Taiwan's Ministry of Education standard and common
// mainland usage while the reading is identical, so only the language model's preference can
// choose between them, and general-purpose models lean to the mainland form. Each entry is
// {non-standard, Taiwan standard}.
inline constexpr std::wstring_view kTaiwanVariants[][2] = {
    {L"身份", L"身分"}, {L"計劃", L"計畫"}, {L"部份", L"部分"}, {L"公佈", L"公布"},
    {L"分佈", L"分布"}, {L"佈置", L"布置"}, {L"散佈", L"散布"}, {L"宣佈", L"宣布"},
    {L"頒佈", L"頒布"}, {L"惟一", L"唯一"}, {L"煙酒", L"菸酒"}, {L"香煙", L"香菸"},
    {L"周末", L"週末"}, {L"周年", L"週年"}, {L"周刊", L"週刊"}, {L"櫃台", L"櫃檯"},
    {L"面包", L"麵包"}, {L"面條", L"麵條"}, {L"裏面", L"裡面"}, {L"這裏", L"這裡"},
    {L"那裏", L"那裡"}, {L"哪裏", L"哪裡"}, {L"着急", L"著急"}, {L"沈默", L"沉默"},
    {L"啓動", L"啟動"}, {L"綫上", L"線上"}, {L"記錄片", L"紀錄片"},
};

// Number of non-standard variant words in `text`.
inline int CountNonTaiwanVariants(std::wstring_view text) {
  int count = 0;
  for (const auto& pair : kTaiwanVariants) {
    if (pair[0] == pair[1]) continue;
    for (size_t pos = text.find(pair[0]); pos != std::wstring_view::npos;
         pos = text.find(pair[0], pos + 1)) {
      ++count;
    }
  }
  return count;
}

}  // namespace llaime
