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
import android.view.WindowManager;

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

    static native void nativeSwitch(int slot);

    static native void nativeHold(int buttons);

    static native int[] nativeLogo();

    // Pick screen: the button that picks each team slot (fragment64), and L to undo.
    private static final int[] PICK_BITS = { 0x4000, 0x0002, 0x0008, 0x8000, 0x0004, 0x0001 };
    private static final int BUTTON_L = 0x0020;

    // A team card needs a second tap within this long to send the Pokemon out.
    private static final long CONFIRM_MS = 3000;

    /** Button icon colours as on the N64 controller: A blue, B green, C yellow, L/R grey. */
    static int chipColor(String label) {
        switch (label) {
            case "A": return Color.rgb(50, 110, 230);
            case "B": return Color.rgb(40, 170, 80);
            case "L":
            case "R": return Color.rgb(160, 165, 180);
            default: return Color.rgb(250, 204, 40);
        }
    }

    /** Label colour that reads on chipColor(label). */
    static int chipTextColor(String label) {
        return label.equals("A") || label.equals("B") ? Color.WHITE : Color.rgb(30, 30, 30);
    }

    /** Label for a send-out button mask from the native side. */
    static String buttonLabel(int bits) {
        switch (bits) {
            case 0x0008: return "C▲";
            case 0x0001: return "C▶";
            case 0x0004: return "C▼";
            case 0x0002: return "C◀";
            case 0x8000: return "A";
            case 0x4000: return "B";
            default: return "";
        }
    }

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
        // Touches still arrive, but key and controller focus stays with the game:
        // a focusable panel took the gamepad away from it on the first tap.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
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
        int slot = -1;      // team slot in pick order
        int switchBits;     // the button that sends it out, 0 if unknown

        boolean canSendOut() {
            return !active && !fainted && slot >= 0 && switchBits != 0;
        }
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
        // Where each team card was drawn, and the slot waiting for a confirming tap.
        private final List<RectF> cardRects = new ArrayList<>();
        private final List<Mon> cardMons = new ArrayList<>();
        private int armedSlot = -1;
        // Pick screen: { slot, name, level } per team slot, where each tile was drawn.
        private boolean pickScreen;
        private final List<String[]> picks = new ArrayList<>();
        private final List<RectF> pickRects = new ArrayList<>();
        private final List<Integer> pickBits = new ArrayList<>();
        private int flashPick = -1;
        private final Runnable disarm = () -> {
            armedSlot = -1;
            invalidate();
        };

        PanelView(Context context) {
            super(context);
            text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent event) {
            if (event.getActionMasked() != android.view.MotionEvent.ACTION_DOWN) {
                return true;
            }
            if (pickScreen) {
                for (int i = 0; i < pickRects.size(); i++) {
                    if (!pickRects.get(i).contains(event.getX(), event.getY())) continue;
                    try {
                        nativeHold(pickBits.get(i));
                    } catch (UnsatisfiedLinkError e) {
                        return true;
                    }
                    performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                    final int tapped = i;
                    flashPick = tapped;
                    invalidate();
                    postDelayed(() -> {
                        if (flashPick == tapped) flashPick = -1;
                        invalidate();
                    }, 150);
                    return true;
                }
                return true;
            }
            // Team cards: the first tap arms the card, a second tap sends it out.
            for (int i = 0; i < cardRects.size(); i++) {
                Mon m = cardMons.get(i);
                if (!cardRects.get(i).contains(event.getX(), event.getY())) continue;
                if (!m.canSendOut()) return true;
                removeCallbacks(disarm);
                if (armedSlot == m.slot) {
                    armedSlot = -1;
                    try {
                        nativeSwitch(m.slot);
                    } catch (UnsatisfiedLinkError e) {
                        return true;
                    }
                    performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                } else {
                    armedSlot = m.slot;
                    performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
                    postDelayed(disarm, CONFIRM_MS);
                }
                invalidate();
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

        // The panel's one text size, set at the start of each draw.
        private float font;

        void update(String snapshot) {
            if (snapshot.equals(last)) return;
            last = snapshot;
            mons.clear();
            picks.clear();
            inBattle = false;
            pickScreen = false;
            for (String line : snapshot.split("\n")) {
                String[] f = line.split("\t", -1);
                if (f[0].equals("pick")) {
                    pickScreen = f.length > 1 && f[1].equals("1");
                } else if (f[0].equals("slot") && f.length >= 4) {
                    picks.add(new String[] { f[1], f[2], f[3] });
                } else if (f[0].equals("battle")) {
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
                    if (f.length >= 18) {
                        m.slot = parse(f[16]);
                        m.switchBits = parse(f[17]);
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
            pickRects.clear();
            pickBits.clear();
            if (pickScreen) {
                for (int i = 0; i < 4; i++) moveTappable[i] = false;
                cardRects.clear();
                cardMons.clear();
                drawPickScreen(canvas, w, h);
                return;
            }
            if (!inBattle || mons.isEmpty()) {
                for (int i = 0; i < 4; i++) moveTappable[i] = false;
                cardRects.clear();
                cardMons.clear();
                drawTitle(canvas, w, h);
                return;
            }
            final float pad = Math.min(w, h) * 0.025f;
            final float split = w * 0.48f;
            final float cardH = Math.min((h - pad * (mons.size() + 1)) / mons.size(), h * 0.3f);
            final float moveH = (h - pad * 5) / 4;
            // One text size for the whole panel: two lines must fit the smaller box.
            font = Math.min(Math.min(cardH, moveH) * 0.3f, h * 0.045f);
            text.setTextSize(font);

            // Left: the team.
            Mon active = null;
            cardRects.clear();
            cardMons.clear();
            for (int i = 0; i < mons.size(); i++) {
                Mon m = mons.get(i);
                if (m.active) active = m;
                float top = pad + i * ((h - pad * (mons.size() + 1)) / mons.size() + pad);
                drawMon(canvas, m, pad, top, split - pad / 2, cardH);
                cardRects.add(new RectF(pad, top, split - pad / 2, top + cardH));
                cardMons.add(m);
            }

            // Right: the active Pokemon's moves.
            final float left = split + pad / 2, right = w - pad;
            for (int i = 0; i < 4; i++) moveTappable[i] = false;
            if (active == null) {
                text.setColor(Color.rgb(150, 160, 185));
                text.setTextAlign(Paint.Align.CENTER);
                canvas.drawText("Choosing...", (left + right) / 2, h / 2, text);
                text.setTextAlign(Paint.Align.LEFT);
                return;
            }
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
            final boolean armed = m.slot == armedSlot && m.canSendOut();
            if (armed) {
                fill.setStyle(Paint.Style.STROKE);
                fill.setStrokeWidth(Math.max(3f, height * 0.035f));
                fill.setColor(Color.rgb(120, 220, 255));
                c.drawRoundRect(rect, round, round, fill);
                fill.setStyle(Paint.Style.FILL);
            }
            final int alpha = m.fainted ? 110 : 255;
            final float inset = font * 0.6f;
            final float line1 = top + height * 0.42f;   // baselines of the two lines
            final float line2 = top + height * 0.82f;

            // Send-out button chip on the left (space kept on every card so names line up).
            final float chipH = font * 1.5f;
            final float chipW = chipH * 1.25f;
            rect.set(left + inset, top + (height - chipH) / 2, left + inset + chipW, top + (height + chipH) / 2);
            if (m.canSendOut() && !armed) { // armed: the centred prompt needs the room
                final String label = buttonLabel(m.switchBits);
                fill.setColor(chipColor(label));
                c.drawRoundRect(rect, chipH / 2, chipH / 2, fill);
                text.setColor(chipTextColor(label));
                text.setTextAlign(Paint.Align.CENTER);
                c.drawText(label, rect.centerX(), rect.centerY() + font * 0.35f, text);
                text.setTextAlign(Paint.Align.LEFT);
            }
            final float x = rect.right + inset;
            final float width = right - inset - x;

            // Line 1: name, and level or status on the right.
            String tag = m.fainted ? "FNT" : m.status.isEmpty() ? "Lv " + m.level : m.status;
            float tagWidth = text.measureText(tag);
            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(m.fainted ? Color.rgb(230, 90, 90) : statusColor(m.status));
            text.setAlpha(alpha);
            c.drawText(tag, right - inset, line1, text);
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(Color.WHITE);
            text.setAlpha(alpha);
            c.drawText(ellipsize(m.name, width - tagWidth - inset), x, line1, text);

            if (armed) {
                // Centred on the whole card so it reads in full.
                text.setColor(Color.rgb(120, 220, 255));
                text.setTextAlign(Paint.Align.CENTER);
                c.drawText(ellipsize("Tap again to switch", right - left - inset * 2), (left + right) / 2, line2, text);
                text.setTextAlign(Paint.Align.LEFT);
                return;
            }

            // Line 2: HP numbers on the right, the bar in the space before them.
            String hpText = m.hp + "/" + m.maxHp;
            float hpWidth = text.measureText(hpText);
            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(Color.rgb(200, 206, 222));
            text.setAlpha(alpha);
            c.drawText(hpText, right - inset, line2, text);
            text.setTextAlign(Paint.Align.LEFT);
            float barH = font * 0.5f;
            float barTop = line2 - font * 0.35f - barH / 2;
            final float barWidth = width - hpWidth - inset;
            rect.set(x, barTop, x + barWidth, barTop + barH);
            fill.setColor(Color.rgb(60, 66, 84));
            c.drawRoundRect(rect, barH / 2, barH / 2, fill);
            float frac = m.maxHp > 0 ? Math.max(0f, Math.min(1f, m.hp / (float) m.maxHp)) : 0f;
            if (frac > 0) {
                rect.set(x, barTop, x + barWidth * frac, barTop + barH);
                fill.setColor(frac > 0.5f ? Color.rgb(76, 200, 110) : frac > 0.2f ? Color.rgb(240, 196, 60) : Color.rgb(230, 80, 70));
                fill.setAlpha(alpha);
                c.drawRoundRect(rect, barH / 2, barH / 2, fill);
                fill.setAlpha(255);
            }
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
            text.setColor(Color.rgb(30, 30, 30));
            text.setTextAlign(Paint.Align.CENTER);
            c.drawText(button, rect.centerX(), rect.centerY() + font * 0.35f, text);
            text.setTextAlign(Paint.Align.LEFT);

            // Line 1: the move name; line 2: its PP.
            final float nameLeft = rect.right + inset;
            final float nameWidth = right - inset - nameLeft;
            text.setColor(Color.WHITE);
            c.drawText(ellipsize(move, nameWidth), nameLeft, top + height * 0.44f, text);
            text.setColor("0".equals(pp) ? Color.rgb(230, 80, 70) : Color.rgb(170, 180, 205));
            c.drawText("PP " + pp, nameLeft, top + height * 0.82f, text);
            return true;
        }

        // The Stadium logo from the player's ROM, shown when there's nothing else.
        private static final int LOGO_W = 100, LOGO_H = 56;
        private android.graphics.Bitmap logo;
        private boolean logoTried;
        private final Paint bitmapPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

        private void drawTitle(Canvas c, float w, float h) {
            if (!logoTried) {
                try {
                    int[] argb = nativeLogo();
                    if (argb != null && argb.length == LOGO_W * LOGO_H) {
                        logo = android.graphics.Bitmap.createBitmap(argb, LOGO_W, LOGO_H, android.graphics.Bitmap.Config.ARGB_8888);
                    }
                    logoTried = true;
                } catch (UnsatisfiedLinkError e) {
                    return; // native library not loaded yet: try again next draw
                }
            }
            // Pure black (OLED pixels off) with a dimmed logo, like single-screen
            // games on dual-screen handhelds: easy on the eyes and the battery.
            c.drawColor(Color.BLACK);
            if (logo == null) return;
            bitmapPaint.setAlpha(90);
            // Centred, half the width (or the height, if that's tighter), keeping its shape.
            float scale = Math.min(w * 0.5f / LOGO_W, h * 0.5f / LOGO_H);
            float dw = LOGO_W * scale, dh = LOGO_H * scale;
            rect.set((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2);
            c.drawBitmap(logo, null, rect, bitmapPaint);
        }

        /** Pick screen: a tile per team slot (3 x 2, slot order) and an L "Undo" bar. */
        private void drawPickScreen(Canvas c, float w, float h) {
            final float pad = Math.min(w, h) * 0.025f;
            final float undoH = h * 0.15f;
            final float tileW = (w - pad * 4) / 3;
            final float tileH = (h - undoH - pad * 4) / 2;
            font = Math.min(h * 0.045f, tileW * 0.11f);
            text.setTextSize(font);
            for (String[] p : picks) {
                int slot = parse(p[0]);
                if (slot < 0 || slot >= PICK_BITS.length) continue;
                float left = pad + (slot % 3) * (tileW + pad);
                float top = pad + (slot / 3) * (tileH + pad);
                RectF tile = new RectF(left, top, left + tileW, top + tileH);
                drawPickTile(c, tile, buttonLabel(PICK_BITS[slot]), p[1], "Lv " + p[2], pickRects.size() == flashPick);
                pickRects.add(tile);
                pickBits.add(PICK_BITS[slot]);
            }
            float top = h - pad - undoH;
            RectF undo = new RectF(pad, top, w - pad, top + undoH);
            drawPickTile(c, undo, "L", "Undo last pick", null, pickRects.size() == flashPick);
            pickRects.add(undo);
            pickBits.add(BUTTON_L);
        }

        private void drawPickTile(Canvas c, RectF tile, String button, String line1, String line2, boolean flash) {
            final float round = Math.min(tile.width(), tile.height()) * 0.1f;
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(flash ? Color.rgb(80, 110, 170) : Color.rgb(30, 38, 58));
            c.drawRoundRect(tile, round, round, fill);
            final float chipH = font * 1.6f;
            final float chipW = chipH * 1.4f;
            final float cx = tile.centerX();
            text.setTextAlign(Paint.Align.CENTER);
            if (line2 == null) {
                // One-line bar: chip on the left of the text.
                float textW = text.measureText(line1);
                float startX = cx - (chipW + font * 0.6f + textW) / 2;
                rect.set(startX, tile.centerY() - chipH / 2, startX + chipW, tile.centerY() + chipH / 2);
                fill.setColor(chipColor(button));
                c.drawRoundRect(rect, chipH / 2, chipH / 2, fill);
                text.setColor(chipTextColor(button));
                c.drawText(button, rect.centerX(), rect.centerY() + font * 0.35f, text);
                text.setTextAlign(Paint.Align.LEFT);
                text.setColor(Color.WHITE);
                c.drawText(line1, rect.right + font * 0.6f, tile.centerY() + font * 0.35f, text);
                return;
            }
            float chipTop = tile.top + tile.height() * 0.2f;
            rect.set(cx - chipW / 2, chipTop, cx + chipW / 2, chipTop + chipH);
            fill.setColor(chipColor(button));
            c.drawRoundRect(rect, chipH / 2, chipH / 2, fill);
            text.setColor(chipTextColor(button));
            c.drawText(button, cx, rect.centerY() + font * 0.35f, text);
            text.setColor(Color.WHITE);
            c.drawText(ellipsize(line1, tile.width() - font), cx, tile.top + tile.height() * 0.66f, text);
            text.setColor(Color.rgb(170, 180, 205));
            c.drawText(line2, cx, tile.top + tile.height() * 0.84f, text);
            text.setTextAlign(Paint.Align.LEFT);
        }

        /** Cuts text to fit width at the current size, ending with an ellipsis. */
        private String ellipsize(String s, float width) {
            if (text.measureText(s) <= width) return s;
            String cut = s;
            while (cut.length() > 1 && text.measureText(cut + "…") > width) {
                cut = cut.substring(0, cut.length() - 1);
            }
            return cut + "…";
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
