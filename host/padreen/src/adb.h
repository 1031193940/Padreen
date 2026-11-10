#pragma once
#include "common.h"

namespace ph {

class Adb {
public:
    explicit Adb(std::wstring exe) : exe_(std::move(exe)) {}
    // `run` gives up (the adb client is killed) as soon as *flag turns true: quitting must not wait for an `adb install` that still waits
    // for the tablet's "continue" tap.
    void abortWhen(const std::atomic<bool>* flag) { abort_ = flag; }
    // Make sure the adb server is running.  Started detached WITHOUT inheriting any handle: a server that inherits a pipe keeps
    // the pipe open and hangs whoever waits for its end of file.
    bool ensureServer(std::string& err);
    // Run `adb <args>` and capture stdout+stderr.  Returns the exit code (-1 on failure to start / timeout).
    int run(const std::wstring& args, std::string& out, int timeoutMs = 20000);

    std::string shell(const std::string& serial, const std::string& cmd, int timeoutMs = 20000);
    bool forward(const std::string& serial, int port);
    bool removeForward(const std::string& serial, int port);
    bool packageInstalled(const std::string& serial, const std::string& pkg);
    bool install(const std::string& serial, const std::wstring& apk, std::string& log);
    bool uninstall(const std::string& serial, const std::string& pkg, std::string& log);
    bool startActivity(const std::string& serial, const std::string& component);
    bool forceStop(const std::string& serial, const std::string& pkg);
    // IPv4 address of the tablet's wlan0 (empty if it is not on Wi-Fi) - used to remember where to connect wirelessly
    std::string wifiAddress(const std::string& serial);

private:
    std::wstring exe_;
    const std::atomic<bool>* abort_ = nullptr;
};

// A device that adb lists but cannot use yet: "unauthorized" (the tablet still waits for "Allow USB debugging"), "offline", ...
struct AdbPending {
    std::string serial, state;
};

// What `adb install` printed, as a sentence for the user (what went wrong and what to do); the raw output belongs in the log.
std::wstring explainInstallFailure(const std::string& adbOutput);

// Streams `host:track-devices` from the adb server; `onChange` gets the serials currently in state "device" and, separately, the others.
class DeviceWatcher {
public:
    using Callback = std::function<void(const std::vector<std::string>& ready, const std::vector<AdbPending>& pending)>;
    ~DeviceWatcher() { stop(); }
    void start(Adb* adb, Callback onChange);
    void stop();

private:
    void loop();
    Adb* adb_ = nullptr;
    Callback cb_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
};

}  // namespace ph
