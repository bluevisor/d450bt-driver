/*
 * rastertod450bt - CUPS raster filter for the D450BT thermal label printer.
 *
 * Converts CUPS raster pages (8-bit grayscale, 203 dpi) into TSPL, the
 * command language the D450BT firmware speaks. Replaces the vendor's
 * x86_64-only "rastertolabeltspl" filter with a native universal binary.
 *
 * Everything here mirrors what the vendor filter emits for its Bluetooth
 * models (cupsModelNumber 1xx), verified by disassembly and byte-diffing
 * output on identical raster input:
 *
 *   SIZE <w> mm,<h> mm            (integer mm, rounded)
 *   REFERENCE 0,0
 *   DIRECTION 0,0
 *   GAP <g>.0 mm,<o> mm           (or BLINE / GAP 0 mm,0 mm)
 *   SET TEAR ON
 *   OFFSET 0 mm
 *   DENSITY <n>                   (only when Darkness is set)
 *   SPEED <n>
 *   CLS
 *   BITMAP 0,0,<row bytes>,<rows>,4,<blocks>
 *   PRINT 1,<copies>
 *
 * BITMAP mode 4: the 1bpp bitmap (bit 0 = black) is split into 4096-byte
 * chunks, each compressed with LZO1X-1 and framed as
 *   <u32le compressed length><compressed bytes>
 * with a final u32le 0 terminator. The printer's Bluetooth link silently
 * drops large uncompressed BITMAPs, so mode 4 is required over bluetooth://.
 *
 * Pacing (vendor "SendNowAndRead"): the framed stream is written in
 * 32-byte slices; after each slice the filter flushes stdout and issues a
 * CUPS_SC_CMD_DRAIN_OUTPUT side-channel request (50 ms timeout) so the
 * backend confirms delivery before more data is queued. That is real flow
 * control when the backend implements it, and degrades to time-based
 * pacing (50 ms/slice) when it does not.
 *
 * Rendering (vendor "ColorOption"):
 *   None          global threshold: black iff gray <= black_levels[BlackLevel-1]
 *   GrayScale     gray >= 246 -> white, gray < gray_levels[Degree-1] -> black,
 *                 else Bayer 8x8 ordered dither (black iff gray <= 4*cell)
 *   FloydSteinberg  our extension; best quality, but the noisy output
 *                 compresses poorly - avoid for large labels
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <cups/sidechannel.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "minilzo/minilzo.h"

#define EOL "\r\n"

#define LZO_CHUNK 4096
/* Worst-case LZO1X output for a 4096-byte input, per LZO docs. */
#define LZO_CHUNK_MAX (LZO_CHUNK + LZO_CHUNK / 16 + 64 + 3)

#define PACE_SLICE   32
#define PACE_TIMEOUT 0.05

/* Standard Bayer 8x8 index matrix, exactly as embedded in the vendor filter.
 * Threshold = 4 * cell (0..252). */
static const unsigned char bayer8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 },
  { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 },
  { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 },
  { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 },
  { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* Vendor blacklevellist / graylevellist (1-based UI index). */
static const int black_levels[8] = { 192, 160, 144, 128, 112, 96, 64, 0 };
static const int gray_levels[7]  = { 192, 160, 144, 128, 96, 64, 16 };

enum render_mode { RENDER_THRESHOLD, RENDER_BAYER, RENDER_FS };

static volatile sig_atomic_t job_canceled = 0;

static void
cancel_job(int sig)
{
  (void)sig;
  job_canceled = 1;
}

/* Emit one TSPL command line and flush, as the vendor's printWithRefresh
 * does for Bluetooth models (no drain between header commands). */
static void
cmd(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fputs(EOL, stdout);
  fflush(stdout);
}

/*
 * Write the framed compressed stream in 32-byte slices. After each slice,
 * flush and ask the backend to drain its output buffer (vendor pacing).
 * drain_stats counts cups_sc_status_t results (0..7).
 */
static void
paced_send(const unsigned char *buf, size_t len, unsigned drain_stats[8])
{
  for (size_t off = 0; off < len && !job_canceled; off += PACE_SLICE)
  {
    size_t n = len - off < PACE_SLICE ? len - off : PACE_SLICE;
    char   sc_data[1];
    int    sc_len = 0;

    fwrite(buf + off, 1, n, stdout);
    fflush(stdout);

    cups_sc_status_t st = cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT,
                                                   sc_data, &sc_len,
                                                   PACE_TIMEOUT);
    if ((int)st >= 0 && (int)st < 8)
      drain_stats[(int)st]++;
  }
}

/* Compress the bitmap into a malloc'd framed mode-4 stream (vendor
 * compressBitmap): [u32le clen][data]... terminated by u32le 0. */
static unsigned char *
compress_bitmap(const unsigned char *bitmap, size_t len, size_t *stream_len,
                size_t *max_block, unsigned *blocks)
{
  static unsigned char workmem[LZO1X_1_MEM_COMPRESS];
  size_t         nchunks = (len + LZO_CHUNK - 1) / LZO_CHUNK;
  unsigned char *stream = malloc(nchunks * (4 + LZO_CHUNK_MAX) + 4);
  size_t         pos = 0;

  *max_block = 0;
  *blocks = 0;

  if (!stream)
    return (NULL);

  for (size_t off = 0; off < len; off += LZO_CHUNK)
  {
    size_t   in_len = len - off < LZO_CHUNK ? len - off : LZO_CHUNK;
    lzo_uint out_len = LZO_CHUNK_MAX;

    if (lzo1x_1_compress(bitmap + off, in_len, stream + pos + 4, &out_len,
                         workmem) != LZO_E_OK)
    {
      free(stream);
      return (NULL);
    }

    stream[pos + 0] = (unsigned char)(out_len & 0xFF);
    stream[pos + 1] = (unsigned char)((out_len >> 8) & 0xFF);
    stream[pos + 2] = (unsigned char)((out_len >> 16) & 0xFF);
    stream[pos + 3] = (unsigned char)((out_len >> 24) & 0xFF);
    pos += 4 + out_len;

    if (out_len > *max_block)
      *max_block = out_len;
    (*blocks)++;
  }

  memset(stream + pos, 0, 4);
  pos += 4;

  *stream_len = pos;
  return (stream);
}

/* Look up a numeric option, falling back to the PPD default, then to dflt. */
static int
int_option(const char *name, int num_options, cups_option_t *options,
           ppd_file_t *ppd, int dflt)
{
  const char   *value = cupsGetOption(name, num_options, options);
  ppd_choice_t *choice;

  if (!value && ppd && (choice = ppdFindMarkedChoice(ppd, name)) != NULL)
    value = choice->choice;

  if (!value || !*value || !strcasecmp(value, "Default"))
    return (dflt);

  return (atoi(value));
}

static const char *
str_option(const char *name, int num_options, cups_option_t *options,
           ppd_file_t *ppd, const char *dflt)
{
  const char   *value = cupsGetOption(name, num_options, options);
  ppd_choice_t *choice;

  if (!value && ppd && (choice = ppdFindMarkedChoice(ppd, name)) != NULL)
    value = choice->choice;

  return (value && *value ? value : dflt);
}

static double
now_seconds(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (tv.tv_sec + tv.tv_usec / 1e6);
}

/* Log a back-channel response in hex + printable form. */
static void
log_response(const char *what, const unsigned char *buf, ssize_t n)
{
  char hex[3 * 64 + 1], txt[64 + 1];
  ssize_t i;

  for (i = 0; i < n && i < 64; i++)
  {
    snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
    txt[i] = buf[i] >= 0x20 && buf[i] < 0x7F ? buf[i] : '.';
  }
  txt[i] = '\0';
  fprintf(stderr, "DEBUG: %s: %zd bytes: %s(\"%s\")\n", what, n, hex, txt);
}

/*
 * After PRINT: watch the back channel for the per-job SPP ACK
 * (7E 01 7E = accepted, 7E 00 7E = rejected) and poll SSSGETPRINTING
 * (reply ":DOING"/":DONE"/":PAUSE") until the printer reports done.
 * Keeps the connection open while the printer is still working and
 * tells us definitively whether a job was accepted. Gives up quickly
 * when the firmware answers neither (3 consecutive silent reads).
 */
static void
wait_printer_done(int wait_secs)
{
  double deadline = now_seconds() + wait_secs;
  int    silent = 0;

  while (now_seconds() < deadline && !job_canceled)
  {
    unsigned char resp[64];
    ssize_t       n;

    fputs("SSSGETPRINTING" EOL, stdout);
    fflush(stdout);

    n = cupsBackChannelRead((char *)resp, sizeof(resp), 2.0);
    if (n > 0)
    {
      silent = 0;
      log_response("printer response", resp, n);

      if (memmem(resp, n, "\x7e\x00\x7e", 3))
        fprintf(stderr, "ERROR: printer REJECTED the job (7E 00 7E NACK)\n");
      if (memmem(resp, n, "\x7e\x01\x7e", 3))
        fprintf(stderr, "DEBUG: printer ACKed the job (7E 01 7E)\n");
      if (memmem(resp, n, "DONE", 4))
      {
        fprintf(stderr, "INFO: Printer reports printing done.\n");
        return;
      }
    }
    else if (++silent >= 3)
    {
      fprintf(stderr, "DEBUG: no status responses; firmware likely "
                      "ignores SSSGETPRINTING\n");
      return;
    }

    sleep(1);
  }
}

int
main(int argc, char *argv[])
{
  int                fd;         /* Raster file descriptor */
  cups_raster_t     *ras;        /* Raster stream */
  cups_page_header2_t header;    /* Current page header */
  int                page = 0;
  int                num_options;
  cups_option_t     *options;
  ppd_file_t        *ppd;
  int                copies;
  unsigned           drain_stats[8] = { 0 };

  if (argc < 6 || argc > 7)
  {
    fputs("Usage: rastertod450bt job user title copies options [file]\n", stderr);
    return (1);
  }

  if (argc == 7)
  {
    if ((fd = open(argv[6], O_RDONLY)) < 0)
    {
      perror("ERROR: Unable to open raster file");
      return (1);
    }
  }
  else
    fd = 0;

  copies = atoi(argv[4]);
  if (copies < 1)
    copies = 1;

  num_options = cupsParseOptions(argv[5], 0, &options);

  ppd = ppdOpenFile(getenv("PPD"));
  if (ppd)
  {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
  }

  signal(SIGTERM, cancel_job);

  if (lzo_init() != LZO_E_OK)
  {
    fputs("ERROR: lzo_init failed\n", stderr);
    return (1);
  }

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

  while (cupsRasterReadHeader2(ras, &header))
  {
    if (job_canceled)
      break;

    page++;
    fprintf(stderr, "PAGE: %d %d\n", page, copies);
    fprintf(stderr, "INFO: Printing page %d\n", page);

    /* Label geometry in mm, from the page size in points. */
    double width_mm  = header.PageSize[0] * 25.4 / 72.0;
    double height_mm = header.PageSize[1] * 25.4 / 72.0;

    const char *darkness = str_option("Darkness", num_options, options, ppd,
                                      "PrinterDefault");
    int speed   = int_option("zePrintRate", num_options, options, ppd, 4);
    int gap_mm  = int_option("GapHeight", num_options, options, ppd, 3);
    int gap_off = int_option("GapOffset", num_options, options, ppd, 0);
    const char *tracking = str_option("zeMediaTracking", num_options, options,
                                      ppd, "Web");
    const char *coloropt = str_option("ColorOption", num_options, options,
                                      ppd, "GrayScale");
    int gray_idx  = int_option("DegreeOfGrayRecognition", num_options, options,
                               ppd, 4);
    int black_idx = int_option("BlackLevel", num_options, options, ppd, 2);
    int rotate    = int_option("Rotate", num_options, options, ppd, 0) != 0;

    enum render_mode mode;
    if (!strcasecmp(coloropt, "GrayScale"))
      mode = RENDER_BAYER;
    else if (!strcasecmp(coloropt, "FloydSteinberg"))
      mode = RENDER_FS;
    else
      mode = RENDER_THRESHOLD;

    if (gray_idx < 1) gray_idx = 1;
    if (gray_idx > 7) gray_idx = 7;
    if (black_idx < 1) black_idx = 1;
    if (black_idx > 8) black_idx = 8;

    int gray_floor = gray_levels[gray_idx - 1];
    int black_thr  = black_levels[black_idx - 1];

    unsigned width      = header.cupsWidth;
    unsigned height     = header.cupsHeight;
    unsigned row_bytes  = (width + 7) / 8;
    unsigned in_bytes   = header.cupsBytesPerLine;

    unsigned char *line   = malloc(in_bytes);
    unsigned char *bitmap = malloc((size_t)row_bytes * height);
    /* Floyd-Steinberg error rows (signed, one extra slot each side) */
    int *err_cur  = calloc(width + 2, sizeof(int));
    int *err_next = calloc(width + 2, sizeof(int));

    if (!line || !bitmap || !err_cur || !err_next)
    {
      fputs("ERROR: Out of memory\n", stderr);
      return (1);
    }

    /* TSPL bitmap: bit 1 = blank, bit 0 = black. Start all blank. */
    memset(bitmap, 0xFF, (size_t)row_bytes * height);

    for (unsigned y = 0; y < height; y++)
    {
      if (cupsRasterReadPixels(ras, line, in_bytes) < 1)
        break;
      if (job_canceled)
        break;

      memset(err_next, 0, (width + 2) * sizeof(int));

      for (unsigned x = 0; x < width; x++)
      {
        /* 8-bit gray, 255 = white. 1-bit input is expanded below. */
        int gray;
        int black;

        if (header.cupsBitsPerPixel == 1)
          gray = (line[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
        else
          gray = line[x];

        switch (mode)
        {
          case RENDER_BAYER:
          {
            /* Vendor GrayScale: clamp, then Bayer 8x8 (threshold 4*cell). */
            int v = gray >= 246 ? 255 : (gray < gray_floor ? 0 : gray);
            black = v <= 4 * bayer8[y & 7][x & 7];
            break;
          }

          case RENDER_THRESHOLD:
            black = gray <= black_thr;
            break;

          default:  /* RENDER_FS */
          {
            gray += err_cur[x + 1];
            black = gray < 128;
            int err = gray - (black ? 0 : 255);

            /* 7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right */
            err_cur[x + 2]  += err * 7 / 16;
            err_next[x]     += err * 3 / 16;
            err_next[x + 1] += err * 5 / 16;
            err_next[x + 2] += err / 16;
            break;
          }
        }

        if (black)
        {
          unsigned ox = rotate ? width - 1 - x : x;
          unsigned oy = rotate ? height - 1 - y : y;

          bitmap[(size_t)oy * row_bytes + (ox >> 3)] &= ~(0x80 >> (ox & 7));
        }
      }

      int *tmp = err_cur;
      err_cur = err_next;
      err_next = tmp;
    }

    if (!job_canceled)
    {
      /* Header commands: vendor order and formats, byte-exact. */
      cmd("SIZE %ld mm,%ld mm", lround(width_mm), lround(height_mm));
      cmd("REFERENCE 0,0");
      cmd("DIRECTION 0,0");

      if (!strcasecmp(tracking, "Continuous"))
        cmd("GAP 0 mm,0 mm");
      else if (!strcasecmp(tracking, "Mark") || !strcasecmp(tracking, "BLine"))
        cmd("BLINE %d mm,%d mm", gap_mm, gap_off);
      else  /* Web / Gap */
        cmd("GAP %d.0 mm,%.1f mm", gap_mm, (double)gap_off);

      cmd("SET TEAR ON");
      cmd("OFFSET 0 mm");
      if (strcasecmp(darkness, "PrinterDefault") &&
          strcasecmp(darkness, "Default") && *darkness)
        cmd("DENSITY %d", atoi(darkness));
      cmd("SPEED %d", speed);
      cmd("CLS");

      size_t         stream_len = 0, max_block = 0;
      unsigned       blocks = 0;
      unsigned char *stream = compress_bitmap(bitmap,
                                              (size_t)row_bytes * height,
                                              &stream_len, &max_block,
                                              &blocks);

      if (!stream)
      {
        fputs("ERROR: LZO compression failed\n", stderr);
        return (1);
      }

      fprintf(stderr,
              "DEBUG: page %d: %ux%u px, mode=%s rotate=%d, bitmap=%zu B, "
              "stream=%zu B in %u blocks (max %zu B)\n",
              page, width, height, coloropt, rotate,
              (size_t)row_bytes * height, stream_len, blocks, max_block);

      printf("BITMAP 0,0,%u,%u,4,", row_bytes, height);
      fflush(stdout);

      double t0 = now_seconds();
      paced_send(stream, stream_len, drain_stats);
      double elapsed = now_seconds() - t0;

      free(stream);

      printf(EOL "PRINT 1,%d" EOL, copies);
      fflush(stdout);

      fprintf(stderr,
              "DEBUG: page %d sent in %.1f s (%.0f B/s); drain: ok=%u "
              "timeout=%u none=%u ioerr=%u noresp=%u notimpl=%u\n",
              page, elapsed, elapsed > 0 ? stream_len / elapsed : 0,
              drain_stats[CUPS_SC_STATUS_OK],
              drain_stats[CUPS_SC_STATUS_TIMEOUT],
              drain_stats[CUPS_SC_STATUS_NONE],
              drain_stats[CUPS_SC_STATUS_IO_ERROR],
              drain_stats[CUPS_SC_STATUS_NO_RESPONSE],
              drain_stats[7]);
      fprintf(stderr, "INFO: Page %d sent (%ux%u px, %.1fx%.1f mm)\n",
              page, width, height, width_mm, height_mm);

      int wait_secs = int_option("WaitForCompletion", num_options, options,
                                 ppd, 60);
      if (wait_secs > 0)
        wait_printer_done(wait_secs);
    }

    free(line);
    free(bitmap);
    free(err_cur);
    free(err_next);
  }

  cupsRasterClose(ras);
  if (fd != 0)
    close(fd);
  if (ppd)
    ppdClose(ppd);

  if (page == 0)
  {
    fputs("ERROR: No pages found in raster data\n", stderr);
    return (1);
  }

  fprintf(stderr, "INFO: Ready to print.\n");
  return (0);
}
