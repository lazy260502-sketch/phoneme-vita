/*
 * test_vitagenuconv.c - host test for phoneme-midp's vitaGenConv.c.
 *
 * Why: the converter runs inside the VM on the Vita, where a wrong table
 * only shows up as mojibake in a game.  This program links the same
 * source on the host (with a stub kni.h) so the tables and the state
 * machine can be checked against Python's codecs for many samples:
 *
 *     gcc -O1 -I tools/charsetgen/hoststub \
 *         -I phoneme-midp/src/i18n/i18n_port/include \
 *         -I phoneme-midp/src/i18n/i18n_port/vita/native \
 *         -o /tmp/vgc tools/charsetgen/test_vitagenuconv.c \
 *         phoneme-midp/src/i18n/i18n_port/vita/native/vitaGenConv.c
 *
 * Modes (see tools/charsetgen/verify_charsets.py):
 *   dec <ENC>   stdin: one hex byte string per line
 *               stdout: "U+XXXX U+XXXX ..." per line
 *   enc <ENC>   stdin: one "U+XXXX U+XXXX" line
 *               stdout: hex byte string per line ('?' when unmappable)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <conv.h>

static LcConvMethods find_conv(const char *name) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%s", name);
    return getLcGenConvMethods(buf);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv) {
    char line[8192];
    LcConvMethods m;

    if (argc < 3) {
        fprintf(stderr, "usage: %s dec|enc ENCODING\n", argv[0]);
        return 2;
    }
    m = find_conv(argv[2]);
    if (m == NULL) {
        fprintf(stderr, "unsupported: %s\n", argv[2]);
        return 3;
    }

    if (strcmp(argv[1], "dec") == 0) {
        while (fgets(line, sizeof(line), stdin) != NULL) {
            unsigned char in[2048];
            jchar out[2048];
            int n = 0;
            int i;
            int cnt;
            int k;

            for (i = 0; line[i] != 0 && line[i] != '\n'; i += 2) {
                int hi = hexval(line[i]);
                int lo = hexval(line[i + 1]);
                if (hi < 0 || lo < 0) break;
                in[n++] = (unsigned char)((hi << 4) | lo);
            }
            cnt = m->nativeToUnicode(in, n, out, 2048);
            for (k = 0; k < cnt; k++) {
                printf(k ? " U+%04X" : "U+%04X", (unsigned)out[k]);
            }
            printf("\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "enc") == 0) {
        while (fgets(line, sizeof(line), stdin) != NULL) {
            jchar in[1024];
            unsigned char out[4096];
            int n = 0;
            int i = 0;
            int cnt;
            int k;

            while (line[i] != 0 && n < 1024) {
                while (line[i] == ' ' || line[i] == '\t') i++;
                if (line[i] == 0 || line[i] == '\n') break;
                if ((line[i] == 'U' || line[i] == 'u') && line[i + 1] == '+') {
                    i += 2;
                }
                {
                    unsigned int v = 0;
                    int digits = 0;
                    while (hexval(line[i]) >= 0) {
                        v = (v << 4) | (unsigned int)hexval(line[i]);
                        i++;
                        digits++;
                    }
                    if (digits > 0) in[n++] = (jchar)v;
                }
            }
            cnt = m->unicodeToNative(in, n, out, 4096);
            for (k = 0; k < cnt; k++) {
                printf("%02X", out[k]);
            }
            printf("\n");
        }
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
