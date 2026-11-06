package javax.microedition.m3g;

/**
 * Vertex attribute bundle: positions, normals, colours and texture
 * coordinates, each optional.
 */
public class VertexBuffer extends Object3D {

    public static final int POSITION = 0;
    public static final int NORMAL = 1;
    public static final int COLOR = 2;
    public static final int TEXCOORD = 3;

    int numVertices;
    VertexArray positions;
    VertexArray normals;
    VertexArray colors;
    VertexArray texCoords;
    float[] defaultColor = { 1.0f, 1.0f, 1.0f, 1.0f };

    /* Position / texcoord encode as value*scale + bias (M3G file format):
     * SHORT arrays are normalised by Renderer, then scaled and biased. */
    final float[] positionBias = { 0.0f, 0.0f, 0.0f };
    float positionScale = 1.0f;
    final float[] texBias = { 0.0f, 0.0f, 0.0f };
    float texScale = 1.0f;

    VertexBuffer(boolean placeholder) {
    }

    /* v03.30: JSR-184 `public VertexBuffer()`. The only public constructor
     * here took nine ints, which is not in the API; a title compiled against
     * the real one (生化危机 BioCtrl.initOverlayMesh: invokespecial
     * VertexBuffer.<init>:()V) got NoSuchMethodError - an Error, which that
     * method's `catch (Exception)` does not intercept. The vertex count is
     * fixed by the first array that is set (see requireSize). */
    public VertexBuffer() {
    }

    public VertexBuffer(int numVertices, int numPositions, int positionType,
                        int numNormals, int normalType, int numColors,
                        int colorType, int numTexCoords, int texCoordType) {
        if (numVertices <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad vertex count"));
        }
        if (numPositions != 0 && numPositions != 3) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("positions must be 3D"));
        }
        if (numNormals != 0 && numNormals != 3) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("normals must be 3D"));
        }
        if (numColors != 0 && numColors != 3 && numColors != 4) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("colours must be 3 or 4"));
        }
        if (numTexCoords != 0 && numTexCoords != 2) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("tex coords must be 2D"));
        }
        this.numVertices = numVertices;
        if (numPositions != 0) {
            positions = new VertexArray(numVertices, 3, positionType);
        }
        if (numNormals != 0) {
            normals = new VertexArray(numVertices, 3, normalType);
        }
        if (numColors != 0) {
            colors = new VertexArray(numVertices, numColors, colorType);
        }
        if (numTexCoords != 0) {
            texCoords = new VertexArray(numVertices, 2, texCoordType);
        }
    }

    public void setPositions(VertexArray v) {
        requireSize(v, positions);
        positions = v;
    }

    /* v03.30: the JSR-184 signature, `setPositions(VertexArray, float scale,
     * float[] bias)`; value = raw * scale + bias, bias may be null (zeros).
     * BioCtrl.initOverlayMesh calls it with bias == null. */
    public void setPositions(VertexArray v, float scale, float[] bias) {
        if (v != null && bias != null && bias.length < 3) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bias needs 3 elements"));
        }
        requireSize(v, positions);
        positions = v;
        positionScale = scale;
        positionBias[0] = (bias != null) ? bias[0] : 0.0f;
        positionBias[1] = (bias != null) ? bias[1] : 0.0f;
        positionBias[2] = (bias != null) ? bias[2] : 0.0f;
    }

    public VertexArray getPositions(float[] scaleBias) {
        /* JSR-184: scaleBias[0..2] = bias, scaleBias[3] = scale. Returned the
         * placeholder [1.0, 0.0] before, which is neither. */
        if (scaleBias != null && scaleBias.length >= 4) {
            scaleBias[0] = positionBias[0];
            scaleBias[1] = positionBias[1];
            scaleBias[2] = positionBias[2];
            scaleBias[3] = positionScale;
        }
        return positions;
    }

    public void setNormals(VertexArray v) {
        requireSize(v, normals);
        normals = v;
    }

    public VertexArray getNormals() {
        return normals;
    }

    public void setColors(VertexArray v) {
        requireSize(v, colors);
        colors = v;
    }

    public VertexArray getColors() {
        return colors;
    }

    public void setTexCoords(int index, VertexArray v, float[] scaleBias) {
        if (index != 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("only one texture unit"));
        }
        requireSize(v, texCoords);
        texCoords = v;
        /* JSR-184: the array is {biasS, biasT, scaleS, scaleT}. */
        if (scaleBias != null && scaleBias.length >= 4) {
            texBias[0] = scaleBias[0];
            texBias[1] = scaleBias[1];
            texScale = scaleBias[2];
        }
    }

    /* v03.30: the JSR-184 signature, `setTexCoords(int, VertexArray, float
     * scale, float[] bias)`; bias may be null (zeros). BioCtrl passes
     * (0, uv, 0.0078125f, null) - 1/128 for SHORT texture coordinates. The
     * (int, VertexArray, float[]) overload above is not in the API and is
     * kept only because vita-port/midlets/M3GTest.java uses it. */
    public void setTexCoords(int index, VertexArray v, float scale,
                             float[] bias) {
        if (index != 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("only one texture unit"));
        }
        if (v != null && bias != null && bias.length < 2) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bias needs 2 elements"));
        }
        requireSize(v, texCoords);
        texCoords = v;
        texScale = scale;
        texBias[0] = (bias != null) ? bias[0] : 0.0f;
        texBias[1] = (bias != null && bias.length > 1) ? bias[1] : 0.0f;
        texBias[2] = (bias != null && bias.length > 2) ? bias[2] : 0.0f;
    }

    public VertexArray getTexCoords(int index, float[] scaleBias) {
        if (index != 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("only one texture unit"));
        }
        /* JSR-184: scaleBias[0..2] = bias, scaleBias[3] = scale. */
        if (scaleBias != null && scaleBias.length >= 4) {
            scaleBias[0] = texBias[0];
            scaleBias[1] = texBias[1];
            scaleBias[2] = 0.0f;
            scaleBias[3] = texScale;
        }
        return texCoords;
    }

    public void setDefaultColor(int argb) {
        defaultColor[0] = ((argb >> 16) & 0xFF) / 255.0f;
        defaultColor[1] = ((argb >> 8) & 0xFF) / 255.0f;
        defaultColor[2] = (argb & 0xFF) / 255.0f;
        defaultColor[3] = ((argb >> 24) & 0xFF) / 255.0f;
    }

    /** The spec's int form packs scale in the high bits; only scale 1:1 here. */
    public int getDefaultColor() {
        return ((int)(defaultColor[3] * 255.0f) << 24)
             | ((int)(defaultColor[0] * 255.0f) << 16)
             | ((int)(defaultColor[1] * 255.0f) << 8)
             |  (int)(defaultColor[2] * 255.0f);
    }

    void collectReferences(java.util.Vector out) {
        if (positions != null) out.addElement(positions);
        if (normals != null) out.addElement(normals);
        if (colors != null) out.addElement(colors);
        if (texCoords != null) out.addElement(texCoords);
    }

    public int getVertexCount() {
        return numVertices;
    }

    private void requireSize(VertexArray v, VertexArray slot) {
        if (v == null) {
            /* v03.30: JSR-184 lets an attribute be unset by passing null.
             * It used to throw NPE, which would abort any title that
             * clears normals/colours. */
            return;
        }
        if (numVertices == 0) {
            /* No-arg constructor: the first array fixes the vertex count. */
            numVertices = v.numVertices;
        }
        if (v.numVertices != numVertices) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("vertex count mismatch"));
        }
    }
}
