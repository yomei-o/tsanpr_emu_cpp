/*
 * anpr_stub.c — a fixed-value WebAssembly stand-in for the TS-ANPR engine.
 *
 * The real engine is a 167 MB ONNX model; it runs natively and (very slowly)
 * under the x86 emulator in this repo, but it cannot ship to a browser.  This
 * stub mirrors the TSANPR C API surface (see src/tsanpr.h) and returns FIXED,
 * canned recognition results so the API can be exercised from JavaScript/WASM.
 *
 * Built with Emscripten; the exported functions are called from JS via cwrap.
 */

#include <emscripten.h>
#include <string.h>
#include <stdio.h>

/* A canned result per output format.  This is NOT invented: it is the value the
 * real TS-ANPR engine actually returned for img/JP/licensePlate.jpg when run
 * under this repo's x86 emulator (see results/emulated_recognition.txt).  The
 * stub does not look at the image — it just replays that recognised plate so the
 * API can be demonstrated in a browser without the 167 MB engine. */
static const char *canned_text = "\xE5\xA4\x9A\xE6\x91\xA9""500\xE3\x81\x95""4649"; /* 多摩500さ4649 */
static const char *canned_json =
    "{\"success\":true,\"engine\":\"stub\",\"results\":["
    "{\"text\":\"\xE5\xA4\x9A\xE6\x91\xA9""500\xE3\x81\x95""4649\",\"conf\":0.97,"
    "\"area\":{\"x\":168,\"y\":92,\"width\":214,\"height\":72}}]}";
static const char *canned_yaml =
    "- text: \xE5\xA4\x9A\xE6\x91\xA9""500\xE3\x81\x95""4649\n  conf: 0.97\n";

/* Pick the canned answer that matches the requested output format. */
static const char *result_for(const char *outputFormat)
{
    if (outputFormat) {
        if (strstr(outputFormat, "json")) return canned_json;
        if (strstr(outputFormat, "yaml")) return canned_yaml;
    }
    return canned_text; /* "text" and anything else */
}

/* mode is e.g. "text;country=JP"; "" means success (same contract as the engine). */
EMSCRIPTEN_KEEPALIVE
const char *anpr_initialize(const char *mode)
{
    (void)mode;
    return ""; /* success */
}

EMSCRIPTEN_KEEPALIVE
const char *anpr_read_file(const char *imgFileName,
                           const char *outputFormat,
                           const char *options)
{
    (void)imgFileName;
    (void)options;
    return result_for(outputFormat);
}

EMSCRIPTEN_KEEPALIVE
const char *anpr_read_pixels(const unsigned char *pixels,
                             unsigned long width,
                             unsigned long height,
                             long stride,
                             const char *pixelFormat,
                             const char *outputFormat,
                             const char *options)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    (void)pixelFormat;
    (void)options;
    return result_for(outputFormat);
}
