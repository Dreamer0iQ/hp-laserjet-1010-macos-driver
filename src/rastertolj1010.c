/*
 * rastertolj1010 - CUPS filter for the HP LaserJet 1010 / 1012.
 *
 * These printers are "host-based": the formatter only understands HP's
 * FastRaster stream, i.e. a PCL XL session whose page content is a sequence
 * of VendorUnique operators, each carrying a 128-row band of 1-bit pixels
 * compressed with DeltaPlus. The stream produced here mirrors HPLIP's
 * hpcups LJFastRaster encapsulator byte for byte, except that the last band
 * of every page is actually sent (HPLIP leaves it in its buffer and prepends
 * it to the next page, which shifts A4 pages and drops the bottom of the last
 * one).
 *
 * Input:  CUPS raster, 600x600 dpi, 8-bit K (0 = white) or 1-bit K.
 * Output: PJL + PCL XL FastRaster on stdout.
 *
 * Usage:  rastertolj1010 job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "deltaplus.h"

#define FR_RES          600
#define FR_HEADER_SIZE  25
#define MEDIA_CUSTOM    96

enum { HT_DIFFUSION, HT_THRESHOLD };

/* PCL XL byte sequences, from HPLIP prnt/hpcups/PrinterCommands.h. */
static const uint8_t FRBeginSession[] = {
    0xC0, 0x00, 0xF8, 0x86,                     /* Measure eInch */
    0xC0, 0x03, 0xF8, 0x8F,                     /* ErrorReport eBackChAndErrPage */
    0xD1, 0x58, 0x02, 0x58, 0x02, 0xF8, 0x89,   /* UnitsPerMeasure 600 600 */
    0x41                                        /* BeginSession */
};
/* VUExtension 0x68702011 + VendorUnique: without it the 1010 reports
   "Unsupported Personality" (the "magic 8 bytes" of foomatic's pxl1010). */
static const uint8_t FRVUExtn3[]         = { 0xC2, 0x11, 0x20, 0x70, 0x68, 0xF8, 0x91, 0x46 };
static const uint8_t FROpenDataSource[]  = { 0xC0, 0x00, 0xF8, 0x88, 0xC0, 0x01, 0xF8, 0x82, 0x48 };
static const uint8_t FRFeedOrientation[] = { 0xC0, 0x00, 0xF8, 0x28 };
static const uint8_t FRCustomMediaSize[] = { 0xC0, 0x00, 0xF8, 0x30 };
static const uint8_t FRBeginPage[] = {
    0x43,                                       /* BeginPage */
    0xD3, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x2A, 0x75,   /* SetPageOrigin 0,0 */
    0xC0, 0x07, 0xF8, 0x03, 0x6A,               /* SetColorSpace */
    0xC0, 0xCC, 0xF8, 0x2C, 0x7B,               /* SetROP 0xCC */
    0xD3, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x4C, 0x6B    /* SetCursor 0,0 */
};
/* VUExtension 0x68702006 = FastRaster band; uint32 VUDataLength follows. */
static const uint8_t FREnterFRMode[]     = { 0xC2, 0x06, 0x20, 0x70, 0x68, 0xF8, 0x91, 0xC2 };
static const uint8_t FREndPage[]         = { 0x44 };
static const uint8_t FRCloseDataSource[] = { 0x49 };
static const uint8_t FREndSession[]      = { 0x42 };

/* PCL XL MediaSize enumerations, as HPLIP's hpcups.drv assigns them. */
static const struct { int w, h, id; } media_ids[] = {
    { 612,  792, 0 },   /* Letter */
    { 612, 1008, 1 },   /* Legal */
    { 595,  842, 2 },   /* A4 */
    { 522,  756, 3 },   /* Executive */
    { 297,  684, 6 },   /* #10 envelope */
    { 459,  649, 8 },   /* C5 envelope */
    { 312,  624, 9 },   /* DL envelope */
    { 499,  709, 11 },  /* B5 */
    { 284,  420, 14 },  /* Hagaki */
    { 420,  567, 15 },  /* Oufuku-Hagaki */
    { 420,  595, 16 },  /* A5 */
};

static volatile sig_atomic_t canceled = 0;

static int density = 3;
static int econo_mode = 0;
static int halftone = HT_DIFFUSION;

static uint8_t *line_buf;   /* one raster line as read from CUPS */
static uint8_t *band;       /* FR_BAND_HEIGHT rows of `slot` bytes */
static uint8_t *comp_buf;
static int *err_cur, *err_next;

static void cancel_job(int sig)
{
    (void) sig;
    canceled = 1;
}

static void put(const void *p, size_t n)
{
    fwrite(p, 1, n, stdout);
}

static int choice_int(ppd_file_t *ppd, const char *option, int def)
{
    ppd_choice_t *c = ppd ? ppdFindMarkedChoice(ppd, option) : NULL;
    return c ? atoi(c->choice) : def;
}

static void read_options(const char *argv5)
{
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv5, 0, &options);
    const char *path = getenv("PPD");
    ppd_file_t *ppd = path ? ppdOpenFile(path) : NULL;
    ppd_choice_t *c;

    if (ppd) {
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, num_options, options);
    }

    density = choice_int(ppd, "Density", density);
    if (density < 1 || density > 5)
        density = 3;
    if (ppd && (c = ppdFindMarkedChoice(ppd, "EconoMode")) != NULL)
        econo_mode = !strcmp(c->choice, "True");
    if (ppd && (c = ppdFindMarkedChoice(ppd, "Halftone")) != NULL)
        halftone = strcmp(c->choice, "Threshold") ? HT_DIFFUSION : HT_THRESHOLD;

    if (ppd)
        ppdClose(ppd);
    cupsFreeOptions(num_options, options);
}

static void start_job(void)
{
    printf("\033E\033%%-12345X");
    printf("@PJL SET RESOLUTION=600\n@PJL SET BITSPERPIXEL=1\n@PJL SET TIMEOUT=900\n");
    printf("@PJL SET PAGEPROTECT=AUTO\n@PJL SET DENSITY=%d\n", density);
    if (econo_mode)
        printf("@PJL SET RET=OFF\n@PJL SET ECONOMODE=ON\n");
    printf("@PJL ENTER LANGUAGE=PCLXL\n) HP-PCL XL;2;0;Comment\n");
    put(FRBeginSession, sizeof(FRBeginSession));
    put(FRVUExtn3, sizeof(FRVUExtn3));
    put(FROpenDataSource, sizeof(FROpenDataSource));
}

static void end_job(void)
{
    put(FRCloseDataSource, sizeof(FRCloseDataSource));
    put(FREndSession, sizeof(FREndSession));
    printf("\033%%-12345X@PJL EOJ\n\033%%-12345X");
    fflush(stdout);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void put_media(const cups_page_header2_t *h)
{
    int w = (int) h->PageSize[0], len = (int) h->PageSize[1];

    for (size_t i = 0; i < sizeof(media_ids) / sizeof(media_ids[0]); i++) {
        if (abs(media_ids[i].w - w) <= 2 && abs(media_ids[i].h - len) <= 2) {
            uint8_t seq[] = { 0xC0, (uint8_t) media_ids[i].id, 0xF8, 0x25 };
            put(seq, sizeof(seq));
            return;
        }
    }

    /* CustomMediaSize real32_xy in inches, then CustomMediaSizeUnits eInch */
    float fw = w / 72.0f, fh = len / 72.0f;
    uint32_t uw, uh;
    uint8_t seq[11];
    memcpy(&uw, &fw, 4);
    memcpy(&uh, &fh, 4);
    seq[0] = 0xD5;
    put_le32(seq + 1, uw);
    put_le32(seq + 5, uh);
    seq[9] = 0xF8;
    seq[10] = 0x2F;
    put(seq, sizeof(seq));
    put(FRCustomMediaSize, sizeof(FRCustomMediaSize));
}

/* Send one band. `slot` is the band row stride and declared row size. */
static void send_band(uint32_t rows, uint32_t slot, uint32_t y_offset)
{
    uint32_t size = 2 * FR_BAND_HEIGHT * slot;
    uint16_t comp = FR_COMP_DELTAPLUS;
    const uint8_t *data = comp_buf;
    uint8_t hdr[FR_HEADER_SIZE] = { 0 };
    uint8_t len[7];

    if (!dp_compress(comp_buf, &size, band, slot, rows, FR_HT_DIST)) {
        comp = FR_COMP_RAW;
        data = band;
        size = rows * slot;
    }

    /* big-endian band header (LJFastRaster::Encapsulate) */
    hdr[1] = 1;                                 /* page number */
    hdr[2] = FR_RES >> 8;
    hdr[3] = FR_RES & 0xFF;
    hdr[4] = comp >> 8;
    hdr[5] = comp & 0xFF;
    hdr[6] = 3;                                 /* colour plane: K */
    hdr[12] = (size >> 24) & 0xFF;
    hdr[13] = (size >> 16) & 0xFF;
    hdr[14] = (size >> 8) & 0xFF;
    hdr[15] = size & 0xFF;
    hdr[16] = ((slot * 8) >> 8) & 0xFF;         /* width in pixels */
    hdr[17] = (slot * 8) & 0xFF;
    hdr[18] = (rows >> 8) & 0xFF;
    hdr[19] = rows & 0xFF;
    hdr[22] = (y_offset >> 8) & 0xFF;
    hdr[23] = y_offset & 0xFF;
    hdr[24] = 1;                                /* bits per pixel */

    put(FREnterFRMode, sizeof(FREnterFRMode));
    put_le32(len, size + FR_HEADER_SIZE);
    len[4] = 0xF8;                              /* VUDataLength */
    len[5] = 0x92;
    len[6] = 0x46;                              /* VendorUnique */
    put(len, sizeof(len));
    put(hdr, sizeof(hdr));
    put(data, size);
}

/* 8-bit K (0 = white) -> 1-bit packed, MSB first, 1 = black. */
static void halftone_row(const uint8_t *k, uint8_t *out, int w, int y)
{
    if (halftone == HT_THRESHOLD) {
        for (int x = 0; x < w; x++)
            if (k[x] >= 128)
                out[x >> 3] |= 0x80 >> (x & 7);
        return;
    }

    /* Floyd-Steinberg, serpentine, errors scaled by 16. Solid white and
       solid black swallow their error so text edges don't spray dots. */
    int *cur = err_cur + 1, *nxt = err_next + 1;
    int dir = (y & 1) ? -1 : 1;
    int x = dir > 0 ? 0 : w - 1;

    memset(err_next, 0, (size_t) (w + 2) * sizeof(int));
    for (int i = 0; i < w; i++, x += dir) {
        int v = k[x], e;
        if (v == 0 || v == 255) {
            if (v)
                out[x >> 3] |= 0x80 >> (x & 7);
            continue;
        }
        v = v * 16 + cur[x];
        if (v >= 128 * 16) {
            out[x >> 3] |= 0x80 >> (x & 7);
            e = v - 255 * 16;
        } else
            e = v;
        cur[x + dir] += e * 7 / 16;
        nxt[x - dir] += e * 3 / 16;
        nxt[x]       += e * 5 / 16;
        nxt[x + dir] += e / 16;
    }

    int *t = err_cur;
    err_cur = err_next;
    err_next = t;
}

static int print_page(cups_raster_t *ras, const cups_page_header2_t *h, int page)
{
    uint32_t w = h->cupsWidth, height = h->cupsHeight;
    uint32_t slot = ((w + 7) / 8) * 8;   /* HPLIP row stride: width rounded to 8, in bytes */
    int invert = h->cupsColorSpace == CUPS_CSPACE_W || h->cupsColorSpace == CUPS_CSPACE_SW;
    uint32_t y, band_row = 0;

    if (h->HWResolution[0] != FR_RES || h->HWResolution[1] != FR_RES) {
        fprintf(stderr, "ERROR: Unsupported resolution %ux%u (need 600x600)\n",
                h->HWResolution[0], h->HWResolution[1]);
        return 0;
    }
    if (h->cupsBitsPerPixel != 8 && h->cupsBitsPerPixel != 1) {
        fprintf(stderr, "ERROR: Unsupported raster: %u bits per pixel\n", h->cupsBitsPerPixel);
        return 0;
    }

    line_buf = realloc(line_buf, h->cupsBytesPerLine);
    band     = realloc(band, (size_t) FR_BAND_HEIGHT * slot);
    comp_buf = realloc(comp_buf, (size_t) 2 * FR_BAND_HEIGHT * slot);
    err_cur  = realloc(err_cur, (w + 2) * sizeof(int));
    err_next = realloc(err_next, (w + 2) * sizeof(int));
    if (!line_buf || !band || !comp_buf || !err_cur || !err_next) {
        fputs("ERROR: Out of memory\n", stderr);
        return 0;
    }
    memset(band, 0, (size_t) FR_BAND_HEIGHT * slot);
    memset(err_cur, 0, (w + 2) * sizeof(int));

    fprintf(stderr, "INFO: Printing page %d\n", page);
    fprintf(stderr, "DEBUG: page %d: %ux%u px, %u bpp, cspace %u, media %ux%u pt, slot %u\n",
            page, w, height, h->cupsBitsPerPixel, h->cupsColorSpace,
            h->PageSize[0], h->PageSize[1], slot);

    put(FRFeedOrientation, sizeof(FRFeedOrientation));
    put_media(h);
    put(FRBeginPage, sizeof(FRBeginPage));

    for (y = 0; y < height && !canceled; y++) {
        uint8_t *row = band + (size_t) band_row * slot;

        if (cupsRasterReadPixels(ras, line_buf, h->cupsBytesPerLine) < h->cupsBytesPerLine)
            break;
        if (invert)
            for (uint32_t i = 0; i < h->cupsBytesPerLine; i++)
                line_buf[i] = ~line_buf[i];

        if (h->cupsBitsPerPixel == 8)
            halftone_row(line_buf, row, (int) w, (int) y);
        else
            memcpy(row, line_buf, (w + 7) / 8);

        if (++band_row == FR_BAND_HEIGHT) {
            send_band(FR_BAND_HEIGHT, slot, y + 1 - FR_BAND_HEIGHT);
            memset(band, 0, (size_t) FR_BAND_HEIGHT * slot);
            band_row = 0;
        }
    }

    /* Pad the last band with white: the printer only ever sees full
       128-row bands from HPLIP, so stay on that well-trodden path. */
    if (band_row > 0)
        send_band(FR_BAND_HEIGHT, slot, y - band_row);

    put(FREndPage, sizeof(FREndPage));
    fflush(stdout);
    fprintf(stderr, "PAGE: %d %u\n", page, h->NumCopies ? h->NumCopies : 1);
    return 1;
}

int main(int argc, char *argv[])
{
    cups_raster_t *ras;
    cups_page_header2_t header;
    int fd = 0, page = 0;

    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: Unable to open raster file");
        return 1;
    }

    signal(SIGTERM, cancel_job);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    read_options(argv[5]);

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    while (!canceled && cupsRasterReadHeader2(ras, &header)) {
        if (page == 0)
            start_job();
        if (!print_page(ras, &header, ++page))
            break;
    }
    if (page > 0)
        end_job();
    cupsRasterClose(ras);
    if (fd)
        close(fd);

    if (page == 0) {
        fputs("ERROR: No pages found\n", stderr);
        return 1;
    }
    fputs("INFO: Ready to print.\n", stderr);
    return 0;
}
