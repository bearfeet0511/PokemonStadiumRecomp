package io.github.bearfeet0511.stadiumrecomp;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Battle info on a second screen (the AYN Thor's bottom screen, or any
 * presentation display). Left: the Pokemon in the battle with HP and status,
 * the one that's out highlighted. Right: the active Pokemon's moves with the
 * button that uses each and its PP. Data comes from the native side.
 */
public class BattlePanel extends Presentation {
    private static final long REFRESH_MS = 250;

    // Stadium's move buttons: the 4 move slots go clockwise round the C-buttons.
    private static final String[] MOVE_BUTTONS = { "C▲", "C▶", "C▼", "C◀" };
    // The matching N64 button bits (CONT_E, CONT_F, CONT_D, CONT_C).
    private static final int[] MOVE_BUTTON_BITS = { 0x0008, 0x0001, 0x0004, 0x0002 };

    static native String nativeSnapshot();

    static native void nativePress(int buttons);

    private final Handler handler = new Handler(Looper.getMainLooper());
    private PanelView view;

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            String text;
            try {
                text = nativeSnapshot();
            } catch (UnsatisfiedLinkError e) {
                text = "battle\t0\n"; // native library not loaded yet
            }
            view.update(text);
            handler.postDelayed(this, REFRESH_MS);
        }
    };

    public BattlePanel(Context context, Display display) {
        super(context, display);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        view = new PanelView(getContext());
        setContentView(view);
    }

    @Override
    protected void onStart() {
        super.onStart();
        handler.post(refresh);
    }

    @Override
    protected void onStop() {
        handler.removeCallbacks(refresh);
        super.onStop();
    }

    static final class Mon {
        String name;
        int level, hp, maxHp;
        String status;
        boolean active, fainted;
        final String[] moves = new String[4];
        final String[] pp = new String[4];
    }

    static final class PanelView extends View {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private boolean inBattle;
        private final List<Mon> mons = new ArrayList<>();
        private String last = "";
        // Where each of the active Pokemon's moves was drawn, for taps.
        private final RectF[] moveRects = { new RectF(), new RectF(), new RectF(), new RectF() };
        private final boolean[] moveTappable = new boolean[4];
        private int flashMove = -1;

        PanelView(Context context) {
            super(context);
            text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent event) {
            if (event.getActionMasked() != android.view.MotionEvent.ACTION_DOWN) {
                return true;
            }
            for (int i = 0; i < 4; i++) {
                if (moveTappable[i] && moveRects[i].contains(event.getX(), event.getY())) {
                    try {
                        nativePress(MOVE_BUTTON_BITS[i]);
                    } catch (UnsatisfiedLinkError e) {
                        return true;
                    }
                    performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                    flashMove = i;
                    invalidate();
                    postDelayed(() -> {
                        flashMove = -1;
                        invalidate();
                    }, 150);
                    return true;
                }
            }
            return true;
        }

        /** Sets the paint to the largest size up to maxSize at which text fits maxWidth. */
        private void fit(String s, float maxSize, float maxWidth) {
            float size = maxSize;
            text.setTextSize(size);
            while (size > 8f && text.measureText(s) > maxWidth) {
                size *= 0.92f;
                text.setTextSize(size);
            }
        }

        void update(String snapshot) {
            if (snapshot.equals(last)) return;
            last = snapshot;
            mons.clear();
            inBattle = false;
            for (String line : snapshot.split("\n")) {
                String[] f = line.split("\t", -1);
                if (f[0].equals("battle")) {
                    inBattle = f.length > 1 && f[1].equals("1");
                } else if (f[0].equals("mon") && f.length >= 16) {
                    Mon m = new Mon();
                    m.name = f[1];
                    m.level = parse(f[2]);
                    m.hp = parse(f[3]);
                    m.maxHp = Math.max(1, parse(f[4]));
                    m.status = f[5];
                    m.active = f[6].equals("1");
                    m.fainted = f[7].equals("1");
                    for (int i = 0; i < 4; i++) {
                        m.moves[i] = f[8 + i * 2];
                        m.pp[i] = f[9 + i * 2];
                    }
                    mons.add(m);
                }
            }
            invalidate();
        }

        private static int parse(String s) {
            try {
                return Integer.parseInt(s);
            } catch (NumberFormatException e) {
                return 0;
            }
        }

        @Override
        protected void onDraw(Canvas canvas) {
            final float w = getWidth(), h = getHeight();
            canvas.drawColor(Color.rgb(16, 22, 38));
            if (!inBattle || mons.isEmpty()) {
                text.setTextSize(h * 0.04f);
                text.setColor(Color.rgb(150, 160, 185));
                text.setTextAlign(Paint.Align.CENTER);
                canvas.drawText("Battle info appears here during battles", w / 2, h / 2, text);
                text.setTextAlign(Paint.Align.LEFT);
                return;
            }
            final float pad = Math.min(w, h) * 0.025f;
            final float split = w * 0.48f;

            // Left: the team.
            final float cardH = (h - pad * (mons.size() + 1)) / mons.size();
            Mon active = null;
            for (int i = 0; i < mons.size(); i++) {
                Mon m = mons.get(i);
                if (m.active) active = m;
                drawMon(canvas, m, pad, pad + i * (cardH + pad), split - pad / 2, Math.min(cardH, h * 0.3f));
            }

            // Right: the active Pokemon's moves.
            final float left = split + pad / 2, right = w - pad;
            for (int i = 0; i < 4; i++) moveTappable[i] = false;
            if (active == null) {
                text.setTextSize(h * 0.04f);
                text.setColor(Color.rgb(150, 160, 185));
                text.setTextAlign(Paint.Align.CENTER);
                canvas.drawText("Choosing...", (left + right) / 2, h / 2, text);
                text.setTextAlign(Paint.Align.LEFT);
                return;
            }
            final float moveH = (h - pad * 5) / 4;
            for (int i = 0; i < 4; i++) {
                float top = pad + i * (moveH + pad);
                if (drawMove(canvas, MOVE_BUTTONS[i], active.moves[i], active.pp[i], left, top, right, moveH, i == flashMove)) {
                    moveRects[i].set(left, top, right, top + moveH);
                    moveTappable[i] = true;
                }
            }
        }

        private void drawMon(Canvas c, Mon m, float left, float top, float right, float height) {
            final float round = height * 0.12f;
            rect.set(left, top, right, top + height);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(m.active ? Color.rgb(44, 70, 120) : Color.rgb(30, 38, 58));
            c.drawRoundRect(rect, round, round, fill);
            if (m.active) {
                fill.setStyle(Paint.Style.STROKE);
                fill.setStrokeWidth(Math.max(3f, height * 0.035f));
                fill.setColor(Color.rgb(255, 204, 64));
                c.drawRoundRect(rect, round, round, fill);
                fill.setStyle(Paint.Style.FILL);
            }
            final int alpha = m.fainted ? 110 : 255;
            final float inset = height * 0.1f;
            final float x = left + inset;
            final float width = right - left - inset * 2;

            // Name, and level or status on the right.
            float size = height * 0.15f;
            float y = top + inset + size * 0.9f;
            String tag = m.fainted ? "FNT" : m.status.isEmpty() ? "Lv " + m.level : m.status;
            text.setTextSize(size);
            float tagWidth = text.measureText(tag);
            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(m.fainted ? Color.rgb(230, 90, 90) : statusColor(m.status));
            text.setAlpha(alpha);
            c.drawText(tag, right - inset, y, text);
            text.setTextAlign(Paint.Align.LEFT);
            fit(m.name, size, width - tagWidth - inset);
            text.setColor(Color.WHITE);
            text.setAlpha(alpha);
            c.drawText(m.name, x, y, text);

            // HP bar with numbers under it.
            float barTop = y + size * 0.45f;
            float barH = height * 0.13f;
            rect.set(x, barTop, x + width, barTop + barH);
            fill.setColor(Color.rgb(60, 66, 84));
            c.drawRoundRect(rect, barH / 2, barH / 2, fill);
            float frac = Math.max(0f, Math.min(1f, m.hp / (float) m.maxHp));
            if (frac > 0) {
                rect.set(x, barTop, x + width * frac, barTop + barH);
                fill.setColor(frac > 0.5f ? Color.rgb(76, 200, 110) : frac > 0.2f ? Color.rgb(240, 196, 60) : Color.rgb(230, 80, 70));
                fill.setAlpha(alpha);
                c.drawRoundRect(rect, barH / 2, barH / 2, fill);
                fill.setAlpha(255);
            }
            text.setTextSize(height * 0.12f);
            text.setColor(Color.rgb(200, 206, 222));
            text.setAlpha(alpha);
            c.drawText("HP " + m.hp + " / " + m.maxHp, x, barTop + barH + height * 0.17f, text);
            text.setAlpha(255);
        }

        /** Draws one move row; returns false for an empty move slot. */
        private boolean drawMove(Canvas c, String button, String move, String pp, float left, float top, float right, float height, boolean flash) {
            if (move == null || move.isEmpty()) return false;
            final float round = height * 0.15f;
            rect.set(left, top, right, top + height);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(flash ? Color.rgb(80, 110, 170) : Color.rgb(30, 38, 58));
            c.drawRoundRect(rect, round, round, fill);

            // Button chip.
            final float chip = height * 0.62f;
            final float inset = (height - chip) / 2;
            rect.set(left + inset, top + inset, left + inset + chip * 1.25f, top + inset + chip);
            fill.setColor(Color.rgb(250, 204, 40));
            c.drawRoundRect(rect, chip / 2, chip / 2, fill);
            text.setTextSize(chip * 0.5f);
            text.setColor(Color.rgb(30, 30, 30));
            text.setTextAlign(Paint.Align.CENTER);
            c.drawText(button, rect.centerX(), rect.centerY() + chip * 0.18f, text);

            // PP on the right, then the move name in the space left between.
            final float nameLeft = rect.right + inset;
            final String ppText = "PP " + pp;
            text.setTextAlign(Paint.Align.RIGHT);
            text.setTextSize(height * 0.19f);
            final float ppWidth = text.measureText(ppText);
            text.setColor("0".equals(pp) ? Color.rgb(230, 80, 70) : Color.rgb(170, 180, 205));
            c.drawText(ppText, right - inset, top + height / 2 + height * 0.07f, text);
            text.setTextAlign(Paint.Align.LEFT);
            fit(move, height * 0.22f, right - inset - ppWidth - inset - nameLeft);
            text.setColor(Color.WHITE);
            c.drawText(move, nameLeft, top + height / 2 + height * 0.08f, text);
            return true;
        }

        private static int statusColor(String status) {
            switch (status) {
                case "PSN": return Color.rgb(190, 110, 230);
                case "BRN": return Color.rgb(255, 140, 70);
                case "FRZ": return Color.rgb(120, 200, 255);
                case "PAR": return Color.rgb(250, 220, 70);
                case "SLP": return Color.rgb(170, 175, 200);
                default: return Color.rgb(200, 206, 222);
            }
        }
    }
}
