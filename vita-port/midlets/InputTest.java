import javax.microedition.lcdui.*;
import javax.microedition.lcdui.game.*;
import javax.microedition.midlet.*;

/**
 * Input smoke test. GameCanvas loop moves a square with the D-pad and
 * prints the last raw key code + game action on screen. Verifies the
 * checkForSystemSignal -> MIDP_KEY_EVENT -> EventQueue -> keyPressed chain.
 */
public class InputTest extends MIDlet implements Runnable {
    private Display display;
    private InputCanvas canvas;
    private Thread loop;
    private boolean running;

    public InputTest() {
        display = Display.getDisplay(this);
        canvas = new InputCanvas();
    }

    public void startApp() {
        display.setCurrent(canvas);
        running = true;
        loop = new Thread(this);
        loop.start();
    }

    public void pauseApp() {
        running = false;
    }

    public void destroyApp(boolean unconditional) {
        running = false;
    }

    public void run() {
        while (running) {
            canvas.tick();
            canvas.repaint();
            try {
                Thread.sleep(33);
            } catch (InterruptedException e) {}
        }
    }

    static class InputCanvas extends GameCanvas {
        private int x = 100, y = 140;
        private String lastKey = "-";

        InputCanvas() {
            super(false); /* deliver key events too */
        }

        void tick() {
            int ks = getKeyStates();
            if ((ks & UP_PRESSED) != 0) y -= 4;
            if ((ks & DOWN_PRESSED) != 0) y += 4;
            if ((ks & LEFT_PRESSED) != 0) x -= 4;
            if ((ks & RIGHT_PRESSED) != 0) x += 4;
        }

        public void paint(Graphics g) {
            int w = getWidth();
            int h = getHeight();
            g.setGrayScale(255);
            g.fillRect(0, 0, w, h);

            g.setColor(0x0000AA);
            g.fillRect(x, y, 40, 40);

            g.setColor(0x000000);
            g.drawString("InputTest", 10, 10, Graphics.TOP | Graphics.LEFT);
            g.drawString("last key: " + lastKey, 10, 35,
                         Graphics.TOP | Graphics.LEFT);
            g.drawString("keyStates: 0x" + Integer.toHexString(getKeyStates()),
                         10, 60, Graphics.TOP | Graphics.LEFT);
        }

        protected void keyPressed(int keyCode) {
            lastKey = String.valueOf(keyCode);
        }

        protected void keyReleased(int keyCode) {
            lastKey = keyCode + " rel";
        }
    }
}
