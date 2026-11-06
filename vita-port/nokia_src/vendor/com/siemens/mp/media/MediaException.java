package com.siemens.mp.media;

/** Siemens API media.MediaException (J2ME-Loader list, Apache-2.0). */
public class MediaException extends Exception {
    public MediaException(Exception e) {
        super(String.valueOf(e));
    }

    public MediaException(String message) {
        super(message);
    }
}
