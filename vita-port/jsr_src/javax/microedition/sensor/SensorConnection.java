/*
 * SensorConnection.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

import java.io.IOException;
import javax.microedition.io.Connection;

/**
 * An open sensor. Obtained with
 * {@code Connector.open("sensor://acceleration;contextType=device;model=vita")},
 * not from SensorManager (the specification has no open method there).
 */
public interface SensorConnection extends Connection {

    int STATE_OPENED = 1;
    int STATE_LISTENING = 2;
    int STATE_CLOSED = 4;

    /**
     * Synchronous read of one buffer per channel.
     *
     * @param bufferSize number of values per Data object, 1..getMaxBufferSize()
     */
    Data[] getData(int bufferSize) throws IOException;

    /**
     * Synchronous read with explicit metadata options.
     *
     * @param bufferSize values per Data, or -1 when bufferingPeriod is used
     * @param bufferingPeriod collection window in ms, or -1 when bufferSize is used
     */
    Data[] getData(int bufferSize, long bufferingPeriod,
                   boolean isTimestampIncluded, boolean isUncertaintyIncluded,
                   boolean isValidityIncluded) throws IOException;

    SensorInfo getSensorInfo();

    /**
     * The Channel object for one of the {@code ChannelInfo} instances returned
     * by {@code getSensorInfo().getChannelInfos()}.
     *
     * The parameter is compared with INSTANCE equality, which is what the
     * specification says ("is compared using instance equality and therefore
     * needs to be an instance previously returned from
     * SensorInfo.getChannelInfos()"), so an equal-looking copy does not work.
     * Conditions are registered on the returned Channel, not on the connection.
     */
    Channel getChannel(ChannelInfo channelInfo);

    int getState();

    void setDataListener(DataListener listener, int bufferSize)
            throws IOException;

    void setDataListener(DataListener listener, int bufferSize,
                         long bufferingPeriod, boolean isTimestampIncluded,
                         boolean isUncertaintyIncluded,
                         boolean isValidityIncluded) throws IOException;

    void removeDataListener();
}
