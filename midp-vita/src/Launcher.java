import javax.microedition.midlet.*;
import com.sun.midp.main.MIDletSuiteLoader;

/**
 * Native entry point for the J2ME emulator on PS Vita.
 *
 * Called by midpRunMainClass() in main.c with classpath
 * "midp_system.jar:Hello.jar" and mainClass="Launcher".
 *
 * The launcher:
 *   1. Hands off to MIDletSuiteLoader which initializes the AMS event
 *      pump and the per-suite isolate plumbing (required for any
 *      MIDlet to receive startApp, paint, and input events).
 *   2. MIDletSuiteLoader then loads and starts HelloMIDlet via the
 *      standard suite path; HelloMIDlet.destroyApp() ends the suite
 *      and MIDletSuiteLoader.main() returns, terminating the VM.
 *
 * This is the pattern used by phoneME's own CommandLineRunner
 * ("com.sun.midp.installer.CommandLineRunner"): start MIDletSuiteLoader
 * with the user JAR appended to the boot classpath.
 */
public class Launcher {
    public static void main(String[] args) {
        try {
            // Forward every arg to MIDletSuiteLoader which parses
            // them the same way runMidlet would.
            MIDletSuiteLoader.main(args);
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }
}
