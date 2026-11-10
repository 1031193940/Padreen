package com.padreen.app;

/** Remembers how the last session ended: the home screen decides from it whether to connect by itself again. */
final class Handoff {
    private static volatile long endedAt;
    private static volatile boolean endedByPc;

    private Handoff() {
    }

    private static volatile boolean userEnded;

    /** The user left the display with the Back gesture: stay on the home screen, do not reconnect by itself. */
    static void markUserEnded() {
        userEnded = true;
    }

    static boolean userEnded() {
        return userEnded;
    }

    static void clearUserEnded() {
        userEnded = false;
    }

    static void ended(boolean byPc) {
        endedAt = android.os.SystemClock.uptimeMillis();
        endedByPc = byPc;
    }

    /** True while the last session just ended: the home screen should not reconnect at once (the PC may be quitting). */
    static boolean coolingDown() {
        return android.os.SystemClock.uptimeMillis() - endedAt < (endedByPc ? 60000 : 6000);
    }
}