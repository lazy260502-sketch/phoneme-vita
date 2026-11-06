/*
 * JSR / API: Vodafone VSCL 1.0/1.1 - com.vodafone.v10.graphics.j3d
 * =================================================================
 * SCOPE OF THIS PORT
 * ------------------
 * The specification defines ten types (Graphics3D, Figure, FigureLayout,
 * Effect3D, Light, Texture, AffineTrans, Vector3D, ActionTable, Util3D) and
 * NOTHING about:
 *   - how a MIDlet obtains a Graphics3D instance (the javadoc shows only
 *     drawFigure(); the implementation class is vendor-private), and
 *   - the model container format (Aplix "M3D", not published).
 *
 * So this port ships the complete API surface (a game can import it, allocate
 * AffineTrans/FigureLayout/Texture/ActionTable, and walk its own code without
 * NoClassDefFoundError) plus Graphics3DImpl, whose drawFigure() draws a
 * wireframe placeholder at the layout's centre/scale and logs once that the
 * model format is unsupported. REAL model rendering is not possible from the
 * published material - see PROJECT_MEMORY.md v04.40.
 *
 * Signatures were checked against the VSCL 1.0 javadoc (nikita36078 mirror);
 * the angle unit is 4096 per revolution throughout, and AffineTrans exposes
 * its matrix as public int fields m00..m23.
 */
package com.vodafone.v10.graphics.j3d;

/** 3D graphics interface: one method, per the VSCL 1.0 javadoc. */
public interface Graphics3D {

    void drawFigure(Figure figure, int x, int y, FigureLayout layout,
                    Effect3D effect);
}
