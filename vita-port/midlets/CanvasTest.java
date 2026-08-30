import javax.microedition.lcdui.*;
import javax.microedition.midlet.*;

/**
 * Rendering smoke test: primitive shapes + offscreen image blit.
 * Expects: red/green/blue bars, a circle outline, diagonal line and a
 * 32x32 image blit — all inside the 240x320 virtual screen.
 */
public class CanvasTest extends MIDlet {
    private Display display;
    private TestCanvas canvas;

    public CanvasTest() {
        display = Display.getDisplay(this);
        canvas = new TestCanvas();
    }

    public void startApp() {
        display.setCurrent(canvas);
    }

    public void pauseApp() {}

    public void destroyApp(boolean unconditional) {}

    static class TestCanvas extends Canvas {
        public void paint(Graphics g) {
            int w = getWidth();
            int h = getHeight();

            g.setGrayScale(255);
            g.fillRect(0, 0, w, h);

            g.setColor(0xFF0000);
            g.fillRect(10, 10, w - 20, 30);
            g.setColor(0x00FF00);
            g.fillRect(10, 50, w - 20, 30);
            g.setColor(0x0000FF);
            g.fillRect(10, 90, w - 20, 30);

            g.setColor(0x000000);
            g.drawLine(10, 130, w - 10, 180);
            g.drawArc(20, 190, 60, 60, 0, 360);
            g.fillArc(100, 190, 40, 40, 0, 360);

            /* Offscreen image + drawImage (blit path) */
            Image img = Image.createImage(32, 32);
            Graphics ig = img.getGraphics();
            ig.setColor(0xFF00FF);
            ig.fillRect(0, 0, 32, 32);
            ig.setColor(0xFFFF00);
            ig.fillRect(8, 8, 16, 16);
            g.drawImage(img, 170, 190, Graphics.TOP | Graphics.LEFT);

            g.setColor(0x000000);
            g.drawString("CanvasTest " + w + "x" + h, 10, 250,
                         Graphics.TOP | Graphics.LEFT);
        }
    }
}
