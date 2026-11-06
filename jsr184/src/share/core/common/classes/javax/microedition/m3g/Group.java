package javax.microedition.m3g;

import java.util.Vector;

/**
 * Node container. Children are drawn in insertion order; a Node can only be
 * in one Group at a time.
 */
public class Group extends Node {

    private final Vector children = new Vector();

    public void addChild(Node child) {
        if (child == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (child == this) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("cannot add a node to itself"));
        }
        if (child.parent != null) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("node already has a parent"));
        }
        if (isAncestorOf(child)) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("would create a cycle"));
        }
        children.addElement(child);
        child.parent = this;
    }

    public void removeChild(Node child) {
        if (child != null && children.removeElement(child)) {
            child.parent = null;
        }
    }

    public int getChildCount() {
        return children.size();
    }

    public Node getChild(int index) {
        if (index < 0 || index >= children.size()) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return (Node)children.elementAt(index);
    }

    public void setAlignment(Node zRef, int zTarget, Node yRef, int yTarget) {
        /* Alignment only affects Sprite3D/SkinnedMesh placement policies;
         * this port keeps the frame's own axes (the default behaviour). */
        if (zTarget != NONE && zTarget != ORIGIN && zTarget != Z_AXIS
                && zTarget != -Z_AXIS) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad z target"));
        }
        if (yTarget != NONE && yTarget != ORIGIN && yTarget != Y_AXIS
                && yTarget != -Y_AXIS) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad y target"));
        }
    }

    private boolean isAncestorOf(Node n) {
        for (Node p = n; p != null; p = p.parent) {
            if (p == this) {
                return true;
            }
        }
        return false;
    }

    void collectReferences(java.util.Vector out) {
        for (int i = 0; i < children.size(); i++) {
            out.addElement(children.elementAt(i));
        }
        super.collectReferences(out);
    }

    Object3D findInternal(int id) {
        Object3D r = super.findInternal(id);
        if (r != null) {
            return r;
        }
        for (int i = 0; i < children.size(); i++) {
            r = ((Node)children.elementAt(i)).findInternal(id);
            if (r != null) {
                return r;
            }
        }
        return null;
    }

    int childCountInternal() {
        return children.size();
    }

    Node childAtInternal(int i) {
        return (Node)children.elementAt(i);
    }
}
