// ============================================================================
//  dinput8.cpp  -  DirectInput 8 proxy that merges a wheel and a separate
//                  pedal set into ONE device for games that only accept one.
//
//  Written for Sebastien Loeb Rally EVO (EngineX64.dll imports
//  DirectInput8Create from dinput8.dll and uses the Unicode IDirectInput8W
//  interface plus DirectInput force feedback).
//
//  STATUS: DRAFT. Written without being compiled or run against real
//  hardware. Expect to fix compile errors and to tune the axis mapping.
//
//  How it works
//   - The game loads this dinput8.dll (from the game folder) instead of the
//     system one. We forward everything to the real system dinput8.dll.
//   - On enumeration we find the wheel and the pedals by (part of) their
//     product name. The pedals are hidden from the game; the wheel is shown.
//   - When the game creates the wheel, it gets a wrapper object instead:
//       * force feedback / effects / properties go straight to the real wheel
//       * GetDeviceState reads the wheel, then overwrites selected axes with
//         values read from the pedals (mapping configured in the .ini)
//   - Optionally the wheel can present itself with another vendor/product ID
//     and name (e.g. a T300RS) so the game picks a built-in wheel profile.
//
//  Build: see README.md.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <initguid.h>   // defines the GUID_* / IID_* constants in this module
#include <dinput.h>

// Documented value; some SDK versions of dinput.h don't define it.
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static const char* kSlotName[8] = { "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider0", "Slider1" };

static std::string W2A(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    return s;
}

static std::string GuidStr(const GUID& g)
{
    char b[80];
    snprintf(b, sizeof(b), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

static std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static bool ContainsI(const std::wstring& hay, const std::wstring& needle)
{
    if (needle.empty()) return false;
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t e = s.find(sep, start);
        out.push_back(Trim(s.substr(start, e == std::wstring::npos ? std::wstring::npos : e - start)));
        if (e == std::wstring::npos) break;
        start = e + 1;
    }
    return out;
}

// Axis name -> slot index: 0..5 = X,Y,Z,Rx,Ry,Rz  6,7 = Slider0, Slider1
static int AxisIndex(const std::wstring& name)
{
    std::wstring n = Lower(Trim(name));
    if (n == L"x") return 0;
    if (n == L"y") return 1;
    if (n == L"z") return 2;
    if (n == L"rx") return 3;
    if (n == L"ry") return 4;
    if (n == L"rz") return 5;
    if (n == L"slider0") return 6;
    if (n == L"slider1") return 7;
    return -1;
}

// Key name -> Windows virtual-key code, or -1.
static int VkFromName(const std::wstring& nameIn)
{
    std::wstring n = Lower(Trim(nameIn));
    if (n == L"esc" || n == L"escape") return VK_ESCAPE;
    if (n == L"enter" || n == L"return") return VK_RETURN;
    if (n == L"space") return VK_SPACE;
    if (n == L"tab") return VK_TAB;
    if (n == L"pause") return VK_PAUSE;
    if (n == L"pageup" || n == L"pgup") return 0x21;       // VK_PRIOR
    if (n == L"pagedown" || n == L"pgdn") return 0x22;     // VK_NEXT
    if (n == L"end") return 0x23;
    if (n == L"home") return 0x24;
    if (n == L"insert" || n == L"ins") return 0x2D;
    if (n == L"delete" || n == L"del") return 0x2E;
    if (n == L"numpadplus" || n == L"add") return 0x6B;    // VK_ADD
    if (n == L"numpadminus" || n == L"subtract") return 0x6D; // VK_SUBTRACT
    if (n.size() == 1 && iswalpha(n[0])) return (int)towupper(n[0]);
    if (n.size() == 1 && iswdigit(n[0])) return (int)n[0];
    if (n.size() >= 2 && n[0] == L'f') {
        int f = (int)wcstol(n.c_str() + 1, nullptr, 10);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    return -1;
}

// DirectInput property IDs are small integers disguised as GUID references.
static inline bool IsProp(REFGUID g, REFGUID p) { return &g == &p; }

// ----------------------------------------------------------------------------
//  Config + logging
// ----------------------------------------------------------------------------
struct MapEntry { int src; int dst; bool invert; };

// Virtual button: make the game see button 'dst' pressed when a key or a wheel button is down.
struct ButtonMap { bool isKey; int vk; int src; int dst; bool move; };

struct Config {
    bool enabled = true;
    bool log     = true;
    bool debug   = true;
    bool ignoreCenter = true;
    std::wstring wheelMatch  = L"TS-PC";
    std::wstring pedalsMatch = L"Fanatec";
    bool spoof = false;
    WORD vid = 0x044F;
    WORD pid = 0xB66E;
    std::wstring spoofName = L"Thrustmaster T300RS Racing Wheel";
    std::vector<MapEntry> maps;
    std::vector<ButtonMap> buttons;
    // Experimental live field-of-view hotkeys (patches the camera setting in game memory)
    bool fovEnabled = false;
    int  fovDownVk  = 0x22;   // Page Down
    int  fovUpVk    = 0x21;   // Page Up
    int  fovStep    = 5;
    int  fovMin     = 30;
    int  fovMax     = 120;
};

static Config       gCfg;
static std::wstring gDir;
static std::wstring gIniPath;
static FILE*        gLog = nullptr;
static std::mutex   gLogMx;

static void Log(const char* fmt, ...)
{
    if (!gLog) return;
    std::lock_guard<std::mutex> lock(gLogMx);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(gLog, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(gLog, fmt, a);
    va_end(a);
    fputc('\n', gLog);
    fflush(gLog);
}

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def, buf, 512, gIniPath.c_str());
    return buf;
}

static long IniNum(const wchar_t* sec, const wchar_t* key, long def)
{
    std::wstring s = IniStr(sec, key, L"");
    if (s.empty()) return def;
    return wcstol(s.c_str(), nullptr, 0);   // base 0: accepts 0x044F
}

static void LoadConfig()
{
    gCfg.enabled = IniNum(L"General", L"Enabled", 1) != 0;
    gCfg.log     = IniNum(L"General", L"Log", 1) != 0;
    gCfg.debug   = IniNum(L"General", L"Debug", 1) != 0;
    gCfg.ignoreCenter = IniNum(L"General", L"IgnoreCenterUntilMoved", 1) != 0;

    gCfg.wheelMatch  = IniStr(L"Devices", L"WheelMatch",  L"TS-PC");
    gCfg.pedalsMatch = IniStr(L"Devices", L"PedalsMatch", L"Fanatec");

    gCfg.spoof     = IniNum(L"Spoof", L"Enabled", 0) != 0;
    gCfg.vid       = (WORD)IniNum(L"Spoof", L"VendorId", 0x044F);
    gCfg.pid       = (WORD)IniNum(L"Spoof", L"ProductId", 0xB66E);
    gCfg.spoofName = IniStr(L"Spoof", L"ProductName", L"Thrustmaster T300RS Racing Wheel");

    gCfg.maps.clear();
    for (int i = 1; i <= 8; ++i) {
        std::wstring key = L"Map" + std::to_wstring(i);
        std::wstring val = IniStr(L"Map", key.c_str(), L"");
        if (val.empty()) continue;
        std::vector<std::wstring> parts = Split(val, L',');
        if (parts.size() < 2) continue;
        MapEntry m;
        m.src = AxisIndex(parts[0]);
        m.dst = AxisIndex(parts[1]);
        m.invert = parts.size() >= 3 && wcstol(parts[2].c_str(), nullptr, 0) != 0;
        if (m.src >= 0 && m.dst >= 0) gCfg.maps.push_back(m);
    }

    gCfg.fovEnabled = IniNum(L"Fov", L"Enabled", 0) != 0;
    int dk = VkFromName(IniStr(L"Fov", L"DownKey", L"PageDown"));
    int uk = VkFromName(IniStr(L"Fov", L"UpKey", L"PageUp"));
    if (dk > 0) gCfg.fovDownVk = dk;
    if (uk > 0) gCfg.fovUpVk = uk;
    gCfg.fovStep = (int)IniNum(L"Fov", L"Step", 5);
    gCfg.fovMin  = (int)IniNum(L"Fov", L"Min", 30);
    gCfg.fovMax  = (int)IniNum(L"Fov", L"Max", 120);
    if (gCfg.fovStep < 1) gCfg.fovStep = 1;
    if (gCfg.fovMin < 1) gCfg.fovMin = 1;
    if (gCfg.fovMax > 999) gCfg.fovMax = 999;

    gCfg.buttons.clear();
    for (int i = 1; i <= 8; ++i) {
        std::wstring key = L"Button" + std::to_wstring(i);
        std::wstring val = IniStr(L"Buttons", key.c_str(), L"");
        if (val.empty()) continue;
        std::vector<std::wstring> parts = Split(val, L',');
        if (parts.size() < 2) continue;
        size_t colon = parts[0].find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring kind = Lower(Trim(parts[0].substr(0, colon)));
        std::wstring what = Trim(parts[0].substr(colon + 1));
        ButtonMap b;
        b.isKey = false; b.vk = -1; b.src = -1;
        b.dst  = (int)wcstol(parts[1].c_str(), nullptr, 0);
        b.move = parts.size() >= 3 && wcstol(parts[2].c_str(), nullptr, 0) != 0;
        if (kind == L"key") {
            b.isKey = true;
            b.vk = VkFromName(what);
            if (b.vk < 0) continue;
        } else if (kind == L"wheel") {
            b.src = (int)wcstol(what.c_str(), nullptr, 0);
            if (b.src < 0 || b.src >= 128) continue;
        } else {
            continue;
        }
        if (b.dst < 0 || b.dst >= 128) continue;
        gCfg.buttons.push_back(b);
    }
}

// ----------------------------------------------------------------------------
//  Global state
// ----------------------------------------------------------------------------
struct DevRef {
    bool         valid = false;
    GUID         inst  = {};
    std::wstring product;
};

struct MergeState {
    bool   active = false;
    DevRef wheel;
    DevRef pedals;
};

static MergeState gMerge;
static std::mutex gMergeMx;
static int        gDiscoverCount = 0;

typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DI8Create gRealCreate = nullptr;
static std::once_flag gInitOnce;

// Minimal data format used to read the pedals: 8 axis slots, 4 bytes each.
static DIOBJECTDATAFORMAT gPedalObjs[8];
static DIDATAFORMAT       gPedalFmt;

static void InitPedalFormat()
{
    const GUID* g[8] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
                         &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider };
    for (int i = 0; i < 8; ++i) {
        gPedalObjs[i].pguid   = g[i];
        gPedalObjs[i].dwOfs   = (DWORD)(i * 4);
        gPedalObjs[i].dwType  = DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE;
        gPedalObjs[i].dwFlags = 0;
    }
    gPedalFmt.dwSize     = sizeof(DIDATAFORMAT);
    gPedalFmt.dwObjSize  = sizeof(DIOBJECTDATAFORMAT);
    gPedalFmt.dwFlags    = DIDF_ABSAXIS;
    gPedalFmt.dwDataSize = 8 * 4;
    gPedalFmt.dwNumObjs  = 8;
    gPedalFmt.rgodf      = gPedalObjs;
}

// Returns slot 0..7 for an axis GUID, or -1. 'sliders' counts Slider GUIDs seen.
static int SlotFromGuid(const GUID& g, int& sliders)
{
    if (IsEqualGUID(g, GUID_XAxis))  return 0;
    if (IsEqualGUID(g, GUID_YAxis))  return 1;
    if (IsEqualGUID(g, GUID_ZAxis))  return 2;
    if (IsEqualGUID(g, GUID_RxAxis)) return 3;
    if (IsEqualGUID(g, GUID_RyAxis)) return 4;
    if (IsEqualGUID(g, GUID_RzAxis)) return 5;
    if (IsEqualGUID(g, GUID_Slider)) {
        int s = sliders++;
        return s < 2 ? 6 + s : -1;
    }
    return -1;
}

static void ApplySpoof(DIDEVICEINSTANCEW& di)
{
    if (!gCfg.spoof) return;
    // Product GUID layout: Data1 = (PID << 16) | VID, tail = "PIDVID"
    di.guidProduct.Data1 = (DWORD)MAKELONG(gCfg.vid, gCfg.pid);
    di.guidProduct.Data2 = 0;
    di.guidProduct.Data3 = 0;
    const BYTE tail[8] = { 0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D' };
    memcpy(di.guidProduct.Data4, tail, 8);
    wcsncpy(di.tszProductName,  gCfg.spoofName.c_str(), MAX_PATH - 1);
    wcsncpy(di.tszInstanceName, gCfg.spoofName.c_str(), MAX_PATH - 1);
    di.tszProductName[MAX_PATH - 1]  = 0;
    di.tszInstanceName[MAX_PATH - 1] = 0;
}

// ----------------------------------------------------------------------------
//  Device discovery
// ----------------------------------------------------------------------------
struct DiscCtx {
    DevRef wheel;
    DevRef pedals;
    bool   verbose = false;
};

static BOOL CALLBACK DiscoverCb(LPCDIDEVICEINSTANCEW di, LPVOID ctx)
{
    DiscCtx* c = (DiscCtx*)ctx;
    if (c->verbose) {
        Log("  device: product='%s' instance='%s' type=0x%08lX vid:pid=%04X:%04X guid=%s",
            W2A(di->tszProductName).c_str(), W2A(di->tszInstanceName).c_str(),
            (unsigned long)di->dwDevType,
            (unsigned)LOWORD(di->guidProduct.Data1), (unsigned)HIWORD(di->guidProduct.Data1),
            GuidStr(di->guidInstance).c_str());
    }
    if (!c->wheel.valid && ContainsI(di->tszProductName, gCfg.wheelMatch)) {
        c->wheel.valid = true;
        c->wheel.inst = di->guidInstance;
        c->wheel.product = di->tszProductName;
    } else if (!c->pedals.valid && ContainsI(di->tszProductName, gCfg.pedalsMatch)) {
        c->pedals.valid = true;
        c->pedals.inst = di->guidInstance;
        c->pedals.product = di->tszProductName;
    }
    return DIENUM_CONTINUE;
}

static void Discover(IDirectInput8W* di)
{
    DiscCtx c;
    c.verbose = (gDiscoverCount < 2);
    if (c.verbose) Log("Discovering game controllers (WheelMatch='%s', PedalsMatch='%s')",
                       W2A(gCfg.wheelMatch.c_str()).c_str(), W2A(gCfg.pedalsMatch.c_str()).c_str());
    ++gDiscoverCount;
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, DiscoverCb, &c, DIEDFL_ATTACHEDONLY);

    std::lock_guard<std::mutex> lock(gMergeMx);
    bool nowActive = c.wheel.valid && c.pedals.valid;
    if (nowActive != gMerge.active || c.verbose) {
        if (nowActive)
            Log("MERGE ACTIVE: wheel='%s' + pedals='%s'",
                W2A(c.wheel.product.c_str()).c_str(), W2A(c.pedals.product.c_str()).c_str());
        else
            Log("merge inactive (wheel found=%d, pedals found=%d) - game sees devices unchanged",
                (int)c.wheel.valid, (int)c.pedals.valid);
    }
    gMerge.wheel  = c.wheel;
    gMerge.pedals = c.pedals;
    gMerge.active = nowActive;
}

// ----------------------------------------------------------------------------
//  Merged device (what the game gets instead of the real wheel)
// ----------------------------------------------------------------------------
class MergedDevice;

struct ObjCtx {
    MergedDevice*                  dev;
    LPDIENUMDEVICEOBJECTSCALLBACKW cb;
    LPVOID                         ref;
    int                            sliders;
};

static BOOL CALLBACK ObjThunk(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ctx);

class MergedDevice final : public IDirectInputDevice8W {
public:
    MergedDevice(IDirectInputDevice8W* wheel, IDirectInputDevice8W* pedals)
        : w_(wheel), p_(pedals), ref_(1), lastDump_(0), dumpTick_(0), acquireLogs_(0),
          haveLast_(false), lastMin_(0), lastMax_(65535), warnedBuffered_(false)
    {
        for (int i = 0; i < 8; ++i) {
            dstOfs_[i] = -1; rmin_[i] = 0; rmax_[i] = 65535; rangeSet_[i] = false;
            ResetStats(i);
        }
        nBtn_ = 0;
        for (int i = 0; i < 128; ++i) { buttonOfs_[i] = -1; prevBtn_[i] = false; }
        vdown_.assign(gCfg.buttons.size(), 0);
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputDevice8W)) {
            *ppv = static_cast<IDirectInputDevice8W*>(this);
            AddRef();
            return S_OK;
        }
        Log("MergedDevice::QueryInterface for unsupported IID %s -> forwarded to wheel (NOT merged)",
            GuidStr(riid).c_str());
        return w_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            w_->Release();
            p_->Release();
            delete this;
        }
        return (ULONG)r;
    }

    // ---- Called from ObjThunk: remember which object ID is which axis ----
    void NoteObject(LPCDIDEVICEOBJECTINSTANCEW o, int& sliders)
    {
        int slot = SlotFromGuid(o->guidType, sliders);
        if (slot >= 0) typeSlot_[o->dwType] = slot;
    }

    // ---- Capabilities / objects ----
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override { return w_->GetCapabilities(c); }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID ref, DWORD flags) override
    {
        ObjCtx c;
        c.dev = this; c.cb = cb; c.ref = ref; c.sliders = 0;
        return w_->EnumObjects(ObjThunk, &c, flags);
    }

    // ---- Properties ----
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER h) override
    {
        HRESULT hr = w_->GetProperty(g, h);
        if (gCfg.debug) Log("GetProperty id=%lu how=%lu obj=0x%lX -> 0x%08lX",
                            (unsigned long)(uintptr_t)&g, (unsigned long)h->dwHow,
                            (unsigned long)h->dwObj, (unsigned long)hr);
        if (SUCCEEDED(hr) && gCfg.spoof) {
            if (IsProp(g, DIPROP_VIDPID)) {
                ((DIPROPDWORD*)h)->dwData = (DWORD)MAKELONG(gCfg.vid, gCfg.pid);
            } else if (IsProp(g, DIPROP_PRODUCTNAME) || IsProp(g, DIPROP_INSTANCENAME)) {
                DIPROPSTRING* s = (DIPROPSTRING*)h;
                wcsncpy(s->wsz, gCfg.spoofName.c_str(), MAX_PATH - 1);
                s->wsz[MAX_PATH - 1] = 0;
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER h) override
    {
        if (IsProp(g, DIPROP_RANGE)) RecordRange(h);
        if (gCfg.debug) Log("SetProperty id=%lu how=%lu obj=0x%lX",
                            (unsigned long)(uintptr_t)&g, (unsigned long)h->dwHow,
                            (unsigned long)h->dwObj);
        return w_->SetProperty(g, h);
    }

    // ---- Acquire / poll ----
    HRESULT STDMETHODCALLTYPE Acquire() override
    {
        HRESULT hr = w_->Acquire();
        HRESULT hp = p_->Acquire();
        if (acquireLogs_ < 5) {
            ++acquireLogs_;
            Log("Acquire: wheel=0x%08lX pedals=0x%08lX (further Acquire calls not logged)",
                (unsigned long)hr, (unsigned long)hp);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        p_->Unacquire();
        return w_->Unacquire();
    }
    HRESULT STDMETHODCALLTYPE Poll() override
    {
        HRESULT hr = w_->Poll();
        p_->Poll();
        return hr;
    }

    // ---- The important part: read the wheel, then overlay pedal axes ----
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override
    {
        HRESULT hr = w_->GetDeviceState(cb, data);
        if (FAILED(hr) || !data) return hr;

        LONG ps[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        HRESULT hp = p_->GetDeviceState(sizeof(ps), ps);
        if (hp == DIERR_INPUTLOST || hp == DIERR_NOTACQUIRED) {
            if (SUCCEEDED(p_->Acquire())) hp = p_->GetDeviceState(sizeof(ps), ps);
        }
        if (FAILED(hp)) {            // pedals unavailable: wheel axes unchanged
            ApplyButtonMaps((BYTE*)data, cb);
            return hr;
        }

        // Some pedal sets send no report until a pedal is first moved, so DirectInput
        // returns mid-scale (32768) on every axis. Without this the game would see all
        // pedals half-pressed. If every mapped pedal axis is exactly mid-scale, treat the
        // pedals as released (0) until they report something else.
        if (gCfg.ignoreCenter && !gCfg.maps.empty()) {
            bool allCenter = true;
            for (size_t i = 0; i < gCfg.maps.size(); ++i)
                if (ps[gCfg.maps[i].src] != 32768) { allCenter = false; break; }
            if (allCenter)
                for (size_t i = 0; i < gCfg.maps.size(); ++i) ps[gCfg.maps[i].src] = 0;
        }

        BYTE* base = (BYTE*)data;

        for (size_t i = 0; i < gCfg.maps.size(); ++i) {
            const MapEntry& m = gCfg.maps[i];
            int ofs = dstOfs_[m.dst];
            if (ofs < 0 || (DWORD)ofs + 4 > cb) continue;
            LONG v = ps[m.src];
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            if (m.invert) v = 65535 - v;
            LONG lo = rmin_[m.dst], hi = rmax_[m.dst];
            if (!rangeSet_[m.dst] && haveLast_) { lo = lastMin_; hi = lastMax_; }
            LONG out = lo + (LONG)((double)v * (double)(hi - lo) / 65535.0);
            *(LONG*)(base + ofs) = out;
        }
        ApplyButtonMaps(base, cb);
        if (gCfg.debug) UpdateStats(base, cb, ps);
        return hr;
    }

    // Buffered mode is not merged (only wheel data). Warn once.
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD flags) override
    {
        if (!warnedBuffered_) {
            warnedBuffered_ = true;
            Log("WARNING: game uses GetDeviceData (buffered). Pedals are NOT merged in buffered mode.");
        }
        return w_->GetDeviceData(cb, d, n, flags);
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override
    {
        HRESULT hr = w_->SetDataFormat(df);
        Log("SetDataFormat(wheel) -> 0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr) && df) {
            ParseFormat(df);
            SetupPedals();
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE ev) override { return w_->SetEventNotification(ev); }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD flags) override
    {
        HRESULT hr = w_->SetCooperativeLevel(hwnd, flags);   // wheel keeps game's flags (exclusive for FFB)
        DWORD pf = DISCL_NONEXCLUSIVE | (flags & (DISCL_FOREGROUND | DISCL_BACKGROUND));
        if (!(pf & (DISCL_FOREGROUND | DISCL_BACKGROUND))) pf |= DISCL_FOREGROUND;
        HRESULT hp = p_->SetCooperativeLevel(hwnd, pf);
        Log("SetCooperativeLevel flags=0x%lX: wheel=0x%08lX pedals=0x%08lX",
            (unsigned long)flags, (unsigned long)hr, (unsigned long)hp);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW o, DWORD obj, DWORD how) override
    { return w_->GetObjectInfo(o, obj, how); }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW di) override
    {
        HRESULT hr = w_->GetDeviceInfo(di);
        if (SUCCEEDED(hr) && di) ApplySpoof(*di);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return w_->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE i, DWORD v, REFGUID g) override { return w_->Initialize(i, v, g); }

    // ---- Force feedback: everything goes to the real wheel ----
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override
    { return w_->CreateEffect(g, e, out, outer); }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW cb, LPVOID ref, DWORD t) override
    { return w_->EnumEffects(cb, ref, t); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW i, REFGUID g) override
    { return w_->GetEffectInfo(i, g); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override { return w_->GetForceFeedbackState(s); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD c) override { return w_->SendForceFeedbackCommand(c); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD f) override
    { return w_->EnumCreatedEffectObjects(cb, ref, f); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override { return w_->Escape(e); }

    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) override
    { return w_->SendDeviceData(cb, d, n, f); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR fn, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD f) override
    { return w_->EnumEffectsInFile(fn, cb, ref, f); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR fn, DWORD n, LPDIFILEEFFECT e, DWORD f) override
    { return w_->WriteEffectToFile(fn, n, e, f); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f) override
    { return w_->BuildActionMap(a, u, f); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f) override
    { return w_->SetActionMap(a, u, f); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW h) override
    { return w_->GetImageInfo(h); }

private:
    // Work out where the game's data format puts each axis.
    void ParseFormat(LPCDIDATAFORMAT df)
    {
        for (int i = 0; i < 8; ++i) dstOfs_[i] = -1;
        nBtn_ = 0;
        int sliders = 0;
        for (DWORD i = 0; i < df->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = df->rgodf[i];
            // Button entries in a data format normally have NO GUID, so recognise them by type.
            if ((o.dwType & DIDFT_BUTTON) != 0 && (o.dwType & DIDFT_AXIS) == 0) {
                if (nBtn_ < 128) buttonOfs_[nBtn_++] = (int)o.dwOfs;
                continue;
            }
            if (!o.pguid) continue;
            int slot = SlotFromGuid(*o.pguid, sliders);
            if (slot >= 0 && dstOfs_[slot] < 0) dstOfs_[slot] = (int)o.dwOfs;
        }
        std::string s;
        for (int i = 0; i < 8; ++i) {
            char t[40];
            snprintf(t, sizeof(t), "%s@%d ", kSlotName[i], dstOfs_[i]);
            s += t;
        }
        Log("Game data format: size=%lu objects=%lu buttons=%d  axis offsets: %s",
            (unsigned long)df->dwDataSize, (unsigned long)df->dwNumObjs, nBtn_, s.c_str());
    }

    // Give the pedals our own tiny 8-axis format with a fixed 0..65535 range.
    void SetupPedals()
    {
        HRESULT hf = p_->SetDataFormat(&gPedalFmt);
        DIPROPRANGE r;
        memset(&r, 0, sizeof(r));
        r.diph.dwSize       = sizeof(DIPROPRANGE);
        r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        r.diph.dwObj        = 0;
        r.diph.dwHow        = DIPH_DEVICE;
        r.lMin = 0;
        r.lMax = 65535;
        HRESULT hr = p_->SetProperty(DIPROP_RANGE, &r.diph);
        Log("Pedals: SetDataFormat=0x%08lX SetProperty(range)=0x%08lX", (unsigned long)hf, (unsigned long)hr);
    }

    // The game tells the wheel its axis range; remember it so pedal values can be scaled to match.
    void RecordRange(LPCDIPROPHEADER h)
    {
        if (!h || h->dwSize < sizeof(DIPROPRANGE)) return;
        const DIPROPRANGE* r = (const DIPROPRANGE*)h;
        haveLast_ = true;
        lastMin_ = r->lMin;
        lastMax_ = r->lMax;
        if (h->dwHow == DIPH_DEVICE) {
            for (int i = 0; i < 8; ++i) SetSlotRange(i, r->lMin, r->lMax);
        } else if (h->dwHow == DIPH_BYOFFSET) {
            for (int i = 0; i < 8; ++i)
                if (dstOfs_[i] >= 0 && (DWORD)dstOfs_[i] == h->dwObj) SetSlotRange(i, r->lMin, r->lMax);
        } else if (h->dwHow == DIPH_BYID) {
            std::map<DWORD, int>::const_iterator it = typeSlot_.find(h->dwObj);
            if (it != typeSlot_.end()) SetSlotRange(it->second, r->lMin, r->lMax);
        }
        if (gCfg.debug)
            Log("Game set axis range %ld..%ld (how=%lu obj=0x%lX)",
                (long)r->lMin, (long)r->lMax, (unsigned long)h->dwHow, (unsigned long)h->dwObj);
    }

    bool BtnDown(const BYTE* base, DWORD cb, int idx) const
    {
        if (idx < 0 || idx >= nBtn_) return false;
        int ofs = buttonOfs_[idx];
        if (ofs < 0 || (DWORD)ofs + 1 > cb) return false;
        return (base[ofs] & 0x80) != 0;
    }

    void SetBtn(BYTE* base, DWORD cb, int idx, bool down) const
    {
        if (idx < 0 || idx >= nBtn_) return;
        int ofs = buttonOfs_[idx];
        if (ofs < 0 || (DWORD)ofs + 1 > cb) return;
        base[ofs] = down ? 0x80 : 0x00;
    }

    static bool GameHasFocus()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // Debug aid: log which wheel button numbers change, so you can pick an unused one.
    void LogWheelButtons(const BYTE* base, DWORD cb)
    {
        for (int i = 0; i < nBtn_; ++i) {
            bool cur = BtnDown(base, cb, i);
            if (cur != prevBtn_[i]) {
                prevBtn_[i] = cur;
                Log("wheel button %d %s", i, cur ? "DOWN" : "up");
            }
        }
    }

    // Virtual buttons: a key or another wheel button makes the game see 'dst' pressed.
    void ApplyButtonMaps(BYTE* base, DWORD cb)
    {
        if (nBtn_ == 0) return;
        if (gCfg.debug) LogWheelButtons(base, cb);
        if (gCfg.buttons.empty()) return;

        bool focused = GameHasFocus();
        std::vector<int> setIdx, clearIdx;
        for (size_t i = 0; i < gCfg.buttons.size(); ++i) {
            const ButtonMap& b = gCfg.buttons[i];
            bool down = b.isKey ? (focused && (GetAsyncKeyState(b.vk) & 0x8000) != 0)
                                : BtnDown(base, cb, b.src);
            if ((char)down != vdown_[i]) {
                vdown_[i] = (char)down;
                Log("virtual button -> game button %d %s", b.dst, down ? "DOWN" : "up");
            }
            if (down) {
                setIdx.push_back(b.dst);
                if (!b.isKey && b.move) clearIdx.push_back(b.src);
            }
        }
        for (size_t i = 0; i < clearIdx.size(); ++i) SetBtn(base, cb, clearIdx[i], false);
        for (size_t i = 0; i < setIdx.size(); ++i)   SetBtn(base, cb, setIdx[i], true);
    }

    void SetSlotRange(int slot, LONG lo, LONG hi)
    {
        rmin_[slot] = lo;
        rmax_[slot] = hi;
        rangeSet_[slot] = true;
    }

    void ResetStats(int i)
    {
        pmin_[i] = 0x7fffffff; pmax_[i] = -0x7fffffff;
        gmin_[i] = 0x7fffffff; gmax_[i] = -0x7fffffff;
    }

    // Once a second: the min..max each pedal axis moved through, and what the game was
    // actually given on each axis after merging. Used to work out the axis mapping.
    void UpdateStats(const BYTE* base, DWORD cb, const LONG* ps)
    {
        for (int i = 0; i < 8; ++i) {
            if (ps[i] < pmin_[i]) pmin_[i] = ps[i];
            if (ps[i] > pmax_[i]) pmax_[i] = ps[i];
            if (dstOfs_[i] >= 0 && (DWORD)dstOfs_[i] + 4 <= cb) {
                LONG v = *(const LONG*)(base + dstOfs_[i]);
                if (v < gmin_[i]) gmin_[i] = v;
                if (v > gmax_[i]) gmax_[i] = v;
            }
        }
        DWORD now = GetTickCount();
        if (now - lastDump_ < 1000) return;
        lastDump_ = now;
        ++dumpTick_;

        bool moved = false;
        for (int i = 0; i < 8; ++i) if (pmax_[i] > pmin_[i]) moved = true;
        if (moved || dumpTick_ % 10 == 0) {
            std::string pp, gg;
            for (int i = 0; i < 8; ++i) {
                char t[80];
                snprintf(t, sizeof(t), "%s=%ld..%ld ", kSlotName[i], (long)pmin_[i], (long)pmax_[i]);
                pp += t;
                if (gmax_[i] >= gmin_[i])
                    snprintf(t, sizeof(t), "%s=%ld..%ld ", kSlotName[i], (long)gmin_[i], (long)gmax_[i]);
                else
                    snprintf(t, sizeof(t), "%s=n/a ", kSlotName[i]);
                gg += t;
            }
            Log("1s ranges  pedals-raw[ %s]  game-sees[ %s]", pp.c_str(), gg.c_str());
        }
        for (int i = 0; i < 8; ++i) ResetStats(i);
    }

    IDirectInputDevice8W* w_;
    IDirectInputDevice8W* p_;
    volatile LONG         ref_;
    int                   dstOfs_[8];
    LONG                  rmin_[8];
    LONG                  rmax_[8];
    std::map<DWORD, int>  typeSlot_;
    bool                  rangeSet_[8];
    DWORD                 lastDump_;
    int                   dumpTick_;
    int                   acquireLogs_;
    bool                  haveLast_;
    LONG                  lastMin_;
    LONG                  lastMax_;
    LONG                  pmin_[8], pmax_[8];
    LONG                  gmin_[8], gmax_[8];
    bool                  warnedBuffered_;
    int                   nBtn_;
    int                   buttonOfs_[128];
    bool                  prevBtn_[128];
    std::vector<char>     vdown_;
};

static BOOL CALLBACK ObjThunk(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ctx)
{
    ObjCtx* c = (ObjCtx*)ctx;
    c->dev->NoteObject(o, c->sliders);
    return c->cb(o, c->ref);
}

// ----------------------------------------------------------------------------
//  IDirectInput8W wrapper
// ----------------------------------------------------------------------------
struct EnumCtx {
    LPDIENUMDEVICESCALLBACKW cb;
    LPVOID                   ref;
};

static BOOL CALLBACK EnumThunk(LPCDIDEVICEINSTANCEW di, LPVOID ctx)
{
    EnumCtx* c = (EnumCtx*)ctx;
    bool active;
    GUID wheel, pedals;
    {
        std::lock_guard<std::mutex> lock(gMergeMx);
        active = gMerge.active;
        wheel  = gMerge.wheel.inst;
        pedals = gMerge.pedals.inst;
    }
    if (active) {
        if (IsEqualGUID(di->guidInstance, pedals))
            return DIENUM_CONTINUE;                       // hide the pedals from the game
        if (IsEqualGUID(di->guidInstance, wheel) && gCfg.spoof) {
            DIDEVICEINSTANCEW copy = *di;
            ApplySpoof(copy);
            return c->cb(&copy, c->ref);
        }
    }
    return c->cb(di, c->ref);
}

class ProxyDI8W final : public IDirectInput8W {
public:
    explicit ProxyDI8W(IDirectInput8W* real) : real_(real), ref_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInput8W)) {
            *ppv = static_cast<IDirectInput8W*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) { real_->Release(); delete this; }
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN outer) override
    {
        if (gDiscoverCount == 0) Discover(real_);
        bool merge = false;
        GUID pedalsGuid = {};
        {
            std::lock_guard<std::mutex> lock(gMergeMx);
            if (gMerge.active && IsEqualGUID(g, gMerge.wheel.inst)) {
                merge = true;
                pedalsGuid = gMerge.pedals.inst;
            }
        }
        if (!merge) return real_->CreateDevice(g, out, outer);

        IDirectInputDevice8W* w = nullptr;
        IDirectInputDevice8W* p = nullptr;
        HRESULT hr = real_->CreateDevice(g, &w, outer);
        if (FAILED(hr)) return hr;
        HRESULT hp = real_->CreateDevice(pedalsGuid, &p, nullptr);
        if (FAILED(hp) || !p) {
            Log("Could not create pedals device (0x%08lX) - giving the game the plain wheel", (unsigned long)hp);
            *out = w;
            return hr;
        }
        Log("CreateDevice: returning merged wheel+pedals device");
        *out = new MergedDevice(w, p);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACKW cb, LPVOID ref, DWORD flags) override
    {
        Discover(real_);
        EnumCtx c;
        c.cb = cb;
        c.ref = ref;
        return real_->EnumDevices(type, EnumThunk, &c, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return real_->GetDeviceStatus(g); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return real_->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE i, DWORD v) override { return real_->Initialize(i, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, LPCWSTR n, LPGUID out) override { return real_->FindDevice(g, n, out); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR u, LPDIACTIONFORMATW a,
            LPDIENUMDEVICESBYSEMANTICSCBW cb, LPVOID ref, DWORD f) override
    { return real_->EnumDevicesBySemantics(u, a, cb, ref, f); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb,
            LPDICONFIGUREDEVICESPARAMSW p, DWORD f, LPVOID ref) override
    { return real_->ConfigureDevices(cb, p, f, ref); }

private:
    IDirectInput8W* real_;
    volatile LONG   ref_;
};

// ----------------------------------------------------------------------------
//  Experimental: live field-of-view change by patching the game's camera setting
//
//  The camera files (e.g. COCKPITCAMERAS.BML) store the field of view as the text
//  "DegFocal" -> "55". If the game keeps that file's bytes in memory, we can find
//  the entries and rewrite the number while the game runs. Whether the game keeps
//  the bytes, and re-reads them when the camera is (re)created, is UNKNOWN: the log
//  reports how many entries were found so we can tell.
// ----------------------------------------------------------------------------
static bool ProcessHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

struct FovHit { BYTE* val; int cur; BYTE* regionBase; BYTE* regionEnd; };

static void ScanForFov(std::vector<FovHit>& hits)
{
    // "DegFocal" (UTF-16), 4 zero bytes (terminator+padding), then value length = 8
    static const BYTE pat[] = { 'D',0,'e',0,'g',0,'F',0,'o',0,'c',0,'a',0,'l',0,
                                0,0,0,0, 8,0,0,0 };
    const size_t patLen = sizeof(pat);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* p = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* end = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        bool ok = mbi.State == MEM_COMMIT &&
                  !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & writable);
        if (ok && size > patLen + 16) {
            BYTE* q = base;
            BYTE* qend = base + size - (patLen + 8);
            while (q < qend) {
                BYTE* d = (BYTE*)memchr(q, 'D', (size_t)(qend - q));
                if (!d) break;
                if (memcmp(d, pat, patLen) == 0) {
                    BYTE* v = d + patLen;                 // value: UTF-16 digits, 8 bytes
                    int cur = 0, n = 0;
                    while (n < 3 && v[n * 2 + 1] == 0 && v[n * 2] >= '0' && v[n * 2] <= '9') {
                        cur = cur * 10 + (v[n * 2] - '0');
                        ++n;
                    }
                    if (n >= 1 && v[n * 2] == 0 && v[n * 2 + 1] == 0) {
                        FovHit h;
                        h.val = v; h.cur = cur;
                        h.regionBase = base; h.regionEnd = base + size;
                        hits.push_back(h);
                    }
                }
                q = d + 1;
            }
        }
        p = base + size;
    }
}

// Printable UTF-16 text runs in [a, b), for working out which camera an entry belongs to.
static std::string TextRuns(const BYTE* a, const BYTE* b, bool fromEnd, size_t maxRuns)
{
    std::vector<std::string> runs;
    const BYTE* p = a;
    while (p + 1 < b) {
        const BYTE* q = p;
        std::string cur;
        while (q + 1 < b && q[1] == 0 && q[0] >= 32 && q[0] < 127) { cur += (char)q[0]; q += 2; }
        if (cur.size() >= 3) { runs.push_back(cur); p = q; } else { ++p; }
    }
    std::string out;
    size_t n = runs.size();
    size_t startIdx = 0, endIdx = n;
    if (n > maxRuns) { if (fromEnd) startIdx = n - maxRuns; else endIdx = maxRuns; }
    for (size_t i = startIdx; i < endIdx; ++i) out += "[" + runs[i] + "] ";
    return out;
}

static void AdjustFov(int delta)
{
    std::vector<FovHit> hits;
    ScanForFov(hits);
    if (hits.empty()) {
        Log("FOV: no DegFocal entries found in game memory (the game may not keep that text in memory)");
        return;
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        const FovHit& h = hits[i];
        BYTE* b0 = (h.val - 256 < h.regionBase) ? h.regionBase : h.val - 256;
        BYTE* e1 = (h.val + 8 + 200 > h.regionEnd) ? h.regionEnd : h.val + 8 + 200;
        Log("FOV hit %d: value=%d addr=%p  before: %s  after: %s", (int)i, h.cur, (void*)h.val,
            TextRuns(b0, h.val, true, 6).c_str(), TextRuns(h.val + 8, e1, false, 6).c_str());
    }
    int cur = hits[0].cur;
    int target = cur + delta;
    if (target < gCfg.fovMin) target = gCfg.fovMin;
    if (target > gCfg.fovMax) target = gCfg.fovMax;
    std::wstring w = std::to_wstring(target);
    for (size_t i = 0; i < hits.size(); ++i) {
        memset(hits[i].val, 0, 8);
        memcpy(hits[i].val, w.c_str(), w.size() * sizeof(wchar_t));
    }
    Log("FOV: found %d DegFocal entries, changed %d -> %d", (int)hits.size(), cur, target);
}

static DWORD WINAPI FovThread(LPVOID)
{
    Log("FOV thread running");
    bool downWas = false, upWas = false;
    for (;;) {
        Sleep(20);
        bool downRaw = (GetAsyncKeyState(gCfg.fovDownVk) & 0x8000) != 0;
        bool upRaw   = (GetAsyncKeyState(gCfg.fovUpVk) & 0x8000) != 0;
        if ((downRaw && !downWas) || (upRaw && !upWas)) {
            bool focused = ProcessHasFocus();
            Log("FOV key pressed (%s), game window in front=%d", downRaw && !downWas ? "down" : "up", (int)focused);
            if (focused) {
                if (downRaw && !downWas) AdjustFov(-gCfg.fovStep);
                else                     AdjustFov(+gCfg.fovStep);
            }
        }
        downWas = downRaw;
        upWas = upRaw;
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  Entry points
// ----------------------------------------------------------------------------
static void InitOnce()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InitOnce, &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    size_t slash = p.find_last_of(L"\\/");
    gDir = (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
    gIniPath = gDir + L"dinput8_merge.ini";

    LoadConfig();
    if (gCfg.log) gLog = _wfopen((gDir + L"dinput8_merge.log").c_str(), L"w");

    Log("dinput8 merge proxy starting. enabled=%d debug=%d spoof=%d (vid=%04X pid=%04X)",
        (int)gCfg.enabled, (int)gCfg.debug, (int)gCfg.spoof, (unsigned)gCfg.vid, (unsigned)gCfg.pid);
    for (size_t i = 0; i < gCfg.maps.size(); ++i)
        Log("  map: pedal %s -> game %s%s", kSlotName[gCfg.maps[i].src], kSlotName[gCfg.maps[i].dst],
            gCfg.maps[i].invert ? " (inverted)" : "");

    for (size_t i = 0; i < gCfg.buttons.size(); ++i) {
        const ButtonMap& b = gCfg.buttons[i];
        if (b.isKey) Log("  button map: key vk=0x%02X -> game button %d", b.vk, b.dst);
        else         Log("  button map: wheel button %d -> game button %d%s", b.src, b.dst, b.move ? " (move)" : "");
    }

    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring real = std::wstring(sys) + L"\\dinput8.dll";
    HMODULE h = LoadLibraryW(real.c_str());
    if (h) gRealCreate = (PFN_DI8Create)GetProcAddress(h, "DirectInput8Create");
    Log("real dinput8: %s -> %s", W2A(real.c_str()).c_str(), gRealCreate ? "loaded" : "FAILED");

    InitPedalFormat();

    if (gCfg.fovEnabled) {
        Log("FOV hotkeys on: down vk=0x%02X, up vk=0x%02X, step=%d, range %d..%d",
            gCfg.fovDownVk, gCfg.fovUpVk, gCfg.fovStep, gCfg.fovMin, gCfg.fovMax);
        CreateThread(nullptr, 0, FovThread, nullptr, 0, nullptr);
    }
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                                   LPVOID* ppv, LPUNKNOWN outer)
{
    std::call_once(gInitOnce, InitOnce);
    if (!gRealCreate) return E_FAIL;

    HRESULT hr = gRealCreate(hinst, ver, riid, ppv, outer);
    if (FAILED(hr) || !gCfg.enabled || !ppv || !*ppv) return hr;

    if (IsEqualGUID(riid, IID_IDirectInput8W) && outer == nullptr) {
        Log("DirectInput8Create(version=0x%lX) -> wrapping IDirectInput8W", (unsigned long)ver);
        *ppv = static_cast<IDirectInput8W*>(new ProxyDI8W(static_cast<IDirectInput8W*>(*ppv)));
    } else {
        Log("DirectInput8Create called with IID %s - NOT wrapped", GuidStr(riid).c_str());
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
