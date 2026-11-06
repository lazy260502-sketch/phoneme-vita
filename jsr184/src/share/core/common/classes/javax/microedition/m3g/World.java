package javax.microedition.m3g;

import java.util.Vector;

/**
 * Root of the scene graph: the active camera, the light set and the
 * background all live here. Group's children form the drawn content.
 */
public class World extends Group {

    private Camera activeCamera = null;
    private Background background = null;
    private final Vector lights = new Vector();

    public void setActiveCamera(Camera camera) {
        activeCamera = camera;
    }

    public Camera getActiveCamera() {
        return activeCamera;
    }

    public void setBackground(Background bg) {
        background = bg;
    }

    public Background getBackground() {
        return background;
    }

    public void addLight(Light light) {
        if (light == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (!lights.contains(light)) {
            lights.addElement(light);
        }
    }

    public void removeLight(Light light) {
        if (light != null) {
            lights.removeElement(light);
        }
    }

    public int getLightCount() {
        return lights.size();
    }

    public Light getLight(int index) {
        if (index < 0 || index >= lights.size()) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return (Light)lights.elementAt(index);
    }

    void collectReferences(java.util.Vector out) {
        if (activeCamera != null) out.addElement(activeCamera);
        if (background != null) out.addElement(background);
        for (int i = 0; i < lights.size(); i++) {
            out.addElement(lights.elementAt(i));
        }
        super.collectReferences(out);
    }

    Object3D findInternal(int id) {
        if (activeCamera != null && activeCamera.userID == id && id != 0) {
            return activeCamera;
        }
        if (background != null && background.userID == id && id != 0) {
            return background;
        }
        for (int i = 0; i < lights.size(); i++) {
            Light l = (Light)lights.elementAt(i);
            if (l.userID == id && id != 0) {
                return l;
            }
        }
        return super.findInternal(id);
    }
}
