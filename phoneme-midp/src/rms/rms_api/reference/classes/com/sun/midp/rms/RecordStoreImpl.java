/*
 *
 *
 * Portions Copyright  2000-2007 Sun Microsystems, Inc. All Rights
 * Reserved.  Use is subject to license terms.
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
 *
 * Copyright 2000 Motorola, Inc. All Rights Reserved.
 * This notice does not imply publication.
 */

package com.sun.midp.rms;

import java.io.IOException;
import javax.microedition.rms.*;

import com.sun.midp.security.Permissions;
import com.sun.midp.security.SecurityToken;
import com.sun.midp.log.Logging;
import com.sun.midp.log.LogChannels;

/**
 * A class implementing a MIDP a record store.
 */

public class RecordStoreImpl implements AbstractRecordStoreImpl {
    /** used to compact the records of the record store */
    private byte[] compactBuffer = new byte[COMPACT_BUFFER_SIZE];

    /**
     * Internal indicator for AUTHMODE_ANY with read only access
     * AUTHMODE_ANY_RO has a value of 2.
     */
    final static int AUTHMODE_ANY_RO = 2;

    /** unique id for suite that owns this record store */
    int suiteId;

    /** 
     * lock used to synchronize this record store between concurrently 
     * running MIDlets 
     */
    AbstractRecordStoreLock recordStoreLock; 

    /** data block header stored here */
    RecordStoreSharedDBHeader dbHeader;

    /** record store index */
    private RecordStoreIndex dbIndex;

    /** record store data */
    private RecordStoreFile dbFile;

    /**
     * VITA (v01.53): one-shot breadcrumb guard for getSizeAvailable().
     * The "not enough RMS space" report the MIDlet shows comes from
     * three very different causes (a negative per-suite limit, a zero
     * storage free space, a bloated store file), so the first tight
     * answer prints every term once and the log identifies the cause
     * without any guessing.
     */
    private boolean spaceReported;

    /**
     * Deletes the named record store. MIDlet suites are only allowed
     * to delete their own record stores. If the named record store is
     * open (by a MIDlet in this suite or a MIDlet in a different
     * MIDlet suite) when this method is called, a
     * RecordStoreException will be thrown.  If the named record store
     * does not exist a RecordStoreNotFoundException will be
     * thrown. Calling this method does NOT result in recordDeleted
     * calls to any registered listeners of this RecordStore.
     *
     * @param token security token for authorization
     * @param suiteId ID of the MIDlet suite that owns the record store
     * @param recordStoreName the MIDlet suite unique record store to
     *          delete
     *
     * @exception RecordStoreException if a record store-related
     *          exception occurred
     * @exception RecordStoreNotFoundException if the record store
     *          could not be found
     */
    public static void deleteRecordStore(SecurityToken token,
                                         int suiteId,
                                         String recordStoreName)
        throws RecordStoreException, RecordStoreNotFoundException {

        token.checkIfPermissionAllowed(Permissions.MIDP);

        // check if file exists and delete it
        
        if (RecordStoreUtil.exists(
                    RmsEnvironment.getSecureFilenameBase(suiteId),
                    recordStoreName, RecordStoreFile.DB_EXTENSION)) {
            boolean success = RecordStoreIndex.deleteIndex(
                suiteId, recordStoreName);

            RecordStoreUtil.deleteFile(
                    RmsEnvironment.getSecureFilenameBase(suiteId),
                    recordStoreName, RecordStoreFile.DB_EXTENSION);

            if (!success) {
                throw new RecordStoreException("deleteRecordStore " +
                                               "failed");
            }
        } else {
            throw new RecordStoreNotFoundException("deleteRecordStore " +
                                                   "error: file " +
                                                   "not found");
        }
    }

    /**
     * Open (and possibly create) a record store associated with the
     * given MIDlet suite. If this method is called by a MIDlet when
     * the record store is already open by a MIDlet in the MIDlet suite,
     * this method returns a reference to the same RecordStoreImpl object.
     *
     * @param token security token for authorization
     * @param suiteId ID of the MIDlet suite that owns the record store
     * @param recordStoreName the MIDlet suite unique name for the
     *          record store, consisting of between one and 32 Unicode
     *          characters inclusive.
     * @param createIfNecessary if true, the record store will be
     *          created if necessary
     *
     * @return <code>RecordStore</code> object for the record store
     *
     * @exception RecordStoreException if a record store-related
     *          exception occurred
     * @exception RecordStoreNotFoundException if the record store
     *          could not be found
     * @exception RecordStoreFullException if the operation cannot be
     *          completed because the record store is full
     * @exception IllegalArgumentException if
     *          recordStoreName is invalid
     */
    public static RecordStoreImpl openRecordStore(SecurityToken token,
                                                  int suiteId,
                                                  String recordStoreName,
                                                  boolean createIfNecessary)
        throws RecordStoreException, RecordStoreFullException,
               RecordStoreNotFoundException {

        token.checkIfPermissionAllowed(Permissions.MIDP);

        return new RecordStoreImpl(token, suiteId, recordStoreName,
                                   createIfNecessary);
    }

    /**
     * Changes the access mode for this RecordStore. The authorization
     * mode choices are:
     *
     * <ul>
     * <li><code>AUTHMODE_PRIVATE</code> - Only allows the MIDlet
     *          suite that created the RecordStore to access it. This
     *          case behaves identically to
     *          <code>openRecordStore(recordStoreName,
     *          createIfNecessary)</code>.</li>
     * <li><code>AUTHMODE_ANY</code> - Allows any MIDlet to access the
     *          RecordStore. Note that this makes your recordStore
     *          accessible by any other MIDlet on the device. This
     *          could have privacy and security issues depending on
     *          the data being shared. Please use carefully.</li>
     * </ul>
     *
     * <p>The owning MIDlet suite may always access the RecordStore and
     * always has access to write and update the store. Only the
     * owning MIDlet suite can change the mode of a RecordStore.</p>
     *
     * @param authmode the mode under which to check or create access.
     *          Must be one of AUTHMODE_PRIVATE or AUTHMODE_ANY.
     * @param writable true if the RecordStore is to be writable by
     *          other MIDlet suites that are granted access
     *
     * @exception RecordStoreException if a record store-related
     *          exception occurred
     * @exception SecurityException if this MIDlet Suite is not
     *          allowed to change the mode of the RecordStore
     * @exception IllegalArgumentException if authmode is invalid
     */
    public void setMode(int authmode, boolean writable)
        throws RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                int newAuthMode = authmode;
                if ((authmode == RecordStore.AUTHMODE_ANY) &&
                        (writable == false)) {
                    newAuthMode = AUTHMODE_ANY_RO;
                }

                byte[] dbHeaderData = dbHeader.getHeaderData(); 
                RecordStoreUtil.putInt(newAuthMode, dbHeaderData, 
                        RS1_AUTHMODE);

                try {
                    // write out the changes to the db header
                    dbFile.seek(RS1_AUTHMODE);
                    dbFile.write(dbHeaderData, RS1_AUTHMODE, 4);
                    dbHeader.headerUpdated(dbHeaderData);
                    // dbFile.commitWrite();
                } catch (java.io.IOException ioe) {
                    throw new RecordStoreException("error writing record " +
                            "store attributes");
                }
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * This method is called when the MIDlet requests to have the
     * record store closed. Note that the record store will not
     * actually be closed until closeRecordStore() is called as many
     * times as openRecordStore() was called. In other words, the
     * MIDlet needs to make a balanced number of close calls as open
     * calls before the record store is closed.
     *
     * <p>When the record store is closed, all listeners are removed
     * and all RecordEnumerations associated with it become invalid.
     * If the MIDlet attempts to perform
     * operations on the RecordStore object after it has been closed,
     * the methods will throw a RecordStoreNotOpenException.
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception RecordStoreException if a different record
     *          store-related exception occurred
     */
    public void closeRecordStore()
        throws RecordStoreNotOpenException, RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                compactRecords();  // compact before close
                dbFile.close();
                dbIndex.close();
            } catch (java.io.IOException ioe) {
                throw new RecordStoreException("error closing .db file. "
                        + ioe);
            } finally {
                unlockRecordStore();
                dbFile = null;
                dbHeader.recordStoreClosed();
            }
        }
    }

    /**
     * Returns an array of the names of record stores owned by the
     * MIDlet suite. Note that if the MIDlet suite does not
     * have any record stores, this function will return null.
     *
     * The order of RecordStore names returned is implementation
     * dependent.
     *
     * @param token security token for authorization
     * @param suiteId ID of the MIDlet suite that owns the record store
     *
     * @return array of the names of record stores owned by the
     * MIDlet suite. Note that if the MIDlet suite does not
     * have any record stores, this function will return null.
     */
    public static String[] listRecordStores(SecurityToken token,
                                            int suiteId) {
        token.checkIfPermissionAllowed(Permissions.MIDP);

        return RecordStoreFile.listRecordStores(suiteId);
    }

    /**
     * Get the authorization mode for this record store.
     *
     * @return authorization mode
     */
    public int getAuthMode() {
        byte[] dbHeaderData = dbHeader.getHeaderData();
        return RecordStoreUtil.getInt(dbHeaderData, RS1_AUTHMODE);
    }

    /**
     * Each time a record store is modified (by
     * <code>addRecord</code>, <code>setRecord</code>, or
     * <code>deleteRecord</code> methods) its <em>version</em> is
     * incremented. This can be used by MIDlets to quickly tell if
     * anything has been modified.
     *
     * The initial version number is implementation dependent.
     * The increment is a positive integer greater than 0.
     * The version number increases only when the RecordStore is updated.
     *
     * The increment value need not be constant and may vary with each
     * update.
     *
     * @return the current record store version
     */
    public int getVersion() throws RecordStoreNotOpenException {
        byte[] dbHeaderData = dbHeader.getHeaderData();        
        return RecordStoreUtil.getInt(dbHeaderData, RS4_VERSION);
    }

    /**
     * Returns the number of records currently in the record store.
     *
     * @return the number of records currently in the record store
     */
    public int getNumRecords() {
        byte[] dbHeaderData = dbHeader.getHeaderData();        
        return RecordStoreUtil.getInt(dbHeaderData, RS3_NUM_LIVE);
    }

    /**
     * Returns the amount of space, in bytes, that the record store
     * occupies. The size returned includes any overhead associated
     * with the implementation, such as the data structures
     * used to hold the state of the record store, etc.
     *
     * @return the size of the record store in bytes
     */
    public int getSize() {
        byte[] dbHeaderData = dbHeader.getHeaderData();
        return DB_HEADER_SIZE + RecordStoreUtil.getInt(dbHeaderData, 
                RS6_DATA_SIZE);
    }

    /**
     * Returns the amount of additional room (in bytes) available for
     * this record store to grow. Note that this is not necessarily
     * the amount of extra MIDlet-level data which can be stored,
     * as implementations may store additional data structures with
     * each record to support integration with native applications,
     * synchronization, etc.
     *
     * @return the amount of additional room (in bytes) available for
     *          this record store to grow
     */
    public int getSizeAvailable() {
        byte[] dbHeaderData = dbHeader.getHeaderData();

        int fileSpace = dbFile.spaceAvailable(suiteId) -
                        BLOCK_HEADER_SIZE - DB_HEADER_SIZE;
        int limitSpace = RMSConfig.STORAGE_SUITE_LIMIT -
                         RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE) -
                         BLOCK_HEADER_SIZE - DB_HEADER_SIZE;

        int rv = (fileSpace < limitSpace) ? fileSpace : limitSpace;

        /* VITA FIX (v01.53): the MIDlet's "not enough RMS space" report
         * is always this method answering 0. Three unrelated causes
         * collapse into that single 0 - fileSpace = 0 (the storage free
         * space used up, e.g. by a bloated store file) or limitSpace < 0
         * (data_size past the per-suite budget) - so print the terms
         * once per store instead of guessing which one it was. */
        if (rv <= 0 && !spaceReported) {
            spaceReported = true;
            System.out.println("[rms] space low area=" +
                RmsEnvironment.getStorageAreaId(suiteId) +
                " fileSpace=" + fileSpace +
                " limitSpace=" + limitSpace +
                " data_size=" +
                RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE) +
                " rv=" + rv);
        }

        return (rv < 0) ? 0 : rv;
    }

    /**
     * Returns the last time the record store was modified, in the
     * format used by System.currentTimeMillis().
     *
     * @return the last time the record store was modified, in the
     *          format used by System.currentTimeMillis()
     */
    public long getLastModified() {
        byte[] dbHeaderData = dbHeader.getHeaderData();        
        return RecordStoreUtil.getLong(dbHeaderData, RS5_LAST_MODIFIED);
    }

    /**
     * Returns the recordId of the next record to be added to the
     * record store. This can be useful for setting up pseudo-relational
     * relationships. That is, if you have two or more
     * record stores whose records need to refer to one another, you can
     * predetermine the recordIds of the records that will be created
     * in one record store, before populating the fields and allocating
     * the record in another record store. Note that the recordId returned
     * is only valid while the record store remains open and until a call
     * to <code>addRecord()</code>.
     *
     * @return the recordId of the next record to be added to the
     *          record store
     */
    public int getNextRecordID() {
        byte[] dbHeaderData = dbHeader.getHeaderData();
        return RecordStoreUtil.getInt(dbHeaderData, RS2_NEXT_ID);
    }

    /**
     * Adds a new record to the record store. The recordId for this
     * new record is returned. This is a blocking atomic operation.
     * The record is written to persistent storage before the
     * method returns.
     *
     * @param data the data to be stored in this record. If the record
     *          is to have zero-length data (no data), this parameter may be
     *          null.
     * @param offset the index into the data buffer of the first
     *          relevant byte for this record
     * @param numBytes the number of bytes of the data buffer to use
     *          for this record (may be zero)
     *
     * @return the recordId for the new record
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception RecordStoreException if a different record
     *          store-related exception occurred
     * @exception RecordStoreFullException if the operation cannot be
     *          completed because the record store has no more room
     * @exception SecurityException if the MIDlet has read-only access
     *          to the RecordStore
     */
    public int addRecord(byte[] data, int offset, int numBytes)
        throws RecordStoreNotOpenException, RecordStoreException,
            RecordStoreFullException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                int recordId = getNextRecordID();

                try {
                    // add a block for this record
                    addBlock(recordId, data, offset, numBytes);

                    // update the db header
                    byte[] dbHeaderData = dbHeader.getHeaderData();
                    RecordStoreUtil.putInt(recordId+1, dbHeaderData, 
                            RS2_NEXT_ID);
                    RecordStoreUtil.putInt(getNumRecords()+1, dbHeaderData,
                            RS3_NUM_LIVE);
                    int newVersion = getVersion()+1;
                    RecordStoreUtil.putInt(newVersion, dbHeaderData, 
                            RS4_VERSION);
                    RecordStoreUtil.putLong(System.currentTimeMillis(), 
                            dbHeaderData, RS5_LAST_MODIFIED);

                    // write out the changes to the db header
                    dbFile.seek(RS2_NEXT_ID);
                    dbFile.write(dbHeaderData, RS2_NEXT_ID, 3*4+8);
                    dbHeader.headerUpdated(dbHeaderData);
                    dbIndex.recordStoreVersionUpdated(newVersion);

                    // VITA FIX (v01.54): make the record durable NOW.
                    // The MIDP contract for addRecord() is "written to
                    // persistent storage before this method returns",
                    // but upstream leaves every commitWrite() in this
                    // class commented out and relies on the midp_file_cache
                    // being flushed when the store is closed. On Vita the
                    // store is NEVER closed: the launcher keeps the VM
                    // (and the MIDlet) alive in a loop, so a 40 byte db
                    // header write stays in the 3 KB RAM cache while the
                    // record payload - which is larger than the cache
                    // limit - goes straight to storage. The result is a
                    // file whose header lags its blocks (on disk:
                    // header next_id=1 live=0 data_size=0 over a 5 KB
                    // block), which is exactly the damage every salvage
                    // in the stderr trail was triggered by; the MIDlet
                    // then opens an "empty" store every launch and the
                    // user's bookmarks look like they were never saved.
                    dbFile.commitWrite();
                } catch (java.io.IOException ioe) {
                    throw new RecordStoreException("error writing new record "
                            + "data");
                }

                // Return the new record id
                return recordId;
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * The record is deleted from the record store. The recordId for
     * this record is NOT reused.
     *
     * @param recordId the ID of the record to delete
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception InvalidRecordIDException if the recordId is invalid
     * @exception RecordStoreException if a general record store
     *          exception occurs
     * @exception SecurityException if the MIDlet has read-only access
     *          to the RecordStore
     */
    public void deleteRecord(int recordId)
        throws RecordStoreNotOpenException, InvalidRecordIDException,
            RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                byte[] header = new byte[BLOCK_HEADER_SIZE];
                int blockOffset = dbIndex.getRecordHeader(recordId, header);

                // free the block
                freeBlock(blockOffset, header);

                // update the db index
                dbIndex.deleteRecordIndex(recordId);

                // update the db header
                byte[] dbHeaderData = dbHeader.getHeaderData();
                RecordStoreUtil.putInt(getNumRecords()-1, dbHeaderData, 
                        RS3_NUM_LIVE);
                int newVersion = getVersion()+1;                
                RecordStoreUtil.putInt(newVersion, dbHeaderData, 
                        RS4_VERSION);
                RecordStoreUtil.putLong(System.currentTimeMillis(), 
                        dbHeaderData, RS5_LAST_MODIFIED);

                // save the updated db header
                dbFile.seek(RS3_NUM_LIVE);
                dbFile.write(dbHeaderData, RS3_NUM_LIVE, 2*4+8);
                dbHeader.headerUpdated(dbHeaderData);
                dbIndex.recordStoreVersionUpdated(newVersion);

                // VITA FIX (v01.54): see addRecord(). A deleted record
                // must be durable before the call returns, otherwise a
                // later salvage still sees the old block as live.
                dbFile.commitWrite();

            } catch (java.io.IOException ioe) {
                throw new RecordStoreException("error updating file after" +
                        " record deletion");
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Returns the size (in bytes) of the MIDlet data available
     * in the given record.
     *
     * @param recordId the ID of the record to use in this operation
     *
     * @return the size (in bytes) of the MIDlet data available
     *          in the given record
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception InvalidRecordIDException if the recordId is invalid
     * @exception RecordStoreException if a general record store
     *          exception occurs
     */
    public int getRecordSize(int recordId)
        throws RecordStoreNotOpenException, InvalidRecordIDException,
               RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                byte[] header = new byte[BLOCK_HEADER_SIZE];

                try {
                    dbIndex.getRecordHeader(recordId, header);
                } catch (java.io.IOException ioe) {
                    throw new RecordStoreException("error reading record data");
                }

                return RecordStoreUtil.getInt(header, 4);
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Returns the data stored in the given record.
     *
     * @param recordId the ID of the record to use in this operation
     * @param buffer the byte array in which to copy the data
     * @param offset the index into the buffer in which to start copying
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception InvalidRecordIDException if the recordId is invalid
     * @exception RecordStoreException if a general record store
     *          exception occurs
     * @exception ArrayIndexOutOfBoundsException if the record is
     *          larger than the buffer supplied
     *
     * @return the number of bytes copied into the buffer, starting at
     *          index <code>offset</code>
     * @see #setRecord
     */
    public int getRecord(int recordId, byte[] buffer, int offset)
        throws RecordStoreNotOpenException, InvalidRecordIDException,
            RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                byte[] header = new byte[BLOCK_HEADER_SIZE];
                int blockOffset = dbIndex.getRecordHeader(recordId, header);

                int dataSize = RecordStoreUtil.getInt(header, 4);

                dbFile.seek(blockOffset+BLOCK_HEADER_SIZE);
                return dbFile.read(buffer, offset, dataSize);
            } catch (java.io.IOException ioe) {
                throw new RecordStoreException("error reading record data");
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Returns a copy of the data stored in the given record.
     *
     * @param recordId the ID of the record to use in this operation
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception InvalidRecordIDException if the recordId is invalid
     * @exception RecordStoreException if a general record store
     *          exception occurs
     *
     * @return the data stored in the given record. Note that if the
     *          record has no data, this method will return null.
     * @see #setRecord
     */
    public byte[] getRecord(int recordId)
        throws RecordStoreNotOpenException, InvalidRecordIDException,
               RecordStoreException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                byte[] header = new byte[BLOCK_HEADER_SIZE];
                int blockOffset = dbIndex.getRecordHeader(recordId, header);

                int dataSize = RecordStoreUtil.getInt(header, 4);
                if (dataSize == 0) {
                    return null;
                }

                byte[] buffer = new byte[dataSize];

                dbFile.seek(blockOffset+BLOCK_HEADER_SIZE);
                dbFile.read(buffer);

                return buffer;
            } catch (java.io.IOException ioe) {
                throw new RecordStoreException("error reading record data");
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Sets the data in the given record to that passed in. After
     * this method returns, a call to <code>getRecord(int recordId)</code>
     * will return an array of numBytes size containing the data
     * supplied here.
     *
     * @param recordId the ID of the record to use in this operation
     * @param newData the new data to store in the record
     * @param offset the index into the data buffer of the first
     *          relevant byte for this record
     * @param numBytes the number of bytes of the data buffer to use
     *          for this record
     *
     * @exception RecordStoreNotOpenException if the record store is
     *          not open
     * @exception InvalidRecordIDException if the recordId is invalid
     * @exception RecordStoreException if a general record store
     *          exception occurs
     * @exception RecordStoreFullException if the operation cannot be
     *          completed because the record store has no more room
     * @exception SecurityException if the MIDlet has read-only access
     *          to the RecordStore
     * @see #getRecord
     */
    public void setRecord(int recordId, byte[] newData,
                          int offset, int numBytes)
        throws RecordStoreNotOpenException, InvalidRecordIDException,
            RecordStoreException, RecordStoreFullException {

        synchronized (recordStoreLock) {
            lockRecordStore();

            try {
                byte[] header = new byte[BLOCK_HEADER_SIZE];
                int blockOffset = dbIndex.getRecordHeader(recordId, header);

                int oldBlockSize =
                  RecordStoreUtil.calculateBlockSize(RecordStoreUtil.getInt(
                                                       header, 4));
                int newBlockSize = RecordStoreUtil.calculateBlockSize(numBytes);
                if (newBlockSize <= oldBlockSize) {
                    // reuse the old block
                    splitBlock(blockOffset, header, newData, offset, numBytes);
                } else {
                    // free the old record data
                    freeBlock(blockOffset, header);

                    // add a block that contains the new record data
                    addBlock(recordId, newData, offset, numBytes);
                }

                // update the db header
                byte[] dbHeaderData = dbHeader.getHeaderData();
                int newVersion = getVersion()+1;                
                RecordStoreUtil.putInt(newVersion, dbHeaderData, 
                        RS4_VERSION);
                RecordStoreUtil.putLong(System.currentTimeMillis(), 
                        dbHeaderData, RS5_LAST_MODIFIED);

                // write out the changes to the db header
                dbFile.seek(RS4_VERSION);
                dbFile.write(dbHeaderData, RS4_VERSION, 4+8);
                dbHeader.headerUpdated(dbHeaderData);
                dbIndex.recordStoreVersionUpdated(newVersion);

                // VITA FIX (v01.54): see addRecord(). setRecord() grew
                // or shrank a block (and possibly freed one), so the
                // header change must reach storage now.
                dbFile.commitWrite();
            } catch (java.io.IOException ioe) {
                throw new RecordStoreException("error setting record data");
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Returns all of the recordId's currently in the record store.
     *
     * @return an array of the recordId's currently in the record store
     *         or null if the record store is closed.
     */
    public int[] getRecordIDs() {
        synchronized (recordStoreLock) {
            lockRecordStore();
            try {
                return dbIndex.getRecordIDs();
            } finally {
                unlockRecordStore();
            }
        }
    }

    /**
     * Returns data base file associated with this record store
     *
     * @return data base file
     */
    public AbstractRecordStoreFile getDbFile() {
        return dbFile;
    }

    /**
     * Creates data base index file associated with this record store
     *
     * @param suiteId unique ID of the suite that owns the store
     * @param name a string to name the index file
     * @return data base index file
     * @exception IOException if failed to create a file
     */
    public AbstractRecordStoreFile createIndexFile(int suiteId, String name)
      throws IOException {
        return new RecordStoreFile(suiteId, name,
                                   AbstractRecordStoreFile.IDX_EXTENSION);
    }

    /**
     * VITA (v01.52): read-only validation of the record store block chain.
     *
     * Walks [DB_HEADER_SIZE, dbSize) the same way compactRecords() and the
     * index do and requires every block to have a positive size and to fit
     * inside the data area, with the walk landing exactly on dbSize, and
     * the number of live blocks (id > 0) matching the header's num_live.
     * That is the invariant every correctly written store upholds, so this
     * can only report "inconsistent" for an already damaged store - which
     * is precisely the case it exists for. Nothing is written and no data
     * is moved; only the file read position is used.
     *
     * @param dbHeaderData the db header of the (open) store
     * @return true if the chain is exactly consistent
     */
    private boolean isBlockChainConsistent(byte[] dbHeaderData) {
        final int dbSize = RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE)
                           + DB_HEADER_SIZE;
        final int numLive = RecordStoreUtil.getInt(dbHeaderData,
                                                   RS3_NUM_LIVE);

        if (dbSize < DB_HEADER_SIZE) {
            return false;
        }

        byte[] header = new byte[BLOCK_HEADER_SIZE];
        int offset = DB_HEADER_SIZE;
        int seenLive = 0;

        while (offset < dbSize) {
            try {
                dbFile.seek(offset);
                if (dbFile.read(header) != BLOCK_HEADER_SIZE) {
                    return false;
                }
            } catch (java.io.IOException ioe) {
                return false;
            }

            int id = RecordStoreUtil.getInt(header, 0);
            int size = RecordStoreUtil.calculateBlockSize(
                           RecordStoreUtil.getInt(header, 4));
            if (size <= 0 || size > dbSize - offset) {
                return false;
            }
            if (id > 0) {
                seenLive++;
            }

            offset += size;
        }

        return offset == dbSize && seenLive == numLive;
    }

    /**
     * VITA (v01.53): hard bound for a block a salvage is willing to
     * accept.
     *
     * A damaged block header can claim an absurd size: the v01.51
     * investigation found a 2584 byte data area whose second block
     * header claimed a 1.6 GB block, and the v01.52 salvage log shows a
     * garbage record id (1615147117) accepted as live. v01.52 took such
     * a claimed size at face value, so the dense rewrite jumped past
     * EOF and the "zero the stale tail" loop then WROTE every byte up
     * to that offset. That physically grows the store file (the port's
     * pcsl_file_getusedspace() sums the real st_size, and its truncate
     * is only a logical clamp that dies with the handle) and drives
     * midp_file_cache's cachedAvailableSpace far below zero, so every
     * later RecordStore space query answers 0 and the MIDlet reports
     * "not enough RMS space" - the v01.53 symptom, appearing only
     * after the v01.52 salvage shipped.
     *
     * A healthy store can never hold one block bigger than the whole
     * per-suite budget (addBlock refuses to grow past it), so the
     * budget is the correct cap, not an arbitrary one.
     */
    private static final int SALVAGE_MAX_BLOCK =
        RMSConfig.STORAGE_SUITE_LIMIT + BLOCK_HEADER_SIZE;

    /**
     * VITA (v01.53): bound for how far a salvage scan may walk.
     *
     * The scan must never be led past the physical file by a lying
     * block header: it would then "resynchronize" 8 bytes at a time
     * through a bloated file, and the tail zero-fill would write over
     * everything it walked. addBlock() refuses to let data_size pass
     * the per-suite budget, so a store's data area can never honestly
     * exceed STORAGE_SUITE_LIMIT (+ the db/block headers); twice that
     * leaves a comfortable margin without giving a garbage header room
     * to drag the scan through a GB-sized stale tail.
     */
    private static final int SALVAGE_MAX_SPAN =
        2 * RMSConfig.STORAGE_SUITE_LIMIT;

    /**
     * VITA (v01.53): scratch byte for the one-byte payload probes
     * (blockFitsInFile / physicalFileHasLiveBlocks). A damaged store
     * makes the probe run thousands of times, and CLDC prefers one
     * shared byte over thousands of 1-byte allocations.
     */
    private static final byte[] PROBE_BYTE = new byte[1];

    /**
     * VITA (v01.53): is the whole block [offset, offset + size) really
     * inside the file?
     *
     * Probes the last payload byte, exactly the way
     * physicalFileHasLiveBlocks() probes it, and additionally rejects
     * any size past SALVAGE_MAX_BLOCK. Both checks are needed: the
     * probe alone cannot catch a garbage size that happens to land
     * inside a bloated file, and the cap alone cannot catch a garbage
     * size past EOF. Only a block that passes both is safe to copy and
     * to advance the write cursor by.
     *
     * @param offset block offset in the db file
     * @param size block size to validate
     * @return true when the payload is physically readable
     */
    private boolean blockFitsInFile(int offset, int size) {
        if (size <= 0 || size > SALVAGE_MAX_BLOCK) {
            return false;
        }
        try {
            dbFile.seek(offset + size - 1);
            return dbFile.read(PROBE_BYTE, 0, 1) == 1;
        } catch (java.io.IOException ioe) {
            return false;
        }
    }

    /**
     * VITA (v01.52): quick probe whether the physical file still holds
     * at least one live block past the db header - even when the
     * header claims the store is empty.
     *
     * This is the exact situation the v01.51 reset created: a 40-byte
     * "empty" header over 5.8 KB of intact record blocks. The probe
     * reads a bounded number of block headers to EOF and stops at the
     * first live one, so it costs almost nothing on a healthy store
     * (the first block is usually live and terminates the probe
     * immediately).
     *
     * @return true when a live block is physically present
     */
    private boolean physicalFileHasLiveBlocks() {
        byte[] header = new byte[BLOCK_HEADER_SIZE];
        int offset = DB_HEADER_SIZE;
        /* bound the probe: an honest walk never needs more blocks
         * than 8 bytes each of the (small) file; the bound only
         * guards a pathological garbage file. */
        int probes = 4096;

        while (probes-- > 0) {
            int readBack;
            try {
                dbFile.seek(offset);
                readBack = dbFile.read(header, 0, BLOCK_HEADER_SIZE);
            } catch (java.io.IOException ioe) {
                return false;
            }
            if (readBack != BLOCK_HEADER_SIZE) {
                return false; /* physical EOF, no live block found */
            }

            int id = RecordStoreUtil.getInt(header, 0);
            int dataSize = RecordStoreUtil.getInt(header, 4);
            int size = RecordStoreUtil.calculateBlockSize(dataSize);

            if (id > 0 && dataSize >= 0 && size > 0) {
                /* Verify the payload is physically there (salvage
                 * must be able to read the whole block, so a header
                 * claiming bytes past EOF does not count - otherwise
                 * the probe would re-trigger a salvage that can never
                 * recover anything, once per open, forever).
                 * v01.53: also enforces SALVAGE_MAX_BLOCK, so a block
                 * header that claims more than the whole per-suite
                 * budget cannot resurrect the store either. */
                if (blockFitsInFile(offset, size)) {
                    return true;
                }
                offset += BLOCK_HEADER_SIZE;
                continue;
            }
            /* follow an honest free-block chain when possible,
             * otherwise slide by one block header */
            if (size > 0) {
                offset += size;
            } else {
                offset += BLOCK_HEADER_SIZE;
            }
        }
        return false;
    }

    /**
     * VITA (v01.52): salvage a store whose on-disk layout no longer
     * matches its header, WITHOUT discarding the records.
     *
     * v01.51 used to reset such a store to empty on open. That was
     * wrong twice over: the physical blocks usually hold every record
     * intact (the mismatch comes from the header fields lagging the
     * block writes when the emulator drops a truncate or the file
     * cache is not flushed on an unclean exit), and erasing them
     * destroyed exactly the data the user expected to see ("second
     * launch: bookmarks gone"). Salvage instead:
     *
     *   1. scan the data area block by block exactly like the index
     *      does, but tolerate a bad block by SKIPPING 8 bytes (one
     *      block header) and resynchronizing, instead of giving up;
     *   2. collect every live block (id > 0) whose payload is fully
     *      inside the data area;
     *   3. rewrite the live blocks densely from offset DB_HEADER_SIZE,
     *      dropping every free block and every unreadable region;
     *   4. recompute the db header from what was actually kept
     *      (next_id = max(id)+1, num_live, data_size, free_size = 0)
     *      and commitWrite() it, so the store is exactly consistent
     *      the next time it is opened.
     *
     * After step 4 the store is pristine: isBlockChainConsistent()
     * returns true, compactRecords() walks a valid chain, and the
     * salvage will never fire twice for the same damage. A store with
     * no salvageable blocks is reset to empty (identical to v01.51)
     * - that case is truly unrecoverable.
     *
     * @param storeName name of the store being salvaged (log breadcrumb
     *                  only - the trail must name the store the damage
     *                  belongs to)
     * @param dbHeaderData the db header read from the (open) store
     */
    private void salvageStore(String storeName, byte[] dbHeaderData) {
        final int oldDataSize =
            RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE);

        /* Scan to PHYSICAL EOF, not to the header's data_size: the
         * whole point of salvage is that the header lies. A store
         * wiped by the v01.51 reset has data_size = 0 while its
         * records are still physically intact past the header, and a
         * store whose header lagged its block writes holds them past
         * data_size. read() coming back short (EOF) ends the scan. */
        byte[] header = new byte[BLOCK_HEADER_SIZE];
        int[] liveIds = new int[512];
        int[] liveOffsets = new int[512];
        int[] liveSizes = new int[512];
        int liveCount = 0;
        int maxId = 0;
        int scanEndOffset = DB_HEADER_SIZE;

        int offset = DB_HEADER_SIZE;
        for (;;) {
            int readBack;

            /* VITA FIX (v01.53): never let a lying block header drag
             * the scan past the region a healthy store could occupy.
             * Without this the "skip one block header and
             * resynchronize" path walks a bloated (or all-zero) file
             * 8 bytes at a time, and the tail zero-fill below then
             * rewrites every byte walked. */
            if (offset > DB_HEADER_SIZE + SALVAGE_MAX_SPAN) {
                scanEndOffset = offset;
                break;
            }

            try {
                dbFile.seek(offset);
                readBack = dbFile.read(header, 0, BLOCK_HEADER_SIZE);
            } catch (java.io.IOException ioe) {
                scanEndOffset = offset;
                break;
            }
            if (readBack != BLOCK_HEADER_SIZE) {
                scanEndOffset = offset;
                break; /* physical EOF: nothing more to salvage */
            }

            int id = RecordStoreUtil.getInt(header, 0);
            int dataSize = RecordStoreUtil.getInt(header, 4);
            int size = RecordStoreUtil.calculateBlockSize(dataSize);

            /* VITA FIX (v01.53): a live block is accepted only when its
             * payload is really inside the file AND its size is within
             * the per-suite budget. v01.52 accepted the header's word
             * (the log shows a garbage id 1615147117 taken as live),
             * and a garbage size then inflated writeOffset / the tail
             * zero-fill / data_size - which is what makes every later
             * space query answer 0 and the MIDlet report "not enough
             * RMS space". */
            if (id > 0 && dataSize >= 0 && size > 0 &&
                    liveCount < liveIds.length &&
                    blockFitsInFile(offset, size)) {
                /* Dedupe by id: a compact that was never physically
                 * truncated leaves an OLD copy of a record in the
                 * stale tail. compactRecords() moves data DOWN, so
                 * the lowest offset holds the newest copy - keep the
                 * first occurrence, skip every later one. */
                boolean duplicate = false;
                for (int j = 0; j < liveCount; j++) {
                    if (liveIds[j] == id) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    liveIds[liveCount] = id;
                    liveOffsets[liveCount] = offset;
                    liveSizes[liveCount] = size;
                    liveCount++;
                    if (id > maxId) {
                        maxId = id;
                    }
                }
                offset += size;
            } else if (id < 0 && dataSize >= 0 && size > 0 &&
                       blockFitsInFile(offset, size)) {
                /* VITA FIX (v01.54): a healthy FREE block (id = -1) must
                 * be stepped over by its FULL size, exactly like the
                 * canonical index walk does (see
                 * RecordStoreIndex.getRecordIDs / getFreeBlock: both do
                 * `currentOffset += currentSize` for live AND free
                 * blocks). v01.52/v01.53 skipped only the 8 byte block
                 * header here, so the scan walked straight into the
                 * free block's PAYLOAD, resynchronized on whatever
                 * record bytes were left there and accepted them as a
                 * "live block" - the stderr trail shows the result:
                 * `SALVAGE live=1 max_id=54307759` (a record id
                 * invented from payload bytes). Rewriting the store
                 * from that garbage rewrote record data and truncated
                 * the real records away, which is the "second launch:
                 * bookmarks gone" symptom. */
                offset += size;
            } else {
                /* A bad header (or full salvage lists): skip one block
                 * header and try to resynchronize. */
                offset += BLOCK_HEADER_SIZE;
            }
        }

        System.out.println("[rms] SALVAGE name='" + storeName +
            "' live=" + liveCount + " max_id=" + maxId +
            " old_data_size=" + oldDataSize +
            " scan_end=" + scanEndOffset);

        /* VITA FIX (v01.54): never destroy a store we failed to
         * understand. When the scan recovered NOTHING while the file
         * still holds bytes past the header, the parser is the thing
         * that failed, not the data - and the code below would zero
         * the whole data area, truncate the file to 40 bytes and
         * persist an empty header (the stderr trail shows exactly
         * that: `SALVAGE live=0 max_id=0 ... scan_end=5280`, i.e. a
         * 5 KB store physically erased, which is unrecoverable). The
         * v01.53 free-block fix (above) removes the known cause of
         * such a false negative; any remaining one must stay
         * harmless. Leaving the file untouched costs nothing: the
         * store still opens, and the bytes survive for a later,
         * better salvage. Only a file that carries nothing but the
         * header is safe to (re)initialise. */
        if (liveCount == 0 && scanEndOffset > DB_HEADER_SIZE) {
            System.out.println("[rms] SALVAGE skipped name='" + storeName +
                "': no live block found, " +
                (scanEndOffset - DB_HEADER_SIZE) +
                " bytes preserved");
            return;
        }

        /* Rewrite the live blocks densely from the top of the data
         * area. The very first salvage write also overwrites whatever
         * the v01.51 reset left behind. */
        int writeOffset = DB_HEADER_SIZE;
        int kept = 0;
        byte[] buf = new byte[COMPACT_BUFFER_SIZE];
        for (int i = 0; i < liveCount; i++) {
            int from = liveOffsets[i];
            int size = liveSizes[i];
            int done = 0;
            while (done < size) {
                int chunk = size - done;
                if (chunk > COMPACT_BUFFER_SIZE) {
                    chunk = COMPACT_BUFFER_SIZE;
                }
                int n;
                try {
                    dbFile.seek(from + done);
                    n = dbFile.read(buf, 0, chunk);
                } catch (java.io.IOException ioe) {
                    n = -1;
                }
                if (n <= 0) {
                    break; /* payload unreadable: drop the tail */
                }
                try {
                    dbFile.seek(writeOffset + done);
                    dbFile.write(buf, 0, n);
                } catch (java.io.IOException ioe) {
                    return; /* cannot write: keep the old header */
                }
                done += n;
            }
            if (done < size) {
                /* The payload ran out at physical EOF mid-block: this
                 * block and everything after it are unreliable. The
                 * partially copied bytes sit past writeOffset, so
                 * they are invisible to the rebuilt data_size. */
                break;
            }
            writeOffset += size;
            kept++;
        }

        /* Zero the region between the dense rewrite and the end of
         * the scan. The Vita truncate is only a logical clamp that
         * dies with the handle, so the physical stale tail would
         * otherwise survive - and the v01.52 probe would resurrect
         * it as "live blocks" on the next open, resurrecting records
         * the user deleted. A zeroed block header has id = 0, which
         * no walk ever treats as live. */
        if (writeOffset < scanEndOffset) {
            byte[] zeros = new byte[COMPACT_BUFFER_SIZE];
            try {
                for (int z = writeOffset; z < scanEndOffset; ) {
                    int chunk = scanEndOffset - z;
                    if (chunk > COMPACT_BUFFER_SIZE) {
                        chunk = COMPACT_BUFFER_SIZE;
                    }
                    dbFile.seek(z);
                    dbFile.write(zeros, 0, chunk);
                    z += chunk;
                }
            } catch (java.io.IOException ioe) {
                /* best effort: truncate below hides the tail anyway */
            }
        }

        /* Recompute the header from the kept content. next_id never
         * regresses: a deleted record's id must not be handed out
         * again (RMS spec), so keep the larger of the old next_id
         * and max salvaged id + 1. */
        int nextId = RecordStoreUtil.getInt(dbHeaderData, RS2_NEXT_ID);
        if (nextId < maxId + 1) {
            nextId = maxId + 1;
        }
        if (nextId < 1) {
            nextId = 1;
        }
        RecordStoreUtil.putInt(nextId, dbHeaderData, RS2_NEXT_ID);
        RecordStoreUtil.putInt(kept, dbHeaderData, RS3_NUM_LIVE);
        RecordStoreUtil.putInt(
            RecordStoreUtil.getInt(dbHeaderData, RS4_VERSION) + 1,
            dbHeaderData, RS4_VERSION);
        RecordStoreUtil.putLong(System.currentTimeMillis(), dbHeaderData,
            RS5_LAST_MODIFIED);
        RecordStoreUtil.putInt(writeOffset - DB_HEADER_SIZE, dbHeaderData,
            RS6_DATA_SIZE);
        RecordStoreUtil.putInt(0, dbHeaderData, RS7_FREE_SIZE);
        try {
            dbFile.seek(0);
            dbFile.write(dbHeaderData, 0, DB_HEADER_SIZE);
            /* Flush EVERYTHING (the dense rewrites + this header - a
             * 40 byte header write stays in the midp_file_cache!)
             * before the truncate clamps the tail. */
            dbFile.commitWrite();
            dbFile.truncate(writeOffset);
        } catch (java.io.IOException ioe) {
            /* header not persisted: next open salvages again */
        }
    }

    /**
     * Remove free blocks from the record store and compact records
     * with data into as small a space in <code>dbFile</code> as
     * possible.  Operates from smallest to greatest offset in
     * <code>dbFile</code>, copying data in chunks towards the
     * beginning of the file, and updating record store meta-data
     * as it progresses.
     *
     * Warning: This is a slow operation that scales linearly
     * with dbFile size.
     *
     * Warning: it is assumed that this method is only called while being
     * protected by record store lock.
     *
     * @exception RecordStoreNotOpenException if this record store
     *            is closed
     * @exception RecordStoreException if an error occurs during record
     *            store compaction
     */
    private void compactRecords()
        throws IOException {

        // VITA FIX (v01.51): the scan below MOVES record data and rewrites
        // RS6_DATA_SIZE/RS7_FREE_SIZE based on every 8-byte block header it
        // meets. On the Vita the data area can contain bytes that are not a
        // valid block header (Vita3K silently drops SCE_O_TRUNC and
        // pcsl_file_truncate is a logical, in-memory clamp that is lost on
        // close, so a stale tail survives in the LIVE region). One such
        // header makes the scan relocate garbage and shrink data_size, which
        // permanently breaks a store that was perfectly readable at open -
        // exactly the observed "first launch works, save a bookmark, second
        // launch wedged in init" on a store whose chain was already
        // inconsistent at open. Validate the chain with a READ-ONLY pass
        // first and refuse to touch anything that is not exactly
        // consistent: no bytes are moved, the header is left alone, the
        // store keeps working and new records simply append at the end.

        // check if the db can be compacted
        byte[] dbHeaderData = dbHeader.getHeaderData();
        if (RecordStoreUtil.getInt(dbHeaderData, RS7_FREE_SIZE) == 0) {
            // no free space to compact
            return;
        }

        // VITA FIX (v01.51): never relocate data based on a chain that is
        // not exactly consistent - see the note at the top of this method.
        if (!isBlockChainConsistent(dbHeaderData)) {
            System.out.println("[rms] compact skipped:"
                + " block chain inconsistent");
            return;
        }

        byte[] header = new byte[BLOCK_HEADER_SIZE];
        int currentId = 0;
        int currentOffset = RecordStoreImpl.DB_HEADER_SIZE;
        int currentSize = 0;
        int moveUpNumBytes = 0;

        // search through the data blocks for a free block that is large enough
        while (currentOffset < getSize()) {
            // seek to the next offset
            dbFile.seek(currentOffset);

            // read the block header
            if (dbFile.read(header) != RecordStoreImpl.BLOCK_HEADER_SIZE) {
                // could not read the block
                throw new IOException();
            }

            currentId = RecordStoreUtil.getInt(header, 0);
            currentSize =
              RecordStoreUtil.calculateBlockSize(RecordStoreUtil.getInt(
                                                   header, 4));

            // VITA FIX (v01.49): a corrupt block header can make
            // calculateBlockSize return <= 0, so currentOffset never
            // advances (or even rewinds) and this loop spins forever,
            // freezing the UI. Fail fast instead.
            if (currentSize <= 0) {
                throw new IOException();
            }

            if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                               "currentId = " + currentId +
                               " currentSize = " + currentSize);
            }

            // check if this is a free block or a record
            if (currentId < 0) {
                if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                    Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                                   "found free block at offset " +
                                   currentOffset);
                }

                // a free block, add to the moveUpNumBytes
                moveUpNumBytes += currentSize;

                // remove from the index
                dbIndex.removeBlock(currentOffset, header);
            } else {
                if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                    Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                                   "found record block at offset " +
                                   currentOffset);
                }

                // a record data block, check if it needs to be moved up
                if (moveUpNumBytes > 0) {

                    if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                        Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                                       "moveUpNumBytes = " + currentOffset);
                    }

                    int numMoved = 0;
                    while (numMoved < currentSize) {
                        int curRead = currentSize - numMoved;
                        if (curRead > COMPACT_BUFFER_SIZE) {
                            curRead = COMPACT_BUFFER_SIZE;
                        }

                        dbFile.seek(currentOffset + numMoved);
                        curRead = dbFile.read(compactBuffer, 0, curRead);
                        if (curRead == -1) {
                            throw new IOException();
                        }

                        dbFile.seek(currentOffset + numMoved - moveUpNumBytes);
                        dbFile.write(compactBuffer, 0, curRead);
                        // dbFile.commitWrite();
                        numMoved += curRead;
                    }

                    dbIndex.updateBlock(currentOffset - moveUpNumBytes, header);
                }
            }

            // added the block size to the currentOffset
            currentOffset += currentSize;
        }

        // check if the db file can be truncated
        if (moveUpNumBytes > 0) {
            // VITA FIX (v01.50): the loop above can mistake stale bytes
            // (Vita3K never physically truncates; an unnamed store's
            // db/idx share ONE file so stale tails survive) for a huge
            // free block. Subtracting it made data_size NEGATIVE and
            // that negative header was written to disk, permanently
            // bricking the store (observed: 2584 - 2880 = -296, then
            // getSize() = -256 -> "nothing is ever saved"). Never write
            // an impossible header: abort the compact instead and let
            // the sanitizer at open reset the store on next launch.
            int compactedSize =
                RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE) -
                    moveUpNumBytes;
            if (compactedSize < 0 || moveUpNumBytes >
                    RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE)) {
                return;
            }
            RecordStoreUtil.putInt(compactedSize,
                dbHeaderData, RS6_DATA_SIZE);
            RecordStoreUtil.putInt(0, dbHeaderData, RS7_FREE_SIZE);
            dbFile.seek(RS6_DATA_SIZE);
            dbFile.write(dbHeaderData, RS6_DATA_SIZE, 4+4);
            dbHeader.headerUpdated(dbHeaderData);
            // dbFile.commitWrite();

            dbFile.truncate(getSize());

            if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                               "compactRecords, truncate to size " + 
                               getSize());
            }
        }
    }

    /**
     * Set the record in the block to the data passed in and adds any remaining
     * space to the free list.
     *
     * @param blockOffset the offset in db file to the block to split
     * @param header the header of the block to split
     * @param newData the new data to store in the record
     * @param offset the index into the data buffer of the first
     *          relevant byte for this record
     * @param numBytes the number of bytes of the data buffer to use
     *          for this record
     *
     * @exception IOException if there is an error accessing the db file
     */
    private void splitBlock(int blockOffset, byte[] header,
                            byte[] newData, int offset,
                            int numBytes) throws IOException {

        if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
            Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                           "splitBlock("+RecordStoreUtil.getInt(header, 0) +
                           ") old numBytes = " + RecordStoreUtil.getInt(header, 4) +
                           " new numBytes=" + numBytes);
        }

        // calculate the size of the block
        int oldBlockSize =
          RecordStoreUtil.calculateBlockSize(RecordStoreUtil.getInt(
                                               header, 4));

        // calculate the size of the block
        int newBlockSize = RecordStoreUtil.calculateBlockSize(numBytes);

        // update the block header
        RecordStoreUtil.putInt(numBytes, header, 4);

        // seek to the location and write the block and header
        writeBlock(blockOffset, header, newData, offset, numBytes);

        // check if there is any left over free space
        int freeSize = oldBlockSize - newBlockSize - BLOCK_HEADER_SIZE;
        if (freeSize >= 0) {
            // update the block header and free block of extra space
            RecordStoreUtil.putInt(freeSize, header, 4);
            freeBlock(blockOffset+newBlockSize, header);
        }
    }

    /**
     * Adds a block for the record and data or sets an existing block to
     * the data.  Splits an exiting block if needed.
     * 
     * Warning: it is assumed that this method is only called while being
     * protected by record store lock.
     *
     * @param recordId the ID of the record to use in this operation
     * @param data the new data to store in the record
     * @param offset the index into the data buffer of the first
     *          relevant byte for this record
     * @param numBytes the number of bytes of the data buffer to use
     *          for this record
     *
     * @exception RecordStoreFullException if the operation cannot be
     *          completed because the record store has no more room
     * @exception IOException if there is an error accessing the db file
     * @exception RecordStoreException if the new consumption is gong to
     *            exceed the resource limit
     *
     * @return the offset in the db file of the block added
     */
    private int addBlock(int recordId, byte[] data, int offset, int numBytes)
        throws IOException, RecordStoreFullException, RecordStoreException {

        if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
            Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                           "addBlock("+recordId+") numBytes="+numBytes);
        }

        // calculate the size of the block needed
        int blockSize = RecordStoreUtil.calculateBlockSize(numBytes);
        int blockOffset = 0;
        byte[] dbHeaderData = dbHeader.getHeaderData();
        int freeBlocksSize = RecordStoreUtil.getInt(dbHeaderData, 
                RS7_FREE_SIZE);

        // initialize the block header
        byte[] header = new byte[BLOCK_HEADER_SIZE];

        // check if there is the potential for a large enough free block
        if (freeBlocksSize >= blockSize) {

            if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                               "blockSize = " + blockSize +
                               "free size = " + freeBlocksSize);
            }

            // initialize the number of bytes and search for an
            // available free block
            RecordStoreUtil.putInt(numBytes, header, 4);
            blockOffset = dbIndex.getFreeBlock(header);
        }

        // set the recordId in to the block header
        RecordStoreUtil.putInt(recordId, header, 0);

        if (blockOffset > 0) {
            // search found a block, use it
            splitBlock(blockOffset, header, data, offset, numBytes);
        } else {
            // search failed, add a new block to the end of the db file
            int spaceAvailable = getSizeAvailable();
            /**
             * spaceAvailable returns the smaller number of total space
             * available and the storage limit per suite. If it is less than
             * the block size to be added, RecordStoreFullException is thrown
             */
            if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                               "spaceAvailable = "+spaceAvailable);
            }

            // Is there room to grow the file?
            if (spaceAvailable < blockSize) {
                 // Is there enough room totally: in storage and free blocks?
                 if (spaceAvailable + freeBlocksSize < blockSize) {
                     throw new RecordStoreFullException();
                 }
                 compactRecords();
            }

            blockOffset = getSize();

            // seek to the location and write the block and header
            RecordStoreUtil.putInt(numBytes, header, 4);
            writeBlock(blockOffset, header, data, offset, numBytes);

            // update the db data size
            RecordStoreUtil.putInt(RecordStoreUtil.getInt(
                    dbHeaderData, RS6_DATA_SIZE) + blockSize, 
                    dbHeaderData, RS6_DATA_SIZE);
            dbFile.seek(RS6_DATA_SIZE);
            dbFile.write(dbHeaderData, RS6_DATA_SIZE, 4);
            dbHeader.headerUpdated(dbHeaderData);
            // dbFile.commitWrite();

            if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
                Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                               "blockOffset = "+blockOffset);
            }
        }

        return blockOffset;
    }

    /**
     * Mark the block at the given offset in db file as free.
     *
     * @param blockOffset the offset in db file to the block to free
     * @param header the header of the block to free
     *
     * @exception IOException if there is an error accessing the db file
     */
    private void freeBlock(int blockOffset, byte[] header) throws IOException {
        int dataSize = RecordStoreUtil.getInt(header, 4);

        // calculate the size of the block
        int blockSize = RecordStoreUtil.calculateBlockSize(dataSize);

        if (Logging.REPORT_LEVEL <= Logging.INFORMATION) {
            Logging.report(Logging.INFORMATION, LogChannels.LC_RMS,
                           "freeBlock(" + blockOffset + ") size = " +
                           blockSize);
        }

        // mark the block as free
        RecordStoreUtil.putInt(-1, header, 0);
        RecordStoreUtil.putInt(blockSize - BLOCK_HEADER_SIZE, header, 4);

        // save the updated block header
        writeBlock(blockOffset, header, null, 0, 0);

        // add to the db free size
        byte[] dbHeaderData = dbHeader.getHeaderData();
        RecordStoreUtil.putInt(RecordStoreUtil.getInt(
                dbHeaderData, RS7_FREE_SIZE) + blockSize,
                dbHeaderData, RS7_FREE_SIZE);
        dbFile.seek(RS7_FREE_SIZE);
        dbFile.write(dbHeaderData, RS7_FREE_SIZE, 4);
        dbHeader.headerUpdated(dbHeaderData);
        // dbFile.commitWrite();
    }

    /**
     * Writes a block to the db file at the given offset.
     *
     * @param blockOffset the offset in db file to write the block
     * @param header the header of the block to write
     * @param data the new data to store in the record
     * @param offset the index into the data buffer of the first
     *          relevant byte for this record
     * @param numBytes the number of bytes of the data buffer to use
     *          for this record
     *
     * @exception IOException if there is an error accessing the db file
     */
    private void writeBlock(int blockOffset, byte[] header,
                            byte[] data, int offset, int numBytes)
        throws IOException {

        int remainder;
        dbFile.seek(blockOffset);
        dbFile.write(header);
        if (data != null && numBytes > 0) {
            dbFile.write(data, offset, numBytes);
            remainder = numBytes % BLOCK_HEADER_SIZE;
            if (remainder != 0) {
                // DB_SIGNATURE used here as meaningless pad bytes
                dbFile.write(DB_SIGNATURE, 0, BLOCK_HEADER_SIZE - remainder);
            }
        }
        // flush the writes
        // dbFile.commitWrite();
        // update the index
        dbIndex.updateBlock(blockOffset, header);
    }

    /**
     * Locks this record store.
     */
    private void lockRecordStore() {
        recordStoreLock.obtain();
        dbHeader.recordStoreLocked();
    }

    /**
     * Unlocks this record store.
     */
    private void unlockRecordStore() {
        dbHeader.recordStoreAboutToBeUnlocked();
        recordStoreLock.release();        
    }

    /**
     * Creates a RecordStoreImpl instance; for internal use only.
     * Callers from outside must use <code>openRecordStore()</code>.
     *
     * @param suiteId unique ID of the suite that owns the store
     * @param recordStoreName a string to name the record store
     * @param create if true, create the record store if it doesn't exist
     *
     * @exception RecordStoreException if something goes wrong setting up
     *            the new RecordStore.
     * @exception RecordStoreNotFoundException if can't find the record store
     *            and create is set to false.
     * @exception RecordStoreFullException if there is no room in storage
     *            to create a new record store
     */
    private RecordStoreImpl(SecurityToken token, 
            int suiteId, String recordStoreName, boolean create)
        throws RecordStoreException, RecordStoreNotFoundException {

        this.suiteId = suiteId;
        recordStoreLock = RecordStoreLockFactory.getLockInstance(
                token, suiteId, recordStoreName);

        /*
         * Even if this MIDlet doesn't have this record store opened yet,
         * it is possible that some other MIDlet already opened it and 
         * is in process of writing something into it, so we have to obtain 
         * a lock before proceeding further.
         */
        recordStoreLock.obtain();

        try {
            boolean exists = RecordStoreUtil.exists(
                    RmsEnvironment.getSecureFilenameBase(suiteId),
                    recordStoreName, RecordStoreFile.DB_EXTENSION);

            // Check for errors between app and record store existance.
            if (!create && !exists) {
                throw new RecordStoreNotFoundException("cannot find record "
                        + "store file");
            }

            /*
             * If a new RecordStoreImpl will be created in storage,
             * check to see if the space required is available.
             */
            if (create && !exists) {
                int space = RecordStoreFile.spaceAvailableNewRecordStore(
                        suiteId);
                if (space - DB_HEADER_SIZE < 0) {
                    throw new RecordStoreFullException();
                }
            }

            // Create a RecordStoreFile for storing the record store.
            try {
                dbFile = new RecordStoreFile(suiteId, recordStoreName,
                        RecordStoreFile.DB_EXTENSION);

                // allocate a new header
                byte[] dbHeaderData = new byte[DB_HEADER_SIZE];

                if (exists) {
                    // load header
                    dbFile.read(dbHeaderData);

                    /* VITA (v01.54) diagnostic: the store's identity and
                     * the on-disk header as READ, before any repair.
                     * Every "second launch lost my data" report has to be
                     * answerable from the log alone: which store, which
                     * file state, which salvage. */
                    System.out.println("[rms] open name='" + recordStoreName
                        + "' suite=" + suiteId + " exists=1 next="
                        + RecordStoreUtil.getInt(dbHeaderData, RS2_NEXT_ID)
                        + " live="
                        + RecordStoreUtil.getInt(dbHeaderData, RS3_NUM_LIVE)
                        + " ver="
                        + RecordStoreUtil.getInt(dbHeaderData, RS4_VERSION)
                        + " data="
                        + RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE)
                        + " free="
                        + RecordStoreUtil.getInt(dbHeaderData, RS7_FREE_SIZE));

                    /*
                     * Verify that the file is actually a record store
                     * by verifying the record store "signature."
                     */
                    for (int i = 0; i < DB_SIGNATURE.length; i++) {
                        if (dbHeaderData[i] != DB_SIGNATURE[i]) {
                            throw new RecordStoreException("invalid record " +
                                    "store contents");
                        }
                    }
                    
                    // VITA FIX (v01.50 -> v01.52): a corrupted db header
                    // (observed: RS6_DATA_SIZE = -296 after an unnamed
                    // store's db/idx handles share ONE file, so stray
                    // writes and the v01.47 logical truncate leave
                    // garbage behind) makes getSize() negative; every
                    // record read finds nothing and every addBlock
                    // computes a negative offset. v01.50 reset such a
                    // store to empty, which threw away every record
                    // the physical blocks still held ("second launch:
                    // bookmarks gone"). v01.52 SALVAGES instead: the
                    // physical blocks are scanned, the live records
                    // are rewritten densely and the header is rebuilt
                    // from what was actually recovered - only a store
                    // with no salvageable block at all ends up empty.
                    if (RecordStoreUtil.getInt(dbHeaderData, RS2_NEXT_ID)
                            < 1 ||
                        RecordStoreUtil.getInt(dbHeaderData, RS3_NUM_LIVE)
                            < 0 ||
                        RecordStoreUtil.getInt(dbHeaderData, RS6_DATA_SIZE)
                            < 0 ||
                        RecordStoreUtil.getInt(dbHeaderData, RS7_FREE_SIZE)
                            < 0) {
                        System.out.println("[rms] salvage (bad header)"
                            + " name='" + recordStoreName + "'"
                            + " data_size=" + RecordStoreUtil.getInt(
                                dbHeaderData, RS6_DATA_SIZE)
                            + " free=" + RecordStoreUtil.getInt(
                                dbHeaderData, RS7_FREE_SIZE));
                        salvageStore(recordStoreName, dbHeaderData);
                    }

                    // VITA FIX (v01.51 -> v01.52): the header can look
                    // sane while the block chain in the data area does
                    // not (observed: a 2584 byte data area whose second
                    // block header claims a 1.6 GB size, so every walk
                    // in the index jumps past the end and the MIDlet
                    // finds no records at all). v01.51 reset such a
                    // store to empty - and destroyed the 5856-byte
                    // store holding the user's bookmarks (the hexdump
                    // proof: version=1 next_id=1 data_size=0 are the
                    // v01.51 reset's exact signature values). v01.52
                    // salvages the physical blocks instead; see
                    // salvageStore().
                    if (!isBlockChainConsistent(dbHeaderData)) {
                        System.out.println("[rms] salvage (bad chain)"
                            + " name='" + recordStoreName + "'"
                            + " live=" + RecordStoreUtil.getInt(
                                dbHeaderData, RS3_NUM_LIVE)
                            + " data_size=" + RecordStoreUtil.getInt(
                                dbHeaderData, RS6_DATA_SIZE));
                        salvageStore(recordStoreName, dbHeaderData);
                    } else if (RecordStoreUtil.getInt(dbHeaderData,
                                       RS3_NUM_LIVE) == 0 &&
                               physicalFileHasLiveBlocks()) {
                        // VITA FIX (v01.52): the third damage shape:
                        // the header claims an EMPTY store while live
                        // blocks are still physically present. This is
                        // exactly what the v01.51 reset produced (a
                        // 40-byte empty header written over intact
                        // records), and also what a lost header write
                        // after successful block writes looks like.
                        // Salvage recovers the records - including the
                        // bookmarks wiped by v01.51 on this very
                        // device.
                        System.out.println("[rms] salvage (empty header,"
                            + " live blocks present) name='" + recordStoreName
                            + "' data_size="
                            + RecordStoreUtil.getInt(dbHeaderData,
                                RS6_DATA_SIZE));
                        salvageStore(recordStoreName, dbHeaderData);
                    }

                } else {
                    // VITA (v01.54) diagnostic, see the exists=1 case.
                    System.out.println("[rms] open name='" + recordStoreName
                        + "' suite=" + suiteId + " exists=0 (new store)");

                    // initialize the header
                    for (int i = 0; i < DB_SIGNATURE.length; i++) {
                        dbHeaderData[i] = DB_SIGNATURE[i];
                    }

                    RecordStoreUtil.putInt(1, dbHeaderData, RS2_NEXT_ID);
                    RecordStoreUtil.putLong(System.currentTimeMillis(), 
                            dbHeaderData, RS5_LAST_MODIFIED);

                    // write the header to the file
                    dbFile.write(dbHeaderData);
                    dbFile.commitWrite();
                }

                dbHeader = new RecordStoreSharedDBHeader(suiteId, 
                        recordStoreName, dbHeaderData);                

                // create the index object
                dbIndex = new RecordStoreIndex(this, suiteId, recordStoreName);

            } catch (java.io.IOException ioe) {
                try {
                    if (dbFile != null) {
                        dbFile.close();
                    }
                } catch (java.io.IOException ioe2) {
                    // ignore exception within exception block
                }

                if (!exists) {
                    // avoid preserving just created damaged files
                    RecordStoreUtil.quietDeleteFile(
                            RmsEnvironment.getSecureFilenameBase(suiteId),
                            recordStoreName, RecordStoreFile.DB_EXTENSION);
                    RecordStoreIndex.deleteIndex(suiteId, recordStoreName);
                }

                dbFile = null;
                throw new RecordStoreException("error opening record store " +
                        "file");
            }
        } finally {
            recordStoreLock.release();
        }
    }
}
