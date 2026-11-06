package javax.microedition.m3g;

/**
 * Indexed triangle geometry with one Appearance per submesh.
 */
public class Mesh extends Node {

    VertexBuffer vertexBuffer;
    IndexBuffer indexBuffer;
    Appearance[] appearances = new Appearance[1];
    IndexBuffer[] indexBuffers = new IndexBuffer[1];

    Mesh(boolean placeholder) {
    }

    public Mesh(VertexBuffer vertexBuffer, IndexBuffer indexBuffer,
                Appearance appearance) {
        if (vertexBuffer == null || indexBuffer == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (vertexBuffer.positions == null) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("mesh needs positions"));
        }
        if (appearance != null) {
            appearances[0] = appearance;
        }
        this.vertexBuffer = vertexBuffer;
        this.indexBuffer = indexBuffer;
        /* v03.30: getIndexBuffer(0) returned null for a mesh built here,
         * because only the legacy `indexBuffer` field was filled in. */
        this.indexBuffers[0] = indexBuffer;
    }

    /* v03.30: the JSR-184 multi-submesh constructor. NFS builds its meshes
     * with it (ab: invokespecial Mesh.<init>:(VertexBuffer;[IndexBuffer;
     * [Appearance;)V); without it that is a NoSuchMethodError. `appearances`
     * may be null, otherwise it needs at least one entry per submesh. */
    public Mesh(VertexBuffer vertexBuffer, IndexBuffer[] triangles,
                Appearance[] appearances) {
        if (vertexBuffer == null || triangles == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (triangles.length == 0
                || (appearances != null && appearances.length < triangles.length)) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad submesh arrays"));
        }
        for (int i = 0; i < triangles.length; i++) {
            if (triangles[i] == null) {
                throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
            }
        }
        if (vertexBuffer.positions == null) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("mesh needs positions"));
        }
        this.vertexBuffer = vertexBuffer;
        this.indexBuffers = new IndexBuffer[triangles.length];
        this.appearances = new Appearance[triangles.length];
        for (int i = 0; i < triangles.length; i++) {
            this.indexBuffers[i] = triangles[i];
            this.appearances[i] = (appearances != null) ? appearances[i] : null;
        }
        this.indexBuffer = this.indexBuffers[0];
    }

    public void setAppearance(int index, Appearance appearance) {
        if (index < 0 || index >= appearances.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        appearances[index] = appearance;
    }

    public Appearance getAppearance(int index) {
        if (index < 0 || index >= appearances.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return appearances[index];
    }

    Mesh duplicateMesh() {
        Mesh c = new Mesh(vertexBuffer, indexBuffer, appearances[0]);
        c.appearances = new Appearance[appearances.length];
        System.arraycopy(appearances, 0, c.appearances, 0, appearances.length);
        c.indexBuffers = new IndexBuffer[indexBuffers.length];
        System.arraycopy(indexBuffers, 0, c.indexBuffers, 0, indexBuffers.length);
        Object3D.copyNodeState(this, c);
        return c;
    }

    public VertexBuffer getVertexBuffer() {
        return vertexBuffer;
    }

    public IndexBuffer getIndexBuffer(int index) {
        if (index < 0 || index >= indexBuffers.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return indexBuffers[index];
    }

    public int getSubmeshCount() {
        return indexBuffers.length;
    }

    void collectReferences(java.util.Vector out) {
        if (vertexBuffer != null) out.addElement(vertexBuffer);
        for (int i = 0; i < indexBuffers.length; i++) {
            if (indexBuffers[i] != null) out.addElement(indexBuffers[i]);
        }
        for (int i = 0; i < appearances.length; i++) {
            if (appearances[i] != null) out.addElement(appearances[i]);
        }
        super.collectReferences(out);
    }

    VertexBuffer getVertexBufferInternal() {
        return vertexBuffer;
    }

    IndexBuffer getIndexBufferInternal() {
        return indexBuffer;
    }
}
