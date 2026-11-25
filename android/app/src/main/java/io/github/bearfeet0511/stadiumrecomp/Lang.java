package io.github.bearfeet0511.stadiumrecomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

/**
 * Text shown by our own screens (launcher, battle panel) in the language chosen
 * in the launcher (launcher.cfg language=en|es|fr). Spanish has translations;
 * French game text is supported, but our own screens stay in English for now.
 */
final class Lang {
    private static volatile boolean spanish;

    private Lang() {
    }

    /** Sets the language from launcher.cfg's value. */
    static void set(String code) {
        spanish = "es".equals(code);
    }

    /** Reads the language from launcher.cfg in the app folder. */
    static void load(File dir) {
        String code = "en";
        try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, "launcher.cfg")))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.startsWith("language=")) code = line.substring(9).trim();
            }
        } catch (IOException ignored) {
        }
        set(code);
    }

    static String tr(String english, String spanishText) {
        return spanish ? spanishText : english;
    }
}
