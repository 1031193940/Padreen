#pragma once
#include "common.h"

namespace ph {

class Adb {
public:
    explicit Adb(std::wstring exe) : exe_(std::move(exe)) {}
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
};

// Streams `host:track-devices` from the adb server; `onChange` gets the serials currently in state "device".
class DeviceWatcher {
public:
    ~DeviceWatcher() { stop(); }
    void start(Adb* adb, std::function<void(const std::vector<std::string>&)> onChange);
    void stop();

private:
    void loop();
    Adb* adb_ = nullptr;
    std::function<void(const std::vector<std::string>&)> cb_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
};

}  // namespace ph
