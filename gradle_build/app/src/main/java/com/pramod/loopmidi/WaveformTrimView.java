package com.pramod.loopmidi;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

final class WaveformTrimView extends View {
    interface OnMarkersChanged {
        void onChanged(float startFraction, float endFraction);
    }

    private float[] samples;
    private float startFraction;
    private float endFraction = 1f;
    private float zoom = 1f;
    private float viewportStart;
    private int dragTarget;
    private float lastTouchX;
    private OnMarkersChanged listener;
    private static final float MAX_ZOOM = 32f;
    private static final float MIN_MARKER_GAP = 0.0000001f;

    private final Paint backgroundPaint = new Paint();
    private final Paint gridPaint = new Paint();
    private final Paint selectedWavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint unselectedWavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint regionPaint = new Paint();
    private final Paint startPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint endPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint handlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint loadingPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    WaveformTrimView(Context context) {
        super(context);
        backgroundPaint.setColor(0xFF0A1628);
        gridPaint.setColor(0x22FFFFFF);
        gridPaint.setStrokeWidth(1f);
        selectedWavePaint.setColor(0xFF38B6FF);
        selectedWavePaint.setStrokeWidth(2f);
        unselectedWavePaint.setColor(0xFF334455);
        unselectedWavePaint.setStrokeWidth(2f);
        regionPaint.setColor(0x2238B6FF);
        startPaint.setColor(0xFF00FF66);
        startPaint.setStrokeWidth(3f);
        endPaint.setColor(0xFFFF4444);
        endPaint.setStrokeWidth(3f);
        handlePaint.setStyle(Paint.Style.FILL);
        textPaint.setTextSize(30f);
        textPaint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        loadingPaint.setColor(0xFF556677);
        loadingPaint.setTextSize(34f);
        loadingPaint.setTextAlign(Paint.Align.CENTER);
        loadingPaint.setAntiAlias(true);
    }

    void setSamples(float[] samples) {
        this.samples = samples;
        invalidate();
    }

    void setMarkers(float startFraction, float endFraction) {
        float start = Math.max(0f, Math.min(1f, startFraction));
        float end = Math.max(0f, Math.min(1f, endFraction));
        if (end - start < MIN_MARKER_GAP) {
            end = Math.min(1f, start + MIN_MARKER_GAP);
            start = Math.max(0f, end - MIN_MARKER_GAP);
        }
        this.startFraction = start;
        this.endFraction = end;
        invalidate();
    }

    void setOnMarkersChanged(OnMarkersChanged listener) {
        this.listener = listener;
    }

    void zoomIn() {
        setZoom(Math.min(MAX_ZOOM, zoom * 2f));
    }

    void zoomOut() {
        setZoom(Math.max(1f, zoom / 2f));
    }

    void resetZoom() {
        zoom = 1f;
        viewportStart = 0f;
        invalidate();
    }

    float getZoomFactor() {
        return zoom;
    }

    private void setZoom(float newZoom) {
        if (newZoom == zoom) return;
        float center = (startFraction + endFraction) * 0.5f;
        zoom = newZoom;
        viewportStart = Math.max(0f, Math.min(1f - 1f / zoom,
                center - 0.5f / zoom));
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        int width = getWidth();
        int height = getHeight();
        if (width == 0 || height == 0) return;

        canvas.drawRect(0, 0, width, height, backgroundPaint);
        float visibleSpan = 1f / zoom;
        for (int i = 1; i < 4; i++) {
            float x = width * i / 4f;
            canvas.drawLine(x, 0, x, height, gridPaint);
        }
        canvas.drawLine(0, height * 0.5f, width, height * 0.5f, gridPaint);

        if (samples == null || samples.length == 0) {
            canvas.drawText("Waveform loading...", width * 0.5f,
                    height * 0.5f + 12, loadingPaint);
            return;
        }

        int saveCount = canvas.save();
        canvas.clipRect(0, 0, width, height);
        float startX = (startFraction - viewportStart) / visibleSpan * width;
        float endX = (endFraction - viewportStart) / visibleSpan * width;
        float centerY = height * 0.5f;
        canvas.drawRect(startX, 0, endX, height, regionPaint);
        int firstSample = Math.max(0, (int) (viewportStart * samples.length));
        int lastSample = Math.min(samples.length,
                (int) Math.ceil((viewportStart + visibleSpan) * samples.length));
        for (int i = firstSample; i < lastSample; i++) {
            float fraction = (i + 0.5f) / samples.length;
            float x = (fraction - viewportStart) / visibleSpan * width;
            float amplitude = samples[i] * height * 0.46f;
            canvas.drawLine(x, centerY - amplitude, x, centerY + amplitude,
                    fraction >= startFraction && fraction <= endFraction
                            ? selectedWavePaint : unselectedWavePaint);
        }

        canvas.drawLine(startX, 0, startX, height, startPaint);
        canvas.drawLine(endX, 0, endX, height, endPaint);
        handlePaint.setColor(0xFF00FF66);
        if (startX >= 0 && startX <= width) canvas.drawCircle(startX, 22, 22, handlePaint);
        textPaint.setColor(0xFF000000);
        textPaint.setTextAlign(Paint.Align.CENTER);
        if (startX >= 0 && startX <= width) canvas.drawText("S", startX, 32, textPaint);
        handlePaint.setColor(0xFFFF4444);
        if (endX >= 0 && endX <= width) canvas.drawCircle(endX, 22, 22, handlePaint);
        textPaint.setColor(0xFFFFFFFF);
        if (endX >= 0 && endX <= width) canvas.drawText("E", endX, 32, textPaint);

        loadingPaint.setTextSize(24f);
        loadingPaint.setTextAlign(Paint.Align.LEFT);
        loadingPaint.setColor(0xFF00FF66);
        if (startX >= 0 && startX <= width) {
            canvas.drawText(String.format(java.util.Locale.US, "%.1f%%",
                    startFraction * 100), startX + 4, height - 6, loadingPaint);
        }
        loadingPaint.setTextAlign(Paint.Align.RIGHT);
        loadingPaint.setColor(0xFFFF4444);
        if (endX >= 0 && endX <= width) {
            canvas.drawText(String.format(java.util.Locale.US, "%.1f%%",
                    endFraction * 100), endX - 4, height - 6, loadingPaint);
        }
        canvas.restoreToCount(saveCount);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int width = getWidth();
        if (width == 0) return true;
        float x = event.getX();
        float visibleSpan = 1f / zoom;
        float startX = (startFraction - viewportStart) / visibleSpan * width;
        float endX = (endFraction - viewportStart) / visibleSpan * width;
        float slop = Math.max(44f, width * 0.04f);

        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                float startDistance = Math.abs(x - startX);
                float endDistance = Math.abs(x - endX);
                boolean startVisible = startX >= 0 && startX <= width;
                boolean endVisible = endX >= 0 && endX <= width;
                if (startVisible && startDistance < slop && endVisible && endDistance < slop) {
                    dragTarget = startDistance <= endDistance ? 1 : 2;
                } else if (startVisible && startDistance < slop) {
                    dragTarget = 1;
                } else if (endVisible && endDistance < slop) {
                    dragTarget = 2;
                } else {
                    dragTarget = 3;
                }
                lastTouchX = x;
                break;
            case MotionEvent.ACTION_MOVE:
                if (dragTarget == 1) {
                    float fraction = viewportStart + x / width * visibleSpan;
                    startFraction = Math.max(0f,
                            Math.min(endFraction - MIN_MARKER_GAP, fraction));
                    notifyMarkersChanged();
                } else if (dragTarget == 2) {
                    float fraction = viewportStart + x / width * visibleSpan;
                    endFraction = Math.min(1f,
                            Math.max(startFraction + MIN_MARKER_GAP, fraction));
                    notifyMarkersChanged();
                } else if (dragTarget == 3) {
                    viewportStart = Math.max(0f, Math.min(1f - visibleSpan,
                            viewportStart - (x - lastTouchX) / width * visibleSpan));
                    lastTouchX = x;
                    invalidate();
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                dragTarget = 0;
                break;
            default:
                return false;
        }
        return true;
    }

    private void notifyMarkersChanged() {
        invalidate();
        if (listener != null) listener.onChanged(startFraction, endFraction);
    }
}
