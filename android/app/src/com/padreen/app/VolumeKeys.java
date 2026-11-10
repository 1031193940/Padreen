package com.padreen.app;

/**
 * What Padreen does with a hardware volume key while one of its windows (the display, the home screen, a dialog on it) is in front - kept
 * free of any Android class so it can be unit-tested on the JVM.
 *
 * As long as the user's switch is on and a PC is attached (a running session, or the Wi-Fi link is being re-established) the volume keys
 * belong to the PC: the tablet's own volume - and its volume panel - must not react to them, so EVERY event of such a key is swallowed
 * (presses, repeats, releases), and a press is sent on to the PC.  Without a PC (or with the switch off) the system handles the key as usual:
 * the tablet never has dead volume keys.  The switch and the link state are looked at for every single event, never remembered.
 * UI thread only.
 */
final class VolumeKeys {
    /** Not ours: the system handles the key (it changes the tablet's own volume). */
    static final int PASS = 0;
    /** Ours: swallow the event; nothing is sent (a release, a repeat that came too soon, or the link is down while it is being re-established). */
    static final int SWALLOW = 1;
    /** Ours: swallow the event and send the key to the PC. */
    static final int SEND = 2;
    /** Key repeat can be faster than a Windows volume step is useful: at most one key per this many ms (holding the key = about ten steps a second). */
    static final long MIN_GAP_MS = 60;

    private long lastSentMs = Long.MIN_VALUE / 2;

    /**
     * @param toPc        the user's switch "the volume keys control the PC", read at the time of the event
     * @param linked      a PC is attached: a session is running, or its Wi-Fi link is being re-established (grace period)
     * @param live        a session channel exists right now, so a key can really be sent (false while the link is being re-established)
     * @param down        ACTION_DOWN (the first press and its repeats); a release or anything else only has to be swallowed
     * @param eventTimeMs time of the key event (uptime milliseconds)
     * @return {@link #PASS}, {@link #SWALLOW} or {@link #SEND}
     */
    int decide(boolean toPc, boolean linked, boolean live, boolean down, long eventTimeMs) {
        if (!toPc || !linked) return PASS;
        if (!down || !live) return SWALLOW;
        if (eventTimeMs - lastSentMs < MIN_GAP_MS) return SWALLOW;
        lastSentMs = eventTimeMs;
        return SEND;
    }
}
