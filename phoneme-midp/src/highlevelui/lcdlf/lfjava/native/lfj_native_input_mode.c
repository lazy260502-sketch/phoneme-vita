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
#include <kni.h>
#include <midpUtilKni.h>
#include <sni.h>
#include <string.h>
#include <midpError.h>
#include <pcsl_string.h>
#include <commonKNIMacros.h>
#include <nim.h>

#ifdef WINCE
#include <sipapi.h>
#endif

#ifdef VITA
/* v02.52: system-keyboard bridge lives in the vita-port layer (vita_ime.c),
 * compiled into the midp_vita executable. These are resolved at final link
 * (libobj.a is linked with --whole-archive before vita_ime.o). */
extern void vita_ime_show(void);
extern void vita_ime_abort(void);
/* v02.64: focus flag read by the SELECT key handler (vita_input.c). */
extern void vita_ime_set_text_wanted(int on);
/* v02.74: current TextBox content, opened as the dialog's initial text. */
extern void vita_ime_set_initial_text(const unsigned short *text, int len,
                                      int max_len);
#include <midpMalloc.h>
#endif

#define numElems(x) sizeof(x)/sizeof(x[0])

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WINCE
extern jboolean bVirtualModeEnabled;
extern int midpPaintAllowed;
#endif

/* Macro to retrieve C structure representation of an Object */

typedef struct Java_com_sun_midp_chameleon_input_NativeInputMode _NativeInputMode;

#define getNativeInputModePtr(handle) (unhand(_NativeInputMode,(handle)))

/* the pair of values that identifies a NativeInputMethod instance */

#define NIM_IDENTITY(thisObject)  getNativeInputModePtr(thisObject)->id, &getNativeInputModePtr(thisObject)->instanceData

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_initialize) {
    int rc = 0;
    jint id = KNI_GetParameterAsInt(1);

    KNI_StartHandles(3);
    KNI_DeclareHandle(thisObject);
    KNI_DeclareHandle(flags2D);
    KNI_DeclareHandle(flags1D);
    KNI_GetThisPointer(thisObject);

    getNativeInputModePtr(thisObject)->id = id;

    if(0 != (rc = nim_initialize(NIM_IDENTITY(thisObject) ))) {
        KNI_ReturnInt(rc);
    } else {
        constraint_map* flags = nim_get_constraint_map(NIM_IDENTITY(thisObject));
        int j,i;
        /* store the array object address into the handle */
        *((jobject_array**)flags2D) =(jobject_array*) 
	  (getNativeInputModePtr(thisObject)->isMap);
        for(j=0; j<NIM_CONSTRAINT_MAP_NROWS; j++) {
            KNI_GetObjectArrayElement(flags2D,j,flags1D);
            for (i = 0; i < NIM_CONSTRAINT_MAP_NCOLS; i++) {
                KNI_SetBooleanArrayElement(flags1D, (jint)i, (*flags)[j][i]);
            }
        }
    }
    KNI_EndHandles();
    KNI_ReturnInt(0); /* return zero for ok, non-zero if bad id */
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_finalize) {
    KNI_StartHandles(1);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    nim_finalize(NIM_IDENTITY(thisObject));
    KNI_EndHandles();
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_supportsConstraints) {
    jboolean res = KNI_FALSE;
    jint constraints = KNI_GetParameterAsInt(1);
    KNI_StartHandles(1);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    res = nim_supports_constraints(NIM_IDENTITY(thisObject), constraints);
    KNI_EndHandles();
    KNI_ReturnBoolean(res);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_getName) {
    pcsl_string xname = PCSL_STRING_NULL_INITIALIZER;
    pcsl_string* name = &xname;
    pcsl_string_status errc;

    KNI_StartHandles(2);
    KNI_DeclareHandle(tempHandle);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);

    errc = nim_get_name(NIM_IDENTITY(thisObject), name);
    if (PCSL_STRING_OK != errc) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
    } else {
        GET_PCSL_STRING_DATA_AND_LENGTH(name)
        if (PCSL_STRING_PARAMETER_ERROR(name)) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
        } else {
            KNI_NewString(name_data, name_len, tempHandle);
        }
        RELEASE_PCSL_STRING_DATA_AND_LENGTH
        pcsl_string_free(name);
    }

    KNI_EndHandlesAndReturnObject(tempHandle);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_getCommandName) {
    pcsl_string xname = PCSL_STRING_NULL_INITIALIZER;
    pcsl_string* name = &xname;
    pcsl_string_status errc;

    KNI_StartHandles(2);
    KNI_DeclareHandle(tempHandle);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);

    errc = nim_get_command_name(NIM_IDENTITY(thisObject), name);
    if (PCSL_STRING_OK != errc) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
    } else {
        GET_PCSL_STRING_DATA_AND_LENGTH(name)
        if (PCSL_STRING_PARAMETER_ERROR(name)) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
        } else {
            KNI_NewString(name_data, name_len, tempHandle);
        }
        RELEASE_PCSL_STRING_DATA_AND_LENGTH
    }

    KNI_EndHandlesAndReturnObject(tempHandle);
}

KNIEXPORT KNI_RETURNTYPE_VOID 
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_beginInput0) {
    KNI_StartHandles(2);

    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    GET_PARAMETER_AS_PCSL_STRING(2, inputSubset)
    {
        jint constraints = KNI_GetParameterAsInt(3);
        nim_begin_input(NIM_IDENTITY(thisObject),&inputSubset,constraints);
    }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_processKey0) {
    jint key = KNI_GetParameterAsInt(1);
    jboolean longPress = KNI_GetParameterAsBoolean(2);
    jint isClearKey = KNI_GetParameterAsInt(3);
    jint res = 0;
    state_data stateData;
    pcsl_string xstringParam = PCSL_STRING_NULL_INITIALIZER;
    pcsl_string * stringParam = &xstringParam;
    KNI_StartHandles(3);
    KNI_DeclareHandle(stringRes);
    KNI_DeclareHandle(stateArr);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    KNI_GetParameterAsObject(4, stateArr);
    KNI_GetRawArrayRegion(stateArr,0,STATE_DATA_ARRAY_SIZE*sizeof(jint),(jbyte*)stateData);
    res = nim_process_key(NIM_IDENTITY(thisObject), key, longPress, isClearKey, &stateData, stringParam);
    KNI_SetRawArrayRegion(stateArr,0,STATE_DATA_ARRAY_SIZE*sizeof(jint),(jbyte*)stateData);
    if (-1 == res) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
    } else if (1 == res){
        GET_PCSL_STRING_DATA_AND_LENGTH(stringParam)
        if (PCSL_STRING_PARAMETER_ERROR(stringParam)) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
        } else {
            KNI_NewString(stringParam_data, stringParam_len, stringRes);
        }
        RELEASE_PCSL_STRING_DATA_AND_LENGTH
    } /* else no string, do nothing */

    KNI_EndHandlesAndReturnObject(stringRes);
}

KNIEXPORT KNI_RETURNTYPE_CHAR
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_getPendingChar) {
    jint res = 0;
    KNI_StartHandles(1);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    res = nim_get_pending_char(NIM_IDENTITY(thisObject));
    KNI_EndHandles();
    KNI_ReturnChar(res);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_getNextMatch) {
    pcsl_string xname = PCSL_STRING_NULL_INITIALIZER;
    pcsl_string* name = &xname;
    pcsl_string_status errc;

    KNI_StartHandles(2);
    KNI_DeclareHandle(tempHandle);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);

    errc = nim_get_next_match(NIM_IDENTITY(thisObject), name);
    if (PCSL_STRING_OK != errc) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
    } else {
        GET_PCSL_STRING_DATA_AND_LENGTH(name)
        if (PCSL_STRING_PARAMETER_ERROR(name)) {
            KNI_ThrowNew(midpOutOfMemoryError, NULL);
        } else {
            KNI_NewString(name_data, name_len, tempHandle);
        }
        RELEASE_PCSL_STRING_DATA_AND_LENGTH
    }

    KNI_EndHandlesAndReturnObject(tempHandle);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_hasMoreMatches) {
    jboolean res;
    KNI_StartHandles(1);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    res = nim_has_more_matches(NIM_IDENTITY(thisObject));
    KNI_EndHandles();
    KNI_ReturnBoolean(res);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_getMatchList) {
    int n;
    int error=0;
    pcsl_string *matches;
    KNI_StartHandles(3);
    KNI_DeclareHandle(matchListObj);
    KNI_DeclareHandle(stringObj);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);

    error = PCSL_STRING_OK != nim_get_match_list(NIM_IDENTITY(thisObject), &matches,&n);

    do {
        int i;

        if (error) {
            break;
        }

        SNI_NewArray(SNI_STRING_ARRAY, n, matchListObj);

        if (KNI_IsNullHandle(matchListObj)) {
            break;
        }

        for (i = 0; i < n; i++) {
            const pcsl_string* const apath = &matches[i];
            GET_PCSL_STRING_DATA_AND_LENGTH(apath)
            if (PCSL_STRING_PARAMETER_ERROR(apath)) {
                error = 1;
            } else {
                KNI_NewString(apath_data, (jsize)apath_len, stringObj);
            }
            RELEASE_PCSL_STRING_DATA_AND_LENGTH
            if (error) {
                break;
            }
            KNI_SetObjectArrayElement(matchListObj, (jint)i, stringObj);
        }

    } while(0);

    if (error) {
        KNI_ThrowNew(midpOutOfMemoryError, NULL);
    }
    KNI_EndHandlesAndReturnObject(matchListObj);
}

KNIEXPORT KNI_RETURNTYPE_VOID 
KNIDECL(com_sun_midp_chameleon_input_NativeInputMode_endInput0) {
    KNI_StartHandles(1);
    KNI_DeclareHandle(thisObject);
    KNI_GetThisPointer(thisObject);
    nim_end_input(NIM_IDENTITY(thisObject));
    KNI_EndHandles();
    KNI_ReturnVoid();
}

/* an example how to return an array of strings
static char* java_input_mode_names[] =
{
        "KeyboardInputMode",
        "NumericInputMode",
        "AlphaNumericInputMode",
        "PredictiveTextInputMode",
        "SymbolInputMode",
};
KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_InputModeFactory_getJavaInputModeClassNames) {
    int error=0;
    KNI_StartHandles(2);
    KNI_DeclareHandle(nameListObj);
    KNI_DeclareHandle(stringObj);

    do {
        unsigned int i;

        SNI_NewArray(SNI_STRING_ARRAY, numElems(java_input_mode_names), nameListObj);

        if (KNI_IsNullHandle(nameListObj)) {
            break;
        }

        for (i = 0; i < numElems(java_input_mode_names); i++) {
            pcsl_string xname = PCSL_STRING_NULL_INITIALIZER;
            pcsl_string * name = &xname;
            pcsl_string_from_chars(java_input_mode_names[i],name);
            GET_PCSL_STRING_DATA_AND_LENGTH(name)
            if (PCSL_STRING_PARAMETER_ERROR(name)) {
                error = 1;
            } else {
                KNI_NewString(name_data, (jsize)name_len, stringObj);
            }
            RELEASE_PCSL_STRING_DATA_AND_LENGTH
            if (error) {
                break;
            }
            KNI_SetObjectArrayElement(nameListObj, (jint)i, stringObj);
        }

    } while(0);

    if (error) {
        KNI_ThrowNew(midpOutOfMemoryError, NULL);
    }
    KNI_EndHandlesAndReturnObject(nameListObj);
}
*/

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_chameleon_input_InputModeFactory_getInputModeIds) {
    int error=0;
    KNI_StartHandles(2);
    KNI_DeclareHandle(idListObj);

    jint n;
    jint* input_mode_ids = nim_get_input_mode_ids(&n);
    do {
        int i;

        SNI_NewArray(SNI_INT_ARRAY, n, idListObj);

        if (KNI_IsNullHandle(idListObj)) {
            break;
        }

        for (i = 0; i < n; i++) {
            KNI_SetIntArrayElement(idListObj, (jint)i, input_mode_ids[i]);
        }

    } while(0);

    if (error) {
        KNI_ThrowNew(midpOutOfMemoryError, NULL);
    }
    KNI_EndHandlesAndReturnObject(idListObj);
}

#if defined(WINCE) || defined(VITA)
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_chameleon_input_VirtualKeyboardInputMode_showNativeKeyboard) {
#ifdef WINCE
    bVirtualModeEnabled = KNI_TRUE;
    midpPaintAllowed = 0;
    SipShowIM(SIPF_ON);
#else
    vita_ime_set_text_wanted(1);
    vita_ime_show();
#endif
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_chameleon_input_VirtualKeyboardInputMode_hideNativeKeyboard) {
#ifdef WINCE
    bVirtualModeEnabled = KNI_FALSE;
    midpPaintAllowed = 0;
    SipShowIM(SIPF_OFF);
#else
    vita_ime_set_text_wanted(0);
    vita_ime_abort();
#endif
    KNI_ReturnVoid();
}

#ifdef VITA
/* v02.74: static native void setNativeKeyboardText(String text, int maxSize);
 * Java pushes the focused TextBox's content whenever it changes, so the
 * system keyboard (also opened natively by SELECT) starts with it. */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_chameleon_input_VirtualKeyboardInputMode_setNativeKeyboardText) {
    jint maxSize = KNI_GetParameterAsInt(2);
    jint len;
    jchar *buf = NULL;

    KNI_StartHandles(1);
    KNI_DeclareHandle(strHandle);
    KNI_GetParameterAsObject(1, strHandle);

    len = KNI_IsNullHandle(strHandle) ? 0 : KNI_GetStringLength(strHandle);
    if (len > 0) {
        buf = (jchar *)midpMalloc((size_t)len * sizeof(jchar));
        if (buf == NULL) {
            len = 0;                      /* OOM: open empty rather than stale */
        } else {
            KNI_GetStringRegion(strHandle, 0, len, buf);
        }
    } else {
        len = 0;
    }
    vita_ime_set_initial_text((const unsigned short *)buf, (int)len,
                              (int)maxSize);
    if (buf != NULL) {
        midpFree(buf);
    }
    KNI_EndHandles();
    KNI_ReturnVoid();
}
#endif
#endif

#ifdef __cplusplus
}
#endif


