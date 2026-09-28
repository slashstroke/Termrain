/*
 * matrix.c - Matrix-style "digital rain" for the terminal.
 *
 * Build:  cc -O2 -Wall -Wextra -o matrix matrix.c
 * Run:    ./matrix [-f fps] [-s speed] [-c color]
 * Quit:   q or Ctrl+C
 *
 * No dependencies beyond a POSIX system and a terminal with ANSI support.
 *
 * License: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION    "1.0.0"
#define MIN_LEN    5    /* shortest tail, in rows                       */
#define MUTATE_DIV 20   /* ~1/20 of all glyphs change every frame       */

static const char GLYPHS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}<>?/\\|";
#define NGLYPHS (sizeof GLYPHS - 1)

typedef struct {
    float y;      /* head position in rows (fractional)  */
    float speed;  /* rows per second                     */
    int   head;   /* integer head row, -1 while offscreen */
    int   len;    /* tail length in rows                 */
} Column;

enum { S_HEAD, S_BRIGHT, S_NORMAL, S_DIM, NSTYLES };

/* ---- configuration (set from the command line) ------------------------- */
static int   g_fps   = 20;
static float g_speed = 1.0f;
static int   g_color = 2;              /* ANSI color index: 2 = green */

/* ---- runtime state ------------------------------------------------------ */
static int    g_rows, g_cols;
static Column *g_col;
static char   *g_glyph;                /* rows * cols persistent glyph grid */
static char   *g_out;                  /* one-shot frame buffer             */
static char    g_style[NSTYLES][16];
static size_t  g_style_len[NSTYLES];
static uint32_t g_rng = 2463534242u;

static volatile sig_atomic_t g_quit, g_resize;
static struct termios g_orig_term;
static int g_term_saved, g_term_active;

/* ---- random numbers (xorshift32: fast, good enough for visuals) --------- */
static inline uint32_t rng(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}
static inline uint32_t rnd(uint32_t n)  { return rng() % n; }
static inline char rand_glyph(void)     { return GLYPHS[rnd(NGLYPHS)]; }

static void seed_rng(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    g_rng = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 16)
          ^ ((uint32_t)getpid() * 2654435761u);
    if (g_rng == 0) g_rng = 2463534242u;
}

/* ---- output -------------------------------------------------------------- */
static void write_all(const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return;   /* drop this frame */
            g_quit = 1;                    /* terminal is gone */
            return;
        }
        buf += w;
        n   -= (size_t)w;
    }
}

static inline char *put(char *p, const char *s, size_t n) {
    memcpy(p, s, n);
    return p + n;
}

/* ---- terminal setup / teardown ------------------------------------------ */
static void term_leave(void) {
    if (!g_term_active) return;
    g_term_active = 0;
    /* reset colors, re-enable autowrap, show cursor, leave alt screen */
    const char seq[] = "\033[0m\033[?7h\033[?25h\033[?1049l";
    (void)!write(STDOUT_FILENO, seq, sizeof seq - 1);
    if (g_term_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

static void term_enter(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_term) == 0) {
        struct termios raw = g_orig_term;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_term_saved = 1;
    }
    g_term_active = 1;
    /* alt screen, hide cursor, disable autowrap, clear */
    const char seq[] = "\033[?1049h\033[?25l\033[?7l\033[2J";
    write_all(seq, sizeof seq - 1);
}

static void get_size(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* ---- simulation ---------------------------------------------------------- */
static void column_reset(Column *c, int initial) {
    c->len   = MIN_LEN + (int)rnd((uint32_t)g_rows / 2 + 1);
    c->speed = (12.0f + 25.0f * (float)(rng() >> 8) / 16777216.0f) * g_speed;
    c->y     = initial ? (float)rnd((uint32_t)g_rows * 2) - (float)g_rows
                       : -(float)rnd((uint32_t)g_rows);
    c->head  = c->y < 0.0f ? -1 : (int)c->y;
}

static void layout_free(void) {
    free(g_col);   g_col   = NULL;
    free(g_glyph); g_glyph = NULL;
    free(g_out);   g_out   = NULL;
}

static int layout_init(void) {
    layout_free();
    get_size(&g_rows, &g_cols);

    size_t cells = (size_t)g_rows * (size_t)g_cols;
    /* worst case per cell: 9-byte style sequence + 1 glyph */
    size_t cap = cells * 10 + (size_t)g_rows * 2 + 16;

    g_col   = calloc((size_t)g_cols, sizeof *g_col);
    g_glyph = malloc(cells);
    g_out   = malloc(cap);
    if (!g_col || !g_glyph || !g_out) {
        layout_free();
        return -1;
    }
    for (size_t i = 0; i < cells; i++) g_glyph[i] = rand_glyph();
    for (int c = 0; c < g_cols; c++)   column_reset(&g_col[c], 1);
    return 0;
}

static void update(void) {
    const float dt = 1.0f / (float)g_fps;

    for (int c = 0; c < g_cols; c++) {
        Column *col = &g_col[c];
        col->y   += col->speed * dt;
        col->head = col->y < 0.0f ? -1 : (int)col->y;

        if (col->head - col->len >= g_rows) {   /* tail fully offscreen */
            column_reset(col, 0);
            continue;
        }
        if (col->head >= 0 && col->head < g_rows)
            g_glyph[(size_t)col->head * (size_t)g_cols + (size_t)c] = rand_glyph();
    }

    /* occasional random glyph flicker, like the real thing */
    size_t total = (size_t)g_rows * (size_t)g_cols;
    for (size_t i = 0, n = total / MUTATE_DIV + 1; i < n; i++)
        g_glyph[rnd((uint32_t)total)] = rand_glyph();
}

static void render(void) {
    char *p = g_out;
    int last = -1;                       /* last style emitted */

    p = put(p, "\033[H", 3);
    for (int r = 0; r < g_rows; r++) {
        const char *row = g_glyph + (size_t)r * (size_t)g_cols;
        for (int c = 0; c < g_cols; c++) {
            const Column *col = &g_col[c];
            int diff = col->head - r;
            if (diff < 0 || diff >= col->len) {
                *p++ = ' ';
                continue;
            }
            int style = diff == 0                  ? S_HEAD
                      : diff * 3 < col->len        ? S_BRIGHT
                      : diff * 3 < col->len * 2    ? S_NORMAL
                                                   : S_DIM;
            if (style != last) {                 /* only emit on change */
                p = put(p, g_style[style], g_style_len[style]);
                last = style;
            }
            *p++ = row[c];
        }
        if (r < g_rows - 1) p = put(p, "\r\n", 2);
    }
    p = put(p, "\033[0m", 4);
    write_all(g_out, (size_t)(p - g_out));
}

static void build_styles(void) {
    int n = g_color;
    snprintf(g_style[S_HEAD],   sizeof g_style[0], "\033[0;1;37m");
    snprintf(g_style[S_BRIGHT], sizeof g_style[0], "\033[0;1;3%dm", n);
    snprintf(g_style[S_NORMAL], sizeof g_style[0], "\033[0;3%dm",   n);
    snprintf(g_style[S_DIM],    sizeof g_style[0], "\033[0;2;3%dm", n);
    for (int i = 0; i < NSTYLES; i++) g_style_len[i] = strlen(g_style[i]);
}

/* ---- signals & input ----------------------------------------------------- */
static void on_signal(int sig) {
    if (sig == SIGWINCH) g_resize = 1;
    else                 g_quit   = 1;
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,   &sa, NULL);
    sigaction(SIGTERM,  &sa, NULL);
    sigaction(SIGHUP,   &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void poll_input(void) {
    if (!g_term_saved) return;           /* stdin is not a tty */
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == 'q' || buf[i] == 'Q') g_quit = 1;
}

/* ---- timing -------------------------------------------------------------- */
static long elapsed_ns(const struct timespec *a, const struct timespec *b) {
    return (long)(b->tv_sec - a->tv_sec) * 1000000000L + (b->tv_nsec - a->tv_nsec);
}

static void sleep_ns(long ns) {
    if (ns <= 0) return;
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    nanosleep(&ts, NULL);                /* may be cut short by a signal: fine */
}

/* ---- command line -------------------------------------------------------- */
static void usage(FILE *f) {
    fprintf(f,
        "matrix %s - digital rain for your terminal\n\n"
        "Usage: matrix [options]\n"
        "  -f FPS     frames per second, 1-120   (default 20)\n"
        "  -s SPEED   speed multiplier, 0.1-10   (default 1.0)\n"
        "  -c COLOR   green red blue yellow cyan magenta white (default green)\n"
        "  -h         show this help\n"
        "  -v         show version\n\n"
        "Quit with q or Ctrl+C.\n", VERSION);
}

static int parse_color(const char *s) {
    static const struct { const char *name; int idx; } t[] = {
        {"red",1}, {"green",2}, {"yellow",3}, {"blue",4},
        {"magenta",5}, {"cyan",6}, {"white",7},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcmp(s, t[i].name) == 0) return t[i].idx;
    return -1;
}

static void parse_args(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "f:s:c:hv")) != -1) {
        char *end;
        switch (opt) {
        case 'f': {
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || v < 1 || v > 120) {
                fprintf(stderr, "matrix: invalid FPS '%s'\n", optarg);
                exit(2);
            }
            g_fps = (int)v;
            break;
        }
        case 's': {
            errno = 0;
            double v = strtod(optarg, &end);
            if (errno || *end || end == optarg || v < 0.1 || v > 10.0) {
                fprintf(stderr, "matrix: invalid speed '%s'\n", optarg);
                exit(2);
            }
            g_speed = (float)v;
            break;
        }
        case 'c':
            g_color = parse_color(optarg);
            if (g_color < 0) {
                fprintf(stderr, "matrix: unknown color '%s'\n", optarg);
                exit(2);
            }
            break;
        case 'h': usage(stdout); exit(0);
        case 'v': printf("matrix %s\n", VERSION); exit(0);
        default:  usage(stderr); exit(2);
        }
    }
}

/* ---- main ---------------------------------------------------------------- */
int main(int argc, char **argv) {
    parse_args(argc, argv);

    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "matrix: output is not a terminal\n");
        return 1;
    }

    seed_rng();
    build_styles();
    install_signals();
    atexit(term_leave);
    term_enter();

    if (layout_init() < 0) {
        term_leave();
        fprintf(stderr, "matrix: out of memory\n");
        return 1;
    }

    const long frame_ns = 1000000000L / g_fps;

    while (!g_quit) {
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (g_resize) {
            g_resize = 0;
            if (layout_init() < 0) {
                term_leave();
                fprintf(stderr, "matrix: out of memory\n");
                return 1;
            }
            write_all("\033[2J", 4);
        }

        poll_input();
        update();
        render();

        clock_gettime(CLOCK_MONOTONIC, &now);
        sleep_ns(frame_ns - elapsed_ns(&start, &now));
    }

    layout_free();
    term_leave();
    return 0;
}
