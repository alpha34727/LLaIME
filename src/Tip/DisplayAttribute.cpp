#include "DisplayAttribute.h"

#include <iterator>
#include <new>

#include "Globals.h"

namespace llaime {
namespace {

struct AttributeDef {
  const GUID* guid;
  const wchar_t* description;
  TF_DISPLAYATTRIBUTE attr;
};

// Matches Microsoft Bopomofo: text being composed gets a thin solid underline in the text color.
const AttributeDef kAttributes[] = {
    {&GUID_DisplayAttributeInput,
     L"LLaIME Input",
     {{TF_CT_NONE, {0}}, {TF_CT_NONE, {0}}, TF_LS_SOLID, FALSE, {TF_CT_NONE, {0}}, TF_ATTR_INPUT}},
    {&GUID_DisplayAttributeConverted,
     L"LLaIME Converted",
     {{TF_CT_NONE, {0}}, {TF_CT_NONE, {0}}, TF_LS_SOLID, TRUE, {TF_CT_NONE, {0}},
      TF_ATTR_TARGET_CONVERTED}},
};

template <typename Interface>
class RefCounted : public Interface {
 public:
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
  }

 protected:
  RefCounted() { DllAddRef(); }
  virtual ~RefCounted() { DllRelease(); }

 private:
  LONG ref_ = 1;
};

class DisplayAttributeInfo final : public RefCounted<ITfDisplayAttributeInfo> {
 public:
  explicit DisplayAttributeInfo(const AttributeDef& def) : def_(def) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) {
      *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP GetGUID(GUID* guid) override {
    if (!guid) return E_INVALIDARG;
    *guid = *def_.guid;
    return S_OK;
  }
  STDMETHODIMP GetDescription(BSTR* desc) override {
    if (!desc) return E_INVALIDARG;
    *desc = SysAllocString(def_.description);
    return *desc ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* attr) override {
    if (!attr) return E_INVALIDARG;
    *attr = def_.attr;
    return S_OK;
  }
  STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override { return E_NOTIMPL; }
  STDMETHODIMP Reset() override { return S_OK; }

 private:
  const AttributeDef& def_;
};

class DisplayAttributeEnum final : public RefCounted<IEnumTfDisplayAttributeInfo> {
 public:
  explicit DisplayAttributeEnum(size_t index = 0) : index_(index) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
      *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** out) override {
    if (!out) return E_INVALIDARG;
    *out = new (std::nothrow) DisplayAttributeEnum(index_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP Next(ULONG count, ITfDisplayAttributeInfo** infos, ULONG* fetched) override {
    if (!infos) return E_INVALIDARG;
    ULONG n = 0;
    while (n < count && index_ < std::size(kAttributes)) {
      infos[n] = new (std::nothrow) DisplayAttributeInfo(kAttributes[index_]);
      if (!infos[n]) break;
      ++n;
      ++index_;
    }
    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Reset() override {
    index_ = 0;
    return S_OK;
  }
  STDMETHODIMP Skip(ULONG count) override {
    index_ += count;
    return index_ <= std::size(kAttributes) ? S_OK : S_FALSE;
  }

 private:
  size_t index_;
};

}  // namespace

HRESULT CreateDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
  if (!info) return E_INVALIDARG;
  *info = nullptr;
  for (const AttributeDef& def : kAttributes) {
    if (*def.guid == guid) {
      *info = new (std::nothrow) DisplayAttributeInfo(def);
      return *info ? S_OK : E_OUTOFMEMORY;
    }
  }
  return E_INVALIDARG;
}

HRESULT CreateDisplayAttributeEnum(IEnumTfDisplayAttributeInfo** enumInfo) {
  if (!enumInfo) return E_INVALIDARG;
  *enumInfo = new (std::nothrow) DisplayAttributeEnum();
  return *enumInfo ? S_OK : E_OUTOFMEMORY;
}

}  // namespace llaime
