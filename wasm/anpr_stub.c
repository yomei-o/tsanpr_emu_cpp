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

/* A canned result per output format.  The sample images are Japanese (img/JP),
 * so the text result is a representative JP plate.  These are placeholders — the
 * stub does not look at the image at all. */
static const char *canned_text = "\xE5\x93\x81\xE5\xB7\x9D 330 \xE3\x81\x82 12-34"; /* UTF-8: 品川 330 あ 12-34 */
static const char *canned_json =
    "{\"success\":true,\"engine\":\"stub\",\"results\":["
    "{\"text\":\"\xE5\x93\x81\xE5\xB7\x9D 330 \xE3\x81\x82 12-34\",\"conf\":0.98,"
    "\"area\":{\"x\":142,\"y\":88,\"width\":210,\"height\":70}}]}";
static const char *canned_yaml =
    "- text: \xE5\x93\x81\xE5\xB7\x9D 330 \xE3\x81\x82 12-34\n  conf: 0.98\n";

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
