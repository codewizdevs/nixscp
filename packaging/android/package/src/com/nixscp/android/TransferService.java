// Keeps NixSCP running while a connection or transfer is busy: Android stops
// background apps and cuts their connections, a foreground service with a
// notification ("Transferring files...") prevents that. Started and stopped
// from C++ (src/mobile/AndroidFiles.cpp) as the session gets busy and idle.
package com.nixscp.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

public class TransferService extends Service
{
    private static final String CHANNEL = "transfers";
    private static final int NOTIFICATION = 1;
    private static final String EXTRA_TEXT = "text";

    public static void start(Context context, String text)
    {
        Intent intent = new Intent(context, TransferService.class);
        intent.putExtra(EXTRA_TEXT, text);
        if (Build.VERSION.SDK_INT >= 26)
            context.startForegroundService(intent);
        else
            context.startService(intent);
    }

    public static void stop(Context context)
    {
        context.stopService(new Intent(context, TransferService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        String text = (intent != null) ? intent.getStringExtra(EXTRA_TEXT) : null;
        Notification notification = build(text != null ? text : "Working...");
        if (Build.VERSION.SDK_INT >= 29)
            startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else
            startForeground(NOTIFICATION, notification);
        return START_NOT_STICKY;
    }

    private Notification build(String text)
    {
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            if (manager.getNotificationChannel(CHANNEL) == null)
                manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Transfers", NotificationManager.IMPORTANCE_LOW));
            builder = new Notification.Builder(this, CHANNEL);
        } else {
            builder = new Notification.Builder(this);
        }
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return builder.setContentTitle("NixSCP")
            .setContentText(text)
            .setSmallIcon(getApplicationInfo().icon)
            .setContentIntent(content)
            .setOngoing(true)
            .build();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }
}
