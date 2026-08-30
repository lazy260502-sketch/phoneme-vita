#!/usr/bin/env python3
"""
Generate _suites.dat for phoneME MIDP
This creates a minimal suite database with one test MIDlet suite.
"""

import struct
import sys
import os

def utf16le(s):
    """Encode string as UTF-16LE with length prefix"""
    encoded = s.encode('utf-16-le')
    return struct.pack('<I', len(encoded)) + encoded

def align4(pos):
    """Align position to 4-byte boundary"""
    while pos % 4 != 0:
        pos += 1
    return pos

def create_suites_dat(suite_id, storage_id, jar_path, jar_size, suite_name, suite_vendor, suite_version, midlet_class, display_name):
    """
    Create _suites.dat binary file
    
    Format:
    - int numOfSuites
    - For each suite:
      - MidletSuiteData fixed part (MIDLET_SUITE_DATA_SIZE bytes)
      - jarHash (jarHashLen bytes, if > 0)
      - 8 strings (each: aligned jint length + UTF16 data)
        - midletClassName
        - displayName
        - iconName
        - suiteVendor
        - suiteName
        - suiteVersion
        - pathToJar
        - pathToSettings
    """
    
    # Fixed part of MidletSuiteData (excluding varSuiteData, isChecked, externalAppId, nextEntry)
    # suiteId (jint) + componentId (jint) + storageId (jint) + folderId (jint) +
    # isEnabled (jboolean) + isTrusted (jboolean) + isTemporary (jboolean) +
    # numberOfMidlets (jint) + installTime (jlong) + jadSize (jint) + jarSize (jint) +
    # suiteSize (jint) + jarHashLen (jint) + type (ComponentType = jint)
    fixed_size = 4 + 4 + 4 + 4 + 1 + 1 + 1 + 4 + 8 + 4 + 4 + 4 + 4 + 4
    
    # Variable length strings
    strings = [
        midlet_class,    # midletClassName
        display_name,    # displayName
        "",              # iconName
        suite_vendor,    # suiteVendor
        suite_name,      # suiteName
        suite_version,   # suiteVersion
        jar_path,        # pathToJar
        "",              # pathToSettings
    ]
    
    # Calculate variable data size
    var_data_size = 0
    if jar_size > 0:
        var_data_size += jar_size  # jarHashLen bytes
    
    for s in strings:
        var_data_size = align4(var_data_size + 4)  # length field
        var_data_size += len(s.encode('utf-16-le'))
    
    # Total suite entry size
    suite_entry_size = fixed_size + var_data_size
    
    # Build the buffer
    buffer = bytearray()
    
    # Number of suites
    buffer.extend(struct.pack('<I', 1))
    
    # Fixed part
    buffer.extend(struct.pack('<i', suite_id))           # suiteId
    buffer.extend(struct.pack('<i', 0))                  # componentId (UNUSED_COMPONENT_ID)
    buffer.extend(struct.pack('<i', storage_id))         # storageId (INTERNAL_STORAGE_ID = 1)
    buffer.extend(struct.pack('<i', 0))                  # folderId
    buffer.extend(struct.pack('B', 1))                   # isEnabled = true
    buffer.extend(struct.pack('B', 0))                   # isTrusted = false
    buffer.extend(struct.pack('B', 0))                   # isTemporary = false
    buffer.extend(struct.pack('<i', 1))                  # numberOfMidlets = 1
    buffer.extend(struct.pack('<q', 0))                  # installTime = 0
    buffer.extend(struct.pack('<i', 0))                  # jadSize = 0 (no JAD)
    buffer.extend(struct.pack('<i', jar_size))           # jarSize
    buffer.extend(struct.pack('<i', jar_size))           # suiteSize (approximate)
    buffer.extend(struct.pack('<i', 0))                  # jarHashLen = 0 (no hash)
    buffer.extend(struct.pack('<i', 0))                  # type = COMPONENT_REGULAR_SUITE = 0
    
    # Variable length data
    # jarHash (empty since jarHashLen = 0)
    
    # 8 strings
    for s in strings:
        pos = align4(len(buffer))
        while len(buffer) < pos:
            buffer.extend(b'\x00')
        buffer.extend(struct.pack('<I', len(s.encode('utf-16-le'))))
        buffer.extend(s.encode('utf-16-le'))
    
    return bytes(buffer)

def main():
    if len(sys.argv) < 2:
        print("Usage: create_suites_dat.py <output_path>")
        sys.exit(1)
    
    output_path = sys.argv[1]
    
    # Create a minimal suite database with one test MIDlet
    # Suite ID 1, internal storage, HelloMIDlet
    dat = create_suites_dat(
        suite_id=1,
        storage_id=1,  # INTERNAL_STORAGE_ID
        jar_path="ux0:/data/J2ME00001/00000001/Hello.jar",
        jar_size=1234,  # approximate size
        suite_name="Hello",
        suite_vendor="Vita",
        suite_version="1.0.0",
        midlet_class="HelloMIDlet",
        display_name="Hello"
    )
    
    with open(output_path, 'wb') as f:
        f.write(dat)
    
    print(f"Created _suites.dat at {output_path}")
    print(f"Size: {len(dat)} bytes")
    
    # Also create the suite directory structure
    suite_dir = os.path.dirname(output_path)
    os.makedirs(suite_dir, exist_ok=True)

if __name__ == '__main__':
    main()
