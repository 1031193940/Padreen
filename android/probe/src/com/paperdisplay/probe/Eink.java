package com.paperdisplay.probe;

import android.util.Log;
import android.view.Surface;

import java.lang.reflect.Method;

/**
 * Reflection wrappers for Huawei's eink.framework (lives on the boot classpath of HMW-W09).
 * Probe only: the production app will call these through JNI / compile-time stubs.
 *
 * Deliberately NOT wrapped (they change system-wide state): setEpdcDefaultMode,
 * setDelayRefreshInterval, setNeedCompareFrame, setRefreshStrategy, setIngnoreAllReresh,
 * setT1000LogStatus, nativePostCommand.
 */
final class Eink {
    static final String TAG = "PaperProbe";

    static Method getDefaultMode, getLastMode, getT1000Info, getPowerStatus, getPmicPowerStatus,
            getNeedCompare, setForceNext, forceRefresh, addEpdc, forceUpdateMode, isPenRefreshComplete;

    /** Look up one method; never throws. A denied hidden-API lookup shows up as NoSuchMethodException. */
    static Method find(String cls, String name, Class<?>... params) {
        try {
            return Class.forName(cls).getMethod(name, params);
        } catch (Throwable t) {
            Log.w(TAG, "find " + cls + "#" + name + " -> " + t.getClass().getSimpleName());
            return null;
        }
    }

    static String init() {
        String E = "android.eink.EPDCDevice";
        getDefaultMode = find(E, "getEpdcDefaultMode");
        getLastMode = find(E, "getEpdcLastMode");
        getT1000Info = find(E, "getT1000Info");
        getPowerStatus = find(E, "getPowerStatus");
        getPmicPowerStatus = find(E, "getPmicPowerStatus");
        getNeedCompare = find(E, "getNeedCompareFrame");
        setForceNext = find(E, "setForceNextPostMode", int.class);
        forceRefresh = find(E, "forceRefresh", int.class);
        isPenRefreshComplete = find(E, "isMarkPenRefreshComplete");
        String S = "android.eink.api.SurfaceAPI";
        addEpdc = find(S, "addEpdc", Surface.class, int[].class);
        forceUpdateMode = find(S, "forceUpdateMode", Surface.class, int.class);
        return "getDefault=" + (getDefaultMode != null) + " getLast=" + (getLastMode != null)
                + " t1000Info=" + (getT1000Info != null) + " power=" + (getPowerStatus != null)
                + " pmic=" + (getPmicPowerStatus != null) + " needCompare=" + (getNeedCompare != null)
                + " forceNext=" + (setForceNext != null) + " forceRefresh=" + (forceRefresh != null)
                + " addEpdc=" + (addEpdc != null) + " forceUpdateMode=" + (forceUpdateMode != null);
    }

    /** Enumerate every declared method of a class and report which ones a normal app may look up. */
    static void dumpAccess(String cls) {
        try {
            Class<?> c = Class.forName(cls);
            int ok = 0, denied = 0;
            for (Method m : c.getDeclaredMethods()) {
                if (m.isSynthetic()) continue;
                String sig = m.getName() + java.util.Arrays.toString(m.getParameterTypes());
                boolean allowed;
                try {
                    c.getDeclaredMethod(m.getName(), m.getParameterTypes());
                    allowed = true;
                } catch (NoSuchMethodException e) {
                    allowed = false;
                }
                int mod = m.getModifiers();
                String flags = (java.lang.reflect.Modifier.isStatic(mod) ? "static " : "")
                        + (java.lang.reflect.Modifier.isNative(mod) ? "native " : "")
                        + (java.lang.reflect.Modifier.isPublic(mod) ? "public" : "nonpublic");
                Log.i(TAG, "API " + (allowed ? "ALLOW " : "DENY  ") + cls.substring(cls.lastIndexOf('.') + 1)
                        + "." + sig + " -> " + m.getReturnType().getSimpleName() + " [" + flags + "]");
                if (allowed) ok++; else denied++;
            }
            Log.i(TAG, "API summary " + cls + ": allowed=" + ok + " denied=" + denied);
        } catch (Throwable t) {
            Log.e(TAG, "dumpAccess " + cls + " failed: " + t);
        }
    }

    static Object call(Method m, Object... args) {
        if (m == null) return null;
        try {
            return m.invoke(null, args);
        } catch (Throwable t) {
            Log.e(TAG, "call " + m.getName() + " failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }
}
