package javax.microedition.m3g;

import java.util.Vector;

/**
 * Index buffer base. Triangles are handed to the renderer as a flat list of
 * triangle indices; strips are converted here so neither backend needs a
 * strip primitive.
 */
public abstract class IndexBuffer extends Object3D {

    IndexBuffer() {
    }

    public abstract void getIndices(int[] indices);

    public abstract int getIndexCount();

    /** Index list expanded to independent triangles. */
    abstract int[] triangleList();

    /** Number of triangles in triangleList(). */
    abstract int triangleCount();
}
