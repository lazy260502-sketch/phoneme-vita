/* Host-only stub: the real kni.h drags in the whole VM header stack, and
 * the converter only needs the jchar type.  Used by
 * tools/charsetgen/test_vitagenuconv.c, never by the Vita build. */
#ifndef KNI_HOST_STUB_H
#define KNI_HOST_STUB_H
typedef unsigned short jchar;
#endif
