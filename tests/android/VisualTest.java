package com.mio.plugin.shimtest;
import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.LinearLayout;
import android.widget.TextView;
public class VisualTest extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("visualtest"); }
    private TextView status;
    private Thread worker;
    private static native String run(Surface surface, String backend, String libDir);
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(128); // Keep screen on during the bounded test.
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        status = new TextView(this);
        status.setText("Testing " + getIntent().getStringExtra("backend"));
        status.setTextSize(22);
        layout.addView(status);
        SurfaceView view = new SurfaceView(this);
        layout.addView(view, new LinearLayout.LayoutParams(512, 512));
        view.getHolder().addCallback(this);
        setContentView(layout);
        view.postDelayed(() -> view.getHolder().setFixedSize(640, 384), 2000);
        view.postDelayed(() -> view.getHolder().setFixedSize(512, 512), 5000);
    }
    @Override public void surfaceCreated(SurfaceHolder h) {
        if (worker != null) return;
        worker = new Thread(() -> {
            String result = run(h.getSurface(), getIntent().getStringExtra("backend"),
                                getApplicationInfo().nativeLibraryDir);
            android.util.Log.i("ShimVisualTest", result);
            runOnUiThread(() -> status.setText(result));
        });
        worker.start();
    }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int height) {}
    @Override public void surfaceDestroyed(SurfaceHolder h) {}
}
