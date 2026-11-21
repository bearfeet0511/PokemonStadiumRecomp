package io.github.bearfeet0511.stadiumrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.FileObserver;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

/**
 * Keeps each player's Game Boy save in step with the player's own .sav file.
 *
 * The game needs a real file path, so it plays from a working copy in the app
 * folder (launcher.cfg pN_save). The launcher links the file the player chose
 * (pN_save_uri, a persistable content URI). Before a session, newer changes in
 * the original are pulled in; during the session every write to the working
 * copy is pushed back to the original within about a second. So a trade in
 * the Pokémon Lab lands in the player's real save, with no export step.
 *
 * saves/sync.properties keeps the checksum each pair last agreed on, which
 * tells which side changed. If both changed, the working copy is kept as a
 * backup and the original wins.
 */
final class SaveSync {
    private static final String TAG = "StadiumRecomp";
    private static final long PUSH_DELAY_MS = 1000;

    private SaveSync() {
    }

    // ---- State ----------------------------------------------------------------

    private static File stateFile(File dir) {
        return new File(dir, "saves/sync.properties");
    }

    private static synchronized Properties loadState(File dir) {
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(stateFile(dir))) {
            p.load(in);
        } catch (IOException ignored) {
        }
        return p;
    }

    private static synchronized void saveState(File dir, String key, String hash) {
        Properties p = loadState(dir);
        p.setProperty(key, hash);
        stateFile(dir).getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(stateFile(dir))) {
            p.store(out, "Last checksum each linked save agreed on (see SaveSync.java)");
        } catch (IOException e) {
            Log.w(TAG, "save sync state not written", e);
        }
    }

    /** Records that the working copy and the original are now the same data. */
    static void markInSync(File dir, int player, byte[] data) {
        saveState(dir, "p" + player, sha256(data));
    }

    // ---- Before a session -------------------------------------------------------

    /**
     * Brings each linked save pair into step before the game starts. Returns
     * warnings to show the player (empty if all went well).
     */
    static List<String> syncBeforePlay(Context context, File dir, Map<String, String> cfg) {
        List<String> warnings = new ArrayList<>();
        Properties state = loadState(dir);
        for (int p = 1; p <= 2; p++) {
            String uriText = cfg.get("p" + p + "_save_uri");
            String localPath = cfg.get("p" + p + "_save");
            if (uriText == null || uriText.isEmpty() || localPath == null || localPath.isEmpty()) continue;
            File local = new File(dir, localPath);
            Uri uri = Uri.parse(uriText);
            try {
                byte[] source = read(context, uri);
                byte[] mine = local.exists() ? readFile(local) : null;
                String last = state.getProperty("p" + p, "");
                String hs = sha256(source);
                String hl = mine != null ? sha256(mine) : "";
                if (hs.equals(hl)) {
                    saveState(dir, "p" + p, hs);
                } else if (mine == null || hl.equals(last)) {
                    // Only the original changed (e.g. played in an emulator): use it.
                    writeFile(local, source);
                    saveState(dir, "p" + p, hs);
                    Log.i(TAG, "player " + p + " save: pulled newer changes from the original");
                } else if (hs.equals(last)) {
                    // Only the working copy changed (written while the original was unreachable): send it.
                    write(context, uri, mine);
                    saveState(dir, "p" + p, hl);
                    Log.i(TAG, "player " + p + " save: sent unsynced Stadium changes to the original");
                } else {
                    File backups = new File(dir, "saves/backups");
                    backups.mkdirs();
                    File keep = new File(backups, local.getName() + ".conflict-" + stamp());
                    writeFile(keep, mine);
                    writeFile(local, source);
                    saveState(dir, "p" + p, hs);
                    warnings.add("Player " + p + "'s save changed both in Stadium and elsewhere. Using the original; "
                            + "Stadium's version was kept as saves/backups/" + keep.getName() + ".");
                }
            } catch (SecurityException | IOException e) {
                Log.w(TAG, "player " + p + " save sync failed", e);
                warnings.add("Player " + p + "'s save couldn't be reached (" + e.getMessage()
                        + "). Choose it again in the launcher so changes keep going back to it.");
            }
        }
        return warnings;
    }

    // ---- During a session ---------------------------------------------------------

    /** Watches the working copies while the game runs and pushes each change to the original. */
    static final class Watcher {
        private final Context context;
        private final File dir;
        private final HandlerThread thread = new HandlerThread("SaveSync");
        private final Handler handler;
        private final List<FileObserver> observers = new ArrayList<>();
        private final List<Runnable> pushes = new ArrayList<>();

        Watcher(Context context, File dir, Map<String, String> cfg) {
            this.context = context.getApplicationContext();
            this.dir = dir;
            thread.start();
            handler = new Handler(thread.getLooper());
            for (int p = 1; p <= 2; p++) {
                String uriText = cfg.get("p" + p + "_save_uri");
                String localPath = cfg.get("p" + p + "_save");
                if (uriText == null || uriText.isEmpty() || localPath == null || localPath.isEmpty()) continue;
                final int player = p;
                final File local = new File(dir, localPath);
                final Uri uri = Uri.parse(uriText);
                final Runnable push = () -> push(player, local, uri);
                pushes.add(push);
                // The game rewrites the whole .sav on each Transfer Pak write; wait for
                // a quiet second, then send it.
                FileObserver observer = new FileObserver(local.getParent(), FileObserver.CLOSE_WRITE | FileObserver.MODIFY) {
                    @Override
                    public void onEvent(int event, String path) {
                        if (local.getName().equals(path)) {
                            handler.removeCallbacks(push);
                            handler.postDelayed(push, PUSH_DELAY_MS);
                        }
                    }
                };
                observer.startWatching();
                observers.add(observer);
                Log.i(TAG, "player " + p + " save: writing changes back to " + uri);
            }
        }

        private void push(int player, File local, Uri uri) {
            try {
                byte[] data = readFile(local);
                if (data.length == 0) return; // mid-rewrite; the next event pushes it
                String hash = sha256(data);
                if (hash.equals(loadState(dir).getProperty("p" + player, ""))) return;
                write(context, uri, data);
                saveState(dir, "p" + player, hash);
                Log.i(TAG, "player " + player + " save: change written back");
            } catch (SecurityException | IOException e) {
                // Left unsynced; the launcher sends it next time (see syncBeforePlay).
                Log.w(TAG, "player " + player + " save: write-back failed", e);
            }
        }

        /** Sends anything pending right away (the game is closing or going to the background). */
        void flush() {
            for (Runnable push : pushes) {
                handler.removeCallbacks(push);
                handler.post(push);
            }
        }

        void stop() {
            for (FileObserver o : observers) o.stopWatching();
            flush();
            thread.quitSafely();
        }
    }

    // ---- Helpers --------------------------------------------------------------------

    static byte[] read(Context context, Uri uri) throws IOException {
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("can't open it");
            return readAll(in);
        }
    }

    static void write(Context context, Uri uri, byte[] data) throws IOException {
        try (OutputStream out = context.getContentResolver().openOutputStream(uri, "wt")) {
            if (out == null) throw new IOException("can't write to it");
            out.write(data);
        }
    }

    static byte[] readFile(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            return readAll(in);
        }
    }

    static void writeFile(File f, byte[] data) throws IOException {
        f.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(data);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 15];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    static String sha256(byte[] data) {
        try {
            StringBuilder s = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) s.append(String.format(Locale.US, "%02x", b & 0xFF));
            return s.toString();
        } catch (Exception e) {
            return "";
        }
    }

    private static String stamp() {
        return new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new java.util.Date());
    }
}
