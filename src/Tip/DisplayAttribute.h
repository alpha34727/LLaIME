#pragma once
#include <msctf.h>

namespace llaime {

// Creates the display attribute info for `guid`, or returns E_INVALIDARG for unknown GUIDs.
HRESULT CreateDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info);
HRESULT CreateDisplayAttributeEnum(IEnumTfDisplayAttributeInfo** enumInfo);

}  // namespace llaime
