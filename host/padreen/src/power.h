#pragma once
#include "common.h"

namespace ph {

// While a display session runs the PC must not go to sleep by itself (idle timeout) - an application power request, no setting is changed.
class SessionPower {
public:
    explicit SessionPower(const wchar_t* reason);
    ~SessionPower();
    void displayRequired(bool on);          // keep the screens on too (used while the e-ink display is the PC's only screen)
    SessionPower(const SessionPower&) = delete;
    SessionPower& operator=(const SessionPower&) = delete;

private:
    void* h_ = nullptr;
    bool display_ = false;
};

// "Closing the lid must not put the PC to sleep while the e-ink display is in use".  The lid-close action is a power-plan setting that only
// administrators may change, so a small Windows service (the "power helper", this same exe started with --power-service) does it:
// while the app sends heartbeats (named event) the service sets the lid action of the active plan to "do nothing"; 15 s without a heartbeat,
// an explicit release, a service stop or a reboot restores the original values.
class LidHold {
public:
    ~LidHold() { stop(); }
    void start();      // heartbeats while a session runs (no-op if the helper service is not there)
    void stop();       // releases at once
private:
    std::thread th_;
    std::atomic<bool> run_{false};
};

// helper service state seen from the app (no admin rights needed)
enum class PowerHelper { Missing, Stopped, Running };
PowerHelper powerHelperState();

// What would closing the lid do right now?  0 = nothing, 1 = sleep, 2 = hibernate, 3 = shut down, -1 = unknown (reads the stored plan values)
int lidActionNow();
std::wstring lidActionName(int action);

// command line modes of padreen.exe (all return the process exit code)
int runPowerService();                 // --power-service        (started by the Service Control Manager)
int installPowerService();             // --install-power-service (needs administrator): create/update + start the service
int removePowerService();              // --remove-power-service  (needs administrator): stop (restores the lid action) + delete

}  // namespace ph
