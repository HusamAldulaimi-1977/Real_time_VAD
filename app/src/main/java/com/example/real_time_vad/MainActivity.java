package com.example.real_time_vad;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Switch;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import org.json.JSONObject;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {
    static {
        System.loadLibrary("vadnative");
    }

    private static final int REQ_MIC = 1;
    private static final int REQ_PICK_AUDIO = 2;

    private static final class ModeConfig {
        final String modeName;
        final String modelAsset;
        final String metaAsset;
        final String percentileAsset;
        final int channels;

        ModeConfig(String modeName, String modelAsset, String metaAsset, String percentileAsset,
                   int channels) {
            this.modeName = modeName;
            this.modelAsset = modelAsset;
            this.metaAsset = metaAsset;
            this.percentileAsset = percentileAsset;
            this.channels = channels;
        }
    }

    private static final ModeConfig MODE_MFCC = new ModeConfig(
            "MFCC",
            "orvae_mfcc.ptl",
            "meta_train_mfcc.json",
            "mse_percentile_table_mfcc.json",
            80
    );

    public native void nativeStart(int sampleRate, int bufferSize);

    public native void nativeStartPipelineOnly();

    public native void nativeStop();

    public native boolean nativeGetFeatures(float[] out);

    public native boolean nativeDequeueWindow(float[] outWin);

        public native void nativeSubmitMSE(float mse);

    public native float nativeGetThreshold();

    public native void nativeSetTrainStats(
            float mn, float mx,
            float mseThreshold
    );

    public native int nativeGetLatestDecision();

    public native float nativeGetLatestMse();

    public native boolean nativeStartFileMode(String path, int bufferSize, int sampleRate);

    public native void nativeSetFileMonitor(boolean enabled);

    public native boolean nativeIsFilePlaying();

    public native boolean nativePush16kChunk(float[] pcm16k, int count);

    public native int nativeGetConfigSr();

    public native int nativeGetConfigHop();

    public native int nativeGetConfigSlideW();

    public native float nativeGetProcessingTimeMs();

    public native float nativeGetFeatureProcessingTimeMs();

    public native float nativeGetMicRms();

    private TextView txtStatus;
    private TextView txtMeter;
    private TextView txtDecision;
    private TextView txtThrValue;
    private TextView txtProcTime;
    private TextView txtFrameTime;
    private TextView noiseLabel;
    private TextView speechLabel;
    private EditText editPercentile;
    private Button btnToggle;
    private Button btnPickFile;
    private RadioGroup radioInputMode;
    private RadioButton rbMic;
    private RadioButton rbFile;

    private Switch swMonitorAudio;

    private boolean isRunning = false;
    private Uri selectedAudioUri = null;

    private volatile boolean inferRunning = false;
    private Thread inferThread;

    private volatile boolean fileFeederRunning = false;
    private Thread fileFeederThread;

    private ORVAEModule orvae;
    private float currentThreshold = 1.0f;
    private ModeConfig activeConfig = MODE_MFCC;
    private int windowSize = 15;
    private final DecisionMovingAverage decisionMa = new DecisionMovingAverage(windowSize);
    private int cfgSr = 16000;
    private int cfgHop = 160;
    private int cfgSlideW = 64;
    private int ioSampleRate = 48000;
    private int ioBufferSize = 480;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Runnable uiTick = new Runnable() {
        @Override
        public void run() {
            int d = nativeGetLatestDecision();
            int displayDecision = d;
            displayDecision = decisionMa.update(d); // Uncomment to enable moving-average smoothing.
            float mse = nativeGetLatestMse();
            txtMeter.setText(String.format(Locale.US, "MSE: %.6f", mse));
            float micRms = nativeGetMicRms();
            txtProcTime.setText(String.format(Locale.US, "Processing Time: %.2f ms | Mic RMS: %.4f", nativeGetProcessingTimeMs(), micRms));
            txtFrameTime.setText(String.format(Locale.US, "Feature Time: %.2f ms", nativeGetFeatureProcessingTimeMs()));
            txtThrValue.setText(String.format(Locale.US, "thr: %.6f", currentThreshold));

            if (displayDecision == 1) {
                txtDecision.setText("Decision: SPEECH");
                noiseLabel.setBackgroundColor(Color.WHITE);
                speechLabel.setBackgroundColor(Color.GREEN);
            } else if (displayDecision == 0) {
                txtDecision.setText("Decision: NOISE");
                noiseLabel.setBackgroundColor(Color.RED);
                speechLabel.setBackgroundColor(Color.WHITE);
            } else {
                txtDecision.setText("Decision: ...");
                noiseLabel.setBackgroundColor(Color.WHITE);
                speechLabel.setBackgroundColor(Color.WHITE);
            }

            //uiHandler.postDelayed(this, 100);
            uiHandler.postDelayed(this, 80);

        }
    };
    private final Runnable filePlayWatcher = new Runnable() {
        @Override
        public void run() {
            if (!isRunning || !rbFile.isChecked()) return;
            boolean playing = nativeIsFilePlaying();
            if (!playing) {
                stopPipeline();
                return;
            }
            uiHandler.postDelayed(this, 100);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        txtStatus = findViewById(R.id.txtStatus);
        txtMeter = findViewById(R.id.txtMeter);
        txtDecision = findViewById(R.id.txtDecision);
        txtThrValue = findViewById(R.id.txtThrValue);
        txtProcTime = findViewById(R.id.txtProcTime);
        txtFrameTime = findViewById(R.id.txtFrameTime);
        noiseLabel = findViewById(R.id.noiseLabel);
        speechLabel = findViewById(R.id.speechLabel);
        editPercentile = findViewById(R.id.editPercentile);
        btnToggle = findViewById(R.id.btnToggle);
        btnPickFile = findViewById(R.id.btnPickFile);
        radioInputMode = findViewById(R.id.radioInputMode);
        rbMic = findViewById(R.id.rbMic);
        rbFile = findViewById(R.id.rbFile);
        swMonitorAudio = findViewById(R.id.swMonitorAudio);

        initAudioIoParams();

        cfgSr = nativeGetConfigSr();
        cfgHop = nativeGetConfigHop();
        cfgSlideW = nativeGetConfigSlideW();

        editPercentile.setText("95");
        activeConfig = getSelectedModeConfig();
        updateModeControls();

        radioInputMode.setOnCheckedChangeListener((group, checkedId) -> {
            if (isRunning) stopPipeline();
            updateModeControls();
        });

        btnPickFile.setOnClickListener(v -> pickAudioFile());
        btnToggle.setOnClickListener(v -> {
            if (isRunning) stopPipeline();
            else startPipeline();
        });
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (isRunning || inferRunning) stopPipeline();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_MIC && grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            startPipeline();
        } else if (requestCode == REQ_MIC) {
            txtStatus.setText("Status: mic permission denied");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_AUDIO || resultCode != RESULT_OK || data == null) return;
        selectedAudioUri = data.getData();
        if (selectedAudioUri == null) return;
        final int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(selectedAudioUri, flags);
        } catch (Exception ignored) {
        }
        txtStatus.setText("Status: file selected");
    }

    private ModeConfig getSelectedModeConfig() {
        return MODE_MFCC;
    }

    private boolean isMicMode() {
        return rbMic.isChecked();
    }

    private void updateModeControls() {
        boolean mic = isMicMode();
        btnPickFile.setEnabled(!mic);
        if (mic) {
            txtStatus.setText("Status: Mic mode");
        } else {
            txtStatus.setText(selectedAudioUri == null ? "Status: File mode (pick an audio file)" : "Status: File mode (ready)");
        }
    }

    private void pickAudioFile() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("audio/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_PICK_AUDIO);
    }

    private void startPipeline() {
        activeConfig = getSelectedModeConfig();
        if (!loadTrainStatsAndThreshold(activeConfig)) {
            btnToggle.setText("Start");
            return;
        }

        if (!inferRunning) startInference(activeConfig);
        if (!inferRunning) {
            txtStatus.setText("Status: failed to start inference");
            return;
        }

        if (isMicMode()) {
                        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
                return;
            }
            android.util.Log.i("ORVAE", "Calling nativeStart sr=" + ioSampleRate + " buf=" + ioBufferSize);
            nativeStart(ioSampleRate, ioBufferSize);
            android.util.Log.i("ORVAE", "nativeStart returned");
            txtStatus.setText("Status: Mic running (" + activeConfig.modeName + ")");
        } else {
            if (selectedAudioUri == null) {
                txtStatus.setText("Status: pick a file first");
                stopInferenceOnly();
                return;
            }
            // Use Superpowered AdvancedAudioPlayer for file mode.
            startFileFeeder(selectedAudioUri);
            txtStatus.setText("Status: File running (" + activeConfig.modeName + ")");
        }

        uiHandler.removeCallbacks(uiTick);
        decisionMa.reset();
        uiHandler.post(uiTick);
        isRunning = true;
        btnToggle.setText("Stop");
        btnToggle.setBackgroundColor(Color.RED);
    }

    private void stopPipeline() {
        uiHandler.removeCallbacks(uiTick);
        uiHandler.removeCallbacks(filePlayWatcher);
                fileFeederRunning = false;
        if (fileFeederThread != null && Thread.currentThread() != fileFeederThread) {
            try {
                fileFeederThread.join(300);
            } catch (InterruptedException ignored) {
            }
        }
        fileFeederThread = null;

        nativeStop();
        stopInferenceOnly();
        decisionMa.reset();
        
        isRunning = false;
        btnToggle.setText("Start");
        btnToggle.setBackgroundColor(Color.GREEN);
        txtStatus.setText("Status: stopped");
    }

    private void stopInferenceOnly() {
        inferRunning = false;
        if (inferThread != null && Thread.currentThread() != inferThread) {
            try {
                inferThread.join(300);
            } catch (InterruptedException ignored) {
            }
        }
        inferThread = null;
    }

    private String copyUriToCacheFile(Uri uri) {
        if (uri == null) return null;
        try {
            String name = "vad_input_" + System.currentTimeMillis();
            java.io.File outFile = new java.io.File(getCacheDir(), name);
            try (InputStream in = getContentResolver().openInputStream(uri);
                 java.io.FileOutputStream out = new java.io.FileOutputStream(outFile)) {
                if (in == null) return null;
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            android.util.Log.i("ORVAE", "cache file=" + outFile.getAbsolutePath() + " size=" + outFile.length());
            return outFile.getAbsolutePath();
        } catch (Exception e) {
            return null;
        }
    }

    private void initAudioIoParams() {
        int fallbackSr = 48000;
        int fallbackBuf = 480;
        try {
            AudioManager audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
            if (audioManager != null) {
                String srStr = audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
                String bsStr = audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
                ioSampleRate = parseOrDefault(srStr, fallbackSr);
                ioBufferSize = parseOrDefault(bsStr, fallbackBuf);
                if (ioSampleRate != 48000) {
                    android.util.Log.w("ORVAE", "Forcing ioSampleRate to 48000 (device=" + ioSampleRate + ")");
                    ioSampleRate = 48000;
                }
            } else {
                ioSampleRate = fallbackSr;
                ioBufferSize = fallbackBuf;
            }
        } catch (RuntimeException e) {
            ioSampleRate = fallbackSr;
            ioBufferSize = fallbackBuf;
        }
    }

    private int parseOrDefault(String s, int def) {
        if (s == null) return def;
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private void startInference(ModeConfig config) {
        try {
            android.util.Log.i("ORVAE", "startInference mode=" + config.modeName
                    + " asset=" + config.modelAsset
                    + " channels=" + config.channels
                    + " slideW=" + cfgSlideW);
            orvae = new ORVAEModule(this, config.modelAsset, config.channels);
        } catch (RuntimeException e) {
            txtStatus.setText("Model load failed: " + e.getMessage());
            inferRunning = false;
            return;
        }

        inferRunning = true;
        final int n = config.channels * cfgSlideW;
        final float[] win = new float[n];

        inferThread = new Thread(() -> {
            android.util.Log.i("ORVAE", "inferThread started");
            int inferLogCount = 0;
            while (inferRunning) {
                boolean ok = nativeDequeueWindow(win);
                if (!ok) {
                    try {
                        Thread.sleep(2);
                    } catch (InterruptedException ignored) {
                    }
                    continue;
                }

                float mse;
                try {
                    if (inferLogCount < 3) {
                        android.util.Log.i("ORVAE", "Calling runMSE on thread=" + Thread.currentThread().getName());
                        inferLogCount++;
                    }
                    mse = orvae.runMSE(win);
                } catch (RuntimeException e) {
                    inferRunning = false;
                    runOnUiThread(() -> txtStatus.setText("Status: inference error: " + e.getMessage()));
                    break;
                }
                nativeSubmitMSE(mse);
            }
        }, "orvae-infer");
        inferThread.start();
    }

    private void startFileFeeder(Uri uri) {
        final boolean monitorAudio = (swMonitorAudio != null) && swMonitorAudio.isChecked();
        fileFeederRunning = true;
        fileFeederThread = new Thread(() -> {
            try {
                String path = copyUriToCacheFile(uri);
                if (path == null) throw new IllegalStateException("Failed to open file.");
                runOnUiThread(() -> {
                    android.util.Log.i("ORVAE", "Starting Superpowered file mode path=" + path);
                    nativeSetFileMonitor(monitorAudio);
                    boolean ok = nativeStartFileMode(path, ioBufferSize, ioSampleRate);
                    if (!ok) {
                        txtStatus.setText("Status: file start failed: nativeStartFileMode");
                        stopPipeline();
                        return;
                    }
                    uiHandler.removeCallbacks(filePlayWatcher);
                    uiHandler.postDelayed(filePlayWatcher, 100);
                });
            } catch (Exception e) {
                runOnUiThread(() -> txtStatus.setText("Status: file start failed: " + e.getMessage()));
            } finally {
                fileFeederRunning = false;
            }
        }, "file-feeder");
        fileFeederThread.start();
    }

    private void decodeAndFeedFile(Uri uri, boolean monitorAudio) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null;
        AudioTrack audioTrack = null;
        try {
            extractor.setDataSource(this, uri, null);

            int trackIndex = -1;
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat f = extractor.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    trackIndex = i;
                    format = f;
                    break;
                }
            }
            if (trackIndex < 0 || format == null) throw new IllegalStateException("No audio track found.");

            extractor.selectTrack(trackIndex);
            String mime = format.getString(MediaFormat.KEY_MIME);
            if (mime == null) throw new IllegalStateException("Missing MIME type.");

            int inRate = format.containsKey(MediaFormat.KEY_SAMPLE_RATE) ? format.getInteger(MediaFormat.KEY_SAMPLE_RATE) : cfgSr;
            int channels = format.containsKey(MediaFormat.KEY_CHANNEL_COUNT) ? format.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 1;

            if (monitorAudio) {
                int minBuffer = AudioTrack.getMinBufferSize(
                        cfgSr,
                        AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT
                );
                if (minBuffer < 0) minBuffer = cfgHop * 8;
                audioTrack = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                                .build())
                        .setAudioFormat(new AudioFormat.Builder()
                                .setSampleRate(cfgSr)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build())
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .setBufferSizeInBytes(Math.max(minBuffer, cfgHop * 8))
                        .build();
                audioTrack.play();
            }

            decoder = MediaCodec.createDecoderByType(mime);
            decoder.configure(format, null, null, 0);
            decoder.start();

            boolean inputEos = false;
            boolean outputEos = false;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

            while (!outputEos && fileFeederRunning) {
                if (!inputEos) {
                    int inIndex = decoder.dequeueInputBuffer(10000);
                    if (inIndex >= 0) {
                        ByteBuffer inBuf = decoder.getInputBuffer(inIndex);
                        if (inBuf != null) {
                            int sampleSize = extractor.readSampleData(inBuf, 0);
                            long pts = extractor.getSampleTime();
                            if (sampleSize < 0) {
                                decoder.queueInputBuffer(inIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputEos = true;
                            } else {
                                decoder.queueInputBuffer(inIndex, 0, sampleSize, Math.max(pts, 0), 0);
                                extractor.advance();
                            }
                        }
                    }
                }

                int outIndex = decoder.dequeueOutputBuffer(info, 10000);
                if (outIndex >= 0) {
                    ByteBuffer outBuf = decoder.getOutputBuffer(outIndex);
                    if (outBuf != null && info.size > 0) {
                        outBuf.position(info.offset);
                        outBuf.limit(info.offset + info.size);

                        int pcmEncoding = format.containsKey(MediaFormat.KEY_PCM_ENCODING)
                                ? format.getInteger(MediaFormat.KEY_PCM_ENCODING)
                                : AudioFormat.ENCODING_PCM_16BIT;
                        float[] mono = pcmToMonoFloat(outBuf.slice(), channels, pcmEncoding);
                        float[] at16k = (inRate == cfgSr) ? mono : resampleLinear(mono, inRate, cfgSr);
                        pushFileChunkedRealtime(at16k, audioTrack);
                    }
                    decoder.releaseOutputBuffer(outIndex, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outputEos = true;
                }
            }
        } finally {
            if (audioTrack != null) {
                try {
                    audioTrack.pause();
                    audioTrack.flush();
                } catch (Exception ignored) {}
                audioTrack.release();
            }
            if (decoder != null) {
                decoder.stop();
                decoder.release();
            }
            extractor.release();
        }
    }

    private void pushFileChunkedRealtime(float[] pcm16k, AudioTrack audioTrack) {
        float[] chunk = new float[cfgHop];
        short[] chunk16 = (audioTrack != null) ? new short[cfgHop] : null;
        int idx = 0;
        long nextTickNs = System.nanoTime();
        while (fileFeederRunning && idx < pcm16k.length) {
            int n = Math.min(cfgHop, pcm16k.length - idx);
            System.arraycopy(pcm16k, idx, chunk, 0, n);
            nativePush16kChunk(chunk, n);
            if (audioTrack != null && chunk16 != null) {
                for (int i = 0; i < n; i++) {
                    float v = chunk[i];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    chunk16[i] = (short) (v * 32767.0f);
                }
                audioTrack.write(chunk16, 0, n, AudioTrack.WRITE_BLOCKING);
            }
            idx += n;
            if (audioTrack == null) {
                nextTickNs += (long)(n * 1_000_000_000.0 / (double)cfgSr);
                long waitNs = nextTickNs - System.nanoTime();
                if (waitNs > 0) {
                    try {
                        Thread.sleep(waitNs / 1_000_000L, (int)(waitNs % 1_000_000L));
                    } catch (InterruptedException ignored) {}
                }
            }
        }
    }

    private static float[] pcmToMonoFloat(ByteBuffer pcm, int channels, int pcmEncoding) {
        if (channels < 1) channels = 1;
        if (pcmEncoding == AudioFormat.ENCODING_PCM_FLOAT) {
            pcm.order(ByteOrder.LITTLE_ENDIAN);
            int samples = pcm.remaining() / 4;
            int frames = samples / channels;
            float[] out = new float[frames];
            for (int i = 0; i < frames; i++) {
                float sum = 0f;
                for (int ch = 0; ch < channels; ch++) sum += pcm.getFloat();
                out[i] = sum / channels;
            }
            return out;
        }

        pcm.order(ByteOrder.LITTLE_ENDIAN);
        int samples = pcm.remaining() / 2;
        int frames = samples / channels;
        float[] out = new float[frames];
        for (int i = 0; i < frames; i++) {
            float sum = 0f;
            for (int ch = 0; ch < channels; ch++) sum += (float)pcm.getShort() / 32768.0f;
            out[i] = sum / channels;
        }
        return out;
    }

    private static float[] resampleLinear(float[] in, int inRate, int outRate) {
        if (inRate <= 0 || outRate <= 0 || in.length == 0) return new float[0];
        if (inRate == outRate) return in;

        int outLen = Math.max(1, (int)Math.round((double)in.length * outRate / inRate));
        float[] out = new float[outLen];
        double ratio = (double)inRate / outRate;
        for (int i = 0; i < outLen; i++) {
            double src = i * ratio;
            int i0 = (int)Math.floor(src);
            int i1 = Math.min(i0 + 1, in.length - 1);
            double a = src - i0;
            out[i] = (float)((1.0 - a) * in[i0] + a * in[i1]);
        }
        return out;
    }

    private static String readAssetText(android.content.res.AssetManager am, String name) throws Exception {
        try (InputStream is = am.open(name)) {
            byte[] bytes = new byte[is.available()];
            int read = is.read(bytes);
            return new String(bytes, 0, read, StandardCharsets.UTF_8);
        }
    }

    private float thresholdFromPercentile(int percentile, String tableAsset) throws Exception {
        if (percentile < 1) percentile = 1;
        if (percentile > 99) percentile = 99;
        String tableTxt = readAssetText(getAssets(), tableAsset);
        JSONObject table = new JSONObject(tableTxt);
        return (float)table.getDouble(String.valueOf(percentile));
    }

    private boolean loadTrainStatsAndThreshold(ModeConfig cfg) {
        try {
            String metaTxt = readAssetText(getAssets(), cfg.metaAsset);
            JSONObject meta = new JSONObject(metaTxt);

            float mn = (float)meta.optDouble("min", 0.0);
            float mx = (float)meta.optDouble("max", 1.0);
            int p = 95;
            try {
                String s = (editPercentile != null) ? editPercentile.getText().toString().trim() : "";
                if (!s.isEmpty()) p = Integer.parseInt(s);
            } catch (Exception ignored) {}

            float mseThr = thresholdFromPercentile(p, cfg.percentileAsset);
            nativeSetTrainStats(mn, mx, mseThr);
            currentThreshold = mseThr;
            return true;
        } catch (Exception e) {
            txtStatus.setText("Status: stats load failed: " + e.getMessage());
            return false;
        }
    }
}









