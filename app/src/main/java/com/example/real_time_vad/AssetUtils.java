package com.example.real_time_vad;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class AssetUtils {
    private AssetUtils() {}
    private static final String TAG = "ORVAE";

    // Copies an asset to internal storage and returns an absolute filesystem path.
    public static String assetFilePath(Context context, String assetName) {
        try {
            File outFile = new File(context.getFilesDir(), assetName);
            if (outFile.exists() && outFile.length() > 0) {
                Log.i(TAG, "Asset cached: " + outFile.getAbsolutePath() + " size=" + outFile.length());
                return outFile.getAbsolutePath();
            }

            try (InputStream is = context.getAssets().open(assetName);
                 FileOutputStream os = new FileOutputStream(outFile, false)) {

                byte[] buffer = new byte[4 * 1024];
                int read;
                while ((read = is.read(buffer)) != -1) {
                    os.write(buffer, 0, read);
                }
                os.flush();
            }

            Log.i(TAG, "Asset copied: " + outFile.getAbsolutePath() + " size=" + outFile.length());
            return outFile.getAbsolutePath();
        } catch (Exception e) {
            throw new RuntimeException("Failed to load asset: " + assetName, e);
        }
    }
}
