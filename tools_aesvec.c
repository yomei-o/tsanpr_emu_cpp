// Do the emulator's AES instructions compute AES?
//
// The emulator answers AESENC/AESENCLAST/AESDEC/AESDECLAST/AESIMC on the host's
// own AES unit where there is one - AES-NI on x86-64, ARMv8 Crypto on AArch64 -
// and with a byte-at-a-time S-box loop where there is not.  Those are three
// different implementations that must all produce FIPS-197, and the ARM one is
// not a one-to-one mapping: x86 fuses a round into one instruction where ARM
// splits it, so it is composed and could be composed wrongly.
//
// So this checks against the standard rather than against another build: the
// vectors below are FIPS-197's own (appendix C.1 and C.3, and B for the schedule).
// The raw single-instruction lines are printed as hex too, so two builds of the
// emulator can be diffed against each other as well:
//
//   x86_64-w64-mingw32-gcc -O2 -maes -o aesvec.exe tools_aesvec.c
//   ./x86emu aesvec.exe                                    # the host's AES unit
//   ./x86emu_soft aesvec.exe                               # -DX86EMU_NO_HOST_AES
#include <stdio.h>
#include <string.h>
#include <wmmintrin.h>

static void show(const char* label, __m128i v) {
    unsigned char b[16];
    _mm_storeu_si128((__m128i*)b, v);
    printf("%-22s", label);
    for (int i = 0; i < 16; i++) printf("%02x", b[i]);
    printf("\n");
}

static int same(__m128i v, const unsigned char* want) {
    unsigned char b[16];
    _mm_storeu_si128((__m128i*)b, v);
    return memcmp(b, want, 16) == 0;
}

static __m128i loadb(const unsigned char* p) { return _mm_loadu_si128((const __m128i*)p); }

// The AES-128 schedule as everyone writes it with AESKEYGENASSIST: the assist
// gives SubWord(RotWord(w3)) xor rcon in the top lane, then the running xor.
#define KEY128_STEP(k, rcon)                                    \
    do {                                                        \
        __m128i a = _mm_aeskeygenassist_si128((k), (rcon));      \
        a = _mm_shuffle_epi32(a, 0xff);                          \
        (k) = _mm_xor_si128((k), _mm_slli_si128((k), 4));        \
        (k) = _mm_xor_si128((k), _mm_slli_si128((k), 4));        \
        (k) = _mm_xor_si128((k), _mm_slli_si128((k), 4));        \
        (k) = _mm_xor_si128((k), a);                             \
    } while (0)

static int expand128(const unsigned char* key, __m128i rk[11]) {
    __m128i k = loadb(key);
    rk[0] = k;
    KEY128_STEP(k, 0x01); rk[1] = k;
    KEY128_STEP(k, 0x02); rk[2] = k;
    KEY128_STEP(k, 0x04); rk[3] = k;
    KEY128_STEP(k, 0x08); rk[4] = k;
    KEY128_STEP(k, 0x10); rk[5] = k;
    KEY128_STEP(k, 0x20); rk[6] = k;
    KEY128_STEP(k, 0x40); rk[7] = k;
    KEY128_STEP(k, 0x80); rk[8] = k;
    KEY128_STEP(k, 0x1b); rk[9] = k;
    KEY128_STEP(k, 0x36); rk[10] = k;
    return 11;
}

// AES-256 takes two words of assist per pair of round keys: the rcon step for the
// even one, and a 0xaa-shuffled zero-rcon step for the odd one.
static int expand256(const unsigned char* key, __m128i rk[15]) {
    __m128i a = loadb(key), b = loadb(key + 16);
    rk[0] = a;
    rk[1] = b;
    static const int rcon[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
    int n = 2;
    for (int i = 0; i < 7 && n < 15; i++) {
        __m128i t;
        switch (rcon[i]) {
            case 0x01: t = _mm_aeskeygenassist_si128(b, 0x01); break;
            case 0x02: t = _mm_aeskeygenassist_si128(b, 0x02); break;
            case 0x04: t = _mm_aeskeygenassist_si128(b, 0x04); break;
            case 0x08: t = _mm_aeskeygenassist_si128(b, 0x08); break;
            case 0x10: t = _mm_aeskeygenassist_si128(b, 0x10); break;
            case 0x20: t = _mm_aeskeygenassist_si128(b, 0x20); break;
            default:   t = _mm_aeskeygenassist_si128(b, 0x40); break;
        }
        t = _mm_shuffle_epi32(t, 0xff);
        a = _mm_xor_si128(a, _mm_slli_si128(a, 4));
        a = _mm_xor_si128(a, _mm_slli_si128(a, 4));
        a = _mm_xor_si128(a, _mm_slli_si128(a, 4));
        a = _mm_xor_si128(a, t);
        rk[n++] = a;
        if (n >= 15) break;
        __m128i u = _mm_aeskeygenassist_si128(a, 0x00);
        u = _mm_shuffle_epi32(u, 0xaa);
        b = _mm_xor_si128(b, _mm_slli_si128(b, 4));
        b = _mm_xor_si128(b, _mm_slli_si128(b, 4));
        b = _mm_xor_si128(b, _mm_slli_si128(b, 4));
        b = _mm_xor_si128(b, u);
        rk[n++] = b;
    }
    return n;
}

static __m128i encrypt(__m128i p, const __m128i* rk, int nk) {
    __m128i s = _mm_xor_si128(p, rk[0]);
    for (int i = 1; i < nk - 1; i++) s = _mm_aesenc_si128(s, rk[i]);
    return _mm_aesenclast_si128(s, rk[nk - 1]);
}

// The equivalent inverse cipher: the same round keys in reverse, each middle one
// through AESIMC, which is the only thing AESIMC is for.
static __m128i decrypt(__m128i c, const __m128i* rk, int nk) {
    __m128i s = _mm_xor_si128(c, rk[nk - 1]);
    for (int i = nk - 2; i >= 1; i--) s = _mm_aesdec_si128(s, _mm_aesimc_si128(rk[i]));
    return _mm_aesdeclast_si128(s, rk[0]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fail = 0;

    static const unsigned char key128[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const unsigned char key256[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
        0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
        0x1e, 0x1f};
    static const unsigned char plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    // FIPS-197 C.1 (AES-128) and C.3 (AES-256).
    static const unsigned char cipher128[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                                0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    static const unsigned char cipher256[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
                                                0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};

    __m128i rk128[11], rk256[15];
    int n128 = expand128(key128, rk128);
    int n256 = expand256(key256, rk256);

    __m128i c1 = encrypt(loadb(plain), rk128, n128);
    __m128i p1 = decrypt(c1, rk128, n128);
    __m128i c2 = encrypt(loadb(plain), rk256, n256);
    __m128i p2 = decrypt(c2, rk256, n256);

    show("aes128 cipher", c1);
    printf("  FIPS-197 C.1        %s\n", same(c1, cipher128) ? "PASS" : (fail = 1, "FAIL"));
    show("aes128 back", p1);
    printf("  round trip          %s\n", same(p1, plain) ? "PASS" : (fail = 1, "FAIL"));
    show("aes256 cipher", c2);
    printf("  FIPS-197 C.3        %s\n", same(c2, cipher256) ? "PASS" : (fail = 1, "FAIL"));
    show("aes256 back", p2);
    printf("  round trip          %s\n", same(p2, plain) ? "PASS" : (fail = 1, "FAIL"));

    // The instructions on their own, so two builds can be diffed byte for byte
    // even where a whole cipher would agree by luck.
    __m128i a = loadb(plain), b = loadb(key128);
    show("aesenc", _mm_aesenc_si128(a, b));
    show("aesenclast", _mm_aesenclast_si128(a, b));
    show("aesdec", _mm_aesdec_si128(a, b));
    show("aesdeclast", _mm_aesdeclast_si128(a, b));
    show("aesimc", _mm_aesimc_si128(a));
    show("aeskeygenassist:01", _mm_aeskeygenassist_si128(a, 0x01));
    show("aeskeygenassist:36", _mm_aeskeygenassist_si128(a, 0x36));
    show("rk128[10]", rk128[10]);
    show("rk256[14]", rk256[14]);

    printf("%s\n", fail ? "FAILED" : "all vectors PASS");
    return fail;
}
