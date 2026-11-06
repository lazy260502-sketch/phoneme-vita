/*
 *   
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 * 
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

package com.sun.midp.chameleon.layers;

import javax.microedition.lcdui.*;

import com.sun.midp.chameleon.ChamDisplayTunnel;
import com.sun.midp.chameleon.CLayer;

import com.sun.midp.chameleon.skins.*;
import com.sun.midp.chameleon.skins.resources.ScrollIndResourcesConstants;

/**
 * AlertLayer 
 */
public class AlertLayer extends BodyLayer {

    /**
     * The AlertLayer constructor. Initializes background image if 
     * there is one set in AlertSkin.
     * @param tunnel - The ChamDisplayTunnel to do paint calls
     */
    public AlertLayer(ChamDisplayTunnel tunnel) {
        super(AlertSkin.IMAGE_BG, AlertSkin.COLOR_BG, tunnel);
        setVisible(false);
    }

    /**
     * Sets content to be displayed in the Alert Layer.
     * This AlertLayer will be made visible if  <code>alertVisible</code>
     * is true and will be hidden - otherwise.
     * @param alertVisible - true if the AlertLayer should be shown,
     *                       and false - otherwise
     * @param alert - The <code>Alert</code> instance that is currently
     *                visible
     * @param height the preferred height for the Alert. This is accepted
     *               as long as it is less than AlertSkin.HEIGHT
     */
    public void setAlert(boolean alertVisible, Alert alert, int height) {
        this.alert = alert;
        setDirty();
        setVisible(alertVisible);
    }
    
    /**
     * Toggle the visibility state of Alert layer within its containing
     * window.
     *
     * @param visible if alert should be visible, false otherwise
     */
    public void setVisible(boolean visible) {
        super.setVisible(visible);
        setSupportsInput(visible);
    }


    /**
     * Initialize the bounds of this AlertLayer. Overrides
     * initialize in superclasses. The dimensions of the
     * specific AlertLayer are specified in AlertSkin.
     *
     * The X and Y coordinates represent the upper left position
     * of this CLayer in the physical display's coordinate space.
     */
    protected void initialize() {
        super.initialize();
        setAnchor();
    }

    /**
     * Align alert depend on skin
     */
    public void setAnchor() {
	if (owner == null)
	    return;
	/* VITA v02.98 (HYPOTHESIS, not yet confirmed on device): the skin has
	 * ALERT_WIDTH=-1 and this method resolves it ONCE by overwriting the
	 * skin constant. log120 showed "skin=0x125" for both UC Alerts, i.e.
	 * AlertSkin.WIDTH == 0 at paint time, which makes a zero-width box
	 * while the wash layer still dims the page (checkerboard screenshot).
	 * If this method ever runs while owner.bounds is still 0x0, 0.95 * 0
	 * = 0 is written and sticks for the whole VM run. Guard against that,
	 * and log what is seen so the next device log settles whether this is
	 * the cause (if "[ALERT-LAYER]" never prints and skin is still 0, the
	 * 0 comes from somewhere else - e.g. the loaded skin value). */
	if (owner.bounds[W] <= 0 || owner.bounds[H] <= 0) {
	    System.out.println("[ALERT-LAYER] owner has no size yet: "
		+ owner.bounds[W] + "x" + owner.bounds[H]
		+ " skin=" + AlertSkin.WIDTH + "x" + AlertSkin.HEIGHT);
	    return;
	}
	if (AlertSkin.WIDTH == -1 || AlertSkin.HEIGHT == -1) {
	    System.out.println("[ALERT-LAYER] resolve skin=" + AlertSkin.WIDTH
		+ "x" + AlertSkin.HEIGHT + " owner=" + owner.bounds[W] + "x"
		+ owner.bounds[H]);
	}
	/* v02.100 (log122): the line above printed "resolve skin=-1x125
	 * owner=240x320" and the very next paint log said skin=0x125, i.e.
	 * (int)(.95 * 240) evaluated to 0 instead of 228 (the owner size was
	 * fine, so v02.98's "owner has no size" guess was wrong). Compute with
	 * integer arithmetic, and print what the double expression gives so
	 * the next log shows whether this VM's double -> int path is broken. */
	if (AlertSkin.WIDTH == -1 || AlertSkin.HEIGHT == -1) {
	    /* log123: (int)(.95*240)=228 but (int)(.95*owner)=0 - the first is
	     * folded by javac (no VM arithmetic), the second runs on the VM's
	     * i2d -> dmul -> d2i. Print each step so the next log shows which
	     * bytecode loses the value. The long forms go through different
	     * VM routines (l2d / d2l) and tell whether it is d2i only. */
	    int w = owner.bounds[W];
	    double dw = w;                   /* i2d */
	    double dp = 0.95;                /* ldc2_w */
	    double pr = dw * dp;             /* dmul */
	    System.out.println("[ALERT-LAYER] fp i2d=" + (int) dw
		+ " dmul*2=" + (int) (dw * 2.0) + " dadd=" + (int) (dw + 1.0)
		+ " dmul.95=" + (int) pr + " d2l=" + (long) pr
		+ " f=" + (int) ((float) w * 0.95f) + " cmp.95<w=" + (dp < dw)
		+ " int-math=" + (w * 95 / 100));
	}
	if (AlertSkin.WIDTH == -1) {
            AlertSkin.WIDTH = owner.bounds[W] * 95 / 100;
        }

        if (AlertSkin.HEIGHT == -1) {
            AlertSkin.HEIGHT = owner.bounds[H] * 75 / 100;
        }

        bounds[W] = AlertSkin.WIDTH;
        bounds[H] = AlertSkin.HEIGHT;

        switch (AlertSkin.ALIGN_X) {
        case Graphics.LEFT:
            bounds[X] = 0;
            break;
        case Graphics.RIGHT:
            bounds[X] = owner.bounds[W] - bounds[W];
            break;
        case Graphics.HCENTER:
        default:
	    bounds[X] = (owner.bounds[W] - bounds[W]) >> 1;
            break;
        }
        switch (AlertSkin.ALIGN_Y) {
        case Graphics.TOP:
            bounds[Y] = 0;
            if (alert != null &&
                alert.getTicker() != null &&
                TickerSkin.ALIGN == Graphics.TOP) {
                bounds[Y] += TickerSkin.HEIGHT;
            } 
            break;
        case Graphics.VCENTER:
	    bounds[Y] = owner.bounds[H] - SoftButtonSkin.HEIGHT - bounds[H];
            if (alert != null && alert.getTicker() != null) {
                bounds[Y] -= TickerSkin.HEIGHT;
            }
            bounds[Y] >>= 1;
            break;
        case Graphics.BOTTOM:
        default:
	    bounds[Y] = owner.bounds[H] - SoftButtonSkin.HEIGHT -
		    bounds[H];
            if (alert != null &&
                alert.getTicker() != null &&
                TickerSkin.ALIGN != Graphics.TOP) {
                bounds[Y] -= TickerSkin.HEIGHT;
            }
            break;
        }
        updateBoundsByScrollInd();

    }

    /** The Alert instance which content is currently visible */
    private Alert alert;

    /**
     * Update bounds of layer
     *
     * @param layers - current layer can be dependant on this parameter
     */
    public void update(CLayer[] layers) {
        super.update(layers);
        setAnchor();
    }

    /**
     *  * Update bounds of layer depend on visability of scroll indicator layer
     */
    public void updateBoundsByScrollInd() {
        bounds[W] = AlertSkin.WIDTH;
        if (owner != null) {
            switch (AlertSkin.ALIGN_X) {
                case Graphics.LEFT:
                    bounds[X] = 0;
                    break;
                case Graphics.RIGHT:
                    bounds[X] = owner.bounds[W] - bounds[W];
                    break;
                case Graphics.HCENTER:
                default:
                    bounds[X] = (owner.bounds[W] - bounds[W]) >> 1;
                    break;
            }
        }
        super.updateBoundsByScrollInd();
    }
}

