package com.pramod.loopmidi.admin;

import android.content.res.ColorStateList;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;

final class AdminUiStyle {
    static final int TONE_NEUTRAL = 0;
    static final int TONE_PRIMARY = 1;
    static final int TONE_DANGER = 2;

    static final int BACKGROUND = 0xff0a101c;
    static final int TEXT = 0xffedf5ff;
    static final int MUTED = 0xffa4b4c9;
    static final int ACCENT = 0xff39c8ff;
    static final int GOLD = 0xffffd27a;

    private AdminUiStyle() {
    }

    static GradientDrawable panel(int topColor, int bottomColor, int strokeColor, int radiusDp,
                                  android.content.Context context) {
        GradientDrawable drawable = new GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM, new int[]{topColor, bottomColor});
        drawable.setCornerRadius(dp(context, radiusDp));
        drawable.setStroke(dp(context, 1), strokeColor);
        return drawable;
    }

    static void applyPanel(View view, android.content.Context context, boolean alert) {
        view.setBackground(panel(alert ? 0xff482832 : 0xff22364e,
                alert ? 0xff251821 : 0xff111b2b,
                alert ? 0xffad5663 : 0xff48627f, 16, context));
        view.setElevation(dp(context, 5));
    }

    static void styleButton(Button button, android.content.Context context, int tone) {
        int top;
        int bottom;
        int stroke;
        if (tone == TONE_PRIMARY) {
            top = 0xff27b9ec;
            bottom = 0xff17629d;
            stroke = 0xff7be1ff;
        } else if (tone == TONE_DANGER) {
            top = 0xffbd5963;
            bottom = 0xff71323e;
            stroke = 0xffff9ca1;
        } else {
            top = 0xff435872;
            bottom = 0xff26364b;
            stroke = 0xff7188a5;
        }
        GradientDrawable background = panel(top, bottom, stroke, 12, context);
        button.setBackground(new RippleDrawable(
                ColorStateList.valueOf(0x55ffffff), background, null));
        button.setTextColor(TEXT);
        button.setAllCaps(false);
        button.setTextSize(12);
        button.setElevation(dp(context, 4));
        button.setPadding(dp(context, 14), dp(context, 5), dp(context, 14), dp(context, 5));
        button.setOnTouchListener((view, event) -> {
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                view.animate().scaleX(0.97f).scaleY(0.97f)
                        .translationZ(dp(context, 1)).setDuration(80).start();
            } else if (event.getAction() == MotionEvent.ACTION_UP
                    || event.getAction() == MotionEvent.ACTION_CANCEL) {
                view.animate().scaleX(1f).scaleY(1f)
                        .translationZ(0f).setDuration(120).start();
            }
            return false;
        });
    }

    static void styleInput(EditText input, android.content.Context context) {
        input.setBackground(panel(0xff1a293d, 0xff101a29, 0xff526984, 12, context));
        input.setTextColor(TEXT);
        input.setHintTextColor(MUTED);
        input.setPadding(dp(context, 14), dp(context, 10), dp(context, 14), dp(context, 10));
        input.setElevation(dp(context, 3));
    }

    static int dp(android.content.Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
