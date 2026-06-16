package com.example.real_time_vad;

import android.content.Context;
import org.pytorch.IValue;
import org.pytorch.LiteModuleLoader;
import org.pytorch.Module;
import org.pytorch.Tensor;
import android.util.Log;

public class ORVAEModule {
    private static final String TAG = "ORVAE";
    // Set true for [1,1,C,W] (NCHW), false for [1,C,W].
    private static final boolean USE_NCHW = true;
    private final Module module;
    private final int channels;
    private final int W = 64;
    private boolean loggedShape = false;

    public ORVAEModule(Context ctx, String assetName, int channels) {
        Log.i(TAG, "Loading model asset=" + assetName + " channels=" + channels + " W=" + W);
        String path = AssetUtils.assetFilePath(ctx, assetName);
        Log.i(TAG, "Loading model from path=" + path);
        module = LiteModuleLoader.load(path);
        Log.i(TAG, "Model loaded OK");
        this.channels = channels;
    }

    public float runMSE(float[] x) {
        if (x == null) {
            throw new IllegalArgumentException("runMSE: input is null");
        }
        if (x.length != channels * W) {
            throw new IllegalArgumentException(
                    "runMSE: input length " + x.length + " != channels*W " + (channels * W)
            );
        }

        long[] shape = USE_NCHW
                ? new long[]{1, 1, channels, W}
                : new long[]{1, channels, W};
        if (!loggedShape) {
            Log.i(TAG, "runMSE input shape=" + java.util.Arrays.toString(shape)
                    + " len=" + x.length + " channels=" + channels + " W=" + W);
            loggedShape = true;
        }
        Tensor in = Tensor.fromBlob(x, shape);

        IValue out = module.forward(IValue.from(in));
        Tensor recon;
        if (out.isTensor()) {
            recon = out.toTensor();
        } else if (out.isTuple()) {
            IValue[] tuple = out.toTuple();
            if (tuple.length == 0 || !tuple[0].isTensor()) {
                throw new IllegalStateException("runMSE: tuple output has no tensor at index 0");
            }
            recon = tuple[0].toTensor();
        } else {
            throw new IllegalStateException("runMSE: unexpected output type from model");
        }

        float[] xr = recon.getDataAsFloatArray();
        if (xr.length != x.length) {
            throw new IllegalStateException(
                    "runMSE: recon length " + xr.length + " != input length " + x.length
            );
        }

        double sum = 0.0;
        for (int i = 0; i < x.length; i++) {
            double d = (xr[i] - x[i]);
            sum += d * d;
        }
        return (float)(sum / x.length);
    }
}
