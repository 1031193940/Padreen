#include "devices.h"

#include <algorithm>
#include <ctime>

namespace ph {

static int64_t unixMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static std::wstring iniGet(const std::wstring& path, const std::wstring& sec, const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 2048, path.c_str());
    return buf;
}

void Devices::load(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    pads_.clear();
    pcId_.clear();
    fromHex(narrow(iniGet(path, L"identity", L"pc_id")), pcId_);
    if (pcId_.size() != 16) pcId_.clear();
    // enumerate the sections "pad.<hex id>"
    std::vector<wchar_t> names(65536);
    DWORD n = GetPrivateProfileSectionNamesW(names.data(), (DWORD)names.size(), path.c_str());
    for (const wchar_t* p = names.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring sec = p;
        if (sec.rfind(L"pad.", 0) != 0) continue;
        PairedPad pad;
        if (!fromHex(narrow(sec.substr(4)), pad.id) || pad.id.size() != 16) continue;
        pad.name = narrow(iniGet(path, sec, L"name"));
        pad.ip = narrow(iniGet(path, sec, L"ip"));
        pad.addedMs = _wtoi64(iniGet(path, sec, L"added").c_str());
        pad.lastMs = _wtoi64(iniGet(path, sec, L"last").c_str());
        if (!dpapiUnprotectHex(narrow(iniGet(path, sec, L"key")), pad.key) || pad.key.size() != 32) {
            logf("devices: the key of paired tablet '%s' cannot be decrypted (another Windows user?) - pair it again", pad.name.c_str());
            continue;
        }
        pads_.push_back(std::move(pad));
    }
    logf("devices: %zu paired tablet(s)", pads_.size());
}

Bytes Devices::pcId() {
    std::lock_guard<std::mutex> lk(mu_);
    if (pcId_.size() != 16) {
        pcId_.assign(16, 0);
        randomBytes(pcId_.data(), 16);
        if (!path_.empty()) WritePrivateProfileStringW(L"identity", L"pc_id", widen(toHex(pcId_.data(), 16)).c_str(), path_.c_str());
    }
    return pcId_;
}

std::string Devices::pcName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) wcscpy_s(buf, L"PC");
    std::string s = narrow(buf);
    if (s.size() > 40) s.resize(40);
    return s;
}

bool Devices::find(const Bytes& padId, PairedPad& out) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const PairedPad& p : pads_)
        if (p.id == padId) {
            out = p;
            return true;
        }
    return false;
}

void Devices::save() {
    if (path_.empty()) return;
    for (const PairedPad& p : pads_) {
        std::wstring sec = L"pad." + widen(toHex(p.id.data(), p.id.size()));
        WritePrivateProfileStringW(sec.c_str(), L"name", widen(p.name).c_str(), path_.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"key", widen(dpapiProtectHex(p.key)).c_str(), path_.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"ip", widen(p.ip).c_str(), path_.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"added", std::to_wstring(p.addedMs).c_str(), path_.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"last", std::to_wstring(p.lastMs).c_str(), path_.c_str());
    }
}

void Devices::put(const PairedPad& p) {
    std::lock_guard<std::mutex> lk(mu_);
    PairedPad q = p;
    if (!q.addedMs) q.addedMs = unixMs();
    bool found = false;
    for (PairedPad& e : pads_)
        if (e.id == q.id) {
            if (e.addedMs) q.addedMs = e.addedMs;
            e = q;
            found = true;
        }
    if (!found) pads_.push_back(q);
    save();
}

void Devices::touch(const Bytes& padId, const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    for (PairedPad& e : pads_)
        if (e.id == padId) {
            e.ip = ip;
            e.lastMs = unixMs();
            if (!path_.empty()) {
                std::wstring sec = L"pad." + widen(toHex(e.id.data(), e.id.size()));
                WritePrivateProfileStringW(sec.c_str(), L"ip", widen(e.ip).c_str(), path_.c_str());
                WritePrivateProfileStringW(sec.c_str(), L"last", std::to_wstring(e.lastMs).c_str(), path_.c_str());
            }
        }
}

bool Devices::remove(const Bytes& padId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::remove_if(pads_.begin(), pads_.end(), [&](const PairedPad& p) { return p.id == padId; });
    if (it == pads_.end()) return false;
    pads_.erase(it, pads_.end());
    if (!path_.empty()) {
        std::wstring sec = L"pad." + widen(toHex(padId.data(), padId.size()));
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, path_.c_str());       // deletes the whole section
    }
    return true;
}

std::vector<PairedPad> Devices::list() {
    std::lock_guard<std::mutex> lk(mu_);
    return pads_;
}

}  // namespace ph
