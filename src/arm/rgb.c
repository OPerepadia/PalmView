/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rgb.h"

#include <string.h>
#include "decoder.h"

/* RGB565 conversion tables, see InitRgbTables. */
#define RGB_TABLE_BIAS 256
#define RGB_TABLE_SIZE 768

static uint32_t gRgbTable[3 * RGB_TABLE_SIZE];
static uint16_t gChroma[512][2];
static uint8_t gRepeat[DEC_RGB_MAX_SOURCE];
static uint16_t gSourceX[DEC_RGB_MAX_SOURCE];
static uint32_t gRgbTableFlags;
static int gRgbTablesReady;

/* Integer BT.601, limited range. Chroma is folded into the luma index:
 * R = clamp(1.164 * (Y + dR(V) - 16)), and likewise for G and B.
 * gRgbTable has an R, a G and a B section, each holding that color's bits
 * in place, so a pixel is the sum of three reads. The bits do not overlap,
 * so byte-swapped entries give big-endian pixels.
 * gChroma[u] holds byte offsets into gRgbTable for B and half of G;
 * gChroma[256 + v] for R and the other half of G. */
static int32_t ChromaStep(int32_t c, int32_t factor)
{
    int32_t v = c * factor;
    return (v + (v < 0 ? -38142 : 38142)) / 76284;
}

static uint32_t Swap16(uint32_t v)
{
    return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8);
}

static void InitRgbTables(uint32_t flags)
{
    int i;
    for (i = 0; i < RGB_TABLE_SIZE; i++) {
        int32_t v = (76284 * (i - RGB_TABLE_BIAS - 16) + 32768) >> 16;
        uint32_t r, g, b;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        r = ((uint32_t)v >> 3) << 11;
        g = ((uint32_t)v >> 2) << 5;
        b = (uint32_t)v >> 3;
        if (flags & DEC_RGB_BIG_ENDIAN) {
            r = Swap16(r);
            g = Swap16(g);
            b = Swap16(b);
        }
        gRgbTable[i] = r;
        gRgbTable[RGB_TABLE_SIZE + i] = g;
        gRgbTable[2 * RGB_TABLE_SIZE + i] = b;
    }
    for (i = 0; i < 256; i++) {
        int32_t c = i - 128;
        gChroma[i][0] = (uint16_t)(4 * (2 * RGB_TABLE_SIZE + RGB_TABLE_BIAS +
            ChromaStep(c, 132252)));
        gChroma[i][1] = (uint16_t)(4 * (RGB_TABLE_BIAS / 2 - ChromaStep(c, 25625)));
        gChroma[256 + i][0] = (uint16_t)(4 * (RGB_TABLE_BIAS + ChromaStep(c, 104595)));
        gChroma[256 + i][1] = (uint16_t)(4 * (RGB_TABLE_SIZE + RGB_TABLE_BIAS / 2 -
            ChromaStep(c, 53281)));
    }
    gRgbTableFlags = flags;
    gRgbTablesReady = 1;
}

/* Stores pixel p n times. The fast form always stores two pixels and
 * advances by n, which is right for n of 1 or 2 when another pixel
 * follows. */
static inline __attribute__((always_inline)) uint16_t *Put(uint16_t *o,
    uint32_t p, uint32_t n, int fast)
{
    if (fast) {
        o[0] = (uint16_t)p;
        o[1] = (uint16_t)p;
        return o + n;
    }
    while (n-- != 0) *o++ = (uint16_t)p;
    return o;
}

#define TABLE_AT(offset) ((const uint32_t *)((const uint8_t *)tableP + (offset)))

/* Converts one source row into one output row, or two when dual. Source
 * pixel x covers gRepeat[x] output pixels. */
static inline __attribute__((always_inline)) void ConvertRow(uint16_t *o0,
    uint16_t *o1, const uint8_t *yRowP, const uint8_t *uRowP,
    const uint8_t *vRowP, uint32_t sourceWidth, int fast, int dual)
{
    const uint32_t *tableP = gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    const uint8_t *yP = yRowP, *uP = uRowP, *vP = vRowP, *repeatP = gRepeat;
    /* The fast loop leaves the last two source pixels to the exact one. */
    uint32_t x = fast && sourceWidth >= 4 ? (sourceWidth - 2) & ~1U : 0;
    const uint8_t *fastEndP = yRowP + x;
    while (yP < fastEndP) {
        const uint16_t *cuP = chromaP[*uP++], *cvP = chromaP[256 + *vP++];
        const uint32_t *rP = TABLE_AT(cvP[0]);
        const uint32_t *gP = TABLE_AT(cvP[1] + cuP[1]);
        const uint32_t *bP = TABLE_AT(cuP[0]);
        uint32_t y0 = yP[0], y1 = yP[1];
        uint32_t p0 = rP[y0] + gP[y0] + bP[y0];
        uint32_t p1 = rP[y1] + gP[y1] + bP[y1];
        uint32_t n0 = repeatP[0], n1 = repeatP[1];
        yP += 2;
        repeatP += 2;
        o0 = Put(o0, p0, n0, 1);
        if (dual) o1 = Put(o1, p0, n0, 1);
        o0 = Put(o0, p1, n1, 1);
        if (dual) o1 = Put(o1, p1, n1, 1);
    }
    for (; x < sourceWidth; x++) {
        const uint16_t *cuP = chromaP[uRowP[x >> 1]];
        const uint16_t *cvP = chromaP[256 + vRowP[x >> 1]];
        uint32_t y = yRowP[x];
        uint32_t p = TABLE_AT(cvP[0])[y] + TABLE_AT(cvP[1] + cuP[1])[y] +
            TABLE_AT(cuP[0])[y];
        o0 = Put(o0, p, gRepeat[x], 0);
        if (dual) o1 = Put(o1, p, gRepeat[x], 0);
    }
}

static void ConvertRowFast(uint16_t *o0, const uint8_t *yP, const uint8_t *uP,
                           const uint8_t *vP, uint32_t sourceWidth)
{
    ConvertRow(o0, 0, yP, uP, vP, sourceWidth, 1, 0);
}

static void ConvertRowFastDual(uint16_t *o0, uint16_t *o1, const uint8_t *yP,
                               const uint8_t *uP, const uint8_t *vP,
                               uint32_t sourceWidth)
{
    ConvertRow(o0, o1, yP, uP, vP, sourceWidth, 1, 1);
}

static void ConvertRowExact(uint16_t *o0, const uint8_t *yP, const uint8_t *uP,
                            const uint8_t *vP, uint32_t sourceWidth)
{
    ConvertRow(o0, 0, yP, uP, vP, sourceWidth, 0, 0);
}

/* Downscaling: converts only the source pixels gSourceX picks. */
static void ConvertRowScaled(uint16_t *o, const uint8_t *yRowP,
                             const uint8_t *uRowP, const uint8_t *vRowP,
                             uint32_t width)
{
    const uint32_t *tableP = gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    const uint16_t *sxP = gSourceX;
    uint16_t *endP = o + width;
    while (o < endP) {
        uint32_t sx = *sxP++, y = yRowP[sx];
        const uint16_t *cuP = chromaP[uRowP[sx >> 1]];
        const uint16_t *cvP = chromaP[256 + vRowP[sx >> 1]];
        *o++ = (uint16_t)(TABLE_AT(cvP[0])[y] + TABLE_AT(cvP[1] + cuP[1])[y] +
            TABLE_AT(cuP[0])[y]);
    }
}

/* Turned pictures go TURN_BAND rows at a time through gBand, then out
 * along the screen's rows. Written straight down a column instead, each
 * pixel would be a memory write of its own. */
#define TURN_BAND 16
static uint16_t gBand[TURN_BAND * DEC_RGB_MAX_TURNED];

/* Any scale: the source pixels gSourceX picks, each converted once. */
static void ConvertRowPicked(uint16_t *o, const uint8_t *yRowP,
                             const uint8_t *uRowP, const uint8_t *vRowP,
                             uint32_t width)
{
    const uint32_t *tableP = gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    const uint16_t *sxP = gSourceX, *endP = gSourceX + width;
    uint32_t last = 0xFFFFFFFFU, pixel = 0;
    while (sxP < endP) {
        uint32_t sx = *sxP++;
        if (sx != last) {
            uint32_t y = yRowP[sx];
            const uint16_t *cuP = chromaP[uRowP[sx >> 1]];
            const uint16_t *cvP = chromaP[256 + vRowP[sx >> 1]];
            pixel = TABLE_AT(cvP[0])[y] + TABLE_AT(cvP[1] + cuP[1])[y] +
                TABLE_AT(cuP[0])[y];
            last = sx;
        }
        *o++ = (uint16_t)pixel;
    }
}

/* Puts the band's n rows, picture rows first on, turned: each picture
 * column along a screen row, two pixels a write where it can. */
static void WriteBand(uint8_t *dstP, uint32_t stride, uint32_t first, uint32_t n,
                      uint32_t width)
{
    uint32_t x;
    for (x = 0; x < width; x++) {
        uint16_t *o = (uint16_t *)(dstP + x * stride - 2 * (first + n - 1));
        const uint16_t *bP = gBand + (n - 1) * width + x;
        uint32_t k = 0;
        if (((uintptr_t)o & 2) != 0) {
            *o++ = *bP;
            bP -= width;
            k++;
        }
        for (; k + 1 < n; k += 2) {
            *(uint32_t *)o = bP[0] | ((uint32_t)*(bP - width) << 16);
            o += 2;
            bP -= 2 * width;
        }
        if (k < n) *o = *bP;
    }
}

/* Nearest source index for output index i of n, sampling pixel centers. */
static uint32_t SourceIndex(uint32_t i, uint32_t n, uint32_t sourceN)
{
    return ((2 * i + 1) * sourceN) / (2 * n);
}

/* Scaling down to between half and full size averages the luma of the
 * source pixels each output pixel covers: up to three rows, then up to
 * three columns. Chroma, at half resolution already, stays nearest. */
#define SHRINK_MAX 512
static uint16_t gShrinkX[SHRINK_MAX];
static uint16_t gShrinkXW[SHRINK_MAX][3];
static uint16_t gShrinkY[SHRINK_MAX];
static uint16_t gShrinkYW[SHRINK_MAX][3];
static uint16_t gShrinkChromaX[SHRINK_MAX];
static uint16_t gShrinkColumns[DEC_RGB_MAX_SOURCE];

/* Output i covers source [a, b), in 1/256 pixels. Its weights, out of
 * 256, go to the three pixels from firstP[i] on. */
static void ShrinkTaps(uint32_t n, uint32_t sourceN, uint16_t *firstP,
                       uint16_t (*wP)[3])
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        uint32_t a = i * sourceN * 256 / n, b = (i + 1) * sourceN * 256 / n;
        uint32_t first = a >> 8, total = 0, big = 0, w[3];
        if (first + 3 > sourceN) first = sourceN - 3;
        for (k = 0; k < 3; k++) {
            uint32_t lo = (first + k) << 8, hi = lo + 256;
            uint32_t from = a > lo ? a : lo, to = b < hi ? b : hi;
            w[k] = to > from ? ((to - from) * 256 + (b - a) / 2) / (b - a) : 0;
            total += w[k];
            if (w[k] > w[big]) big = k;
        }
        w[big] = w[big] + 256 - total;
        firstP[i] = (uint16_t)first;
        for (k = 0; k < 3; k++) wP[i][k] = (uint16_t)w[k];
    }
}

/* Averages source rows into gShrinkColumns, times 256. The row loops are
 * kept out of line: inlined, they run out of registers. */
static inline __attribute__((always_inline)) void ShrinkRows(const uint8_t *aP,
    const uint8_t *bP, const uint8_t *cP, const uint16_t *wP, uint32_t n,
    int three)
{
    uint32_t w0 = wP[0], w1 = wP[1], w2 = wP[2];
    uint16_t *o = gShrinkColumns, *endP = o + n;
    while (o < endP) {
        uint32_t v = *aP++ * w0 + *bP++ * w1;
        if (three) v += *cP++ * w2;
        *o++ = (uint16_t)v;
    }
}

static __attribute__((noinline)) void ShrinkRows2(const uint8_t *aP,
    const uint8_t *bP, const uint16_t *wP, uint32_t n)
{
    ShrinkRows(aP, bP, 0, wP, n, 0);
}

static __attribute__((noinline)) void ShrinkRows3(const uint8_t *aP,
    const uint8_t *bP, const uint8_t *cP, const uint16_t *wP, uint32_t n)
{
    ShrinkRows(aP, bP, cP, wP, n, 1);
}

/* Averages gShrinkColumns across and converts the row. */
static __attribute__((noinline)) void ShrinkColumns(uint16_t *o,
    const uint8_t *uP, const uint8_t *vP, uint32_t width)
{
    const uint32_t *tableP = gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    const uint16_t *columnsP = gShrinkColumns, *xP = gShrinkX, *cxP = gShrinkChromaX;
    const uint16_t (*wP)[3] = gShrinkXW;
    uint16_t *endP = o + width;
    while (o < endP) {
        const uint16_t *p = columnsP + *xP++;
        uint32_t cx = *cxP++;
        const uint16_t *cuP = chromaP[uP[cx]], *cvP = chromaP[256 + vP[cx]];
        uint32_t y = (p[0] * (*wP)[0] + p[1] * (*wP)[1] + p[2] * (*wP)[2]) >> 16;
        wP++;
        *o++ = (uint16_t)(TABLE_AT(cvP[0])[y] + TABLE_AT(cvP[1] + cuP[1])[y] +
            TABLE_AT(cuP[0])[y]);
    }
}

static void ConvertShrink(const AVFrame *f, uint8_t *dstP, uint32_t stride,
                          uint32_t width, uint32_t height)
{
    uint32_t sourceWidth = (uint32_t)f->width, sourceHeight = (uint32_t)f->height;
    uint32_t lumaStride = (uint32_t)f->linesize[0], x, y;
    ShrinkTaps(width, sourceWidth, gShrinkX, gShrinkXW);
    ShrinkTaps(height, sourceHeight, gShrinkY, gShrinkYW);
    for (x = 0; x < width; x++)
        gShrinkChromaX[x] = (uint16_t)(SourceIndex(x, width, sourceWidth) >> 1);
    for (y = 0; y < height; y++) {
        const uint8_t *rowP = f->data[0] + gShrinkY[y] * lumaStride;
        const uint16_t *wP = gShrinkYW[y];
        uint32_t cy = SourceIndex(y, height, sourceHeight) >> 1;
        if (wP[2] != 0)
            ShrinkRows3(rowP, rowP + lumaStride, rowP + 2 * lumaStride, wP, sourceWidth);
        else
            ShrinkRows2(rowP, rowP + lumaStride, wP, sourceWidth);
        ShrinkColumns((uint16_t *)(dstP + y * stride),
            f->data[1] + cy * (uint32_t)f->linesize[1],
            f->data[2] + cy * (uint32_t)f->linesize[2], width);
    }
}

/* The pixel for luma table offset y and chroma table offsets r, g, b. */
#define TABLE_PIXEL(y, r, g, b) ((uint16_t)( \
    *(const uint32_t *)(tableP + (r) + (y)) + \
    *(const uint32_t *)(tableP + (g) + (y)) + \
    *(const uint32_t *)(tableP + (b) + (y))))

/* Scaling down by 4 to 3, as 240p to 180 rows, has fixed weights: output
 * pixels take 3:1, 1:1 and 1:3 of neighboring source pixels, down and
 * across. A word holds four source pixels. Masking splits it into the
 * even and odd columns, two 16-bit lanes each, blended together. */

/* Up to three output pixels from four columns of rows a and b, weighted
 * wa:(4 - wa). */
static inline __attribute__((always_inline)) void Shrink43Group(uint16_t *o,
    const uint8_t *aP, const uint8_t *bP, uint32_t wa, const uint8_t *uP,
    const uint8_t *vP, uint32_t count)
{
    const uint8_t *tableP = (const uint8_t *)gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    uint32_t a = *(const uint32_t *)aP, b = *(const uint32_t *)bP, wb = 4 - wa;
    uint32_t even = (a & 0x00FF00FFU) * wa + (b & 0x00FF00FFU) * wb;
    uint32_t odd = ((a >> 8) & 0x00FF00FFU) * wa + ((b >> 8) & 0x00FF00FFU) * wb;
    /* Lane 0 of 3 * even + odd is 3:1 of columns 0 and 1, lane 1 of
     * even + 3 * odd 1:3 of columns 2 and 3. The weights total 16. */
    uint32_t y0 = ((even * 3 + odd) & 0xFFFFU) >> 4 << 2;
    uint32_t y1 = ((odd & 0xFFFFU) + (even >> 16)) >> 3 << 2;
    uint32_t y2 = (even + odd * 3) >> 20 << 2;
    const uint16_t *cuP = chromaP[uP[0]], *cvP = chromaP[256 + vP[0]];
    uint32_t r, g, bl;
    o[0] = TABLE_PIXEL(y0, cvP[0], cvP[1] + cuP[1], cuP[0]);
    if (count < 2) return;
    cuP = chromaP[uP[1]];
    cvP = chromaP[256 + vP[1]];
    r = cvP[0];
    g = cvP[1] + cuP[1];
    bl = cuP[0];
    o[1] = TABLE_PIXEL(y1, r, g, bl);
    if (count > 2) o[2] = TABLE_PIXEL(y2, r, g, bl);
}

static __attribute__((noinline)) void Shrink43Row(uint16_t *o, const uint8_t *aP,
    const uint8_t *bP, uint32_t wa, const uint8_t *uP, const uint8_t *vP,
    uint32_t width)
{
    uint16_t *endP = o + width - width % 3;
    while (o < endP) {
        Shrink43Group(o, aP, bP, wa, uP, vP, 3);
        o += 3;
        aP += 4;
        bP += 4;
        uP += 2;
        vP += 2;
    }
    if (width % 3 != 0) Shrink43Group(o, aP, bP, wa, uP, vP, width % 3);
}

/* Whether the picture is 3/4 of the source, its width rounded down (426
 * to 319). Groups read whole words, so rows must hold whole groups. */
static int Shrinks43(const AVFrame *f, uint32_t width, uint32_t height)
{
    uint32_t sourceWidth = (uint32_t)f->width, groups = (width + 2) / 3;
    return height * 4 == (uint32_t)f->height * 3 &&
        width * 4 <= sourceWidth * 3 && sourceWidth * 3 < width * 4 + 4 &&
        4 * groups <= (uint32_t)f->linesize[0] &&
        2 * groups <= (uint32_t)f->linesize[1] &&
        2 * groups <= (uint32_t)f->linesize[2] &&
        (((uintptr_t)f->data[0] | (uint32_t)f->linesize[0]) & 3) == 0;
}

static void ConvertShrink43(const AVFrame *f, uint8_t *dstP, uint32_t stride,
                            uint32_t width, uint32_t height)
{
    uint32_t lumaStride = (uint32_t)f->linesize[0], y;
    for (y = 0; y < height; y++) {
        uint32_t k = y % 3, cy = SourceIndex(y, height, (uint32_t)f->height) >> 1;
        const uint8_t *aP = f->data[0] + (y / 3 * 4 + k) * lumaStride;
        Shrink43Row((uint16_t *)(dstP + y * stride), aP, aP + lumaStride, 3 - k,
            f->data[1] + cy * (uint32_t)f->linesize[1],
            f->data[2] + cy * (uint32_t)f->linesize[2], width);
    }
}

/* Growing by 4 to 5, as 144p to 180 rows, interpolates by area: each
 * output pixel takes the source pixels it overlaps, in proportion. Of
 * every five, the first and last lie inside one source pixel, and the
 * others take 1:3, 1:1 and 3:1 of two. Edges stay sharp, and every
 * source pixel gets the same width. Words split into even and odd
 * columns as when shrinking. */
static inline __attribute__((always_inline)) void Grow54Group(uint16_t *o,
    const uint8_t *aP, const uint8_t *bP, uint32_t wa, const uint8_t *uP,
    const uint8_t *vP)
{
    const uint8_t *tableP = (const uint8_t *)gRgbTable;
    const uint16_t (*chromaP)[2] = gChroma;
    uint32_t a = *(const uint32_t *)aP, b = *(const uint32_t *)bP, wb = 4 - wa;
    uint32_t even = (a & 0x00FF00FFU) * wa + (b & 0x00FF00FFU) * wb;
    uint32_t odd = ((a >> 8) & 0x00FF00FFU) * wa + ((b >> 8) & 0x00FF00FFU) * wb;
    /* Lane 0 of even + 3 * odd is 1:3 of columns 0 and 1, lane 1 of
     * 3 * even + odd 3:1 of columns 2 and 3. */
    uint32_t y0 = (even & 0xFFFFU) >> 2 << 2;
    uint32_t y1 = ((even + odd * 3) & 0xFFFFU) >> 4 << 2;
    uint32_t y2 = ((odd & 0xFFFFU) + (even >> 16)) >> 3 << 2;
    uint32_t y3 = (even * 3 + odd) >> 20 << 2;
    uint32_t y4 = odd >> 18 << 2;
    const uint16_t *cuP = chromaP[uP[0]], *cvP = chromaP[256 + vP[0]];
    uint32_t r = cvP[0], g = cvP[1] + cuP[1], bl = cuP[0];
    o[0] = TABLE_PIXEL(y0, r, g, bl);
    o[1] = TABLE_PIXEL(y1, r, g, bl);
    cuP = chromaP[uP[1]];
    cvP = chromaP[256 + vP[1]];
    r = cvP[0];
    g = cvP[1] + cuP[1];
    bl = cuP[0];
    o[2] = TABLE_PIXEL(y2, r, g, bl);
    o[3] = TABLE_PIXEL(y3, r, g, bl);
    o[4] = TABLE_PIXEL(y4, r, g, bl);
}

static __attribute__((noinline)) void Grow54Row(uint16_t *o, const uint8_t *aP,
    const uint8_t *bP, uint32_t wa, const uint8_t *uP, const uint8_t *vP,
    uint32_t width)
{
    uint16_t *endP = o + width;
    while (o < endP) {
        Grow54Group(o, aP, bP, wa, uP, vP);
        o += 5;
        aP += 4;
        bP += 4;
        uP += 2;
        vP += 2;
    }
}

static int Grows54(const AVFrame *f, uint32_t width, uint32_t height)
{
    return width * 4 == (uint32_t)f->width * 5 && height * 4 == (uint32_t)f->height * 5 &&
        (((uintptr_t)f->data[0] | (uint32_t)f->linesize[0]) & 3) == 0;
}

static void ConvertGrow54(const AVFrame *f, uint8_t *dstP, uint32_t stride,
                          uint32_t width, uint32_t height)
{
    /* Output row k of five: source rows a and a + 1, weighted wa:(4 - wa).
     * The last takes only row a, which may be the last row. */
    static const uint8_t kRow[5] = { 0, 0, 1, 2, 3 }, kWeight[5] = { 4, 1, 2, 3, 4 };
    uint32_t lumaStride = (uint32_t)f->linesize[0], y;
    for (y = 0; y < height; y++) {
        uint32_t k = y % 5, cy = SourceIndex(y, height, (uint32_t)f->height) >> 1;
        const uint8_t *aP = f->data[0] + (y / 5 * 4 + kRow[k]) * lumaStride;
        Grow54Row((uint16_t *)(dstP + y * stride), aP, k == 4 ? aP : aP + lumaStride,
            kWeight[k], f->data[1] + cy * (uint32_t)f->linesize[1],
            f->data[2] + cy * (uint32_t)f->linesize[2], width);
    }
}

int RgbConvert(const AVFrame *f, uint8_t *dstP, uint32_t stride, uint32_t width,
               uint32_t height, uint32_t flags)
{
    uint32_t sourceWidth, sourceHeight, x, y, turn = flags & DEC_RGB_TURN;
    int fast = 1;
    if (f == 0 || f->data[0] == 0 || width == 0 || height == 0)
        return decErrState;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return decErrState;
    sourceWidth = (uint32_t)f->width;
    sourceHeight = (uint32_t)f->height;
    if (sourceWidth > DEC_RGB_MAX_SOURCE || width > 4 * sourceWidth ||
        height > 4 * sourceHeight || (((uintptr_t)dstP | stride) & 1) != 0 ||
        (turn && width > DEC_RGB_MAX_TURNED))
        return decErrState;
    flags &= DEC_RGB_BIG_ENDIAN;
    if (!gRgbTablesReady || gRgbTableFlags != flags) InitRgbTables(flags);
    if (turn) {
        uint32_t lastSy = 0xFFFFFFFFU, first, i;
        for (x = 0; x < width; x++)
            gSourceX[x] = (uint16_t)SourceIndex(x, width, sourceWidth);
        /* Up to twice as wide, the faster row loop can fill the band. */
        if (width < sourceWidth) fast = 0;
        if (fast) {
            memset(gRepeat, 0, sourceWidth);
            for (x = 0; x < width; x++) gRepeat[gSourceX[x]]++;
            for (x = 0; x < sourceWidth; x++)
                if (gRepeat[x] == 0 || gRepeat[x] > 2) fast = 0;
        }
        for (first = 0; first < height; first += TURN_BAND) {
            uint32_t n = height - first < TURN_BAND ? height - first : TURN_BAND;
            for (i = 0; i < n; i++) {
                uint32_t sy = SourceIndex(first + i, height, sourceHeight);
                uint16_t *rowP = gBand + i * width;
                /* Only the last band is short, so a row repeated from the
                 * band before is still in the band's last row. */
                if (sy == lastSy)
                    memcpy(rowP, i != 0 ? rowP - width
                        : gBand + (TURN_BAND - 1) * width, width * 2);
                else if (fast)
                    ConvertRowFast(rowP, f->data[0] + sy * (uint32_t)f->linesize[0],
                        f->data[1] + (sy >> 1) * (uint32_t)f->linesize[1],
                        f->data[2] + (sy >> 1) * (uint32_t)f->linesize[2], sourceWidth);
                else
                    ConvertRowPicked(rowP, f->data[0] + sy * (uint32_t)f->linesize[0],
                        f->data[1] + (sy >> 1) * (uint32_t)f->linesize[1],
                        f->data[2] + (sy >> 1) * (uint32_t)f->linesize[2], width);
                lastSy = sy;
            }
            WriteBand(dstP, stride, first, n, width);
        }
        return 0;
    }
    if (width < sourceWidth && height < sourceHeight && 2 * width >= sourceWidth &&
        2 * height >= sourceHeight && width <= SHRINK_MAX && height <= SHRINK_MAX &&
        sourceWidth >= 3 && sourceHeight >= 3) {
        if (Shrinks43(f, width, height)) ConvertShrink43(f, dstP, stride, width, height);
        else ConvertShrink(f, dstP, stride, width, height);
        return 0;
    }
    if (width < sourceWidth) {
        for (x = 0; x < width; x++)
            gSourceX[x] = (uint16_t)SourceIndex(x, width, sourceWidth);
        for (y = 0; y < height; y++) {
            uint32_t sy = SourceIndex(y, height, sourceHeight);
            ConvertRowScaled((uint16_t *)(dstP + y * stride),
                f->data[0] + sy * (uint32_t)f->linesize[0],
                f->data[1] + (sy >> 1) * (uint32_t)f->linesize[1],
                f->data[2] + (sy >> 1) * (uint32_t)f->linesize[2], width);
        }
        return 0;
    }
    if (Grows54(f, width, height)) {
        ConvertGrow54(f, dstP, stride, width, height);
        return 0;
    }
    memset(gRepeat, 0, sourceWidth);
    for (x = 0; x < width; x++) gRepeat[SourceIndex(x, width, sourceWidth)]++;
    for (x = 0; x < sourceWidth; x++)
        if (gRepeat[x] == 0 || gRepeat[x] > 2) fast = 0;
    for (y = 0; y < height;) {
        uint32_t sy = SourceIndex(y, height, sourceHeight), rows = 1;
        const uint8_t *yP = f->data[0] + sy * (uint32_t)f->linesize[0];
        const uint8_t *uP = f->data[1] + (sy >> 1) * (uint32_t)f->linesize[1];
        const uint8_t *vP = f->data[2] + (sy >> 1) * (uint32_t)f->linesize[2];
        while (y + rows < height && SourceIndex(y + rows, height, sourceHeight) == sy)
            rows++;
        for (; rows != 0; rows--, y++) {
            uint16_t *o0 = (uint16_t *)(dstP + y * stride);
            if (!fast) {
                ConvertRowExact(o0, yP, uP, vP, sourceWidth);
            } else if (rows >= 2) {
                ConvertRowFastDual(o0, (uint16_t *)(dstP + (y + 1) * stride),
                    yP, uP, vP, sourceWidth);
                rows--;
                y++;
            } else {
                ConvertRowFast(o0, yP, uP, vP, sourceWidth);
            }
        }
    }
    return 0;
}
