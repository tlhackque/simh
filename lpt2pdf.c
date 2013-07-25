/* 
 *  Copyright (c) 2013, Timothe Litt
 *                       litt @acm.org
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:

 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.

 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 *  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 *  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 *  Except as contained in this notice, the name of the author shall not be
 *  used in advertising or otherwise to promote the sale, use or other dealings
 *  in this Software without prior written authorization from the author.
 *
 */


/* This compiles two ways:
 * As a callable library (default)
 * As a stand-alone utility (define PDF_MAIN for this)
 *
 * The API is documented in lpt2pdf.h.  The utility in its usage(), below.
 * 
 * The PDF generated conforms to a subset of the PDF specification:
 * http://www.adobe.com/content/dam/Adobe/en/devnet/acrobat/pdfs/pdf_reference_1-7.pdf
 *
 * When appending, only files produced by this library are accepted, as a number of
 * assumptions are made about the structure to simplify this code.
 */

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h> /* ftruncate */
#endif

#define PDF_BUILD_
#include "lpt2pdf.h"

/* Colors:
 *
 * PDF RGB takes values from 0 to 1.0
 */

#define RGB_BLACK  "0 0 0"
#define RGB_WHITE "1.000 1.000 1.000"

#define RGB_GREEN_LINE "0.780 0.860 0.780"
#define RGB_GREEN_TEXT "0.780 0.860 0.780"
#define RGB_GREEN_BAR  "0.880 0.960 0.880"

#define RGB_BLUE_LINE  "0.794 0.900 0.900"
#define RGB_BLUE_TEXT  "0.794 0.900 0.900"
#define RGB_BLUE_BAR   "0.804 1.000 1.000"

#define RGB_YELLOW_LINE  "0.900 0.900 0.800"
#define RGB_YELLOW_TEXT  "0.700 0.700 0.700" /* Use gray; yellow text not readable */
#define RGB_YELLOW_BAR   "1.000 1.000 0.600"

#define RGB_GRAY_LINE  "0.700 0.700 0.700"
#define RGB_GRAY_TEXT  "0.700 0.700 0.700"
#define RGB_GRAY_BAR   "0.800 0.800 0.800"

#define RGB_HOLE_LINE "0.85 0.85 0.85"
#define RGB_HOLE_FILL "0.90 0.90 0.90"

typedef struct {
    const char *const line;
    const char *const bar;
    const char *const text;
    const char *const name;
} COLORS;
static const COLORS colors[] = {
#define    PDF_PLAIN       (0)
    {RGB_BLACK,        RGB_BLACK,       RGB_BLACK,       "PLAIN", }, /* PLAIN is used for images too */
#define    PDF_GREENBAR    (1)
    {RGB_GREEN_LINE,   RGB_GREEN_BAR,   RGB_GREEN_TEXT,  "GREENBAR", },
#define    PDF_BLUEBAR     (2)
    {RGB_BLUE_LINE,    RGB_BLUE_BAR,    RGB_BLUE_TEXT,   "BLUEBAR", },
#define    PDF_GRAYBAR     (3)
    {RGB_GRAY_LINE,    RGB_GRAY_BAR,    RGB_GRAY_TEXT,   "GRAYBAR", },
#define    PDF_YELLOWBAR   (4)
    {RGB_YELLOW_LINE,  RGB_YELLOW_BAR,  RGB_YELLOW_TEXT, "YELLOWBAR", },
};
#define    PDF_JPEG       (99) /* Internal use */

#define DIM(x) (sizeof (x) / sizeof ((x)[0]))

/* Library error codes */

#define E(x) PDF_E_ ## x

static const char **formlist = NULL;

/* These are the built-in fonts that every reader is required to know about.
 * Embedding fonts would be nice, but requires a lot of work to read the file,
 * decode its format &... deal with the licensing issues.
 */

static const char *const validFonts[] = {
    "Courier",     "Courier-Bold",   "Courier-Oblique",  "Courier-BoldOblique",
    "Times-Roman", "Times-Bold",     "Times-Italic",     "Times-BoldItalic",
    "Helvetica",   "Helvetica-Bold", "HelveticaOblique", "Helvetica-BoldOblique",
    "Symbol",      "ZapfDingbats",
    NULL
};

/* SHA1: used for creating a document ID
 */
/* Requires: uint32_t, uint8_t, int_least16_t */

#define SHA1HashSize 20

/*
 *  This structure will hold context information for the SHA-1
 *  hashing operation
 */
typedef struct SHA1Context {
    uint32_t Intermediate_Hash[SHA1HashSize/4]; /* Message Digest  */

    uint32_t Length_Low;                       /* Message length in bits      */
    uint32_t Length_High;                      /* Message length in bits      */

                                              /* Index into message block array   */
    int_least16_t Message_Block_Index;
    uint8_t Message_Block[64];                /* 512-bit message blocks      */

    int Computed;                             /* Is the digest computed?         */
    int Corrupted;                            /* Is the message digest corrupted? */
} SHA1Context;

int SHA1Reset(  SHA1Context *);
int SHA1Input(  SHA1Context *,
                const uint8_t *,
                unsigned int);
int SHA1Result( SHA1Context *,
                uint8_t Message_Digest[SHA1HashSize]);
/* *** End SHA1 *** */

#ifdef ERRDEBUG
static void errout (void ) {
    int a = 0;
    a = a;
    return;
}
#endif

typedef long t_fpos;        /* File position */

/* Context for all operations.
 * Presented to the caller as a PDF_HANDLE, but
 * intentionally opaque.  First section has defaults,
 * second section is simply cleared.  General principle is
 * that there are no fixed sizes, no global state, and
 * memory, once allocated is reused as much as possible.
 * Dynamic structures grow until pdf_close().
 *
 * update pdf_free() with any new dynamic memory pointers.
 */

typedef struct {
    char key[3];            /* Handle validator */
    unsigned int frequire;  /* File requirements */
#define PDF_FILE_NEW     (0)/*  File must be empty */
#define PDF_FILE_APPEND  (1)/*  File can be appended (if PDF) */
#define PDF_FILE_REPLACE (2)/*  Non-empty file's contents are replaced */
    double cpi;             /* Note: LA120 had several fractional cpi */
    unsigned int lpi;
    unsigned int newlpi;
    unsigned int cols;      /* Print columns */
    double wid;             /* Sheet width, in */
    double len;             /* Sheet length, in */
    char *font;             /* Name of text font */
    char *nfont;            /* Name of number font */
    char *nbold;            /* Name of label font */
    char *title;            /* Document title string */
    double top;             /* Top margin, in (offset of first bar) */
    unsigned int tof;       /* Top Of Form line, logical line 1 (e.g of CC tape) */
    double bot;             /* Bottom margin, in (space after last bar) */
    double margin;          /* Horizonatal margin, in (tractor feed) */
    double lno;             /* Width of line number, in column */
    unsigned int formtype;  /* Type of background/form */
    double barh;            /* Height of form bar */

    /* Below this point initialized to zero */
    int errnum;             /* Last error */
    char *formfile;         /* File containing form image */
    FILE *pdf;              /* Output file handle */
    unsigned int escstate;  /* Escape sequence parser */
#define ESC_IDLE     (0)
#define ESC_ESCSEQ   (1)
#define ESC_CSI      (2)
#define ESC_CSIP     (3)
#define ESC_CSIINT   (4)
#define ESC_BADCSI   (5)
#define ESC_BADESC   (6)
#define ESC_BADSTR   (7)
    char escints[4];        /* Intermediate characters of sequence */
    unsigned int escin;     /* Number of intermediates */
    char escprv;            /* Private sequence (DEC uses ?) */
    unsigned short int escpars[16]; /* CSI parameters */
#define ESC_PDEFAULT ((unsigned short int)(~0u))
#define ESC_POVERFLOW ((((unsigned long)ESC_PDEFAULT) +1) >> 1)
#define ESC_PMAX (ESC_POVERFLOW -1)
    unsigned int escpn;     /* Number of parameters */
    char *formbuf;          /* Graphics data for page */
    size_t formsize;        /* Buffer allocation */
    size_t formlen;         /* Length of form data */
#define FORMBUF &pdf->formbuf, &pdf->formsize, &pdf->formlen
    unsigned int formobj;   /* Form object number */
    jmp_buf env;
#ifdef ERRDEBUG
#define ABORT(err) {errout();longjmp(pdf->env,(err));}
#define ABORTps(err) {errout();longjmp(ps->env,(err));}
#else
#define ABORT(err) longjmp (pdf->env, (err))
#define ABORTps(err) longjmp (ps->env, (err))
#endif
    char oid[(SHA1HashSize*2) +1]; /* Original document id (when appending) */
    char ctime[32];         /* Original doc creation time */
    unsigned int prevpc;    /* Page count from previous sessions */
    char *trail;            /* Trailer from previous sessions */
    t_fpos anchorp;         /* Anchor object location for previous session */
    t_fpos anchorpp;        /* Position to rewrite parentof previous session */
    t_fpos checkpp;         /* File position at checkpoint */
    unsigned int aobj;      /* Anchor object number */
    unsigned int obj;       /* Last object number assigned */
    t_fpos xpos;            /* Location of xref */
    t_fpos *xref;           /* Xref - file position of each object */
    size_t xsize;           /* Max objects in current xref allocation */
    unsigned int flags;
#define PDF_ACTIVE        0x0001 /* Printing active (no more SETs) */
#define PDF_UPDATING      0x0002 /* Updating existing file (append) */
#define PDF_UNCOMPRESSED  0x0004 /* Always write uncompressed text objects */
#define PDF_INIT          0x0008 /* Initialized - metadata read if appending */
#define PDF_WRITTEN       0x0010 /* Headers written */
#define PDF_RESUMED       0x0020 /* Resumed from a checkpoint */
    unsigned int lpp;       /* Lines per page */
    short **lines;          /* Data for each line */
    unsigned int  nlines;   /* Number of lines allocated */
    unsigned int *linesize; /* Allocation for each line */
    unsigned int *linelen;  /* Highest column written */
    unsigned int page;      /* Current page number */
    unsigned int line;      /* Current line number, 0 if nothing written */
    SHA1Context sha1;       /* Context for document ID hash */
    unsigned int pbase;     /* Object number of sessions page data */
    unsigned int iobj;      /* Doc information object number */
    short *parsebuf;        /* Buffer with input controls expanded */
    size_t parsesize;       /* Allocated size in shorts */
    size_t parseused;       /* Shorts used */
#define PARSEBUF &pdf->parsebuf, &pdf->parsesize, &pdf->parseused
    char *pagebuf;          /* Buffer containing rendered page data */
    size_t pbsize;          /* Allocated size of pagebuf */
    size_t pbused;          /* Bytes used in pagebuf */
#define PAGEBUF &pdf->pagebuf, &pdf->pbsize, &pdf->pbused
    char *lzwbuf;           /* Buffer containing compressed page */
    size_t lzwsize;         /* Allocated size */
    size_t lzwused;         /* Bytes used */
#define LZWBUF &pdf->lzwbuf, &pdf->lzwsize, &pdf->lzwused
} PDF;

#define QS(str) (str), (sizeof (str) -1)

/* Used to initialize new contexts.
 * Note that any strings:
 *  Must be copied to malloc'ed memory if a user can pdf_set them
 *  Must be free'd in pdf_close
 * There is exception-style error handling; be careful with allocating
 * memory elsewhere.
 *
 * If you find it necessary to change this change the defaults in the
 * SET table below.
 */

static const PDF defaults = {
    {'P', 'D', 'F'},
    PDF_FILE_NEW,
    10,
    6,
    6,
    132,
    14.875,
    11.000,
    "Courier",
    "Times-Roman",
    "Times-Bold",
    "Lineprinter data",
    1.00,
    ~1u,
    0.500,
    0.470,
    0.100,
    PDF_GREENBAR,
    0.500,
};

#define ps ((PDF *)pdf)

/* Points/inch */
#define PT (72)

/* Forward references */

static int dupstr (PDF *pdf, char **ptr);
static int pdfset ( PDF *pdf, int arg, va_list ap);
static int checkfont (const char *newfont);
static void pdfinit (PDF *pdf);
static int checkupdate (PDF *pdf);
static void wrhdr (PDF *pdf);
static void wrpage (PDF *pdf);
static void setform (PDF *pdf);
static void barform (PDF *pdf);
static void imageform (PDF *pdf);
static unsigned int addobj (PDF *pdf);
static unsigned int getref (PDF *pdf, char *buf, const char *name);
static unsigned int getint (PDF *pdf, char *buf, const char *name, const char **end);
static char *getstr (PDF *pdf, char *buf, const char *name);
static char *getstr (PDF *pdf, char *buf, const char *name);
static int parsestr (PDF *pdf, const char *string, size_t length, int initial);
static int pdfclose (PDF *pdf, int checkpoint);
static void pdf_free (PDF *pdf);
static void wrstmf (PDF *pdf, char **buf, size_t *len, size_t *used, const char *fmt, ...);
static void wrstm (PDF *pdf, char **buf, size_t *bufsize, size_t *used, char *string, size_t length);
static void wrstw (PDF *pdf, short **buf, size_t *bufsize, size_t *used, short *string, size_t length);
static t_fpos readobj (PDF *pdf, unsigned int obj, char **buf, size_t *len);
static void add2line (PDF *pdf, const short *text, size_t textlen);
static void circle (PDF *pdf, double x, double y, double r);
static int xstrcasecmp (const char *s1, const char *s2);

/* validate a PDF_HANDLE */

#define valarg(arg) { if (!(arg) ||                        \
                          ((PDF *)arg)->key[0] != 'P' ||   \
                          ((PDF *)arg)->key[1] != 'D' ||   \
                          ((PDF *)arg)->key[2] != 'F' ||   \
                          !((PDF *)arg)->pdf) { errno = E(BAD_HANDLE); return errno; } }

/* Coordinate transformations */

#define yp(y) ((pdf->len - (y)) * PT) /* In from top -> page coord */
#define xp(x) ((x) * PT)              /* In to page coord */
#define CircleK (0.551784)            /* Constant for approximating circles */

/* LZW encoding
*/

/* These constants are in the PDF spec and can not be changed.
 */

#define LZW_CLRCODE   (256)               /* Clear table code */
#define LZW_EODCODE   (257)               /* End of data code */
#define LZW_IDCODES   (258)               /* Number of identity codes */

#define LZW_MINBITS    (9)                /* Smallest (and initial) codesize */
#define LZW_MAXBITS   (12)                /* Largest permitted codesize */
#define LZW_DSIZE     (1 << LZW_MAXBITS)  /* Size of LZW directory */

typedef uint16_t t_lzwCode;

#define TREE_NULL ((t_lzwCode)~0u)        /* Null code in directory tree */

typedef struct {
    t_lzwCode  prev;
    t_lzwCode  first;
    t_lzwCode  next;
    short      ch;
} t_lzwNode;

typedef struct {
    FILE         *fh;                     /* File handle for output */
    uint8_t      **outbuf;                /* Buffer for output */
    size_t       *outsize;                /* Size of output buffer */
    size_t       *outused;                /* Data in output buffer */
    uint32_t      bitbuf;                 /* Bit packing buffer */
#if LZ_MAXBITS > 32
#error t_lzw.bitbuf is too small to hold LZ_MAXBITS, reduce or find a larger datatype
#endif
    unsigned int  nbits;                  /* Number of bits pending in buffer */
    t_lzwNode     dict[LZW_DSIZE];        /* Standard LZW prefix directory */
    t_lzwCode     assigned;               /* Highest code assigned */
    uint16_t      codesize;               /* Size of current code (bits) */
} t_lzw;

static void lzw_init(t_lzw *lzw, int mode, ...);
#define LZW_FILE     (1)
#define LZW_BUFFER   (2)
#define LZW_REINIT   (3)
#define LZW_BUFALCQ (512)

static void lzw_encode (t_lzw *lzw, char *stream, size_t len);
static t_lzwCode lzw_add_str (t_lzw *lzw, t_lzwCode code, char c);
static t_lzwCode lzw_lookup_str (t_lzw *lzw, t_lzwCode code, char c);
static void lzw_writebits (t_lzw *lzw, unsigned int bits, unsigned int nbits);
static void lzw_flushbits (t_lzw *lzw);

/* *** End LZW *** */

static int encstm (PDF *pdf, char *stream, size_t len);


#ifdef PDF_MAIN
/* This is a very simple driver for the API, and is not optimized.
 */

#define SET(_key, _code, _type, _def, _help) { "-" #_key, PDF_##_code, AT_##_type, #_def, #_help },

typedef struct {
    const char *const keyword;
    int arg;
    int atype;
#define AT_STRING  1
#define AT_NUMBER  2
#define AT_INTEGER 3
    const char *const def;
    const char *const help;
} ARG;
static const ARG argtable [] = {
    SET (bar,     BAR_HEIGHT,     NUMBER,  0.500in,     (Specifies the height of the bar on forms.))
    SET (bottom,  BOTTOM_MARGIN,  NUMBER,  0.500in,     (Specifies the height of the bottom margin in inches.  Below this there is no bar.))
    SET (columns, COLS,           INTEGER, 132,         (Specifies the number of columns to be printed.  Used to center output))
    SET (cpi,     CPI,            NUMBER,  10,          (Specifies the characters per inch (horizontal pitch).  Fractional pitch is supported.))
    SET (font,    TEXT_FONT,      STRING,  Courier,     (Specifies the name of the font to use for rendering the input data.  Accepted are:%F))
    SET (form,    FORM_TYPE,      STRING,  greenbar,    (Specifies the form background to be applied. One of:%fPlain is white page.))
    SET (image,   FORM_IMAGE,     STRING,  <none>,      (Specifies a .jpg image to be used as the form background\nIt will be scaled to fill the area within the margins.\n))
    SET (length,  PAGE_LENGTH,    NUMBER,  11.000in,    (Specifies the length of the page in inches, inclusive of all margins))
    SET (lfont,   LABEL_FONT,     STRING,  Times-Bold,  (Specifies the name of the font used to render labels on the form))
    SET (lno,     LNO_WIDTH,      NUMBER,  0.100in,     (Specifies the width of the line number column on the form; 0 to omit cols.))
    SET (lpi,     LPI,            INTEGER, 6,           (Specifies the lines per inch (vertical pitch): 6 or 8 are supported.))
    SET (nfont,   LNO_FONT,       STRING,  Times-Roman, (Specifies the name of the font used to render the numbers on the form))
    SET (require, FILE_REQUIRE,   STRING,  new,         (Specifies how to treat the output file.  \nNEW will create the file, or if it exists, the file must be empty.\nAPPEND will create the file, or if it exists and is in PDF fomat, data will be appended.\nREPLACE will completely replace the contents of an existing file.))
    SET (side,    SIDE_MARGIN,    NUMBER,  0.470,       (Specifies the width of the tractor feed margin on each side of the page.))
    SET (title,   TITLE,          STRING, ("Lineprinter data"),
                                                        (Specifies the title embedded in the PDF document))
    SET (tof,     TOF_OFFSET,     INTEGER, (topmargin (6 lines at 6LPI, 8 at 8LPI)),
                                                        (Specifies the offset of the logical top of form from the top of page.\nThis is the line on to which <FF> advances, similar to the offset that\noccurs when an operator positions a form with respect to a carriage))
    SET (top,     TOP_MARGIN,     NUMBER,  1.000in,     (Specifies the height of the top margin in inches -- above first bar.))
    SET (width,   PAGE_WIDTH,     NUMBER,  14.875in,    (Specifies the width of the page in inches, inclusive of all margins))
};

static void do_file (PDF_HANDLE pdf, FILE *fh, const char *filename);
static int usage (const ARG *argtable, size_t nargs);
static void print_hlplist (FILE *file, const char *const *list, int adjcase);

int main (int argc, char **argv, char **env) {
    PDF_HANDLE pdf;
    int i, of;
    int r;
    char *infile = NULL, *outfile = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp (argv[i], "--help") || !strcmp (argv[i], "-h")) {
            exit (usage(argtable, DIM(argtable)));
        }
        if (argv[i][0] == '-') {
            i++;
            continue;
        }
        break;
    }

    of = 0;
    if (i < argc) {
        of = argc -1;

        if (strcmp (argv[of], "-")) {
            outfile = argv[of];
        }
    }

    pdf = pdf_open (outfile);
    if (!pdf) {
        pdf_perror (pdf, outfile);
        exit (2);
    }

    for (i = 1; i < argc; i++) {
        const char *sw = argv[i];
        size_t k;

        if (!strcmp (sw, "--")) {
            i++;
            break;
        }
        if (sw[0] != '-') {
            break;
        }
        if (!argv[++i]) {
            fprintf (stderr, "? %s requires an argument\n", argv[i-1]);
            exit (3);
        }

        for (k = 0; k < DIM (argtable); k++) {
            if (!strcmp (sw, argtable[k].keyword) ) {
                double arg;
                char *ep;
                long iarg;

                switch (argtable[k].atype) {
                case AT_STRING:
                    r = pdf_set (pdf, argtable[k].arg, argv[i]);
                    break;
                case AT_INTEGER:
                    iarg = strtol (argv[i], &ep, 10);
                    if (!*argv[i] ||*ep || ep == argv[i]) {
                        fprintf (stderr, "? not an integer for %s value: %s\n",
                                 argtable[k].keyword, ep);
                        exit (3);
                    }
                    arg = (double) iarg;
                    r = pdf_set (pdf, argtable[k].arg, arg);
                    break;
                case AT_NUMBER:
                    if (!*argv[i]) {
                        fprintf (stderr, "?Missing value for %s\n", argtable[k].keyword);
                        exit (3);
                    }
                    arg = strtod (argv[i], &ep);
                    if (*ep) {
                        if (!strcmp (ep, "cm")) {
                            arg /= 2.54;
                        } else {
                            if (!strcmp (ep, "mm")) {
                                arg /= 25.4;
                            } else if (strcmp (ep, "in")) {
                                fprintf (stderr, "?Unknown qualifier for %s value: %s\n",
                                                 argtable[k].keyword,  ep);
                                exit (3);
                            }
                        }
                    }
                    r = pdf_set (pdf, argtable[k].arg, arg);
                    break;
                default:
                    exit (3);
                }
                if (r != PDF_OK) {
                    pdf_perror (pdf, argv[i]);
                    exit (3);
                }
                break;
            }
        }
        if (k < DIM (argtable)) {
            continue;
        }

        fprintf (stderr, "Unknown switch %s, --help for usage\n", sw);
        exit (3);
    }

    /* And after all that: */

    if (i >= of ) {
        do_file (pdf, stdin, "<stdin>");
    } else {
        while (i < of) {
            infile = argv[i];
                if (!strcmp (argv[i], "-")) {
                    do_file (pdf, stdin, "<stdin>");
                } else {
                    FILE *fh = fopen (argv[i], "r" );
                    if (!fh) {
                        pdf_perror (pdf, argv[i]);
                        exit (1);
                    }
                    do_file (pdf, fh, argv[i]);
                    fclose (fh);
                }
            i++;
        }
    }

    r = pdf_close (pdf);
    if (r) {
        pdf_perror (pdf, "pdf_close failed");
        exit (4);
    }

    exit (0);
}

static void do_file (PDF_HANDLE pdf, FILE *fh, const char *filename) {
    int c;
    size_t page, line;
    char lbuf[60];
    long bc = 0;

    while (fgets (lbuf, sizeof (lbuf), fh)) {
        size_t len = strlen (lbuf);
        bc += len;

        c = pdf_print (pdf, lbuf, len);
        if (c) {
            pdf_perror (pdf, "pdf_print failed");
            exit (4);
        }
    }

    if (bc) {
        fprintf (stderr, "Read %lu characters from %s\n", bc, filename);
    }
    if (ferror (fh)) {
        pdf_perror (pdf, "Error reading input");
    }
    if (pdf_where (pdf, &page, &line)) {
        pdf_perror (pdf, "Error getting position");
    }
#if 0
    pdf_checkpoint (pdf);
#endif
    fprintf (stderr, "End of %s, at page %u line %u\n", filename, page, line);
    return;
}

static int usage (const ARG *argtable, size_t nargs) {
    size_t i;

    fprintf (stderr, "Usage:\n\
lpt2pdf [-options] infiles outile\n\
\n\
lpt2pdf will turn an ASCII input file into a PDF file on simulated paper.\n\
\n\
The defaults are for a standard lineprinter - 14.875 x 11.000 in,\n\
6LPI, 10 CPI.  (Lines and Characters per inch.)\n\
\n\
Default is to read from stdin and write to stdout.  Because PDF is\n\
a binary format, the output file must not be a tty.  It must be capable\n\
of random access in update mode.  A pipe will not work.\n\n\
'-' for either input or output is interpreted as stdin/stdout respectivey.\n\
Any output file must be seekable, generally a disk\n\
\n\
Options, naturally are optional:\n");

    for (i = 0; i < nargs; i++, argtable++) {
        char c;
        const char *p;

        fprintf (stderr, "    %s %s\n        ", 
                 argtable->keyword, (argtable->atype == AT_NUMBER)? "n.m":
                                        (argtable->atype == AT_INTEGER)? "integer": "string");
        p = argtable->help;
        if (*p == '(') {
            p++;
        }
        for (; *p && (*p != ')' || p[1]); p++) {
            c = *p;
            if (c == '%') {
                c = *++p;
                switch (c) {
                case 'F':
                    print_hlplist( stderr, pdf_get_fontlist (NULL), 0);
                    break;
                case 'f': {
                    print_hlplist( stderr, pdf_get_formlist (NULL), 1);
                    break;
                }
                default:
                    fprintf (stderr, "Help bug\n");
                    exit (7);
                }
                continue;
            }

            fputc (c, stderr);
            if (c == '\n') {
                fputs ("        ", stderr);
            }
        }
        fputs ("\n        Default: ", stderr );
        p = argtable->def;
        if (*p == '(') {
            p++;
        }
        for (; *p && (*p != ')' || p[1]); p++) {
            fputc (*p, stderr);
        }
        fputs ("\n\n", stderr);
    }

    fprintf (stderr, "\
     Font notes:\n\
        Fonts may only be one of of the default fonts supported by PDF.  The utility\n\
        does not embed fonts in the PDF file (messy licensing issues.)\n\
        Stick with monspace fonts for the correct effects.  Better to use -cpi.\n\
        Font names are case-sensitive\n");

    fprintf (stderr, "\n\
    Linear dimensions may be specified in cm or mm by suffixing the number with cm or mm\n\
    respectively.\n\
\n\
    In general, you don't need any options to get usable output\n\
\n\
    Bugs, praise to litt@acm.org\n\
");
    exit (0);
}

static void print_hlplist (FILE *file, const char *const *list, int adjcase) {
    if (list) {
        fputc ('\n', file);
        while (*list) {
            const char *r = *list++;
            char c = *r++;
            fprintf (file, "            %c", adjcase? toupper (c): c);
            while ((c = *r++)) {
                fputc ((adjcase? tolower (c): c), file);
            }
            fputc ('\n', file);
        }
    }
    return;
}
/* *** End of stand-alone utility *** */
#endif

/* *** Start of public API *** */

/* open a pdf file for output (or update)
 * returns handle.  If NULL, see errno.
 * Attempts to open for update
 * Will not write until first output.
 */

PDF_HANDLE pdf_open (const char *filename) {
    int fd;
    PDF *pdf;
    const char *p;

    if (!filename) {
        errno = E(BAD_FILENAME);
        return NULL;
    }
    
    p = strrchr (filename, '.');
    if (p) {
        const char *fn, *ext = "pdf";
        int islc = islower (p[1]);
        for (fn = p + 1;
#if defined (VMS)
                        (*fn && (*fn != ';');
#else
                        *fn;
#endif
                         fn++, ext++) {
            if ((islc? (*fn != *ext): (*fn != toupper (*ext)))) {
                errno = E(BAD_FILENAME);
                return NULL;
            }
        }
        if (*ext) {
            errno = E(BAD_FILENAME);
            return NULL;
        }
    }

    pdf = (PDF *) malloc (sizeof (PDF));
    if (!pdf) {
        return NULL;
    }
    memcpy (pdf, &defaults, sizeof (PDF));

    if (dupstr (pdf, &pdf->font) ||
        dupstr (pdf, &pdf->nfont) ||
        dupstr (pdf, &pdf->nbold) ||
        dupstr (pdf, &pdf->title) ||
        dupstr (pdf, &pdf->formfile)) {
        return NULL;
    }

    /* Open for read/write, creating if non-existent but not truncating if exists.
     * This can't be done without a race condition with fopen.  So,
     * system-dependent code follows.
     */
#ifdef _WIN32
    fd = _open (filename, _O_BINARY|_O_CREAT|_O_RDWR, _S_IREAD | _S_IWRITE);
#else
    fd = open (filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR |
                                           S_IRGRP | S_IWGRP |
                                           S_IROTH | S_IWOTH
    #ifdef VMS
                       , "ALQ=32", "DEQ=4096",
                         "MBF=6", "MBC=127", "FOP=cbt,tef",
                         "ROP=rah,wbh", "CTX=stm"
    #endif
            );
#endif
    if (fd == -1) {
        pdf_free (pdf);
        return NULL;
    }

#ifdef _WIN32
    pdf->pdf = _fdopen (fd, "rb+");
#else
    pdf->pdf = fdopen (fd, "rb+");
#endif
    if (!pdf->pdf) {
        pdf_free (pdf);
        return NULL;
    }

    return (void *)pdf;    
}

/* Helper to copy constant strings to dynamic memory
 * Used so default strings aren't a special case if they are
 * replaced - they can just be free()'d.
 */

static int dupstr (PDF *pdf, char **ptr) {
    char *p;

    if (!*ptr) {
        return 0;
    }

    p = (char *) malloc (strlen (*ptr) +1);
    if (!p) {
        pdf_free (pdf);
        return errno;
    }
    strcpy (p, *ptr);
    *ptr = p;

    return 0;
}

/* Set parameters.
 *
 * Applies some gross sanity checks (the magic numbers are based on
 * what reasonable hardware limits are).  Try to avoid ugly overflow;
 */
int pdf_set (PDF_HANDLE pdf, int arg, ...) {
    int r;
    va_list ap;
    valarg (ps);

    va_start (ap, arg);
    r = pdfset (ps, arg, ap);
    if (r != PDF_OK) {
        ps->errnum = r;
    }
    va_end (ap);

    return r;
}

/* Print a string to the pdf file
 * Parses string into lines and pages.
 * Handles <FF>, <LF> and <CR>.
 * Updates state - string does not need to be a complete
 * line or page.  If the length is available, supply it.
 * otherwise, specify PDF_USE_STRLEN for the obvious.
 * Note that \0s do not terminate the string if a length is specified.
 */

int pdf_print (PDF_HANDLE pdf, const char *string, size_t length) {
    int r;
    short lbuf[150];
    long nc = 0;
    short *parsed;

    valarg (ps);

    r = setjmp (ps->env);
    if (r) {
        ps->errnum = r;
        return r;
    }

    if (length == PDF_USE_STRLEN) {
        length = strlen (string);
    }

    if (!(ps->flags & PDF_WRITTEN)) {
#define pdf ps
        pdf->lpp = (long) (pdf->len * pdf->lpi);

        if (pdf->tof == ~1u) {
            pdf->tof = ((unsigned int) (pdf->top * pdf->lpi));
        }

        /* Some more checks - make sure there is a reasonable printable area.
         * VFUs impose a 2.0 in limit; tractors 3.0.  Done here because the
         * values interact & can't be validated until all have been set.
         */

        /* Min printable area 2.0 in high. Note that top/bot don't prevent printing. */
        if ( ((int)(pdf->len)) < 2.0 ) {
            ABORT (E(INCON_GEO));
        }

        /* Min printable area 3.0 in wide */
        if ( ((int)(pdf->wid - (2*(pdf->margin + pdf->lno)))) < 3.0 ) {
            ABORT (E(INCON_GEO));
        }

        /* Selected cols must fit in printable width */
        if ( ((int)(pdf->wid - (2*(pdf->margin + pdf->lno)))) < (int)(pdf->cols / pdf->cpi) ) {
            ABORT (E(INCON_GEO));
        }

        /* Printable height must have room for 4 lines */
        if ( (pdf->len * pdf->lpi) < 4 ) {
            ABORT (E(INCON_GEO));
        }

        /* TOF offset can't be more than a page. */
        if ( pdf->tof > (pdf->len * pdf->lpi)) {
            ABORT (E(INCON_GEO));
        }
        /* Bar height must be at least one line */
        if (pdf->formtype != PDF_JPEG && pdf->barh < 1.0 / (double)pdf->lpi) {
            ABORT (E(INCON_GEO));
        }
#undef pdf

        /* Lock down context as init may allocate objects */

        ps->flags |= PDF_ACTIVE;

        errno = 0;
        if (!(ps->flags & PDF_INIT)) {
            pdfinit (ps);
            ps->flags |= PDF_INIT;
        }

        /* Initial formfeed (sometimes with <cr> is common for printers, but would 
         * produce a blank page here.  If present, discard it (but include in the hash).
         * Do this only for the initial write to the file - not after resuming from a
         * checkpoint.
         */

        ps->parseused = 0;
        r = parsestr (ps, string, length, !(ps->flags & PDF_RESUMED));

        length = ps->parseused;
        ps->parseused = 0;

        ps->flags &= ~PDF_RESUMED;

        /* A write of length zero is useful because the file structure of
         * an updated file, as well as all pdf_set parameters, has been validated.
         * This early return ensures that the file won't be written until there is
         * new data in the file.  If a FF was seen and stripped, that counts as data
         * (otherwise another FF in the next call would also be removed.)
         */
        if (!length && !r) {
            return PDF_OK;
        }

        /* This is the actual first write, the file will be modified
         * There is similar code in pdfclose.
         */

        wrhdr (ps);
        if (!ps->formlen) {
            setform (ps);
        }

        /* PDF_WRITTEN has been set */

        if (errno) {
            ps->errnum = errno;
            ABORTps (errno);
        }
    } else {
        (void) parsestr (ps, string, length, 0);
        length = ps->parseused;
    }
    ps->parseused = 0;

    if (ps->errnum) {
        return ps->errnum;
    }

    if (!length) {
        return PDF_OK;
    }

    /* Line buffer must be flushed before any reference to state that
     * printing it might update.  It's effective for long strings of
     * text.
     */

#define FLUSH_LBUF { if (nc) {                   \
    add2line (ps, lbuf, nc);                     \
    nc = 0;                                      \
}}

    for(parsed = ps->parsebuf; length--;) {
        short c = *parsed++;

        if (c == '\f') {
            if (ps->line == 0) {
                ps->line = ps->tof +1;
            }
            FLUSH_LBUF;
            wrpage (ps);
            continue;
        } 
        if (ps->line > ps->lpp + ps->tof) {
            FLUSH_LBUF;
            wrpage (ps);
        }
        if (c == '\n') {
            if (ps->line == 0) {
                ps->line = ps->tof +1;
            }
            FLUSH_LBUF;
            ps->line++;
            continue;
        }
        /* Ordinary data.  If first on page, set line to TOF. */
        if (ps->line == 0) {
            ps->line = ps->tof +1;
        }

        lbuf[nc++] = c;

        if (nc >= sizeof (lbuf) -1) {
            FLUSH_LBUF;
        }
    }
    FLUSH_LBUF;
#undef FLUSH_LBUF

    return ps->errnum;
}

/* Return current location
 *
 * This is the physical location on the page, 1-based numbering.
 */

int pdf_where (PDF_HANDLE pdf, size_t *page, size_t *line) {
    size_t p, l;

    valarg (ps);

    p = ps->page +1;
    l = ps->line;
    if (l == 0) {
        l = ps->tof +1;
    }
    if (l > ps->lpp) {
        l -= (ps->lpp -1);
        p++;
    }
    p += ps->prevpc;

    if (page) {
        *page = p;
    }
    if (line) {
        *line = l;
    }
    return PDF_OK;
}

/* Get list of known font names */

const char *const* pdf_get_fontlist ( size_t *length ) {
    if (length) {
        *length = DIM (validFonts) -1;
    }
    return validFonts;
}
/* Get list of known form names */

const char *const* pdf_get_formlist ( size_t *length ) {
    size_t i;

    if (formlist) {
        return formlist;
    }
    formlist = (const char **)malloc ((DIM(colors) + 1) * sizeof (const char *));
    if (!formlist) {
        if (length) {
            *length = 0;
        }
        return NULL;
    }
    for (i = 0; i < DIM(colors); i++) {
        formlist[i] = colors[i].name;
    }
    formlist[i] = NULL;
    if (length) {
        *length = i;
    }

    return formlist;
}

/* Checkpoint a pdf file
 * This writes the metadata for the file and flushes
 * I/O buffers.  The first write thereafter will corrupt it again.
 * the file remains open and the PDF_HANDLE remains valid.
 * The file is positioned to overwrite the new metadata with page data.  This allows
 * processes (such as simulated LPTs) that leave files open for long periods
 * between bursts of writing to have some chance of surviving a crash.
 */

int pdf_checkpoint (PDF_HANDLE pdf) {
    int r = PDF_OK;
    unsigned int obj;
    SHA1Context sha1;

    valarg (ps);

    if (ps->flags & PDF_WRITTEN) {
        unsigned int line = ps->line;

        ps->line = 0;

        obj = ps->obj;
        ps->checkpp = ftell (ps->pdf);
        memcpy (&sha1, &ps->sha1, sizeof (sha1));

        r = pdfclose (ps, 1);

        memcpy (&ps->sha1, &sha1, sizeof (sha1));
        fseek (ps->pdf, ps->checkpp, SEEK_SET);
        ps->obj = obj;
        ps->line = line;
        fflush (ps->pdf);

        ps->flags &= ~PDF_WRITTEN;
        ps->flags |= PDF_RESUMED;
 
        if (r != PDF_OK) {
            ps->errnum = r;
        }
    }
    return r;
}

/* Obtain a snapshot of an active file.
 *
 * Checkpoints the active file and copies it in a consistent state to
 * the specified file.
 *
 */

int pdf_snapshot (PDF_HANDLE pdf, const char *filename) {
    FILE *fh;
    t_fpos fpos;
#define SNAP_BUFSIZE  (8192)
    unsigned char *buffer;
    size_t n;
    int r;

    valarg (ps);

    r = pdf_checkpoint (pdf);
    if (r != PDF_OK) {
        return r;
    }

    if ((buffer = (unsigned char *) malloc (SNAP_BUFSIZE)) == NULL) {
        return errno;
    }

    if ((fh = fopen (filename, "wb")) == NULL ) {
        free (buffer);
        return errno;
    }

    fpos = ftell (ps->pdf);

    if ((r = setjmp (ps->env)) != 0) {
        free (buffer);
        fseek (ps->pdf, fpos, SEEK_SET);
        fclose (fh);
        return r;
    }

    fseek (ps->pdf, 0, SEEK_SET);

    while ((n = fread (buffer, 1, SNAP_BUFSIZE, ps->pdf)) > 0) {
        size_t w;

        if ((w = fwrite (buffer, n, 1, fh)) != 1) {
            ABORTps (errno);
        }
     }

     fseek (ps->pdf, fpos, SEEK_SET);

     if (ferror (ps->pdf)) {
         ABORTps (E(IO_ERROR));
     }

     if (fclose (fh) == EOF) {
        ABORTps (errno);
     }

     free (buffer);

     return PDF_OK;
}

/* close a pdf file
 *
 * Writes updated metadata
 */

int pdf_close (PDF_HANDLE pdf) {
    valarg (ps);

    return pdfclose (ps, 0);
}

/* Test if a file seems to be a PDF file.
 * Simple header check.
 *
 * PDF_OK if it passes.
 */

int pdf_file (const char *name) {
    FILE *fh;
    char buf[100];
    char *p;

    if (!(fh = fopen (name, "rb"))) {
        return errno;
    }
    if (!fgets (buf, sizeof (buf), fh)) {
        fclose (fh);
        return errno;
    }
    fclose (fh);
    if (strncmp (buf, "%PDF-1.", 7)) {
        return E(NOT_PDF);
    }
    p = buf + 7;

    while (*p && isdigit (*p)) {
        p++;
    }
    if (*p == '\n') {
        return PDF_OK;
    }
    return E(NOT_PDF);
}

/* Print string with last error.
 */

void pdf_perror (PDF_HANDLE pdf, const char *s) {

    if (s && *s) {
        fprintf (stderr, "%s: ", s);
    }

    fprintf (stderr, "%s\n", pdf_strerror (pdf_error(pdf)));

    return;
}

/* Get string for error number
 */

const char *pdf_strerror (int errnum) {

    if (!errnum) {
        return "Success";
    }

    if (errnum >= E(BASE)) {
        errnum -= E(BASE);
        if (errnum >= DIM(errortext)) {
            errnum = 0;
        }
        return errortext[errnum];
    } else {
        return strerror (errnum);
    }
}

/* Get last error
 */

int pdf_error (PDF_HANDLE pdf) {
    int err;

    if (ps) {
        err = ps->errnum;
    } else {
        err = errno;
    }

    if (err >= E(BASE)) {
        if (err - E(BASE) >= DIM(errortext)) {
            err = E(BAD_ERRNO);
        }
    }

    return err;
}

/* Clear last error */

void pdf_clearerr (PDF_HANDLE pdf) {

    if (!pdf) {
        errno = E(BAD_HANDLE);
        return;
    }
    ps->errnum = errno = 0;

    return;
}


/* *** End of public API *** */

/* The work of pdf_set
 */

#define REJECT_NULL { if (svalue == NULL) {         \
                        pdf->errnum = E(INVAL);     \
                        return pdf->errnum; } }

static int pdfset (PDF *pdf, int arg, va_list ap) {
    unsigned int ivalue;
    double dvalue;
    const char *svalue;
    char **font = NULL;
    char *oldf;
    PDF oldvals;
    char tbuf[2048];
    int r = PDF_OK;
    size_t i;
    FILE *fh;

    if (pdf->flags & PDF_ACTIVE) {
        return E(ACTIVE);
    }

    switch (arg) {
    case PDF_FILE_REQUIRE:
        svalue = va_arg (ap, const char *);
        REJECT_NULL
        if (!xstrcasecmp (svalue, "NEW")) {
            pdf->frequire = PDF_FILE_NEW;
        } else if (!xstrcasecmp (svalue, "APPEND")) {
            pdf->frequire = PDF_FILE_APPEND;
        } else if (!xstrcasecmp (svalue, "REPLACE")) {
            pdf->frequire = PDF_FILE_REPLACE;
#if 0
        } else if (!xstrcasecmp (svalue, "AUTO")) {
            pdf->frequire = PDF_FILE_AUTO;
#endif
        } else {
            return E(BAD_SET);
        }
        return PDF_OK;

    case PDF_FORM_TYPE:
        svalue = va_arg (ap, const char *);
        REJECT_NULL
        for (i = 0; i < DIM (colors); i++) {
            if (!xstrcasecmp (svalue, colors[i].name)) {
                pdf->formtype = i;
                return PDF_OK;
            }
        }
        return E(UNKNOWN_FORM);

    case PDF_TEXT_FONT:
        svalue = va_arg (ap, const char *);
        font = &pdf->font;
        r = checkfont (svalue);
        break;

    case PDF_LNO_FONT:
        svalue = va_arg (ap, const char *);
        font = &pdf->font;
        r = checkfont (svalue);
        break;

    case PDF_LABEL_FONT:
        svalue = va_arg (ap, const char *);
        font = &pdf->font;
        r = checkfont (svalue);
        break;

    case PDF_FORM_IMAGE:
        svalue = va_arg (ap, const char *);
        REJECT_NULL
        if (!(fh = fopen (svalue, "rb"))){
            return errno;
        }
        fclose (fh);
        font = &pdf->formfile;
        pdf->formtype = PDF_JPEG;
        break;

    case PDF_TITLE:
        svalue = va_arg (ap, const char *);
        REJECT_NULL
        font = &pdf->title;
        oldf = tbuf;

        for (r = 0; svalue[r] && r < sizeof (tbuf) -2; r++) {
            if (svalue[r] == '\\' || svalue[r] == '(' || svalue[r] == ')') {
                *oldf++ = '\\';
            }
            *oldf++ = svalue[r];
        }
        *oldf = '\0';

        svalue = tbuf;
        r = PDF_OK;
        break;

    default:
        break;
    }

    if (font) {
        if (r != PDF_OK) {
            pdf->errnum = r;
            return r;
        }

        oldf = *font;
        *font = (char *) realloc (*font, strlen (svalue) + 1);
        if (!*font) {
            *font = oldf;
            return errno;
        }
        strcpy (*font, svalue);
        return PDF_OK;
    }

    dvalue = va_arg (ap, double);
    ivalue = (unsigned int) dvalue;

    if (dvalue < 0) {
        return E(NEGVAL);
    }

    memcpy (&oldvals, pdf, sizeof (PDF));

    r = setjmp (pdf->env);
    if (r) {
        memcpy (pdf, &oldvals, sizeof (PDF));
        return r;
    }

    switch (arg) {
    case PDF_NO_LZW: /* Debugging only */
        if (dvalue) 
            pdf->flags |= PDF_UNCOMPRESSED;
        else 
            pdf->flags &= ~PDF_UNCOMPRESSED;
        return PDF_OK;

    case PDF_TOP_MARGIN:
        pdf->top = dvalue;
        break;

    case PDF_TOF_OFFSET:
        pdf->tof = ivalue;
        break;

    case PDF_BOTTOM_MARGIN:
        pdf->bot = dvalue;
        break;

    case PDF_SIDE_MARGIN:
        if (dvalue < 0.350) {
            ABORT (E(INVAL));
        }
        pdf->margin = dvalue;
        break;

    case PDF_LNO_WIDTH:
        if (dvalue && dvalue < 0.1) {
            ABORT (E(INVAL));
        }
        pdf->lno = dvalue;
        break;

    case PDF_BAR_HEIGHT:
        if (dvalue < 0.0) {
            ABORT (E(INVAL));
        }
        pdf->barh = dvalue;
        break;

    case PDF_CPI:
        if (dvalue < 1.0 || dvalue > 20.0) {
            ABORT (E(INVAL));
        }
        pdf->cpi = dvalue;
        return PDF_OK;

    case PDF_LPI:
        if (dvalue != 6.0 && dvalue != 8.0) {
            ABORT (E(INVAL));
        }
        pdf->lpi = ivalue;
        return PDF_OK;

    case PDF_PAGE_WIDTH:
        if (dvalue < 3.0) {
            ABORT (E(INVAL));
        }
        pdf->wid = dvalue;
        break;

    case PDF_PAGE_LENGTH:
        if (dvalue < 2.0) {
            ABORT (E(INVAL));
        }
        pdf->len = dvalue;
        break;

    case PDF_COLS:
        pdf->cols = ivalue;
        break;

    default:
        ABORT (E(BAD_SET));
    }

#undef REJECT_NULL
    return PDF_OK;
}

static int checkfont (const char *newfont) {
    const char *const *vf = validFonts;

    if (!newfont) {
        return E(INVAL);
    }

    while (*vf) {
        if (!strcmp (*vf++, newfont)) {
            return PDF_OK;
        }
    }
    return E(UNKNOWN_FONT);
}

/* At first output, initialize for output
 */

static void pdfinit (PDF *pdf) {    
    int r;

    SHA1Reset (&pdf->sha1);

    if (ftell (pdf->pdf)) {
        ABORT (errno);
    }

    if (pdf->frequire == PDF_FILE_APPEND) {
        r = checkupdate (pdf);
        if (r == PDF_OK) {
            return;
        }
        if (r > 1) {
            ABORT (r);
        }
        /* r < 0 => empty file, write as new */
    } else {
        if (fseek (pdf->pdf, 0, SEEK_END)) {
            ABORT (errno);
        }
        if (ftell (pdf->pdf)) {
            if (pdf->frequire == PDF_FILE_NEW) {
                ABORT (E(NOT_EMPTY));
            }
            /* Existing, overwrite (PDF_FILE_REPLACE) */
            fseek (pdf->pdf, 0, SEEK_SET);
            r = PDF_OK;
#ifdef _WIN32
            if (_chsize (_fileno (pdf->pdf), 0) == -1) {
                r = E(IO_ERROR);
            }
#else
            if (ftruncate (fileno (pdf->pdf), 0) == -1) {
                r = E(IO_ERROR);
            }
#endif
            if (r != PDF_OK) {
                ABORT (r);
            }
        }
        /* File empty, or emptied.  */
    }

    /* New file */

    pdf->pbase = 1;

    return;
}

/* Handle the gymnastics for adding pages to an existing file.
 * Does not handle the general case PDF, but will update those
 * written by this library.  Produces unbalanced page trees to
 * minimize work in update.
 *
 * returns PDF_OK if updating, < 0 if OK to write (empty file)
 * ABORTs on most errors.
 */

static int checkupdate (PDF *pdf) {
    char buf[512];
    char *p, *trail, *q;
    size_t tsize, ll;
    t_fpos amt = -1;
    int lf = 0;
    unsigned int obj, objs, objn, gen;
    t_fpos objp;
    t_fpos end;

    /* Make sure file is seekable, if zero length, treat as new. */

    if (fseek (pdf->pdf, 0, SEEK_END)) {
        ABORT (errno);
    }
    end = ftell (pdf->pdf);
    if (!end) {
        return -1;
    }

    /* Validate PDF header */

    fseek (pdf->pdf, 0, SEEK_SET);
    if (!fgets (buf, sizeof (buf), pdf->pdf)) {
        fclose (pdf->pdf);
        ABORT (errno);
    }
    if (strncmp (buf, "%PDF-1.", 7)) {
        ABORT (E(NOT_PDF));
    }
    p = buf + 7;

    while (*p && isdigit (*p)) {
        p++;
    }
    if (*p != '\n') {
        ABORT (E(NO_APPEND));
    }
    /* Probable PDF.  Find the XREF
     * This is painful, but will find the right place.
     *
     * The last three lines of the file must be
     * startxref
     *  byte offset of xref table
     * %%EOF
     *
     * 4 \ns ensure that the startxref is at the start of a line.
     * We could read a hunk of the file from the end into a buffer,
     * but there are lots of corner cases, and this only happens
     * once per open and it should not have to read much of the
     * file.  Finally, a use for 1 step forward, two steps back...
     */
    fseek (pdf->pdf, end, SEEK_SET);
    p = buf + sizeof (buf) -1;
    *p-- = '\0';

    while (p > buf +1 && --end > 0 && lf < 4) {
        int c;
        fseek (pdf->pdf, amt, SEEK_CUR);
        c = fgetc (pdf->pdf);
        if (c == EOF) {
            ABORT (E(NO_APPEND));
        }
        *--p = c;
        if ( c == '\n' ) {
            lf++;
        }
        amt = -2;
    }
    if (strncmp (p, "\nstartxref\n", 11)) {
        ABORT (E(NO_APPEND));
    }
    p += 11;

    pdf->xpos = 0;
    while (*p && *p != '\n') {
        if (!isdigit (*p)) {
            ABORT (E(NO_APPEND));
        }
        pdf->xpos *= 10;
        pdf ->xpos += *p++ - '0';
    }
    if (pdf->xpos <= 9 ||
        pdf->xpos >= ftell (pdf->pdf) ||
        strncmp (p, "\n%%EOF\n", 7)) {
        ABORT (E(NO_APPEND));
    }

    /* Found the xref pointer and verified EOF
     * For the moment, the only version of the xref that will
     * be processed is the one written by this library.  The full
     * variety of deleted pages, etc is more than what's needed.
     */
    fseek (pdf->pdf, pdf->xpos, SEEK_SET);
    fgets (buf, sizeof (buf), pdf->pdf);
    if (strcmp (buf, "xref\n")) {
        ABORT (E(NO_APPEND));
    }

    /* section header - there should only be one
     *
     * dec dec \n := first object in section, # objects in section
     */
    fgets (buf, sizeof (buf), pdf->pdf);
    objs = 0;
    p = buf;
    while (*p && isdigit (*p)) {
        objs = (objs * 10) + *p++ - '0';
    }
    if (p == buf || *p != ' ' || objs != 0) {
        ABORT (E(NO_APPEND));
    }
    p++;

    objn = 0;
    while (*p && isdigit (*p)) {
        objn = (objn * 10) + *p++ - '0';
    }
    if (p == buf || *p != '\n' || objn < 4) { /* Must be at least freelist, info, cat, page dir */
        ABORT (E(NO_APPEND));
    }

    /* Read the xref into the context. */

    for (lf = 0; ((unsigned int)lf) < objn; lf++) {
        int i;

        fgets (buf, sizeof (buf), pdf->pdf);
        objp = 0;
        for (p = buf, i = 0; i < 10; i++) {
            if (!isdigit (*p)) {
                ABORT (E(NO_APPEND));
            }
            objp = (objp * 10) + *p++ - '0';
        }
        if (*p++ != ' ') {
            ABORT (E(NO_APPEND));
        }

        gen = 0;
        for (i = 0; i < 5; i++) {
            if (!isdigit (*p)) {
                ABORT (E(NO_APPEND));
            }
            gen = (gen * 10) + *p++ - '0';
        }
        if (*p++ != ' ') {
            ABORT (E(NO_APPEND));
        }
        if (p[0] == 'f' && gen == 65535 && objp == 0 && lf == 0) {
            continue;
        }
        if (gen != 0 || objp == 0 || p[0] != 'n') {
            ABORT (E(NO_APPEND));
        }

        obj = addobj (pdf);
        if (obj != objs + lf) {
            ABORT (E(NO_APPEND));
        }
        pdf->xref[obj-1]= objp;
    }
    /* Advance to the trailer */
    do {
        if (!fgets (buf, sizeof (buf), pdf->pdf) ) {
            ABORT (E(NO_APPEND));
        }
    } while ( strcmp (buf, "trailer\n"));

    /* read the trailer into a buffer */

    trail = NULL;
    tsize = 0;
    do {
        if (!fgets (buf, sizeof (buf), pdf->pdf) ) {
            free (trail);
            ABORT (E(NO_APPEND));
        }
        if (!strcmp (buf, "startxref\n")) {
            break;
        }
        ll = strlen (buf);
        p = (char *) realloc (trail, tsize + ll +1);
        if (!p) {
            free (trail);
            ABORT (errno);
        }
        trail = p;
        strcpy (p + tsize, buf);
        tsize += ll;
    } while ( 1 );

    if (!trail) {
        ABORT (E(NO_APPEND));
    }

    /* Extract the data needed to navigate and to restore at close */
    q = "/ID [";
    if (!(p = strstr (trail, q))) {
        free (trail);
        ABORT (E(NO_APPEND));
    }
    p += strlen (q);

    while (*p == ' ')
            p++;
    if ( *p++ != '<') {
        free (trail);
        ABORT (E(NO_APPEND));
    }
    if (strlen (p) < (SHA1HashSize*2) + 1 || p[SHA1HashSize*2] != '>') {
        free (trail);
        ABORT (E(NO_APPEND));
    }
    memcpy (pdf->oid, p, SHA1HashSize*2);
    SHA1Input (&pdf->sha1, (uint8_t *)p, SHA1HashSize*2);

    pdf->iobj  = getref (pdf, trail, "/Info");

    /* Old catalog will be new start of page streams */

    pdf->pbase = getref (pdf, trail, "/Root");

    if (pdf->pbase >= pdf->iobj) {
        free (trail);
        ABORT (E(NO_APPEND));
    }

    /* Follow link to the document information object */

    (void) readobj (pdf, pdf->iobj, &trail, &tsize);
    if (!strstr (trail, "/Producer (LPTPDF Version ")) {
        free (trail);
        ABORT (E(NOT_PRODUCED));
    }

    p = getstr (pdf, trail, "/CreationDate");
    if (!p || strlen (p) >= sizeof (pdf->ctime) || strlen(p) < strlen ("(D:YYYY)") ||
        strncmp (p, "(D:", 3) || p[strlen (p) -1] != ')') {
        free (p);
        free (trail);
        ABORT (E(NO_APPEND));
    }
    strcpy (pdf->ctime, p+3);
    pdf->ctime[strlen (pdf->ctime) -1] ='\0';
    free (p);

    /* Follow link to root (catalog), soon to be over-written */

    (void) readobj (pdf, pdf->pbase, &trail, &tsize);
    if (!strstr (trail, "/Type /Catalog")) {
        free (trail);
        ABORT (E(NO_APPEND));
    }

    /* Page tree is anchor node for all previous sessions */
    
    pdf->aobj = getref (pdf, trail, "/Pages");
    if (pdf->aobj != pdf->pbase -1) {
        free (trail);
        ABORT (E(NO_APPEND));
    }

    /* Get current page count, confirming this is the tree root (no /Parent) */

    pdf->anchorp = readobj (pdf, pdf->aobj, &trail, &tsize);
    if (!strstr (trail, "/Type /Pages") || strstr (trail, "/Parent")) {
        free (trail);
        ABORT (E(NO_APPEND));
    }
    pdf->prevpc = getint (pdf, trail, "/Count", (const char **)&q);

    /* Save trailer for wrhdr, both first write and checkpoints */

    pdf->trail = trail;

    /* Ready for update, in update mode */

    pdf->flags |= PDF_UPDATING;

    /* New objects are allocated starting with pdf->pagebase - the old catalog */

    pdf->obj = pdf->pbase -1;

    return PDF_OK;
}

/* Write the file header on first output of data.
 * If update allowed, activates update processing instead.
 * Validates sensible parameters.
 */

static void wrhdr (PDF *pdf) {
    char *trail, *q;

    if (!(pdf->flags & PDF_UPDATING)) {
        /* New file.  First time, write file ID.
         * After that, resuming from checkpoint.  File is already positioned.
         */
        if (!pdf->checkpp) {
            if (fputs ("%PDF-1.4\n%\0302\0245\0302\0261\0303\0253\n", pdf->pdf) == EOF) {
               ABORT (E(IO_ERROR));
            }
        }
        pdf->flags |= PDF_WRITTEN;
        return;
    }

    /* Position to over-write the old anchor, catalog, info object with
     * a new /Pages node that holds all existing pages.  The parent will be
     * determined at the end of this session, so enough space is left to be able
     * to patch it during pdf_close.
     */

    trail = pdf->trail;

    if (pdf->prevpc != getint (pdf, trail, "/Count", (const char **)&q)) {
        ABORT (E(INCON_GEO));
    }

    pdf->flags |= PDF_WRITTEN;

    fseek (pdf->pdf, pdf->anchorp, SEEK_SET);
    fprintf (pdf->pdf, "%u 0 obj\n%.*s /Parent ", pdf->aobj, q - trail, trail);

    /* From here on, the file has been written and is corrupt.
     * Hopefully, a temporary condition, but errors will be permanent.
     */
    pdf->anchorpp = ftell (pdf->pdf);
    fprintf (pdf->pdf, "%10.10s 0 R %s\nendobj\n\n", "", q);

    /* When resuming from chckpoint, restore position for next page */

    if (pdf->checkpp) {
        fseek (pdf->pdf, pdf->checkpp, SEEK_SET);
    }

    if (ferror (pdf->pdf)) {
        ABORT (E(IO_ERROR));
    }

    return;
}

/* Write out data stream for current page.
 *
 * This is actually buffered in memory with formatting to enable compression.
 *
 * Updates page and line numbers.
 */

static void wrpage (PDF *pdf) {
    double lm = xp( pdf->margin ) +
        xp( ((pdf->wid - (pdf->margin *2)) - (pdf->cols/pdf->cpi))/2 );

    unsigned int obj, l;

    pdf->pbused = 0;

    /* Render the page up to lpp.
     */

    if (pdf->line > pdf->lpp) {
        pdf->line = pdf->lpp;
    }
    obj = addobj (pdf);

    /* Graphics are precomputed, so simply add the content */

    wrstm (pdf, PAGEBUF, pdf->formbuf, pdf->formlen);

    /* Text */

    wrstmf (pdf, PAGEBUF,
        " q 0 Tr %s rg BT /F1 %u Tf 1 0 0 1 %f %f Tm  %u TL %u Tc %u Tz %u %u Td",
             RGB_BLACK,
             PT/pdf->lpi,
             lm, 0.0,
             (unsigned int)( PT/pdf->lpi ),
             0,
             100,
             0, (unsigned int)( (pdf->len * PT) +2) );

    for (l = 0; l < pdf->line && l < pdf->nlines; l++) {
        short *c = pdf->lines[l];
        if (c) {
            int online = 0;
            unsigned int col, pcol;
            short ch;

            for (col = 0, pcol = 0; col < pdf->linelen[l]; col++, c++) {

                if (!online) {
                    wrstm (pdf, PAGEBUF, QS(" T* ("));
                    online = 1;
                }
                ch = *c;
                if (ch == '\\' || ch == '(' || *c == ')') {
                    wrstm (pdf, PAGEBUF, QS("\\"));
                } else {
                    if (ch == '\015') {
                        unsigned int p;
                        for (p = col+1; p < pdf->linelen[l]; p++) {
                            short ch = pdf->lines[l][p];
                            if (ch == '\015' || ch == ' ') {
                                continue;
                            }
                            /* Data follows, setup overprint */
                            pcol = 0;
                            wrstmf (pdf, PAGEBUF, QS(")Tj 0 0 Td ("));
                            break;
                        }
                        continue;
                    }
                }
                pcol++;
                /* Optimize the most common case:  adding one character */
                if (pdf->pbused +1 > pdf->pbsize ) {
                    wrstmf (pdf, PAGEBUF, "%c", (char) ch);
                } else {
                    pdf->pagebuf[pdf->pbused++] = (char) ch;
                }
            }
            if (online) {
                wrstm (pdf, PAGEBUF, QS(")Tj"));
            } else {
                wrstm (pdf, PAGEBUF, QS(" T*"));
            }
            pdf->linelen[l] = 0;
        } else {
            wrstm (pdf, PAGEBUF, QS(" T*"));
        }
    }
    wrstm (pdf, PAGEBUF, QS(" ET Q"));

    /* Done with rendering this physical page */

    pdf->page++;
    pdf->line = 0;

    /* Lines may have been written for the next page due to a TOF_OFFSET.
     * Swap them with the the (now empty) lines at the top of the new page.
     * If they have been written, set the line accordingly.
     */
    if (pdf->tof < pdf->nlines) {
        for (l = 0; l < pdf->tof; l++) {
            unsigned int el = pdf->lpp + l;

            if (el >= pdf->nlines) {
                break;
            }
            if (pdf->lines[el]) {
                short *t;
                unsigned int ln;

                t              = pdf->lines[l];
                pdf->lines[l]  = pdf->lines[el];
                pdf->lines[el] = t;

                ln =               pdf->linelen[l];
                pdf->linelen[l] =  pdf->linelen[el];
                pdf->linelen[el] = ln;

                ln =                pdf->linesize[l];
                pdf->linesize[l] =  pdf->linesize[el];
                pdf->linesize[el] = ln;

                if (pdf->linelen[l]) {
                    pdf->line = pdf->tof +1;
                }
            }
        }
    }

    /* The rendering data is ready for the file.
     *  Unless forbidden, see if it's compressible.
     *  Write the PDF stream accordingly.
     */
    if ((pdf->flags & PDF_UNCOMPRESSED) || encstm (pdf, pdf->pagebuf, pdf->pbused)) {
        fprintf (pdf->pdf, "%u 0 obj\n"
                 "<< /Length %u >>\n"
                 "stream\n", obj, pdf->pbused);
        fwrite (pdf->pagebuf, pdf->pbused, 1, pdf->pdf);
    } else {
        fprintf (pdf->pdf, "%u 0 obj\n"
                 "  << /Length %u /DL %u /Filter /LZWDecode"
                 " /DecodeParams << /EarlyChange 0 >> >>\n"
                 "stream\n", obj, pdf->lzwused, pdf->pbused);
        fwrite (pdf->lzwbuf, pdf->lzwused, 1, pdf->pdf);
    }
    fputs ("\nendstream\n"
                "endobj\n"
           "\n", pdf->pdf);

    if (ferror (pdf->pdf)) {
        pdf->errnum = E(IO_ERROR);
    }
    return;
}

/* Setup form */

static void setform (PDF *pdf) {
    double tb = yp( pdf->top );                /* Top border */
    double li = xp( pdf->margin );             /* Left inner (line) */
    double ri = xp( pdf->wid - pdf->margin );  /* Right inner */
    double lo = li - xp( pdf->lno );           /* Left outer */
    double p;
    unsigned int l;
    const COLORS *color = &colors[(pdf->formtype == PDF_JPEG)?
                                    PDF_PLAIN: pdf->formtype];

    /* Setup items common to all pages */

    /* Holes */
    wrstmf (pdf, FORMBUF, 
        " q 1 w %s rg %s RG", RGB_HOLE_FILL, RGB_HOLE_LINE);
/* These constants define the standard tractor feed requirements.
 * There is no reason to make them user-accessible.
 */
#define HOLE_DIA  (0.1575)
#define HOLE_VSP  (0.500)   /* Vertical space between holes */
#define HOLE_HPOS (0.236)   /* Horizontal distance of center from edge */
#define HOLE_VOFS (0.250)   /* Vertical offset of first & last holes from edge */

    for( p = HOLE_VOFS; p <= (pdf->len - HOLE_VOFS); p += HOLE_VSP ) {
        circle (pdf, xp(HOLE_HPOS),            yp(p), xp(HOLE_DIA/2) );
        circle (pdf, xp(pdf->wid - HOLE_HPOS), yp(p), xp(HOLE_DIA/2) );
    }
    wrstm (pdf, FORMBUF, QS(" B Q"));

    /* Customizable content */

    if (pdf->formtype != PDF_PLAIN) {
        wrstm (pdf, FORMBUF, QS(" q "));

        switch (pdf->formtype) {
        case PDF_JPEG:
            imageform (pdf);
            break;
        default:
            barform (pdf);
            break;
        }

        wrstm (pdf, FORMBUF, QS(" Q"));
    }

    /* Line numbers - apply after any enclosing paths */

    if (pdf->lno) {
        wrstmf (pdf, FORMBUF, 
            " q 1 w BT 0 Tr %s rg"
            " /F3 %u Tf 55 Tz 1 0 0 1 %f %f Tm %u TL (6)' /F2 %u Tf",
             color->text,
             PT/6,
             lo, tb+(PT/6),
             PT/6,
             PT/6
             );
        for (l = 1; l <= ((pdf->len - (pdf->top+pdf->bot)) * 6); l++) { /* 6 LPI labels */
            wrstmf (pdf, FORMBUF, " (%2u)'", l);
        }

        wrstmf (pdf, FORMBUF, 
            " /F3 %u Tf 1 0 0 1 %f %f Tm 65 Tz %u TL (8)' /F2 %u Tf",
                 PT/8,
                 ri, tb+(PT/8),
                 PT/8,
                 PT/8
                 );
        for (l = 1; l <= ((pdf->len - (pdf->top+pdf->bot)) * 8); l++) { /* 8 LPI labels */
            wrstmf (pdf, FORMBUF, " (%2u)'", l);
        }

        wrstm (pdf, FORMBUF, QS(" ET Q"));
    }

    return;
}

/* Generate a colorbar form body
 */

static void barform (PDF *pdf) {
    double tb = yp( pdf->top );                /* Top border */
    double bb = yp( pdf->len - pdf->bot );     /* Bottom border */

    double li = xp( pdf->margin );             /* Left inner */
    double ri = xp( pdf->wid - pdf->margin );  /* Right inner */

    double lnw = xp ( pdf->lno );              /* Line number col width */

    double lo = li - xp( pdf->lno );           /* Left inner */
    double ro = ri + xp( pdf->lno );           /* Right inner */

    double cbr = lnw / 2;                      /* Corner border radius */
    double k   = CircleK * cbr;

    unsigned int bars, b;
    const COLORS *color = &colors[(pdf->formtype == PDF_JPEG)?
                                    PDF_PLAIN: pdf->formtype];

    /* Draw outline clockwise as a closed path */
    wrstmf (pdf, FORMBUF,
            "1 w %s RG %s rg %f %f m %f %f %f %f %f %f c %f %f l"
            " %f %f %f %f %f %f c %f %f l %f %f %f %f %f %f c"
            " %f %f l %f %f %f %f %f %f c h",
            color->line, RGB_WHITE,            /* Line, leave inside white */
            lo, tb-cbr,                        /* Top left */
            lo, tb-cbr+k, lo+cbr-k, tb, lo+cbr, tb,
            ri, tb,                            /* Top right */
            ri+cbr+k, tb, ro, tb-cbr+k, ro, tb-cbr,
            ro, bb+cbr,                        /* Bottom right */
            ro, bb+cbr-k, ri+cbr+k, bb, ri+cbr, bb,
            li, bb,                            /* Bottom left */
            lo+cbr-k, bb, lo, bb+cbr-k, lo, bb+cbr
        );
    /* Inner lines */
    if (pdf->lno) {
        wrstmf (pdf, FORMBUF, " %f %f m %f %f l %f %f m %f %f l",
                li, tb,                        /* Left top */
                li, bb,                        /* Left inside */
                ri, bb,                        /* Bottom right */
                ri, tb                         /* Right inside */
            );
    }
    wrstmf (pdf, FORMBUF, " B %s rg %s RG", color->bar, color->line);

    /* Bars */
    bars = (unsigned int) (((pdf->len - (pdf->top+pdf->bot)) / pdf->barh) +0.5);

    for (b = 0; b < bars; b++) {
        double bart = tb - (b * (pdf->barh * PT));
        double barb = bart - (pdf->barh *PT);

        if( !(b & 1) ) {
            wrstmf (pdf, FORMBUF, " %f %f %f %f re",
                     li, barb, ri - li, bart - barb );
        }
    }
    wrstm (pdf, FORMBUF, QS(" B"));

    return;
}

/* Generate body for an image-based form
 * This will also write an XObject with the image data.
 */

static void imageform (PDF *pdf) {
    unsigned int obj, len;
    t_fpos fs;
    FILE *fh;
    int c;
    double imgwid, imghgt, pw, sh, scale, vpos;
    unsigned char buf[17];

    if (pdf->flags & PDF_INIT) { /* Can't change image after init */
        return;
    }

    if (!(fh = fopen (pdf->formfile, "rb"))) {
        ABORT (errno);
    }

    /* Parse the JPEG to get the dimensions of the image.
     */

    if (fread (buf, 4, 1, fh) != 1) {
        fclose (fh);
        ABORT (E(BAD_JPEG));
    }
    if (!(buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF &&
          (buf[3] & ~0x01) == 0xE0)) {
        fclose (fh);
        ABORT (E(BAD_JPEG));
    }
    c = buf[3];
    fseek (fh, -2, SEEK_CUR);

    while (1) {
        if (c == 0xDA) {
            fclose (fh);
            ABORT (E(BAD_JPEG));
        }
        while (1) {
            c = fgetc (fh);
            if (c == EOF) {
                fclose (fh);
                ABORT (E(BAD_JPEG));
            }
            if (c == 0xFF)
                break;
        }
        while (1) {
            c = fgetc (fh);
            if (c == EOF) {
                fclose (fh);
                ABORT (E(BAD_JPEG));
            }
            if (c != 0xFF)
                break;
        }
        if (c >= 0xC0 && c <= 0xC3) {
            if (fread (buf, 7, 1, fh) != 1) {
                fclose (fh);
                ABORT (E(BAD_JPEG));
            }
            imgwid = (double) ((buf[5] << 8) | buf[6]);
            imghgt = (double) ((buf[3] << 8) | buf[4]);
            break;
        }
        if (fread (buf, 2, 1, fh) != 1 ) {
            fclose (fh);
            ABORT (E(BAD_JPEG));
        }
        len = (buf[0] << 8) | buf[1];
        if (len < 2) {
            fclose (fh);
            ABORT (E(BAD_JPEG));
        }
        fseek (fh, len -2, SEEK_CUR);
    }

    /* Get the file size */

    fseek (fh, 0, SEEK_END);
    fs = ftell (fh);

    /* Write an XObject dictionary and stream
     * Note: This is NOT bufferered; there is one per session.
     */

    fseek (fh, 0, SEEK_SET);

    if (errno || ferror (fh)) {
        fclose (fh);
        ABORT (E(BAD_JPEG));
    }

    pdf -> formobj =
        obj = addobj(pdf);
    fprintf (pdf->pdf, "%u 0 obj\n<< /Type /XObject /Subtype /Image"
        " /Width %u /Height %u /Length %lu /Filter /DCTDecode"
        " /BitsPerComponent 8 /ColorSpace /DeviceRGB >>\nstream\n",
        obj, ((unsigned int)imgwid), ((unsigned int)imghgt), fs);

    while ((c = fgetc (fh)) != EOF) {
        fputc (c, pdf->pdf);
    }

    if (ferror (fh) || fclose (fh) == EOF) {
        ABORT (E(OTHER_IO_ERROR));
    }
    fprintf (pdf->pdf, "\nendstream\nendobj\n\n");

    /* Scale to usable page width and center vertically.
     * Add to the per-page form data.
     */

    pw = pdf->wid-(2*(pdf->margin + pdf->lno));
    scale = pw / imgwid;
    sh = imghgt * scale * PT;
    vpos = ((pdf->len *PT) - sh)/2;
    wrstmf (pdf, FORMBUF, " %f 0 0 %f %f %f cm /form Do",
        xp(pw), sh, xp(pdf->margin+pdf->lno), vpos);

    pdf->pbase = obj +1;
}

/* The work of close.
 * Metadata for this session is written here.
 *
 * The /Pages are written at this point, since the location of their
 * parent /Pages is now computable.  Also written are font dictionaries
 * and any form image.
 *
 * Each session ends with a /Pages anchor, which is what enables appending
 * without finding or updating all the previous sessions.
 *
 * If this an update session, the old page tree (found during open)
 * is made a subtree of the new anchor, and its page count added.
 *
 * The rest of the metadata has been overwritten with new page streams if
 * the file is updated.
 *
 * For checkpoint, everything is done, except that the file is left open
 * and the PDF is not freed.  The work of checkpoint is done in pdf_checkpoint.
 *
 * Any PDF field updated here that controls writing metadata needs to be
 * saved/restored in pdf_checkpoint.  Try to avoid that; in a normal close,
 * the PDF will be destroyed, so there's no point in putting state there.
 */

static int pdfclose (PDF *pdf, int checkpoint) {
    int r = PDF_OK;
    long l;
    uint8_t hash[SHA1HashSize];
    char id[1 + 2*sizeof(hash)];
    unsigned int p, cat, plist, anchor;
    unsigned int aobj, iobj;
    struct tm *tm;
    time_t now;
    char tbuf[32], ibuf[513];
    t_fpos xref;

    if (!pdf->pdf) { /* File never opened */
        if (!checkpoint) {
            free (pdf);
        }
        errno = E(NOT_OPEN);
        return errno;
    }
    valarg (pdf);

    r = setjmp (pdf->env);
    if (r) {
        if (!checkpoint) {
            pdf_free (pdf);
        }
        errno = r;
        return r;
    }

    if (pdf->line && !(pdf->flags & PDF_WRITTEN) && (pdf->flags & PDF_INIT)) {
        /* If a checkpoint held a partial page, the headers have not been written
         * if there was not subsequent output.
         * In this case, the headers and the partial page are written here.
         */
        wrhdr (pdf);
        if (!pdf->formlen) {
            setform (pdf);
        }
    }

    if (!(pdf->flags & PDF_WRITTEN)) {
        if (!checkpoint) {
            fclose (pdf->pdf);
            pdf_free (pdf);
        }
        return PDF_OK;
    }

    /* Force out last page if anything was written on it */

    if (pdf->line != 0) {
        wrpage (pdf);
    }

    /* Page list for this session */

    plist = addobj (pdf);
    anchor = plist + 1 + 1 + pdf->page;
    fprintf (pdf->pdf, "%u 0 obj\n"
             " << /Type /Pages /Kids [", plist);
    
    for(p = 0; p < pdf->page; p++) {
        fprintf (pdf->pdf," %u 0 R",
                 (plist + 1 + 1 + p));
    }
    fprintf (pdf->pdf, "] /Count %u /Parent %010u 0 R >>\nendobj\n\n",
                        pdf->page, anchor);

    /* Font directory object */

    (void) addobj (pdf);
    fprintf (pdf->pdf, "%u 0 obj\n"
             " << /F1 << /Type /Font /Subtype /Type1 /BaseFont /%s >>"
             " /F2 << /Type /Font /Subtype /Type1 /BaseFont /%s >>"
             " /F3 << /Type /Font /Subtype /Type1 /BaseFont /%s >> >>\n"
             "endobj\n\n", plist+1, pdf->font, pdf->nfont, pdf->nbold);

    /* Each page leaf object */

    for( p = 0; p < pdf->page; p++) {
        unsigned int obj = addobj (pdf);

        fprintf (pdf->pdf, "%u 0 obj\n"
                 " << /Type /Page /Parent %u 0 R /Resources << /Font %u 0 R"
                 " /ProcSet [/PDF /Text /ImageC /ImageI /ImageB]",
                 obj, plist, plist +1);
        if (pdf->formobj) {
            fprintf (pdf->pdf, " /XObject << /form %u 0 R >>", pdf->formobj);
        }
        fprintf (pdf->pdf, " >> /MediaBox [0 0 %f %f] /Contents %u 0 R >>\n"
                 "endobj\n\n", pdf->wid * PT, pdf->len * PT,
                 pdf->pbase + p );
    }

     /* anchor pagelist for this session */

    aobj = addobj (pdf);
    cat = aobj +1;
    fprintf (pdf->pdf, "%u 0 obj\n"
                " << /Type /Pages /Kids [", aobj);
    if (pdf->aobj) {     /* If previous session, link to it */
        fprintf (pdf->pdf,
                "%u 0 R ", pdf->aobj);
    }
    fprintf (pdf->pdf,
                 "%u 0 R] /Count %u >>\n"
                "endobj\n\n", plist, pdf->page + pdf->prevpc);

    /* Write catalog */

    cat = addobj (pdf);
    fprintf (pdf->pdf, "%u 0 obj\n"
             "  << /Type /Catalog /Pages %u 0 R"
             " /PageLayout /SinglePage"
             " /ViewerPreferences << ", cat, aobj);
    fputs ((pdf->wid > pdf->len)?
            " /Duplex /DuplexFlipLongEdge":
            " /Duplex /DuplexFlipShortEdge", pdf->pdf);
    if (strcmp (pdf->title, defaults.title)) {
        fputs (
            " /DisplayDocTitle true", pdf->pdf);
    }
    fputs (
        " /PickTrayByPDFSize true >> >>\n"
        "endobj\n\n", pdf->pdf);

    /* Document information object */

    now = time (NULL);
    tm = localtime (&now);
    strftime (tbuf, sizeof (tbuf) -1, "%Y%m%d%H%M%S", tm);

    iobj = addobj (pdf);
    sprintf (ibuf, "%u 0 obj\n"
             "  << /Title (%s) /Creator (Midnight Engineering)"
             " /Subject (Preserving the history of computing)"
             " /Producer (LPTPDF Version 1.0)"
             " /CreationDate (D:%s) /ModDate (D:%s) >>\n"
             "endobj\n\n", iobj, pdf->title,
             ((pdf->flags & PDF_UPDATING)? pdf->ctime: tbuf), tbuf);

    SHA1Input (&pdf->sha1, (uint8_t *)ibuf, strlen (ibuf));
    fputs (ibuf, pdf->pdf);

    /* Write the xref */

    xref = ftell (pdf->pdf);
    /* Trailing space is part of required 2-byte EOL marker in xref entries */
    fprintf (pdf->pdf,"xref\n"
             "0 %u\n"
             "%010u %05u f \n",                /* << TSP */
             1+pdf->obj, 0, 65535);

    for( p = 0; ((unsigned int)p) < pdf->obj; p++ ) {
        fprintf (pdf->pdf,"%010lu %05u n \n", /* << TSP */
            pdf->xref[p], 0);
    }

    /* Write trailer */

    SHA1Result (&pdf->sha1, hash);
    for (l = 0; l < SHA1HashSize; l++) {
        sprintf (id+(l*2), "%02X", ((int)hash[l] & 0xFF));
    }

    fprintf (pdf->pdf,"trailer\n"
             " << /Root %u 0 R /Size %u /Info %u 0 R /ID [<%s> <%s>] >>\n"
             "startxref\n"
             "%lu\n"
             "%%%%EOF\n",
             cat, pdf->obj +1, iobj,
             ((pdf->oid[0])? pdf->oid: id), id, xref);

    /* There may be an obscure corner case where a file is opened for
     * append with a much shorter title and a trivial page, so the new
     * EOF is before the old.  Although it seems unlikely, truncate the
     * file at the correct place to be safe.
     */

#ifdef _WIN32
    if (_chsize (_fileno (pdf->pdf), ftell (pdf->pdf)) == -1) {
        r = E(IO_ERROR);
    }
#else
    if (ftruncate (fileno (pdf->pdf), ftell (pdf->pdf)) == -1) {
        r = E(IO_ERROR);
    }
#endif

    /* If previous session, update its parent pointer with new anchor */

    if (pdf->anchorpp) {
        fseek (pdf->pdf, pdf->anchorpp, SEEK_SET);
        fprintf (pdf->pdf, "%010u", aobj);
    }

    if (ferror (pdf->pdf)) {
        r = E(IO_ERROR);
    }

    if (checkpoint) {
        return r;
    }

    if (fclose (pdf->pdf) == EOF) {
        if (r == PDF_OK) {
            r = errno;
        }
    }
    pdf_free (pdf);
    return r;
}

/* Free all dynamic memory associated with a context
 */

static void pdf_free (PDF *pdf) {
    if (pdf->lines) {
        unsigned int l;
        for (l = 0; l < pdf->nlines; l++) {
            free (pdf->lines[l]);
        }
        free (pdf->lines);
    }
    free (pdf->linelen);
    free (pdf->linesize);
    free (pdf->font);
    free (pdf->nfont);
    free (pdf->nbold);
    free (pdf->title);
    free (pdf->formfile);
    free (pdf->formbuf);
    free (pdf->trail);
    free (pdf->xref);
    free (pdf->pagebuf);
    free (pdf->lzwbuf);

    pdf->key[0] = '\0';

    free (pdf);
    return;
}

/* Allocate a new object number
 * Adds current file position to slot in xref.
 */

static unsigned int addobj (PDF *pdf) {
    t_fpos *xt;

    if (pdf->obj + 1 > pdf->xsize) {
        xt = (t_fpos *) realloc (pdf->xref, (pdf->obj +1 + 100) * sizeof (t_fpos));
        if (!xt) {
            ABORT (errno);
        }
        pdf->xref = xt;
        pdf->xsize = pdf->obj + 1 + 100;
    }
    pdf->xref[pdf->obj++] = ftell (pdf->pdf);

    return pdf->obj;
}

/* Extract a reference to an object from a buffer
 * Errors free the buffer and ABORT.
 */

static unsigned int getref (PDF *pdf, char *buf, const char *name) {
    unsigned long n;
    char *p, *q;

    if (!(p = strstr (buf, name))) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    p += strlen (name);
    n = strtoul (p, &q, 10);
    if (!n || !q || q == buf || strncmp (q, " 0 R", 4)) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    if (n > (unsigned long) pdf->obj) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    return (unsigned int)n;
}

/* Extract an integer value for a name from a buffer
 * Errors free the buffer and ABORT.
 */

static unsigned int getint (PDF *pdf, char *buf, const char *name, const char **end) {
    unsigned long n;
    char *p, *q;

    if (!(p = strstr (buf, name))) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    p += strlen (name);
    n = strtoul (p, &q, 10);
    if ( !q || q == buf || (*q != '\n' && *q != ' ' && *q != ']')) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    if (end) {
        *end = q;
    }
    return (unsigned int)n;
}

/* Find a string value for a name in a buffer
 * Errors free the buffer and ABORT.
 * Return value is a string that must be free'd
 */

static char *getstr (PDF *pdf, char *buf, const char *name) {
    char *p, *q, *r;
    int n;

    if (!(p = strstr (buf, name))) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    p += strlen (name);
    while (*p && *p == ' ') {
        p++;
    }
    q = p + 1;
    n = 1;
    if (*p != '(') {
        free (buf);
        ABORT ( E(NO_APPEND));
    }
    while (*q && n) {
        if (*q == '\\')
            q++;
        else {
            if (*q == '(')
                n++;
            else
                if (*q == ')')
                    n--;
        }
        q++;
    }
    if (n) {
        free (buf);
        ABORT (E(NO_APPEND));
    }
    r = (char *) calloc (1 + q - p, 1);
    memcpy (r, p, q - p);

    return r;
}

/* Parse input string for controls.
 * Return true if any initial data was discarded
 */

static int parsestr (PDF *pdf, const char *string, size_t length, int initial) {
    int ffseen = 0;

    if (length == PDF_USE_STRLEN) {
        length = strlen (string);
    }

    /* Escape and control sequence parser
     * Not quite a full implmentation, but close enough.
     */

    SHA1Input (&ps->sha1, (uint8_t *) string, length );

    while (length) {
        short ch = 0xFF & *string++;
        length--;

#define STORE goto store
#define DISCARD continue

        /* Controls */

        /* Code extension */

        if (pdf -> escstate == ESC_ESCSEQ &&
            ch >= 0x40 && ch <= 0x5F) {
            ch += 0x40;
            pdf->escstate = ESC_IDLE;
        }

        switch (ch) {
        case 0x0A: /* LF */
            STORE;
        case 0x0D: /* CR */
            if (initial && !ffseen) {
                DISCARD;
            }
            STORE;
        case 0x0C: /* FF */
            if (initial && !ffseen++) {
                DISCARD;
            }
            STORE;
        case 0x18: /* CAN */
        case 0x1A: /* SUB */
            pdf->escstate = ESC_IDLE;
            DISCARD;
        case 0x1B: /* ESC */
            pdf->escstate = ESC_ESCSEQ;
            pdf->escin = 0;
            pdf->escpn = 0;
            DISCARD;
        case 0x9B: /* CSI */
            pdf->escstate = ESC_CSI;
            pdf->escin = 0;
            pdf->escpn = 0;
            memset (pdf->escpars, 0xFF, sizeof (pdf->escpars));
            DISCARD;
        case 0x9C: /* ST */
            pdf->escstate = ESC_IDLE;
            DISCARD;
        case 0x9d: /* OSC */
        case 0x9e: /* PM */
        case 0x9f: /* APC */
            pdf-> escstate = ESC_BADSTR;
            DISCARD;
        default:
            break;
        }
        
        /* Sequences */
#undef STORE
#define STORE break
       
        switch (pdf->escstate) {
        case ESC_IDLE: /* Discard remaining C0 and C1 */
            if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f)) {
                DISCARD;
            }
            STORE;
        case ESC_ESCSEQ:
            if (ch >= 0x20 && ch <= 0x2F) { /* Int */
                if (pdf->escin < DIM (pdf->escpars)) {
                    pdf->escpars[pdf->escin++] = ch;
                } else {
                    pdf->escstate = ESC_BADESC;
                }
                DISCARD;
            }
            if (ch >= 0x30 && ch <= 0x7E) {
                /* Do ESCSEQ */
                pdf->escstate = ESC_IDLE;
                DISCARD;
            }
            STORE;
        case ESC_CSI:
            if (ch >= 0x3C && ch <= 0x3F) {
                pdf->escprv = (char) ch;
                pdf->escstate = ESC_CSIP;
                DISCARD;
            }
            pdf->escstate = ESC_CSIP;
        case ESC_CSIP:
            if (ch >= 0x30 && ch <= 0x3F) {
                if (ch == ';') { /* 3B */
                    if (pdf->escpn +1 < DIM (pdf->escpars)) {
                        pdf->escpn++;
                    } else {
                        pdf->escstate = ESC_BADCSI;
                    }
                    DISCARD;
                }
                if (ch <= 0x39 ) {
                    if (pdf->escpars[pdf->escpn] == ESC_PDEFAULT) {
                        pdf->escpars[pdf->escpn] = ch - 0x30;
                    } else if (pdf->escpars[pdf->escpn] & ESC_POVERFLOW) {
                        pdf->escstate = ESC_BADCSI;
                    } else {
                        pdf->escpars[pdf->escpn] =
                            (pdf->escpars[pdf->escpn] * 10) + (ch - 0x30);
                    }
                    DISCARD;
                }
                pdf->escstate = ESC_BADCSI;
                DISCARD;
            }
            if (pdf->escpars[pdf->escpn] != ESC_PDEFAULT) {
                pdf->escpn++;
            }
            pdf->escstate = ESC_CSIINT;
        case ESC_CSIINT:
            if (ch >= 0x20 && ch <= 0x2F) {
                if (pdf->escin < DIM (pdf->escints)) {
                    pdf->escints[pdf->escin++] = (char)ch;
                } else {
                    pdf->escstate = ESC_BADCSI;
                }
                DISCARD;
            }
            if (ch >= 0x40 && ch <= 0x7E) {
                /* Execute CSI */
                if (ch == 'z' && !pdf->escints && !pdf->escprv) {
                    unsigned int p0 = pdf->escpars[0];

                    if (p0 == ESC_PDEFAULT) {
                        p0 = 1;
                    }
                    if (p0 == 1) {
                        p0 = 6;
                    } else if (p0 == 2) {
                        p0 = 8;
                    } else {
                        DISCARD;
                    }
                    pdf->newlpi = p0;
                    DISCARD;
                }
                DISCARD;
            }
            STORE;
        case ESC_BADCSI:
            if (ch >= 0x40 && ch <= 0x7E) {
                pdf->escstate = ESC_IDLE;
                DISCARD;
            }
            DISCARD;
        case ESC_BADESC:
            if (ch >= 0x30 && ch <= 0x7E) {
                pdf->escstate = ESC_IDLE;
                DISCARD;
            }
            DISCARD;
        case ESC_BADSTR:
            DISCARD;
        default:
            STORE;
        }

        /* Ordinary character, more or less */
#undef STORE
#undef DISCARD

     store:
        initial = 0;
        wrstw (pdf, PARSEBUF, &ch, 1);
    }
    return ffseen;
}

/* Formatted output to an expandable buffer stream.
 * Enables compression.
 */

static void wrstmf (PDF *pdf, char **buf, size_t *len, size_t *used, const char *fmt, ...) {
    va_list ap;
    char tbuf [64];
    char fbuf [16];
    char *p = tbuf;
    double d;
    unsigned int i, c;

    va_start(ap, fmt);
    while ((c = *fmt++)) {
        if (p > tbuf + sizeof (tbuf) -2 || c == '%') {
            if (p != tbuf) {
                wrstm (pdf, buf, len, used, tbuf, p - tbuf);
                p = tbuf;
            }
        }

        if (c == '%') {
            char *f = fbuf;
            *f++ = '%';
            while ((c = *fmt++)) {
                *f++ = c;
                if ( isdigit (c) || c == '.')
                    continue;
                break;
            }
            *f++ = '\0';
            switch (c) {
            case '%':
                break;
            case 'c':
                c = va_arg (ap, int);
                break;
            case 'u':
                i = va_arg (ap, unsigned int);
                p += sprintf (tbuf, fbuf, i);
                continue;
            case'f':
                d = va_arg (ap, double);
                p += sprintf (tbuf, fbuf, d);
                continue;
            case 's':
                wrstm (pdf, buf, len, used, va_arg (ap, char *), PDF_USE_STRLEN);
                continue;
            default:
                ABORT (E(BUGCHECK));
            }
        }
        *p++ = c;
    }
    va_end(ap);
    if (p != tbuf) {
         wrstm (pdf, buf, len, used, tbuf, p - tbuf);
   }

    return;
}

/* Write a string to an expandable buffer.
 */

static void wrstm (PDF *pdf, char **buf, size_t *bufsize, size_t *used, char *string, size_t length) {
    if (length == PDF_USE_STRLEN) {
        length = strlen (string);
    }

    if (*used + length +1 > *bufsize) {
        char *p = (char *) realloc (*buf, *used + length + 1 + 256);
        if (!p) {
            free (*buf);
            ABORT (errno);
        }
        *buf = p;
        *bufsize = *used + length +1 + 256;
    }
    memcpy (*buf + *used, string, length);
    *used += length;

    return;
}

/* Write a wide string to an expandable buffer.
 */

static void wrstw (PDF *pdf, short **buf, size_t *bufsize, size_t *used, short *string, size_t length) {
    if (*used + length +1 > *bufsize) {
        short *p = (short *) realloc (*buf, (*used + length + 1 + 256) * sizeof (short));
        if (!p) {
            free (*buf);
            ABORT (errno);
        }
        *buf = p;
        *bufsize = *used + length +1 + 256;
    }
    memcpy (*buf + *used, string, length *2);
    *used += length;

    return;
}

/* Read an object into memory
 * If buf is NULL, one is allocated; if too small, resized.
 * len is updated with new size;
 * returns file position of first byte of object.
 * data excludes obj and endobj lines.
 */

static t_fpos readobj (PDF *pdf, unsigned int obj, char **buf, size_t *len) {
    unsigned int o;
    char lbuf[512];
    char *p = lbuf;
    size_t used;
    t_fpos pos;
    size_t lines = 0;

    if (!*buf) {
        *len = 0;
    }
    if (obj > pdf->obj) {
        free (*buf);
        ABORT (E(NO_APPEND));
    }
    fseek (pdf->pdf, (pos = pdf->xref[obj-1]), SEEK_SET);
    if (!fgets (lbuf, sizeof (lbuf), pdf->pdf)) {
        free (*buf);
        ABORT (E(NO_APPEND));
    }

    o = 0;
    while (*p && isdigit (*p)) {
        o = (o * 10) + *p++ - '0';
    }
    if (o != obj || *p++ != ' ') {
        free (*buf);
        ABORT (E(NO_APPEND));
    }
    o = 0;
    while (*p && isdigit (*p)) {
        o = (o * 10) + *p++ - '0';
    }
    if (o != 0 || *p++ != ' ' || strcmp (p, "obj\n")) {
        free (*buf);
        ABORT (E(NO_APPEND));
    }
    used = 0;
    do {
        size_t ll;

        if (!fgets (lbuf, sizeof (lbuf), pdf->pdf)) {
            free (*buf);
            ABORT (E(NO_APPEND));
        }
        if (!strcmp (lbuf, "endobj\n")) {
            break;
        }
        ll = strlen (lbuf);
        if (!*buf || *len < used + ll +1) {
            p = (char *) realloc (*buf, used + ll +1);
            if (!p) {
                free (*buf);
                ABORT (errno);
            }
            *buf = p;
            *len = used + ll +1;
        }
        strcpy (*buf + used, lbuf);
        used += ll;
        lines++;
    } while (1);

    return pos;
}

/* Add text to a line
 * Allocates/resizes line buffer.
 * Note that lines > lpp are legal (caused by TOF offset)
 */

static void add2line (PDF *pdf, const short *text, size_t textlen) {
    unsigned int line = pdf->line;
    size_t linelen;

    if (line <= 0) {
        ABORT (E(BUGCHECK));
    }

    if (line > pdf->nlines) {
        short **p = (short **) realloc (pdf->lines, (line +1) * sizeof (short *));
        unsigned int *s;
        unsigned int i;

        if (!p) {
            ABORT( errno );
        }
        pdf->lines = p;

        s = (unsigned int *) realloc (pdf->linesize, (line +1) * sizeof (unsigned int *));
        if (!s) {
            ABORT( errno );
        }
        pdf->linesize = s;

        s = (unsigned int *) realloc (pdf->linelen, (line +1) * sizeof (unsigned int *));
        if (!s) {
            ABORT( errno );
        }
        pdf->linelen = s;

        for (i = pdf->nlines; i < line; i++) {
            pdf->lines[i] = NULL;
            pdf->linelen[i] = 0;
            pdf->linesize[i] = 0;
        }
        pdf->nlines = line;
    }

    linelen = pdf->linelen[line-1] + textlen;
    if (linelen +1 > pdf->linesize[line-1]) {
        short *p;
        p = (short *) realloc (pdf->lines[line-1], (linelen +1) * sizeof (short));
        if (!p) {
            ABORT( errno );
        }
        pdf->lines[line-1] = p;
        pdf->linesize[line-1] = linelen;
    }
    memcpy (pdf->lines[line-1] + pdf->linelen[line-1], text, textlen * sizeof (short));

    pdf->linelen[line-1] = linelen;
    pdf->lines[line-1][linelen] = '\0';

    return;
}

/* Draw a circle of radius r
 */

static void circle (PDF *pdf, double x, double y, double r) {
    double k = CircleK * r;
    char buf[1024];
    size_t len;

    len = sprintf (buf, 
        " %f %f m %f %f %f %f %f %f c %f %f %f %f %f %f c"
        " %f %f %f %f %f %f c %f %f %f %f %f %f c", 
        x-r, y,
        x-r, y+k, x-k, y+r, x, y+r,  /* TL quadrant */
        x+k, y+r, x+r, y+k, x+r, y,  /* TR */
        x+r, y-k, x+k, y-r, x, y-r,  /* BR */
        x-k, y-r, x-r, y-k, x-r, y); /* BL */

    wrstm (pdf, FORMBUF, buf, len);
    return;

}

/* strcasecmp
 *
 * Not available everywhere, easier to replace...
 */

static int xstrcasecmp (const char *s1, const char *s2) {

    while (*s1 && *s2) {
        if (*s1 != *s2 && tolower (*s1) != tolower (*s2)) {
            break;
        }
        s1++;
        s2++;
    }
    if (!*s1 && !*s2) {
        return 0;
    }
    return tolower (*s1) - tolower (*s2);
}

/* Encode a stream, currently LZW.
 * A reasonable trade-off - deflate is better compression, but much more
 * expensive to compute.  Tests show LZW compressing LPT output 5:1.
 *
 * If the compression fails (unlikely, but possible, with worst-case
 * expansion of 1.125 - 1.5), returns non-zero to cause the text to be
 * written instead.
 */

static int encstm (PDF *pdf, char *stream, size_t len) {
    t_lzw lzw;

    pdf->lzwused = 0;
    lzw_init(&lzw, LZW_BUFFER, LZWBUF);
    lzw_encode (&lzw, stream, len);

#ifdef ERRDEBUG
    return 1;
#endif
    return pdf->lzwused >= len;
}

/* *********************** LZW *********************** */

/* Initialze LZW encoding context
 *   fh == NULL when directory overflows.
 */

static void lzw_init(t_lzw *lzw, int mode, ...) {
    va_list ap;
    unsigned int i;

    if (mode != LZW_REINIT) {
        memset (lzw, 0, sizeof(*lzw));
        va_start (ap, mode);
        if (mode == LZW_FILE) {
            lzw->fh = va_arg (ap, FILE *);
        } else {
            lzw->outbuf = va_arg (ap, uint8_t **);
            lzw->outsize = va_arg (ap, size_t *);
            lzw->outused = va_arg (ap, size_t *);
        }
        va_end (ap);
    }

    for (i = 0; i < LZW_IDCODES; i++) {
        lzw->dict[i].prev =  TREE_NULL;
        lzw->dict[i].first = TREE_NULL;
        lzw->dict[i].next =  TREE_NULL;
        lzw->dict[i].ch = i;
    }

    lzw->assigned = LZW_IDCODES-1;
    lzw->codesize = LZW_MINBITS;

    return;
}

/* Encode a buffer */

static void lzw_encode (t_lzw *lzw, char *stream, size_t len) {
    t_lzwCode code;
    int c;

    lzw_writebits (lzw, LZW_CLRCODE, lzw->codesize);

    if (len == 0) {
        lzw_writebits (lzw, LZW_EODCODE, lzw->codesize);
        lzw_flushbits (lzw);
        return;
    }

    code = 0xff & *stream++;
    len--;
    while (len--) {
        t_lzwCode nc;

        c = 0xff & *stream++;

        nc = lzw_lookup_str (lzw, code, c);

        if (nc == TREE_NULL) {
            t_lzwCode tmp;

            lzw_writebits (lzw, code, lzw->codesize);

            tmp = lzw_add_str (lzw, code, c);
            if (tmp == TREE_NULL) {
                lzw_writebits (lzw, LZW_CLRCODE, lzw->codesize);
                lzw_init (lzw, LZW_REINIT);
            }
            code = c;
        } else {
            code = nc;
        }
    }

    lzw_writebits (lzw, code, lzw->codesize);
    lzw_writebits (lzw, LZW_EODCODE, lzw->codesize);
    lzw_flushbits(lzw);

    return;
}

/* Add a prefix string to the directory.
 * Returns TREE_NULL if full, new code otherwise.
 */
static t_lzwCode lzw_add_str (t_lzw *lzw, t_lzwCode code, char c) {
    t_lzwCode nc = ++lzw->assigned;

    if (nc >= LZW_DSIZE) {
        return TREE_NULL;
    }
    /* Codesize changes when last code allocated, except if
     * it's the last code in MAXBITS.
     */
    if (nc == (1 << lzw->codesize)-1 && lzw->codesize != LZW_MAXBITS) {
        lzw->codesize++;
    }
    lzw->dict[nc].ch = c;
    lzw->dict[nc].prev = code;
    lzw->dict[nc].first = TREE_NULL;
    lzw->dict[nc].next = lzw->dict[code].first;
    lzw->dict[code].first = nc;

    return nc;
}

/* Lookup a (prefix code, char) string in the directory.
 * Return its code, or TREE_NULL.
 */
static t_lzwCode lzw_lookup_str (t_lzw *lzw, t_lzwCode code, char c) {
    t_lzwCode nc;

    for (nc = lzw->dict[code].first;
        nc != TREE_NULL; nc = lzw->dict[nc].next) {
        if (code == lzw->dict[nc].prev && c == lzw->dict[nc].ch)
            return nc;
    }

    return TREE_NULL;
}

/* Pack and write a variable number of bits to the output file or buffer.
 * Packing is big-endian.
 * Bits that don't fill a byte are buffered.
 */
static void lzw_writebits (t_lzw *lzw, unsigned int bits,
                            unsigned int nbits) {
    lzw->bitbuf = (lzw->bitbuf << nbits) | (bits & ((1 << nbits)-1));

    nbits += lzw->nbits;
    while (nbits >= 8) {
        nbits -= 8;

        if (lzw->fh) {
            fputc ((lzw->bitbuf >> nbits) & 0xFF, lzw->fh);
        } else {
            if (*lzw->outused >= *lzw->outsize) {
                uint8_t *p;

                p = (uint8_t *) realloc (*lzw->outbuf, *lzw->outsize + LZW_BUFALCQ);
                if (!p) {
                    exit (errno);
                }
                *lzw->outbuf = p;
                *lzw->outsize += LZW_BUFALCQ;
            }
            (*lzw->outbuf)[*lzw->outused] = (lzw->bitbuf >> nbits) & 0xFF;
            ++*lzw->outused;
        }
    }

    lzw->nbits = nbits;
}

/* Flush any buffered bits, padding unused bits with 0.
 * There can be at most 7, since more would have been
 * written by lzw_writebits when they were added.
 */
static void lzw_flushbits (t_lzw *lzw) {
    if (lzw->nbits) {
        lzw_writebits(lzw, 0, 8-lzw->nbits);
    }
}

/* *********************** SHA1 *********************** */
/* SHA1 computation
 * Used to generate ID.
 * Source did not have an author or copyright notice.
 */

/*
 *  sha1.c
 *
 *  Description:
 *      This file implements the Secure Hashing Algorithm 1 as
 *      defined in FIPS PUB 180-1 published April 17, 1995.
 *
 *      The SHA-1, produces a 160-bit message digest for a given
 *      data stream.  It should take about 2**n steps to find a
 *      message with the same digest as a given message and
 *      2**(n/2) to find any two messages with the same digest,
 *      when n is the digest size in bits.  Therefore, this
 *      algorithm can serve as a means of providing a
 *      "fingerprint" for a message.
 *
 *  Portability Issues:
 *      SHA-1 is defined in terms of 32-bit "words".  This code
 *      uses <stdint.h> (included via "sha1.h" to define 32 and 8
 *      bit unsigned integer types.  If your C compiler does not
 *      support 32 bit unsigned integers, this code is not
 *      appropriate.
 *
 *  Caveats:
 *      SHA-1 is designed to work with messages less than 2^64 bits
 *      long.  Although SHA-1 allows a message digest to be generated
 *      for messages of any number of bits less than 2^64, this
 *      implementation only works with messages with a length that is
 *      a multiple of the size of an 8-bit character.
 *
 */

#define _SHA_enum_
enum {
    shaSuccess = 0,
    shaNull,            /* Null pointer parameter */
    shaInputTooLong,    /* input data too long */
    shaStateError       /* called Input after Result */
};


/*
 *  Define the SHA1 circular left shift macro
 */
#define SHA1CircularShift(bits,word) \
                (((word) << (bits)) | ((word) >> (32-(bits))))

/* Local Function Prototyptes */
void SHA1PadMessage(SHA1Context *);
void SHA1ProcessMessageBlock(SHA1Context *);

/*
 *  SHA1Reset
 *
 *  Description:
 *      This function will initialize the SHA1Context in preparation
 *      for computing a new SHA1 message digest.
 *
 *  Parameters:
 *      context: [in/out]
 *          The context to reset.
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1Reset(SHA1Context *context) {
    if (!context) {
        return shaNull;
    }

    context->Length_Low             = 0;
    context->Length_High            = 0;
    context->Message_Block_Index    = 0;

    context->Intermediate_Hash[0]   = 0x67452301;
    context->Intermediate_Hash[1]   = 0xEFCDAB89;
    context->Intermediate_Hash[2]   = 0x98BADCFE;
    context->Intermediate_Hash[3]   = 0x10325476;
    context->Intermediate_Hash[4]   = 0xC3D2E1F0;

    context->Computed   = 0;
    context->Corrupted  = 0;

    return shaSuccess;
}

/*
 *  SHA1Result
 *
 *  Description:
 *      This function will return the 160-bit message digest into the
 *      Message_Digest array  provided by the caller.
 *      NOTE: The first octet of hash is stored in the 0th element,
 *            the last octet of hash in the 19th element.
 *
 *  Parameters:
 *      context: [in/out]
 *          The context to use to calculate the SHA-1 hash.
 *      Message_Digest: [out]
 *          Where the digest is returned.
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1Result( SHA1Context *context,
                uint8_t Message_Digest[SHA1HashSize]) {
    int i;

    if (!context || !Message_Digest) {
        return shaNull;
    }

    if (context->Corrupted) {
        return context->Corrupted;
    }

    if (!context->Computed) {
        SHA1PadMessage(context);
        for(i=0; i<64; ++i) {
            /* message may be sensitive, clear it out */
            context->Message_Block[i] = 0;
        }
        context->Length_Low = 0;    /* and clear length */
        context->Length_High = 0;
        context->Computed = 1;

    }

    for(i = 0; i < SHA1HashSize; ++i) {
        Message_Digest[i] = context->Intermediate_Hash[i>>2]
                            >> 8 * ( 3 - ( i & 0x03 ) );
    }

    return shaSuccess;
}

/*
 *  SHA1Input
 *
 *  Description:
 *      This function accepts an array of octets as the next portion
 *      of the message.
 *
 *  Parameters:
 *      context: [in/out]
 *          The SHA context to update
 *      message_array: [in]
 *          An array of characters representing the next portion of
 *          the message.
 *      length: [in]
 *          The length of the message in message_array
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1Input(    SHA1Context    *context,
                  const uint8_t  *message_array,
                  unsigned       length) {
    if (!length) {
        return shaSuccess;
    }

    if (!context || !message_array) {
        return shaNull;
    }

    if (context->Computed) {
        context->Corrupted = shaStateError;

        return shaStateError;
    }

    if (context->Corrupted) {
         return context->Corrupted;
    }
    while(length-- && !context->Corrupted) {
    context->Message_Block[context->Message_Block_Index++] =
                    (*message_array & 0xFF);

    context->Length_Low += 8;
    if (context->Length_Low == 0) {
        context->Length_High++;
        if (context->Length_High == 0) {
            /* Message is too long */
            context->Corrupted = 1;
        }
    }

    if (context->Message_Block_Index == 64) {
        SHA1ProcessMessageBlock(context);
    }

    message_array++;
    }

    return shaSuccess;
}

/*
 *  SHA1ProcessMessageBlock
 *
 *  Description:
 *      This function will process the next 512 bits of the message
 *      stored in the Message_Block array.
 *
 *  Parameters:
 *      None.
 *
 *  Returns:
 *      Nothing.
 *
 *  Comments:

 *      Many of the variable names in this code, especially the
 *      single character names, were used because those were the
 *      names used in the publication.
 *
 *
 */
void SHA1ProcessMessageBlock(SHA1Context *context) {
    const uint32_t K[] =    {       /* Constants defined in SHA-1   */
                            0x5A827999,
                            0x6ED9EBA1,
                            0x8F1BBCDC,
                            0xCA62C1D6
                            };
    int           t;                 /* Loop counter                */
    uint32_t      temp;              /* Temporary word value        */
    uint32_t      W[80];             /* Word sequence               */
    uint32_t      A, B, C, D, E;     /* Word buffers                */

    /*
     *  Initialize the first 16 words in the array W
     */
    for(t = 0; t < 16; t++) {
        W[t] = context->Message_Block[t * 4] << 24;
        W[t] |= context->Message_Block[t * 4 + 1] << 16;
        W[t] |= context->Message_Block[t * 4 + 2] << 8;
        W[t] |= context->Message_Block[t * 4 + 3];
    }

    for(t = 16; t < 80; t++) {
       W[t] = SHA1CircularShift(1,W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]);
    }

    A = context->Intermediate_Hash[0];
    B = context->Intermediate_Hash[1];
    C = context->Intermediate_Hash[2];
    D = context->Intermediate_Hash[3];
    E = context->Intermediate_Hash[4];

    for(t = 0; t < 20; t++) {
        temp =  SHA1CircularShift(5,A) +
                ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);

        B = A;
        A = temp;
    }

    for(t = 20; t < 40; t++) {
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[1];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    for(t = 40; t < 60; t++) {
        temp = SHA1CircularShift(5,A) +
               ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    for(t = 60; t < 80; t++) {
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[3];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    context->Intermediate_Hash[0] += A;
    context->Intermediate_Hash[1] += B;
    context->Intermediate_Hash[2] += C;
    context->Intermediate_Hash[3] += D;
    context->Intermediate_Hash[4] += E;

    context->Message_Block_Index = 0;
}

/*
 *  SHA1PadMessage
 *

 *  Description:
 *      According to the standard, the message must be padded to an even
 *      512 bits.  The first padding bit must be a '1'.  The last 64
 *      bits represent the length of the original message.  All bits in
 *      between should be 0.  This function will pad the message
 *      according to those rules by filling the Message_Block array
 *      accordingly.  It will also call the ProcessMessageBlock function
 *      provided appropriately.  When it returns, it can be assumed that
 *      the message digest has been computed.
 *
 *  Parameters:
 *      context: [in/out]
 *          The context to pad
 *      ProcessMessageBlock: [in]
 *          The appropriate SHA*ProcessMessageBlock function
 *  Returns:
 *      Nothing.
 *
 */

void SHA1PadMessage(SHA1Context *context) {
    /*
     *  Check to see if the current message block is too small to hold
     *  the initial padding bits and length.  If so, we will pad the
     *  block, process it, and then continue padding into a second
     *  block.
     */
    if (context->Message_Block_Index > 55) {
        context->Message_Block[context->Message_Block_Index++] = 0x80;
        while(context->Message_Block_Index < 64)
        {
            context->Message_Block[context->Message_Block_Index++] = 0;
        }

        SHA1ProcessMessageBlock(context);

        while(context->Message_Block_Index < 56) {
            context->Message_Block[context->Message_Block_Index++] = 0;
        }
    } else {
        context->Message_Block[context->Message_Block_Index++] = 0x80;
        while(context->Message_Block_Index < 56) {

            context->Message_Block[context->Message_Block_Index++] = 0;
        }
    }

    /*
     *  Store the message length as the last 8 octets
     */
    context->Message_Block[56] = context->Length_High >> 24;
    context->Message_Block[57] = context->Length_High >> 16;
    context->Message_Block[58] = context->Length_High >> 8;
    context->Message_Block[59] = context->Length_High;
    context->Message_Block[60] = context->Length_Low >> 24;
    context->Message_Block[61] = context->Length_Low >> 16;
    context->Message_Block[62] = context->Length_Low >> 8;
    context->Message_Block[63] = context->Length_Low;

    SHA1ProcessMessageBlock(context);
}