package com.siemens.mp.media;

import java.io.IOException;
import java.io.InputStream;

import com.siemens.mp.media.protocol.DataSource;

/**
 * Siemens API media.Manager. Same as J2ME-Loader (Apache-2.0): a Siemens
 * player cannot be created - createPlayer throws MediaException, which games
 * are written to survive on handsets without media support. playTone is
 * accepted and silent. (Routing Siemens players to the standard MMAPI would be
 * the way to make them audible; not done - no game needing it is known.)
 */
public final class Manager {
    private Manager() {}

    public static String[] getSupportedContentTypes(String protocol) {
        return new String[0];
    }

    public static String[] getSupportedProtocols(String content_type) {
        return new String[0];
    }

    public static Player createPlayer(String locator)
        throws IOException, MediaException {
        throw new MediaException("Not implemented");
    }

    public static Player createPlayer(InputStream stream, String type)
        throws IOException, MediaException {
        throw new MediaException("Content not supported");
    }

    public static Player createPlayer(DataSource source)
        throws IOException, MediaException {
        throw new MediaException("Not implemented");
    }

    public static void playTone(int note, int duration, int volume)
        throws MediaException {
    }
}
