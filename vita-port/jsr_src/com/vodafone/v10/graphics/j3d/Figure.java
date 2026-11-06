/*
 * Figure.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * The model container format is not published, so the data is only stored:
 * setPosture()/setTexture() record what the game asked for and drawFigure()
 * ignores the geometry (it draws a placeholder).
 */
package com.vodafone.v10.graphics.j3d;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class Figure {

    private byte[] data;
    private Texture texture;
    private int textureSet;

    public Figure(byte[] data) {
        if (data == null) {
            throw new NullPointerException("data");
        }
        this.data = data;
    }

    public Figure(String name) throws IOException {
        if (name == null) {
            throw new NullPointerException("name");
        }
        InputStream in = Figure.class.getResourceAsStream(name);
        if (in == null) {
            throw new IOException("resource not found: " + name);
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            data = out.toByteArray();
        } finally {
            try {
                in.close();
            } catch (IOException e) {
            }
        }
    }

    /** Recorded only: the animation format is not published. */
    public void setPosture(ActionTable actTable, int action, int frame) {
        if (actTable == null) {
            throw new NullPointerException("actTable");
        }
        if (action < 0 || action >= actTable.getNumAction()) {
            throw new IllegalArgumentException("action out of range");
        }
    }

    public void setTexture(Texture texture) {
        if (texture == null) {
            throw new NullPointerException("texture");
        }
        this.texture = texture;
        this.textureSet = 1;
    }

    /* --- package access for Graphics3DImpl --- */

    boolean hasTexture() {
        return textureSet != 0;
    }

    int size() {
        return (data == null) ? 0 : data.length;
    }
}
