package com.charepa;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;

/**
 * Stages the three files the chain needs and runs it, streaming the output to
 * the log view and to files/last.log.
 *
 * Assets are not executable and app data is the only directory an
 * untrusted_app process may execute from, so everything is copied to files/bin
 * first. The chain finds its two companions through /proc/self/exe, so keeping
 * them in the same directory is what makes the module and the boot payload
 * reachable.
 *
 * The activity also accepts an explicit mode, so the chain can be started
 * without touching the screen:
 *
 *     am start -n com.charepa/.MainActivity --es mode all
 */
public class MainActivity extends Activity {

    private static final String LOGFILE = "last.log";

    /** Copied into files/bin on every launch; the first is the entry point. */
    private static final String[] FILES = { "charepa", "bootpayload", "myroot.ko" };

    private TextView log;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private Writer fileLog;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        log = (TextView) findViewById(R.id.log);
        TextView device = (TextView) findViewById(R.id.device);
        device.setText(Build.MODEL + "  Android " + Build.VERSION.RELEASE
                + "  SDK " + Build.VERSION.SDK_INT + "  uid=" + android.os.Process.myUid());

        findViewById(R.id.validate).setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { run("validate"); }
        });
        findViewById(R.id.scan).setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { run("all"); }
        });

        Intent it = getIntent();
        String mode = it == null ? null : it.getStringExtra("mode");
        if (mode != null) run(mode);
    }

    private String stage() throws IOException {
        File dir = new File(getFilesDir(), "bin");
        if (!dir.exists() && !dir.mkdirs()) throw new IOException("mkdir failed");

        for (String name : FILES) {
            File out = new File(dir, name);
            try (InputStream in = getAssets().open("arm64/" + name);
                 OutputStream os = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            }
            out.setReadable(true, true);
            // Only the entry point needs the execute bit; the module is read by
            // init_module and the payload is copied onto /system by charepa.
            out.setExecutable(name.equals("charepa"), false);
        }
        return new File(dir, "charepa").getAbsolutePath();
    }

    private void run(String mode) {
        log.setText("");
        final String path;
        try {
            path = stage();
            fileLog = new OutputStreamWriter(
                    new FileOutputStream(new File(getFilesDir(), LOGFILE)), "UTF-8");
        } catch (IOException e) {
            append("準備に失敗しました: " + e.getMessage());
            return;
        }
        append("実行 " + path + " " + mode);

        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    ProcessBuilder pb = new ProcessBuilder(path, mode);
                    pb.redirectErrorStream(true);
                    Process p = pb.start();
                    try (BufferedReader r =
                                 new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                        String line;
                        while ((line = r.readLine()) != null) append(line);
                    }
                    int code = p.waitFor();
                    append("終了コード " + code);
                } catch (Exception e) {
                    append("起動に失敗しました: " + e.getMessage());
                }
                closeFileLog();
            }
        }).start();
    }

    private void append(final String line) {
        synchronized (this) {
            if (fileLog != null) {
                try {
                    fileLog.write(line);
                    fileLog.write("\n");
                    fileLog.flush();
                } catch (IOException ignored) {
                }
            }
        }
        ui.post(new Runnable() {
            @Override public void run() {
                log.append(line);
                log.append("\n");
            }
        });
    }

    private void closeFileLog() {
        synchronized (this) {
            if (fileLog != null) {
                try {
                    fileLog.close();
                } catch (IOException ignored) {
                }
                fileLog = null;
            }
        }
    }
}
