import javax.microedition.lcdui.*;
import javax.microedition.midlet.*;

/**
 * A simple test MIDlet for PS Vita
 */
public class HelloMIDlet extends MIDlet implements CommandListener {
    private Display display;
    private Form form;
    private Command exitCommand;

    public HelloMIDlet() {
        display = Display.getDisplay(this);
        exitCommand = new Command("Exit", Command.EXIT, 0);
        form = new Form("Hello MIDlet");
        form.append("Hello from J2ME on PS Vita!");
        form.addCommand(exitCommand);
        form.setCommandListener(this);
    }

    public void startApp() {
        display.setCurrent(form);
    }

    public void pauseApp() {}

    public void destroyApp(boolean unconditional) {}

    public void commandAction(Command c, Displayable d) {
        if (c == exitCommand) {
            destroyApp(true);
            notifyDestroyed();
        }
    }
}
