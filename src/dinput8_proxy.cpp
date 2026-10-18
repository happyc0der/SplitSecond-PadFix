// dinput8.dll proxy for Split/Second (2010). The game reads gamepads through DirectInput and recognises Xbox 360
// pads with Microsoft's "IG_" check, then reads them with the wired-360 DirectInput layout (X/Y left stick,
// Z = both triggers, Rx/Ry right stick, buttons 0-9 = A B X Y LB RB Back Start LS RS, POV = D-pad). A Bluetooth
// Xbox controller has a different DirectInput layout, so its sticks/triggers land on the wrong fields. For every
// Microsoft (VID 045E) game controller this proxy keeps the real device (identity, acquire, effects) but answers
// GetDeviceState/GetDeviceData with the 360 layout built from XInput. Keyboard and mouse pass straight through.
#include "common.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

using namespace padfix;

namespace {

const char* GuidName(const GUID* g) {
    if (!g) return "null";
    if (*g == GUID_XAxis) return "X";
    if (*g == GUID_YAxis) return "Y";
    if (*g == GUID_ZAxis) return "Z";
    if (*g == GUID_RxAxis) return "Rx";
    if (*g == GUID_RyAxis) return "Ry";
    if (*g == GUID_RzAxis) return "Rz";
    if (*g == GUID_Slider) return "Slider";
    if (*g == GUID_POV) return "POV";
    if (*g == GUID_Button) return "Button";
    if (*g == GUID_Key) return "Key";
    return "other";
}

// Split/Second picks a controller's default control map by exact product GUID: only 045E:028E (wired Xbox 360) and
// 045E:02A1 (360 wireless receiver) get the Xbox map; anything else (a Bluetooth Elite is 045E:0B22) falls into a
// generic type with no default bindings. So a translated Microsoft pad also introduces itself as a wired 360 pad.
constexpr DWORD kX360PidVid = 0x028E045E;
bool IsMicrosoftPad(const DIDEVICEINSTANCEA* d) {
    BYTE type = GET_DIDEVICE_TYPE(d->dwDevType);
    bool pad = type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK || type == DI8DEVTYPE_DRIVING || type == DI8DEVTYPE_1STPERSON;
    return pad && LOWORD(d->guidProduct.Data1) == 0x045E;
}
void SpoofAs360(DIDEVICEINSTANCEA* d) {
    if (!g_cfg.translate || !g_cfg.spoof360 || !IsMicrosoftPad(d) || d->guidProduct.Data1 == kX360PidVid) return;
    d->guidProduct.Data1 = kX360PidVid; // {PIDVID-0000-0000-0000-504944564944} keeps its "PIDVID" tail
    if (d->dwSize >= offsetof(DIDEVICEINSTANCEA, tszProductName) + MAX_PATH) {
        strcpy_s(d->tszInstanceName, "Controller (XBOX 360 For Windows)");
        strcpy_s(d->tszProductName, "Controller (XBOX 360 For Windows)");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Translating device
// ---------------------------------------------------------------------------------------------------------------
enum class Src { None, LX, LY, Trig, RX, RY, Zero, Pov, PovIdle, Button };
struct Obj {
    DWORD ofs;
    Src src;
    int button; // for Src::Button
};
struct AxisProps {
    LONG min = 0, max = 65535;
    DWORD deadzone = 0, saturation = 10000;
};

class PadDevice : public IDirectInputDevice8A {
public:
    PadDevice(IDirectInputDevice8A* real, const char* name) : real_(real), name_(name) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDirectInputDevice8A) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG n = --refs_;
        if (n == 0) {
            real_->Release();
            delete this;
        }
        return n;
    }

    // IDirectInputDevice8A
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override {
        HRESULT hr = real_->GetCapabilities(caps);
        if (SUCCEEDED(hr) && caps) {
            Log("[di] %s GetCapabilities: real axes %lu buttons %lu povs %lu -> reporting 5/10/1", name_.c_str(),
                caps->dwAxes, caps->dwButtons, caps->dwPOVs);
            caps->dwAxes = 5;
            caps->dwButtons = 10;
            caps->dwPOVs = 1;
        }
        return hr;
    }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
        Log("[di] %s EnumObjects(flags %08lX) -> real", name_.c_str(), flags);
        return real_->EnumObjects(cb, ref, flags);
    }
    STDMETHOD(GetProperty)(REFGUID prop, LPDIPROPHEADER h) override {
        if (&prop == &DIPROP_RANGE && h && h->dwHow != DIPH_DEVICE) {
            auto* r = reinterpret_cast<LPDIPROPRANGE>(h);
            const AxisProps& a = PropsFor(h->dwObj, h->dwHow);
            r->lMin = a.min;
            r->lMax = a.max;
            return DI_OK;
        }
        return real_->GetProperty(prop, h);
    }
    STDMETHOD(SetProperty)(REFGUID prop, LPCDIPROPHEADER h) override {
        HRESULT hr = real_->SetProperty(prop, h);
        const char* what = &prop == &DIPROP_RANGE ? "RANGE" : &prop == &DIPROP_DEADZONE ? "DEADZONE"
                         : &prop == &DIPROP_SATURATION ? "SATURATION" : &prop == &DIPROP_BUFFERSIZE ? "BUFFERSIZE"
                         : &prop == &DIPROP_AXISMODE ? "AXISMODE" : "other";
        if (h) {
            std::lock_guard<std::mutex> lock(m_);
            if (&prop == &DIPROP_RANGE) {
                auto* r = reinterpret_cast<LPCDIPROPRANGE>(h);
                for (AxisProps* a : Targets(h->dwObj, h->dwHow)) { a->min = r->lMin; a->max = r->lMax; }
                Log("[di] %s SetProperty RANGE how %lu obj %08lX = %ld..%ld (real hr %08lX)", name_.c_str(), h->dwHow, h->dwObj, r->lMin, r->lMax, hr);
            } else if (&prop == &DIPROP_DEADZONE || &prop == &DIPROP_SATURATION) {
                auto* d = reinterpret_cast<LPCDIPROPDWORD>(h);
                for (AxisProps* a : Targets(h->dwObj, h->dwHow)) {
                    if (&prop == &DIPROP_DEADZONE) a->deadzone = d->dwData; else a->saturation = d->dwData;
                }
                Log("[di] %s SetProperty %s how %lu obj %08lX = %lu (real hr %08lX)", name_.c_str(), what, h->dwHow, h->dwObj, d->dwData, hr);
            } else if (&prop == &DIPROP_BUFFERSIZE) {
                bufferSize_ = reinterpret_cast<LPCDIPROPDWORD>(h)->dwData;
                Log("[di] %s SetProperty BUFFERSIZE = %lu (real hr %08lX)", name_.c_str(), bufferSize_, hr);
            } else {
                Log("[di] %s SetProperty %s how %lu obj %08lX (real hr %08lX)", name_.c_str(), what, h->dwHow, h->dwObj, hr);
            }
        }
        return g_cfg.translate ? DI_OK : hr; // objects the BT device lacks (Rx/Ry) still "succeed"
    }
    STDMETHOD(Acquire)() override {
        HRESULT hr = real_->Acquire();
        if (acquireLogs_++ < 3) Log("[di] %s Acquire -> %08lX", name_.c_str(), hr);
        return g_cfg.translate && FAILED(hr) && hr != DIERR_OTHERAPPHASPRIO ? DI_OK : hr;
    }
    STDMETHOD(Unacquire)() override { return real_->Unacquire(); }
    STDMETHOD(GetDeviceState)(DWORD cb, LPVOID data) override {
        if (stateLogs_ < 3) {
            ++stateLogs_;
            Log("[di] %s GetDeviceState(%lu bytes)%s", name_.c_str(), cb, g_cfg.translate ? " -> translated" : "");
        }
        ++stateCalls_;
        if (!g_cfg.translate) return real_->GetDeviceState(cb, data);
        if (!data || cb < dataSize_ || objs_.empty()) return real_->GetDeviceState(cb, data);
        std::lock_guard<std::mutex> lock(m_);
        Fill(static_cast<uint8_t*>(data), cb, ReadPad());
        return DI_OK;
    }
    STDMETHOD(GetDeviceData)(DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) override {
        if (dataLogs_ < 3) {
            ++dataLogs_;
            Log("[di] %s GetDeviceData(cbObj %lu, n %lu, flags %08lX)%s", name_.c_str(), cbObj, inOut ? *inOut : 0, flags,
                g_cfg.translate ? " -> synthesised" : "");
        }
        if (!g_cfg.translate) return real_->GetDeviceData(cbObj, rg, inOut, flags);
        if (!inOut) return DIERR_INVALIDPARAM;
        if (bufferSize_ == 0) return DIERR_NOTBUFFERED;
        std::lock_guard<std::mutex> lock(m_);
        Diff(ReadPad());
        DWORD want = *inOut, n = 0;
        if (!rg) { // count or flush
            n = static_cast<DWORD>(std::min<size_t>(want, events_.size()));
            if (!(flags & DIGDD_PEEK)) events_.erase(events_.begin(), events_.begin() + n);
            *inOut = n;
            return DI_OK;
        }
        for (; n < want && n < events_.size(); ++n)
            memcpy(reinterpret_cast<uint8_t*>(rg) + n * cbObj, &events_[n], std::min<DWORD>(cbObj, sizeof(DIDEVICEOBJECTDATA)));
        if (!(flags & DIGDD_PEEK)) events_.erase(events_.begin(), events_.begin() + n);
        *inOut = n;
        return overflow_ ? (overflow_ = false, DI_BUFFEROVERFLOW) : DI_OK;
    }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT f) override {
        HRESULT hr = real_->SetDataFormat(f);
        std::lock_guard<std::mutex> lock(m_);
        objs_.clear();
        if (!f) return hr;
        dataSize_ = f->dwDataSize;
        int nextButton = 0, nextAxis = 0, povs = 0;
        const Src axisOrder[] = {Src::LX, Src::LY, Src::Trig, Src::RX, Src::RY, Src::Zero};
        for (DWORD i = 0; i < f->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = f->rgodf[i];
            Obj m{o.dwOfs, Src::None, -1};
            const GUID* g = o.pguid;
            DWORD inst = DIDFT_GETINSTANCE(o.dwType);
            bool anyInst = (o.dwType & DIDFT_ANYINSTANCE) == DIDFT_ANYINSTANCE;
            DWORD aspect = o.dwFlags & DIDOI_ASPECTMASK;
            if ((g && *g == GUID_POV) || (o.dwType & DIDFT_POV)) m.src = povs++ == 0 ? Src::Pov : Src::PovIdle; // a 360 pad has one POV
            else if (aspect && aspect != DIDOI_ASPECTPOSITION) m.src = Src::Zero; // velocity/accel/force copies read 0
            else if ((g && *g == GUID_Button) || (!g && (o.dwType & DIDFT_BUTTON))) {
                m.src = Src::Button;
                m.button = anyInst ? nextButton++ : static_cast<int>(inst);
            } else if (g && *g == GUID_XAxis) m.src = Src::LX;
            else if (g && *g == GUID_YAxis) m.src = Src::LY;
            else if (g && *g == GUID_ZAxis) m.src = Src::Trig;
            else if (g && *g == GUID_RxAxis) m.src = Src::RX;
            else if (g && *g == GUID_RyAxis) m.src = Src::RY;
            else if (g && (*g == GUID_RzAxis || *g == GUID_Slider)) m.src = Src::Zero;
            else if (!g && (o.dwType & DIDFT_AXIS)) m.src = nextAxis < 6 ? axisOrder[nextAxis++] : Src::Zero;
            objs_.push_back(m);
            if (formatLogs_ < 1)
                Log("[di]   obj %2lu ofs %3lu type %08lX flags %08lX guid %-6s -> %s%s", i, o.dwOfs, o.dwType, o.dwFlags, GuidName(g),
                    m.src == Src::Button ? "button " : m.src == Src::Pov ? "POV/D-pad" : m.src == Src::PovIdle ? "(POV centred)" : m.src == Src::LX ? "left X"
                    : m.src == Src::LY ? "left Y" : m.src == Src::Trig ? "triggers" : m.src == Src::RX ? "right X"
                    : m.src == Src::RY ? "right Y" : m.src == Src::Zero ? "(zero, as on a wired 360)" : "(none)",
                    m.src == Src::Button ? std::to_string(m.button).c_str() : "");
        }
        Log("[di] %s SetDataFormat: %lu bytes, %lu objects, flags %08lX (real hr %08lX)", name_.c_str(), f->dwDataSize, f->dwNumObjs, f->dwFlags, hr);
        ++formatLogs_;
        return g_cfg.translate ? DI_OK : hr;
    }
    STDMETHOD(SetEventNotification)(HANDLE e) override { return real_->SetEventNotification(e); }
    STDMETHOD(SetCooperativeLevel)(HWND w, DWORD f) override {
        HRESULT hr = real_->SetCooperativeLevel(w, f);
        Log("[di] %s SetCooperativeLevel(%08lX) -> %08lX", name_.c_str(), f, hr);
        return hr;
    }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA i, DWORD obj, DWORD how) override { return real_->GetObjectInfo(i, obj, how); }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA i) override {
        HRESULT hr = real_->GetDeviceInfo(i);
        if (SUCCEEDED(hr) && i) SpoofAs360(i);
        return hr;
    }
    STDMETHOD(RunControlPanel)(HWND w, DWORD f) override { return real_->RunControlPanel(w, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v, REFGUID g) override { return real_->Initialize(h, v, g); }
    STDMETHOD(CreateEffect)(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* o, LPUNKNOWN u) override { return real_->CreateEffect(g, e, o, u); }
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t) override { return real_->EnumEffects(cb, r, t); }
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA i, REFGUID g) override { return real_->GetEffectInfo(i, g); }
    STDMETHOD(GetForceFeedbackState)(LPDWORD s) override { return real_->GetForceFeedbackState(s); }
    STDMETHOD(SendForceFeedbackCommand)(DWORD c) override { return real_->SendForceFeedbackCommand(c); }
    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD f) override { return real_->EnumCreatedEffectObjects(cb, r, f); }
    STDMETHOD(Escape)(LPDIEFFESCAPE e) override { return real_->Escape(e); }
    STDMETHOD(Poll)() override {
        HRESULT hr = real_->Poll();
        return g_cfg.translate && FAILED(hr) ? DI_OK : hr;
    }
    STDMETHOD(SendDeviceData)(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD io, DWORD f) override { return real_->SendDeviceData(cb, d, io, f); }
    STDMETHOD(EnumEffectsInFile)(LPCSTR n, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID r, DWORD f) override { return real_->EnumEffectsInFile(n, cb, r, f); }
    STDMETHOD(WriteEffectToFile)(LPCSTR n, DWORD c, LPDIFILEEFFECT e, DWORD f) override { return real_->WriteEffectToFile(n, c, e, f); }
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return real_->BuildActionMap(a, u, f); }
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) override { return real_->SetActionMap(a, u, f); }
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA h) override { return real_->GetImageInfo(h); }

    long StateCalls() const { return stateCalls_; }
    const std::string& Name() const { return name_; }

private:
    // property targets: device-wide or one object (by offset in the data format)
    std::vector<AxisProps*> Targets(DWORD obj, DWORD how) {
        std::vector<AxisProps*> out;
        if (how == DIPH_DEVICE) {
            out.push_back(&deviceProps_);
            for (auto& kv : objProps_) out.push_back(&kv.second);
            for (const Obj& o : objs_) if (o.src != Src::Button && o.src != Src::Pov) out.push_back(&objProps_[o.ofs]);
        } else if (how == DIPH_BYOFFSET) {
            out.push_back(&objProps_[obj]);
        } else { // DIPH_BYID: map an object ID to our axes by type only (rare) -> apply device-wide
            out.push_back(&deviceProps_);
        }
        return out;
    }
    const AxisProps& PropsFor(DWORD obj, DWORD how) {
        if (how == DIPH_BYOFFSET) {
            auto it = objProps_.find(obj);
            if (it != objProps_.end()) return it->second;
        }
        return deviceProps_;
    }
    LONG Scale(double v, const AxisProps& a) const { // v in [-1, 1]
        double mag = std::fabs(v), dz = a.deadzone / 10000.0, sat = std::max<double>(a.saturation / 10000.0, dz + 1e-6);
        double out = mag <= dz ? 0.0 : mag >= sat ? 1.0 : (mag - dz) / (sat - dz);
        out = v < 0 ? -out : out;
        double mid = (a.min + static_cast<double>(a.max)) / 2.0, half = (a.max - static_cast<double>(a.min)) / 2.0;
        return static_cast<LONG>(std::lround(mid + out * half));
    }
    static double Stick(int16_t v) { return v < 0 ? v / 32768.0 : v / 32767.0; }
    DWORD Value(const Obj& o, const Pad& p) {
        static const uint16_t kButtons[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080};
        switch (o.src) {
        case Src::Zero: return 0;
        case Src::PovIdle: return 0xFFFFFFFF;
        case Src::Button: return (o.button >= 0 && o.button < 10 && (p.buttons & kButtons[o.button])) ? 0x80 : 0;
        case Src::Pov: {
            bool u = p.buttons & 1, d = p.buttons & 2, l = p.buttons & 4, r = p.buttons & 8;
            if (u && r) return 4500; if (r && d) return 13500; if (d && l) return 22500; if (l && u) return 31500;
            if (u) return 0; if (r) return 9000; if (d) return 18000; if (l) return 27000;
            return 0xFFFFFFFF;
        }
        default: break;
        }
        const AxisProps& a = PropsFor(o.ofs, DIPH_BYOFFSET);
        double v = 0.0;
        switch (o.src) {
        case Src::LX: v = Stick(p.lx); break;
        case Src::LY: v = -Stick(p.ly); break; // DirectInput Y grows downwards
        case Src::RX: v = Stick(p.rx); break;
        case Src::RY: v = -Stick(p.ry); break;
        case Src::Trig: v = (p.lt - p.rt) / 255.0; if (g_cfg.swapTriggers) v = -v; break;
        default: v = 0.0; break;
        }
        return static_cast<DWORD>(Scale(v, a));
    }
    void Fill(uint8_t* data, DWORD cb, const Pad& p) {
        memset(data, 0, std::min(cb, dataSize_));
        for (const Obj& o : objs_) {
            if (o.src == Src::None) continue;
            DWORD v = Value(o, p);
            if (o.src == Src::Button) { if (o.ofs < cb) data[o.ofs] = static_cast<uint8_t>(v); }
            else if (o.ofs + 4 <= cb) memcpy(data + o.ofs, &v, 4);
        }
        if (!loggedPad_ && p.connected) { loggedPad_ = true; Log("[di] %s: XInput pad connected, translation live", name_.c_str()); }
        if (!p.connected && loggedPad_ && !loggedLost_) { loggedLost_ = true; Log("[di] %s: XInput reports no pad connected", name_.c_str()); }
        if (p.connected) loggedLost_ = false;
        // the first input changes, so a test session shows exactly what the game was given
        bool moved = p.buttons != lastLogged_.buttons || std::abs(p.lt - lastLogged_.lt) > 40 || std::abs(p.rt - lastLogged_.rt) > 40 ||
                     std::abs(p.lx - lastLogged_.lx) > 12000 || std::abs(p.ly - lastLogged_.ly) > 12000 ||
                     std::abs(p.rx - lastLogged_.rx) > 12000 || std::abs(p.ry - lastLogged_.ry) > 12000;
        if (g_cfg.verbose && moved && inputLogs_ < 60) {
            ++inputLogs_;
            lastLogged_ = p;
            LONG x, y, z, pov;
            memcpy(&x, data + 0, 4); memcpy(&y, data + 4, 4); memcpy(&z, data + 8, 4); memcpy(&pov, data + 32, 4);
            Log("[pad] buttons %04X LT %3u RT %3u L(%6d,%6d) R(%6d,%6d) -> game sees X %6ld Y %6ld Z %6ld POV %ld", p.buttons, p.lt, p.rt,
                p.lx, p.ly, p.rx, p.ry, x, y, z, pov);
        }
    }
    void Diff(const Pad& p) {
        for (const Obj& o : objs_) {
            if (o.src == Src::None) continue;
            DWORD v = Value(o, p);
            auto it = last_.find(o.ofs);
            if (it != last_.end() && it->second == v) continue;
            last_[o.ofs] = v;
            DIDEVICEOBJECTDATA e{};
            e.dwOfs = o.ofs;
            e.dwData = v;
            e.dwTimeStamp = GetTickCount();
            e.dwSequence = ++seq_;
            if (events_.size() >= bufferSize_) { events_.pop_front(); overflow_ = true; }
            events_.push_back(e);
        }
    }

    IDirectInputDevice8A* real_;
    std::string name_;
    std::atomic<ULONG> refs_{1};
    std::mutex m_;
    std::vector<Obj> objs_;
    DWORD dataSize_ = 0, bufferSize_ = 0, seq_ = 0;
    AxisProps deviceProps_;
    std::map<DWORD, AxisProps> objProps_;
    std::map<DWORD, DWORD> last_;
    std::deque<DIDEVICEOBJECTDATA> events_;
    bool overflow_ = false, loggedPad_ = false, loggedLost_ = false;
    Pad lastLogged_;
    int inputLogs_ = 0;
    int stateLogs_ = 0, dataLogs_ = 0, formatLogs_ = 0, acquireLogs_ = 0;
    std::atomic<long> stateCalls_{0};
};

// ---------------------------------------------------------------------------------------------------------------
// IDirectInput8A wrapper
// ---------------------------------------------------------------------------------------------------------------
struct EnumCtx {
    LPDIENUMDEVICESCALLBACKA cb;
    LPVOID ref;
};
BOOL CALLBACK EnumLogger(LPCDIDEVICEINSTANCEA d, LPVOID p) {
    auto* c = static_cast<EnumCtx*>(p);
    DIDEVICEINSTANCEA copy{};
    memcpy(&copy, d, std::min<DWORD>(d->dwSize, sizeof(copy)));
    SpoofAs360(&copy);
    Log("[di]   enum: \"%s\" type %08lX product %08lX%s", d->tszProductName, d->dwDevType, d->guidProduct.Data1,
        copy.guidProduct.Data1 != d->guidProduct.Data1 ? " -> shown to the game as a wired Xbox 360 pad (045E:028E)" : "");
    return c->cb(&copy, c->ref);
}

class DI8 : public IDirectInput8A {
public:
    explicit DI8(IDirectInput8A* real) : real_(real) {}
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDirectInput8A) { AddRef(); *out = this; return S_OK; }
        return real_->QueryInterface(riid, out);
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG n = --refs_;
        if (n == 0) { real_->Release(); delete this; }
        return n;
    }
    STDMETHOD(CreateDevice)(REFGUID g, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
        HRESULT hr = real_->CreateDevice(g, out, outer);
        if (FAILED(hr) || !out || !*out) { Log("[di] CreateDevice -> %08lX", hr); return hr; }
        DIDEVICEINSTANCEA info{};
        info.dwSize = sizeof(info);
        (*out)->GetDeviceInfo(&info);
        BYTE type = GET_DIDEVICE_TYPE(info.dwDevType);
        WORD vid = LOWORD(info.guidProduct.Data1), pid = HIWORD(info.guidProduct.Data1);
        bool pad = type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK || type == DI8DEVTYPE_DRIVING || type == DI8DEVTYPE_1STPERSON;
        Log("[di] CreateDevice \"%s\" type %02X vid %04X pid %04X%s", info.tszProductName, type, vid, pid,
            pad && vid == 0x045E ? " -> Xbox pad, wrapped" : "");
        if (pad && vid == 0x045E) *out = new PadDevice(*out, info.tszProductName);
        return hr;
    }
    STDMETHOD(EnumDevices)(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override {
        Log("[di] EnumDevices(type %lu, flags %08lX)", type, flags);
        EnumCtx c{cb, ref};
        return real_->EnumDevices(type, EnumLogger, &c, flags);
    }
    STDMETHOD(GetDeviceStatus)(REFGUID g) override { return real_->GetDeviceStatus(g); }
    STDMETHOD(RunControlPanel)(HWND w, DWORD f) override { return real_->RunControlPanel(w, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }
    STDMETHOD(FindDevice)(REFGUID g, LPCSTR n, LPGUID o) override { return real_->FindDevice(g, n, o); }
    STDMETHOD(EnumDevicesBySemantics)(LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID r, DWORD f) override {
        Log("[di] EnumDevicesBySemantics (action mapping) used");
        return real_->EnumDevicesBySemantics(u, a, cb, r, f);
    }
    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID r) override {
        return real_->ConfigureDevices(cb, p, f, r);
    }

private:
    IDirectInput8A* real_;
    std::atomic<ULONG> refs_{1};
};

using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    LoadConfigOnce();
    static Create_t real = [] {
        HMODULE h = LoadSystemDll(L"dinput8.dll");
        return h ? reinterpret_cast<Create_t>(GetProcAddress(h, "DirectInput8Create")) : nullptr;
    }();
    if (!real) { Log("[di] system dinput8.dll not found"); return DIERR_GENERIC; }
    HRESULT hr = real(inst, ver, riid, out, outer);
    bool ansi = riid == IID_IDirectInput8A;
    Log("[di] DirectInput8Create(version %08lX, %s) -> %08lX", ver, ansi ? "IDirectInput8A" : "other interface (passed through)", hr);
    if (SUCCEEDED(hr) && ansi && out && *out) *out = static_cast<IDirectInput8A*>(new DI8(static_cast<IDirectInput8A*>(*out)));
    return hr;
}
