#pragma once
#include "common.h"
#include "crypto.h"

namespace ph {

// One tablet that is paired with this PC.
struct PairedPad {
    Bytes id;             // 16 bytes (the tablet's padId)
    std::string name;     // e.g. "HMW-W09"
    Bytes key;            // 32-byte pairing key K
    std::string ip;       // last address it connected from (informational)
    int64_t addedMs = 0;  // unix ms
    int64_t lastMs = 0;   // unix ms of the last session
};

// This PC's identity and the tablets it trusts.  Persisted in devices.ini next to the exe; keys are protected with Windows DPAPI (per user).
class Devices {
public:
    void load(const std::wstring& path);
    Bytes pcId();                 // 16 random bytes, created on first use
    std::string pcName();         // the computer name
    bool find(const Bytes& padId, PairedPad& out);
    void put(const PairedPad& p);  // add or replace (by id) and save
    void touch(const Bytes& padId, const std::string& ip);   // remember the last address / time
    bool remove(const Bytes& padId);
    std::vector<PairedPad> list();

private:
    void save();
    std::mutex mu_;
    std::wstring path_;
    Bytes pcId_;
    std::vector<PairedPad> pads_;
};

}  // namespace ph
