package com.samsung.util;

/** Samsung API SM: a short message value object (J2ME-Loader list, Apache-2.0). */
public class SM {
    private String destAddress;
    private String callbackAddress;
    private String data;

    public SM() {
    }

    public SM(String dest, String callback, String textMessage) {
        setDestAddress(dest);
        setCallbackAddress(callback);
        setData(textMessage);
    }

    public String getCallbackAddress() {
        return callbackAddress;
    }

    public String getData() {
        return data;
    }

    public String getDestAddress() {
        return destAddress;
    }

    public void setCallbackAddress(String address) {
        this.callbackAddress = address;
    }

    public void setData(String textMessage) {
        this.data = textMessage;
    }

    public void setDestAddress(String address) {
        this.destAddress = address;
    }
}
