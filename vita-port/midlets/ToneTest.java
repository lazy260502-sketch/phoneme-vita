import javax.microedition.lcdui.*;
import javax.microedition.lcdui.game.*;
import javax.microedition.media.*;
import javax.microedition.media.control.*;
import javax.microedition.midlet.*;

/**
 * Tone/MMAPI smoke test v3 - SELF-DRIVING.
 *
 * v2 lesson: it waited for a CROSS press before doing anything, and a
 * frozen VM shows the same symptom as "key not working" - zero output.
 * v3 removes the human from the critical path:
 *
 *   t=0s     banner in constructor + startApp (stdout + screen)
 *   1 Hz     heartbeat counter on screen (stdout every 10th tick)
 *            proves the polling thread + event pump are alive
 *   t=2.5s   AUTO mode: stages advance on a 2.5s timer, no key needed.
 *            CROSS advances immediately, TRIANGLE exits.
 *
 * Stages:
 *   1  Manager.playTone(60,400,80)     -> tone player thread
 *   2  createPlayer("device://tone")   -> native create
 *   3  realize + prefetch
 *   4  ToneControl sequence + start    -> JTS delivery
 *   5  wait END_OF_MEDIA (10s timeout) -> event bridge check
 *
 * Every step logs "[TONE]" to stdout (midp_stdout.log) and paints the
 * log on screen. The last line names the blocked call if we hang.
 */
public class ToneTest extends MIDlet implements Runnable, PlayerListener {
    static final String VER = "v3";

    private Display display;
    private ToneCanvas canvas;
    private Thread loop;
    private boolean running;

    private Player player;
    private ToneControl tone;
    private int stage = 0;          /* current stage (0 = banner only) */
    private boolean busy = false;
    private boolean eom = false;
    private int hbCount = 0;        /* heartbeat ticks */
    private long t0;                /* startApp time */
    private long stageDeadline;     /* auto-advance when reached */

    private String[] log = new String[10];
    private int logn = 0;
    private int lastKey = 0;

    public ToneTest() {
        display = Display.getDisplay(this);
        canvas = new ToneCanvas();
        log("ctor " + VER);
    }

    public void startApp() {
        log("startApp " + VER);
        display.setCurrent(canvas);
        if (loop == null) {
            running = true;
            t0 = System.currentTimeMillis();
            stageDeadline = t0 + 2500; /* first auto stage at 2.5s */
            loop = new Thread(this);
            loop.start();
            log("loop started");
        }
    }

    public void pauseApp() { running = false; }

    public void destroyApp(boolean u) {
        running = false;
        closePlayer();
    }

    private void log(String s) {
        System.out.println("[TONE] " + s);
        if (logn == log.length) {
            for (int i = 1; i < logn; i++) log[i - 1] = log[i];
            logn--;
        }
        log[logn++] = s;
    }

    private void closePlayer() {
        if (player != null) {
            try { player.close(); } catch (Exception e) {}
            player = null;
            tone = null;
        }
    }

    private void tick() {
        int ks = canvas.getKeyStates();

        if ((ks & GameCanvas.GAME_A_PRESSED) != 0) {   /* TRIANGLE */
            log("exit key");
            destroyApp(true);
            notifyDestroyed();
            return;
        }

        boolean wantNext = (ks & GameCanvas.FIRE_PRESSED) != 0; /* CROSS */
        /* AUTO mode covers stages 0..3 only. Stage 4 (EOM wait) must
         * NEVER auto-skip: it ends only via EOM arrival or the 15s
         * watchdog in run() - otherwise the event-bridge test is
         * inconclusive (v01.19 lesson: 2.5s skip raced the ~2.25s
         * tone and proved nothing). */
        boolean timeout = (stage <= 3 &&
                           System.currentTimeMillis() >= stageDeadline);

        if ((!wantNext && !timeout) || busy) return;

        busy = true;
        try {
            doStage(stage);
        } finally {
            busy = false;
            stageDeadline = System.currentTimeMillis() + 2500;
        }
    }

    private void doStage(int st) {
        try {
            switch (st) {
            case 0:
                log("stg1: playTone(60,400,80)...");
                Manager.playTone(60, 400, 80);
                log("stg1: playTone returned");
                stage = 1;
                break;

            case 1:
                log("stg2: createPlayer(device://tone)...");
                player = Manager.createPlayer("device://tone");
                log("stg2: createPlayer OK");
                stage = 2;
                break;

            case 2:
                log("stg3: realize...");
                player.realize();
                log("stg3: realize OK, setSequence...");
                /* MMAPI: setSequence must happen in REALIZED state,
                 * i.e. AFTER realize but BEFORE prefetch (v3 lesson:
                 * calling it after prefetch throws ISE). */
                tone = (ToneControl) player.getControl("ToneControl");
                if (tone == null) {
                    log("stg3: NO ToneControl - stop");
                    stage = 5;
                    break;
                }
                byte[] seq = new byte[] {
                    ToneControl.VERSION, 1,
                    ToneControl.TEMPO, 30,
                    ToneControl.C4, 16,
                    (byte)64, 16,     /* E4 */
                    (byte)67, 16,     /* G4 */
                    ToneControl.C4, 16,
                    ToneControl.SILENCE, 8,
                };
                tone.setSequence(seq);
                log("stg3: setSequence OK, prefetch...");
                player.prefetch();
                log("stg3: prefetch OK");
                stage = 3;
                break;

            case 3:
                player.addPlayerListener(this);
                eom = false;
                log("stg4: start()...");
                player.start();
                log("stg4: start returned - wait EOM");
                stage = 4;
                break;

            case 4:
                /* manual skip of the EOM wait */
                log("stg5: skipped, eom=" + eom);
                closePlayer();
                stage = 5;
                break;

            default:
                log("all stages done " + VER);
                break;
            }
        } catch (MediaException me) {
            log("MediaException@" + st + ": " + me.getMessage());
        } catch (Exception e) {
            log("Exception@" + st + ": " + e);
        }
    }

    /* PlayerListener: END_OF_MEDIA arriving here proves the whole
     * native -> ring -> master-mode -> EventQueue -> listener bridge. */
    public void playerUpdate(Player p, String event, Object data) {
        if (PlayerListener.END_OF_MEDIA.equals(event)) {
            eom = true;
            log("stg5: EOM RECEIVED - bridge OK");
        } else {
            log("playerUpdate: " + event);
        }
    }

    public void run() {
        long lastHb = 0;
        while (running) {
            tick();

            long now = System.currentTimeMillis();

            /* 1 Hz heartbeat on screen; stdout every 10th so the log
             * file stays readable but liveness is provable. */
            if (now - lastHb >= 1000) {
                hbCount++;
                lastHb = now;
                if ((hbCount % 10) == 1) {
                    log("alive #" + hbCount);
                }
            }

            /* watchdog while waiting for EOM: give the bridge a real
             * 15s window (tone ~2.25s + margin) before declaring it
             * dead. CROSS still skips manually. */
            if (stage == 4) {
                long d = now - (stageDeadline - 2500);
                if (eom) {
                    log("stg5: EOM total wait " + d + "ms");
                    closePlayer();
                    stage = 5;
                } else if (d > 15000) {
                    log("stg5: STUCK - no EOM in 15s");
                    closePlayer();
                    stage = 5;
                }
            }

            /* global 60s watchdog */
            if (now - t0 > 60000) {
                log("60s watchdog exit");
                destroyApp(true);
                notifyDestroyed();
                return;
            }

            canvas.repaint();
            try { Thread.sleep(50); } catch (InterruptedException e) { return; }
        }
    }

    class ToneCanvas extends GameCanvas {
        ToneCanvas() { super(false); }

        protected void keyPressed(int keyCode) {
            lastKey = keyCode;
        }

        public void paint(Graphics g) {
            g.setColor(0x000000);
            g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0xFFFFFF);
            g.drawString("ToneTest " + VER + " stg=" + stage, 8, 8, 0);
            g.setColor(0x00FF00);
            g.drawString("hb=" + hbCount + " key=" + lastKey, 8, 28, 0);
            g.setColor(0xFFFF00);
            g.drawString("EOM: " + (eom ? "RECEIVED" : "-"), 8, 48, 0);
            g.setColor(0xFFFFFF);
            int y = 72;
            for (int i = 0; i < logn; i++) {
                g.drawString(log[i], 8, y, 0);
                y += 18;
            }
        }
    }
}
