/*
 * ActionTable.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * The animation table format is part of the unpublished model container, so the
 * data is stored and the counts are reported conservatively (one action, one
 * frame) - that is enough for Figure.setPosture(act, 0, 0) to be accepted.
 */
package com.vodafone.v10.graphics.j3d;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class ActionTable {

    private byte[] data;

    public ActionTable(byte[] data) {
        if (data == null) {
            throw new NullPointerException("data");
        }
        this.data = data;
    }

    public ActionTable(String name) throws IOException {
        if (name == null) {
            throw new NullPointerException("name");
        }
        InputStream in = ActionTable.class.getResourceAsStream(name);
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

    public int getNumAction() {
        return 1;
    }

    public int getNumFrame(int action) {
        if (action != 0) {
            throw new IllegalArgumentException("action out of range");
        }
        return 1;
    }

    int size() {
        return (data == null) ? 0 : data.length;
    }
}
