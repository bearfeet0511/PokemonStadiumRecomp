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

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "PokemonStadiumRecomp" };
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
        super.onPause();
    }

    /** Shows the panel on the first presentation display, if there is one. */
    private void updateBattlePanel() {
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Display target = displays.length > 0 ? displays[0] : null;
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

    private void closeBattlePanel() {
        if (battlePanel != null) {
            battlePanel.dismiss();
            battlePanel = null;
        }
    }
}
