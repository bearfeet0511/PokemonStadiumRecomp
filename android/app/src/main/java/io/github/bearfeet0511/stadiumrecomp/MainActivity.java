package io.github.bearfeet0511.stadiumrecomp;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the recompiled game. SDLActivity loads these libraries, starts its own
 * thread, and calls SDL_main inside the last one. If the device has a second
 * screen (like the AYN Thor), battle info is shown on it.
 */
public class MainActivity extends SDLActivity {
    private static final String TAG = "StadiumRecomp";
    private BattlePanel battlePanel;
    private DisplayManager displayManager;

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int displayId) {
            updateBattlePanel();
        }

        @Override
        public void onDisplayRemoved(int displayId) {
            updateBattlePanel();
        }

        @Override
        public void onDisplayChanged(int displayId) {
        }
    };

    // Writes Transfer Pak save changes back to the players' own .sav files as they happen.
    private SaveSync.Watcher saveSync;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "PokemonStadiumRecomp" };
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        java.io.File dir = getExternalFilesDir(null);
        if (dir != null) saveSync = new SaveSync.Watcher(this, dir, readConfig(dir));
    }

    @Override
    protected void onDestroy() {
        if (saveSync != null) {
            saveSync.stop();
            saveSync = null;
        }
        super.onDestroy();
    }

    private static java.util.Map<String, String> readConfig(java.io.File dir) {
        java.util.Map<String, String> cfg = new java.util.HashMap<>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(new java.io.File(dir, "launcher.cfg")))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (!line.startsWith("#") && eq > 0) cfg.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (java.io.IOException ignored) {
        }
        return cfg;
    }

    @Override
    protected void onResume() {
        super.onResume();
        displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        displayManager.registerDisplayListener(displayListener, new Handler(Looper.getMainLooper()));
        updateBattlePanel();
    }

    @Override
    protected void onPause() {
        if (displayManager != null) {
            displayManager.unregisterDisplayListener(displayListener);
        }
        closeBattlePanel();
        if (saveSync != null) saveSync.flush();
        super.onPause();
    }

    /** Shows the panel on the first presentation display, if there is one. */
    private void updateBattlePanel() {
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Display target = displays.length > 0 ? displays[0] : null;
        // Off when the launcher's "Bottom-screen battle panel" setting is off
        // (launcher.cfg bottom_screen=off), or with a no_panel test file.
        java.io.File dir = getExternalFilesDir(null);
        if (dir != null && (new java.io.File(dir, "no_panel").exists() || panelSettingOff(dir))) target = null;
        Log.i(TAG, "presentation displays: " + displays.length + (target != null ? " -> " + target.getDisplayId() + " " + target.getName() : ""));
        if (battlePanel != null && (target == null || battlePanel.getDisplay().getDisplayId() != target.getDisplayId())) {
            closeBattlePanel();
        }
        if (battlePanel == null && target != null) {
            battlePanel = new BattlePanel(this, target);
            try {
                battlePanel.show();
            } catch (RuntimeException e) {
                Log.w(TAG, "battle panel could not be shown", e);
                battlePanel = null; // display went away, or the window was refused
            }
        }
    }

    private static boolean panelSettingOff(java.io.File dir) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(new java.io.File(dir, "launcher.cfg")))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.trim().equals("bottom_screen=off")) return true;
            }
        } catch (java.io.IOException ignored) {
        }
        return false;
    }

    private void closeBattlePanel() {
        if (battlePanel != null) {
            battlePanel.dismiss();
            battlePanel = null;
        }
    }
}
