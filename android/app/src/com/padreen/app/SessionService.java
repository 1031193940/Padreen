package com.padreen.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.os.IBinder;

/**
 * Keeps the app's process in the "foreground" while a display session exists, so Android does not stop the network connection when the user
 * switches to another app or the screen goes off.  Shows one low-priority notification; tapping it returns to Padreen.
 */
public class SessionService extends Service {
    private static final String CHANNEL = "session";
    private static final int NOTIF_ID = 1;

    static void start(Context c, String label) {
        try {
            Intent i = new Intent(c, SessionService.class).putExtra("label", label);
            c.startForegroundService(i);
        } catch (Throwable t) {
            android.util.Log.w("Padreen", "cannot start the session service: " + t);
        }
    }

    static void stop(Context c) {
        try {
            c.stopService(new Intent(c, SessionService.class));
        } catch (Throwable ignored) {
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String label = intent != null ? intent.getStringExtra("label") : null;
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        NotificationChannel ch = new NotificationChannel(CHANNEL, "连接状态", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, HomeActivity.class), PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setContentTitle("Padreen 已连接")
                .setContentText(label == null ? "点按返回" : label + " · 点按返回")
                .setContentIntent(open)
                .setOngoing(true);
        try {
            b.setSmallIcon(Icon.createWithResource(this, getApplicationInfo().icon));
        } catch (Throwable t) {
            b.setSmallIcon(android.R.drawable.stat_sys_data_bluetooth);
        }
        startForeground(NOTIF_ID, b.build());
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
