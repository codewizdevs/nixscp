// Downloads in the public Downloads folder (Downloads/NixSCP), where the
// Files app and the Downloads list show them: through MediaStore on Android
// 10+ (no storage permission), as a plain copy on Android 9.
package com.nixscp.android;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class PublicDownloads
{
    // Copies a downloaded file; returns where it went ("" on failure)
    public static String save(Context context, String path, String mimeType)
    {
        File source = new File(path);
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                ContentResolver resolver = context.getContentResolver();
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, source.getName());
                values.put(MediaStore.MediaColumns.MIME_TYPE, mimeType);
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/NixSCP");
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (uri == null)
                    return "";
                try (InputStream in = new FileInputStream(source); OutputStream out = resolver.openOutputStream(uri)) {
                    copy(in, out);
                }
                values.clear();
                values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                resolver.update(uri, values, null, null);
                return Environment.DIRECTORY_DOWNLOADS + "/NixSCP/" + source.getName();
            }
            File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "NixSCP");
            dir.mkdirs();
            File target = new File(dir, source.getName());
            try (InputStream in = new FileInputStream(source); OutputStream out = new FileOutputStream(target)) {
                copy(in, out);
            }
            return target.getAbsolutePath();
        } catch (Exception e) {
            return "";
        }
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException
    {
        byte[] buffer = new byte[1 << 16];
        int count;
        while ((count = in.read(buffer)) > 0)
            out.write(buffer, 0, count);
    }
}
