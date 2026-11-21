package io.github.bearfeet0511.stadiumrecomp;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The first screen: pick the Stadium ROM and each player's Transfer Pak cart and
 * save, change settings, then Play. Everything is written to launcher.cfg and
 * rom.cfg in the app folder, which the game reads at startup (the same keys as
 * the PC launcher). Files picked with Android's file picker are copied into the
 * app folder, because the game needs real file paths.
 */
public class LauncherActivity extends Activity {
    private static final String TAG = "StadiumRecomp";
    private static final String ROM_MD5 = "ed1378bc12115f71209a77844965ba50"; // Pokemon Stadium (US v1.0)

    private static final int PICK_ROM = 1;
    private static final int PICK_CART = 10;   // + player index
    private static final int PICK_SAVE = 20;   // + player index
    private static final int EXPORT_SAVE = 30; // + player index

    private final Handler ui = new Handler(Looper.getMainLooper());
    private File dir;
    private final Map<String, String> cfg = new LinkedHashMap<>();

    private TextView romStatus, message;
    private final TextView[] cartStatus = new TextView[2];
    private final TextView[] saveStatus = new TextView[2];
    private Button playButton;
    private BattlePanel panel;
    private boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dir = getExternalFilesDir(null);
        readConfig();
        setContentView(buildUi());
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        showPanel();
    }

    @Override
    protected void onPause() {
        if (panel != null) {
            panel.dismiss();
            panel = null;
        }
        super.onPause();
    }

    // ---- UI -----------------------------------------------------------------

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(16, 22, 38));
        scroll.setFillViewport(true);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(24), dp(16), dp(24), dp(24));
        FrameLayoutCompat.center(scroll, column, dp(720));

        TextView title = text("Stadium Recomp", 26, Color.WHITE, true);
        column.addView(title);
        column.addView(text("Version " + versionName(), 13, Color.rgb(150, 160, 185), false));

        column.addView(header("Game"));
        romStatus = text("", 15, Color.rgb(200, 206, 222), false);
        column.addView(romStatus);
        column.addView(button("Choose Pokémon Stadium ROM (US v1.0)", v -> pick(PICK_ROM)));

        for (int p = 0; p < 2; p++) {
            final int player = p;
            column.addView(header("Transfer Pak – Player " + (p + 1)));
            cartStatus[p] = text("", 15, Color.rgb(200, 206, 222), false);
            column.addView(cartStatus[p]);
            LinearLayout cartRow = row();
            cartRow.addView(button("Choose Game Boy ROM", v -> pick(PICK_CART + player)), weight());
            cartRow.addView(button("Remove cart", v -> removeCart(player)), weight());
            column.addView(cartRow);
            saveStatus[p] = text("", 15, Color.rgb(200, 206, 222), false);
            column.addView(saveStatus[p]);
            LinearLayout saveRow = row();
            saveRow.addView(button("Choose save (.sav)", v -> pick(PICK_SAVE + player)), weight());
            saveRow.addView(button("Export save", v -> exportSave(player)), weight());
            column.addView(saveRow);
        }

        column.addView(header("Settings"));
        column.addView(toggle("Widescreen battles (experimental)", "Menus stay 4:3; a few battle effects may not fill the screen yet",
                "widescreen".equals(cfg.get("aspect_ratio")),
                (b, on) -> cfg.put("aspect_ratio", on ? "widescreen" : "original")));
        column.addView(toggle("60 fps (experimental)", "Adds in-between frames; brief glitches on fast motion",
                "60".equals(cfg.get("framerate")),
                (b, on) -> cfg.put("framerate", on ? "60" : "30")));
        column.addView(toggle("Battle panel (dual-screen devices)", "Moves, team and tap controls on the second screen, e.g. the AYN Thor's bottom screen",
                !"off".equals(cfg.get("bottom_screen")),
                (b, on) -> cfg.put("bottom_screen", on ? "on" : "off")));

        message = text("", 14, Color.rgb(250, 204, 40), false);
        message.setPadding(0, dp(12), 0, 0);
        column.addView(message);

        playButton = button("Play", v -> play());
        playButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(16);
        column.addView(playButton, lp);
        return scroll;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private TextView header(String s) {
        TextView t = text(s, 18, Color.rgb(120, 220, 255), true);
        t.setPadding(0, dp(20), 0, dp(4));
        return t;
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        return r;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(52), 1f);
        lp.setMargins(0, dp(4), dp(8), dp(4));
        return lp;
    }

    /** Buttons show a clear outline when a controller has focus on them. */
    private Button button(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setFocusable(true);
        b.setBackground(stateBackground(Color.rgb(44, 70, 120), Color.rgb(64, 100, 170)));
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(52));
        lp.setMargins(0, dp(4), 0, dp(4));
        b.setLayoutParams(lp);
        return b;
    }

    private Switch toggle(String label, String detail, boolean on, CompoundButton.OnCheckedChangeListener change) {
        Switch s = new Switch(this);
        s.setText(label + "\n" + detail);
        s.setTextColor(Color.WHITE);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        s.setChecked(on);
        s.setFocusable(true);
        s.setPadding(dp(12), dp(8), dp(12), dp(8));
        s.setBackground(stateBackground(Color.rgb(30, 38, 58), Color.rgb(50, 64, 96)));
        s.setOnCheckedChangeListener((b, value) -> {
            change.onCheckedChanged(b, value);
            writeConfig();
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(4), 0, dp(4));
        s.setLayoutParams(lp);
        return s;
    }

    private StateListDrawable stateBackground(int normal, int focused) {
        StateListDrawable states = new StateListDrawable();
        GradientDrawable f = new GradientDrawable();
        f.setColor(focused);
        f.setCornerRadius(dp(10));
        f.setStroke(dp(3), Color.rgb(255, 204, 64));
        GradientDrawable n = new GradientDrawable();
        n.setColor(normal);
        n.setCornerRadius(dp(10));
        states.addState(new int[] { android.R.attr.state_focused }, f);
        states.addState(new int[] { android.R.attr.state_pressed }, f);
        states.addState(new int[] {}, n);
        return states;
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    // Controller: A selects, B goes back, Start plays.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_A) {
            return super.dispatchKeyEvent(new KeyEvent(event.getDownTime(), event.getEventTime(), event.getAction(),
                    KeyEvent.KEYCODE_DPAD_CENTER, event.getRepeatCount(), event.getMetaState()));
        }
        if (code == KeyEvent.KEYCODE_BUTTON_START && event.getAction() == KeyEvent.ACTION_UP) {
            play();
            return true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_B && event.getAction() == KeyEvent.ACTION_UP) {
            finish();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    // ---- State --------------------------------------------------------------

    private File romFile() {
        File rom = new File(dir, "baserom.z64");
        return rom.exists() ? rom : null;
    }

    private void refresh() {
        File rom = romFile();
        romStatus.setText(rom != null ? "ROM: ready" : "ROM: not chosen yet");
        for (int p = 0; p < 2; p++) {
            String c = cfg.get("p" + (p + 1) + "_rom");
            String s = cfg.get("p" + (p + 1) + "_save");
            cartStatus[p].setText("Cart: " + (c != null && !c.isEmpty() && new File(dir, c).exists() ? new File(c).getName() : "none"));
            saveStatus[p].setText("Save: " + (s != null && !s.isEmpty() && new File(dir, s).exists() ? new File(s).getName() : "none"));
        }
        playButton.setEnabled(rom != null && !busy);
        playButton.setAlpha(playButton.isEnabled() ? 1f : 0.5f);
    }

    private void say(String s) {
        ui.post(() -> message.setText(s));
    }

    private void readConfig() {
        cfg.clear();
        File f = new File(dir, "launcher.cfg");
        if (!f.exists()) return;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (line.startsWith("#") || eq <= 0) continue;
                cfg.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException e) {
            Log.w(TAG, "launcher.cfg not readable", e);
        }
    }

    private void writeConfig() {
        try (PrintWriter w = new PrintWriter(new FileOutputStream(new File(dir, "launcher.cfg")))) {
            w.println("# Written by the Stadium Recomp launcher. Paths are relative to this folder.");
            for (Map.Entry<String, String> e : cfg.entrySet()) {
                w.println(e.getKey() + "=" + e.getValue());
            }
        } catch (IOException e) {
            say("Couldn't save settings: " + e.getMessage());
        }
    }

    private void play() {
        if (busy || romFile() == null) return;
        writeConfig();
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }

    // ---- Files --------------------------------------------------------------

    private void pick(int request) {
        if (busy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, request);
    }

    private void exportSave(int player) {
        String s = cfg.get("p" + (player + 1) + "_save");
        if (s == null || s.isEmpty() || !new File(dir, s).exists()) {
            say("Player " + (player + 1) + " has no save to export.");
            return;
        }
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/octet-stream");
        i.putExtra(Intent.EXTRA_TITLE, new File(s).getName());
        startActivityForResult(i, EXPORT_SAVE + player);
    }

    private void removeCart(int player) {
        String key = "p" + (player + 1);
        cfg.remove(key + "_rom");
        cfg.remove(key + "_save");
        writeConfig();
        refresh();
        say("Player " + (player + 1) + "'s Transfer Pak is empty (the files stay in the carts folder).");
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        busy = true;
        refresh();
        new Thread(() -> {
            try {
                if (request == PICK_ROM) importRom(uri);
                else if (request >= EXPORT_SAVE) exportSaveTo(request - EXPORT_SAVE, uri);
                else if (request >= PICK_SAVE) importSave(request - PICK_SAVE, uri);
                else if (request >= PICK_CART) importCart(request - PICK_CART, uri);
            } catch (Exception e) {
                Log.w(TAG, "file operation failed", e);
                say("That didn't work: " + e.getMessage());
            }
            ui.post(() -> {
                busy = false;
                writeConfig();
                refresh();
            });
        }).start();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        return "file";
    }

    private static String safeName(String name) {
        return name.replaceAll("[\\\\/:*?\"<>|]", "_");
    }

    private byte[] readAll(Uri uri) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("can't open the file");
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    /** Copies the ROM in as baserom.z64, fixing .n64/.v64 byte order, and checks it's US v1.0. */
    private void importRom(Uri uri) throws Exception {
        say("Checking the ROM…");
        byte[] rom = readAll(uri);
        if (rom.length < 0x1000) throw new IOException("that file is too small to be an N64 ROM");
        int b0 = rom[0] & 0xFF, b1 = rom[1] & 0xFF;
        if (b0 == 0x37 && b1 == 0x80) { // .v64: bytes swapped in pairs
            for (int i = 0; i + 1 < rom.length; i += 2) { byte t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
        } else if (b0 == 0x40 && b1 == 0x12) { // .n64: little-endian words
            for (int i = 0; i + 3 < rom.length; i += 4) {
                byte t0 = rom[i], t1 = rom[i + 1];
                rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = t1; rom[i + 3] = t0;
            }
        }
        String md5 = hex(MessageDigest.getInstance("MD5").digest(rom));
        if (!ROM_MD5.equals(md5)) {
            throw new IOException("this isn't Pokémon Stadium (US v1.0). Its MD5 is " + md5 + ", expected " + ROM_MD5);
        }
        File out = new File(dir, "baserom.z64");
        try (OutputStream o = new FileOutputStream(out)) {
            o.write(rom);
        }
        try (PrintWriter w = new PrintWriter(new FileOutputStream(new File(dir, "rom.cfg")))) {
            w.println(out.getAbsolutePath());
        }
        say("ROM ready.");
    }

    private void importCart(int player, Uri uri) throws Exception {
        byte[] gb = readAll(uri);
        if (gb.length < 0x150) throw new IOException("that file is too small to be a Game Boy ROM");
        String title = new String(gb, 0x134, 16, "US-ASCII").replace('\0', ' ').trim();
        File carts = new File(dir, "carts");
        carts.mkdirs();
        String name = safeName(displayName(uri));
        try (OutputStream o = new FileOutputStream(new File(carts, name))) {
            o.write(gb);
        }
        cfg.put("p" + (player + 1) + "_rom", "carts/" + name);
        say("Player " + (player + 1) + " cart: " + title + ". Choose its save next (or play with an empty one).");
    }

    /** Copies the save in. A save already there with that name is kept as a dated backup first. */
    private void importSave(int player, Uri uri) throws Exception {
        byte[] sav = readAll(uri);
        File carts = new File(dir, "carts");
        carts.mkdirs();
        String name = safeName(displayName(uri));
        File out = new File(carts, name);
        if (out.exists()) {
            File backups = new File(dir, "saves/backups");
            backups.mkdirs();
            String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new java.util.Date());
            copy(out, new File(backups, name + "." + stamp));
        }
        try (OutputStream o = new FileOutputStream(out)) {
            o.write(sav);
        }
        cfg.put("p" + (player + 1) + "_save", "carts/" + name);
        say("Player " + (player + 1) + " save: " + name + " (" + sav.length / 1024 + " KB).");
    }

    private void exportSaveTo(int player, Uri uri) throws Exception {
        File src = new File(dir, cfg.get("p" + (player + 1) + "_save"));
        try (InputStream in = new FileInputStream(src); OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
            if (out == null) throw new IOException("can't write there");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        say("Exported " + src.getName() + ".");
    }

    private static void copy(File from, File to) throws IOException {
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(String.format(Locale.US, "%02x", x & 0xFF));
        return s.toString();
    }

    // ---- Second screen ------------------------------------------------------

    /** The bottom screen shows the dimmed logo while the launcher is open. */
    private void showPanel() {
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display[] displays = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (displays.length == 0 || panel != null || romFile() == null) return;
        try {
            System.loadLibrary("SDL2");
            System.loadLibrary("PokemonStadiumRecomp");
            BattlePanel.nativeSetDataDir(dir.getAbsolutePath());
            panel = new BattlePanel(this, displays[0]);
            panel.show();
        } catch (Throwable t) {
            Log.w(TAG, "launcher panel not shown", t);
            panel = null;
        }
    }

    /** Centres a fixed-width column in the scroll view. */
    private static final class FrameLayoutCompat {
        static void center(ScrollView scroll, LinearLayout column, int maxWidth) {
            android.widget.FrameLayout frame = new android.widget.FrameLayout(scroll.getContext());
            android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(maxWidth, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.gravity = Gravity.CENTER_HORIZONTAL;
            frame.addView(column, lp);
            scroll.addView(frame);
        }
    }
}
