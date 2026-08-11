/**
 * The MIT License (MIT)
 * Copyright © 2022-2025 TS-Solution Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to all conditions.
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsanpr.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <ctype.h>

TSANPR tsanpr;

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// Directory containing this executable, so the program works from any cwd.
static const char *baseDir()
{
    static char buffer[512];
    if (buffer[0])
        return buffer;
#ifdef _WIN32
    GetModuleFileNameA(NULL, buffer, sizeof(buffer));
#else
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    buffer[n > 0 ? n : 0] = 0;
#endif
    char *slash = strrchr(buffer, '/');
    char *back = strrchr(buffer, '\\');
    if (back > slash)
        slash = back;
    if (slash)
        *slash = 0;
    return buffer;
}

// Generate engine filename depending on platform
const char *getEngineFileName()
{
    static char buffer[512];
#ifdef _WIN32
    snprintf(buffer, sizeof(buffer), "%s\\engine\\tsanpr.dll", baseDir());
#else
    snprintf(buffer, sizeof(buffer), "%s/engine/libtsanpr.so", baseDir());
#endif
    return buffer;
}

// Convert UTF-8 string to wchar_t* on Windows
#ifdef _WIN32
#include <windows.h>
wchar_t *to_wstring(const char *str)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    wchar_t *wstr = (wchar_t *)malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, len);
    // Remove trailing null character
    if (len > 1 && wstr[len - 1] == L'\0')
        wstr[len - 1] = 0;
    return wstr;
}
#endif

// Read image file using anpr_read_file
void readImageFile(const char *imgfile, const char *outputFormat, const char *options)
{
    printf("%s (outputFormat=\"%s\", options=\"%s\") => ", imgfile, outputFormat, options);
    const char *result = tsanpr.anpr_read_file(imgfile, outputFormat, options);
    printf("%s\n", result);
}

// Read encoded image as binary and call anpr_read_pixels
void readEncodedImage(const char *imgfile, const char *outputFormat, const char *options)
{
    printf("%s (outputFormat=\"%s\", options=\"%s\") => ", imgfile, outputFormat, options);
    FILE *file = fopen(imgfile, "rb");
    if (!file)
    {
        printf("File open failed\n");
        return;
    }
    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (filesize <= 0)
    {
        printf("File size invalid or zero\n");
        fclose(file);
        return;
    }
    unsigned char *encodedImg = (unsigned char *)malloc(filesize);
    if (!encodedImg)
    {
        fclose(file);
        printf("Memory allocation failed\n");
        return;
    }
    size_t nread = fread(encodedImg, 1, filesize, file);
    if (nread != (size_t)filesize)
    {
        printf("File read error: expected %ld bytes, got %zu bytes\n", filesize, nread);
        free(encodedImg);
        fclose(file);
        return;
    }
    fclose(file);

    const char *result = tsanpr.anpr_read_pixels(
        encodedImg,
        filesize,
        0,
        0,
        "encoded",
        outputFormat,
        options);
    printf("%s\n", result);
    free(encodedImg);
}

// Convert file extension to lowercase and compare
int get_lowercase_ext(const char *filename, char *extbuf, size_t extbufsize)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || strlen(dot) < 2)
        return 0; // No extension
    size_t len = strlen(dot + 1);
    if (len + 1 > extbufsize)
        len = extbufsize - 1;
    for (size_t i = 0; i < len; ++i)
        extbuf[i] = (char)tolower((unsigned char)dot[1 + i]);
    extbuf[len] = '\0';
    return 1;
}

// Read PNG/JPEG file using stb_image (single-header, no external dependency).
// Returns stride in bytes, or -1 on failure. Output is BGR (3 channels).
int read_image(const char *filename, unsigned char **out_pixels, int *out_width, int *out_height, int *out_channels)
{
    int width = 0, height = 0, orig_channels = 0;
    // Force 3 channels; stb gives RGB order.
    unsigned char *rgb = stbi_load(filename, &width, &height, &orig_channels, 3);
    if (!rgb)
        return -1;

    // tsanpr expects BGR, so swap R and B in place.
    for (long i = 0, n = (long)width * height * 3; i < n; i += 3)
    {
        unsigned char t = rgb[i];
        rgb[i] = rgb[i + 2];
        rgb[i + 2] = t;
    }

    *out_pixels = rgb;
    *out_width = width;
    *out_height = height;
    *out_channels = 3;
    return width * 3;
}

// Get pixel format string for tsanpr based on channels
const char *getPixelFormat(int channels)
{
    if (channels == 1)
        return "GRAY";
    else if (channels == 2)
        return "BGR565";
    else if (channels == 3)
        return "BGR";
    else if (channels == 4)
        return "BGRA";
    return NULL;
}

// Load image file (PNG/JPEG) and call anpr_read_pixels
void readPixelBuffer(const char *imgfile, const char *outputFormat, const char *options)
{
    printf("%s (outputFormat=\"%s\", options=\"%s\") => ", imgfile, outputFormat, options);

    unsigned char *pixels = NULL;
    int width = 0, height = 0, channels = 0, stride = 0;
    const char *pixelFormat = NULL;
    int ok = 0;

    char ext[16] = {0};
    if (!get_lowercase_ext(imgfile, ext, sizeof(ext)))
    {
        printf("No file extension found!\n");
        return;
    }

    // Compare extension in lowercase
    if (strcmp(ext, "png") == 0)
    {
        stride = read_image(imgfile, &pixels, &width, &height, &channels);
        ok = stride > 0;
    }
    else if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0)
    {
        stride = read_image(imgfile, &pixels, &width, &height, &channels);
        ok = stride > 0;
    }
    else
    {
        printf("Unsupported image format!\n");
        return;
    }

    if (!ok)
    {
        perror("Image load failed!");
        if (pixels)
            free(pixels);
        return;
    }

    pixelFormat = getPixelFormat(channels);
    if (!pixelFormat)
    {
        perror("Unknown pixel format!");
        free(pixels);
        return;
    }

    const char *result = tsanpr.anpr_read_pixels(
        pixels,
        width,
        height,
        stride,
        pixelFormat,
        outputFormat,
        options);
    printf("%s\n", result);
    free(pixels);
}

int readLicensePlates(const char *countryCode)
{
    // NOTICE:
    // anpr_initialize should be called only once after library load.
    // Therefore, it is not possible to change the country code after anpr_initialize has been called.
    // While using the free trial license, you can try all languages.
    // When you purchase a commercial license, you can only use the selected language.
    char initParams[128];
    snprintf(initParams, sizeof(initParams), "text;country=%s", countryCode);
    const char *error = tsanpr.anpr_initialize(initParams);
    if (error && error[0])
    {
        printf("anpr_initialize() failed (error=%s)\n", error);
        return -1;
    }

    char imageDir[512];
    snprintf(imageDir, sizeof(imageDir), "%s/img/%s/", baseDir(), countryCode);

    // TODO: Try each function as needed
    void (*anprFunc)(const char *, const char *, const char *) = readImageFile;
    // void (*anprFunc)(const char *, const char *, const char *) = readEncodedImage;
    // void (*anprFunc)(const char *, const char *, const char *) = readPixelBuffer;

    // TODO: Try each output format as needed
    const char *outputFormat = "text";
    // const char* outputFormat = "json";
    // const char* outputFormat = "yaml";
    // const char* outputFormat = "xml";
    // const char* outputFormat = "csv";

    char path[1024];
    snprintf(path, sizeof(path), "%slicensePlate.jpg", imageDir);
    anprFunc(path, outputFormat, ""); // Single license plate recognition (default)

    snprintf(path, sizeof(path), "%smultiple.jpg", imageDir);
    anprFunc(path, outputFormat, "vm"); // Recognize multiple license plates attached to vehicles

    anprFunc(path, outputFormat, "vmb"); // Recognize multiple license plates including motorcycles

    snprintf(path, sizeof(path), "%ssurround.jpg", imageDir);
    anprFunc(path, outputFormat, "vms"); // Recognize multiple license plates with surround detection

    anprFunc(path, outputFormat, "dms"); // Recognize multiple surrounding objects (vehicles)

    anprFunc(path, outputFormat, "dmsr"); // Recognize multiple surrounding objects and license plates

    // Recognize multiple surrounding objects and license plates within RoI
    anprFunc(path, outputFormat, "dmsri549,700,549,2427,1289,2427,1289,700");

    return 0;
}

int main(int ac, char **av)
{
    // Stream results as each image finishes instead of buffering them all until
    // exit — under the emulator a single recognition takes minutes, so line-by-
    // line output is what makes progress visible.
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *engineFileName = getEngineFileName();
    int res = 0;
#ifdef _WIN32
    wchar_t *wEngineFileName = to_wstring(engineFileName);
    res = TSANPR_load(&tsanpr, wEngineFileName);
    free(wEngineFileName);
#else
    res = TSANPR_load(&tsanpr, engineFileName);
#endif
    if (res < 0)
        return res;

    // TODO: Try each country code as needed
    // readLicensePlates("KR");
    readLicensePlates("JP");
    // readLicensePlates("VN");

    TSANPR_unload();
    return 0;
}
