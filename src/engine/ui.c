#include "ui.h"
#include "cr2.h"
#include "se_libc.h"

/* ====================================================== primitives ==== */

void se_ui_px(se_ui_fb *fb, int x, int y, uint8_t c)
{
    uint8_t *p;
    if (x < 0 || y < 0 || x >= SE_UI_W || y >= SE_UI_H) return;
    p = &fb->px[(x >> 1) + y * SE_UI_PITCH];
    /* odd x in the high nibble, matching Magic Lantern's SET_4BIT_PIXEL */
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
}

void se_ui_clear(se_ui_fb *fb, uint8_t c)
{
    uint8_t b = (uint8_t)((c & 0x0F) | (c << 4));
    memset(fb->px, b, SE_UI_FB_SIZE);
}

void se_ui_fill(se_ui_fb *fb, int x, int y, int w, int h, uint8_t c)
{
    int left = SE_MAX(x, 0), right = SE_MIN(x + w, SE_UI_W);
    int top = SE_MAX(y, 0), bottom = SE_MIN(y + h, SE_UI_H);
    int first_byte, whole_bytes, row;
    uint8_t pair = (c & 15) * 17;

    if (left >= right || top >= bottom) return;
    first_byte = (left + 1) / 2;
    whole_bytes = right / 2 - first_byte;

    for (row = top; row < bottom; row++) {
        uint8_t *dst = fb->px + row * SE_UI_PITCH;
        /* A partial byte shares its other pixel with the background. */
        if (left & 1) dst[left / 2] = (dst[left / 2] & 15) | (pair & 240);
        if (whole_bytes == 1) dst[first_byte] = pair;
        else if (whole_bytes > 1) memset(dst + first_byte, pair, whole_bytes);
        if (right & 1) dst[right / 2] = (dst[right / 2] & 240) | (pair & 15);
    }
}

void se_ui_frame(se_ui_fb *fb, int x, int y, int w, int h, uint8_t c)
{
    se_ui_fill(fb, x,         y,         w, 1, c);
    se_ui_fill(fb, x,         y + h - 1, w, 1, c);
    se_ui_fill(fb, x,         y,         1, h, c);
    se_ui_fill(fb, x + w - 1, y,         1, h, c);
}

static int glyph_index(unsigned char ch)
{
    if (ch >= 0x01 && ch <= 0x04) return (0x5F - 0x20 + 1) + (ch - 1);
    if (ch >= 'a' && ch <= 'z')   ch = (unsigned char)(ch - 'a' + 'A');
    if (ch < 0x20 || ch > 0x5F)   return 0;   /* space */
    return ch - 0x20;
}

int se_ui_text_w(const char *s, int sx)
{
    int n = 0;
    for (; *s; s++) if (*s != '\x05') n++;
    if (n == 0) return 0;
    return n * (SE_FONT_W + 1) * sx - sx;   /* no trailing gap */
}

int se_ui_text(se_ui_fb *fb, int x, int y, const char *s,
               uint8_t c, int sx, int sy)
{
    int x0 = x, it = 0;
    for (; *s; s++) {
        const uint8_t *g;
        int col, row;
        if (*s == '\x05') { it = !it; continue; }   /* SE_ITALIC */
        g = se_ui_font[glyph_index((unsigned char)*s)];
        for (col = 0; col < SE_FONT_W; col++) {
            uint8_t bits = g[col];
            for (row = 0; row < SE_FONT_H; row++) {
                /* italic: slant the top rows right */
                int lean = it ? (SE_FONT_H - 1 - row) * sx / 3 : 0;
                if (bits & (1u << row))
                    se_ui_fill(fb, x + col * sx + lean, y + row * sy, sx, sy, c);
            }
        }
        x += (SE_FONT_W + 1) * sx;
    }
    return x - x0;
}

int se_ui_text_c(se_ui_fb *fb, int cx, int y, const char *s,
                 uint8_t c, int sx, int sy)
{
    return se_ui_text(fb, cx - se_ui_text_w(s, sx) / 2, y, s, c, sx, sy);
}

/* ------------------------------------------------- tiny string helpers */

static char *put_str(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = 0;
    return d;
}

static char *put_int(char *d, int v)
{
    char tmp[12];
    int  n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

/* ====================================================== settings ====== */

static const char *g_res_name[3]  = { "FULL", "HALF", "QUARTER" };
static const int   g_res_scale[3] = { 1, 2, 4 };
/*
 * The output width each resolution gives, which depends on the source: the
 * sensor frame is 4368 active columns, the JPEG Canon embeds is 2496. The
 * screen said 4368 either way, which was simply wrong half the time.
 */
static const int   g_res_px[2][3] = { { 2496, 1248, 624 },
                                      { 4368, 2184, 1092 } };

/*
 * Short, because this is read on a 2.5-inch panel at arm's length and the
 * word that carries the information is the only word worth having.
 * "FROM PREVIEW JPEG" made the reader work through two words of grammar
 * to get to it.
 */
static const char *g_src_name[2] = { "JPEG", "RAW" };

static const int g_set_max[SE_SET_COUNT] = { 1, 2, 1 };

/*
 * Settings are restored from a file on the card, so they are untrusted
 * input: a truncated or hand-edited config must not be able to index past
 * a name table. Every read of a setting goes through this.
 */
static int setval(const se_ui *u, int which)
{
    int v;
    if (which < 0 || which >= SE_SET_COUNT) return 0;
    v = u->value[which];
    return SE_CLAMP(v, 0, g_set_max[which]);
}

int   se_ui_scale(const se_ui *u)    { return g_res_scale[setval(u, SE_SET_RES)]; }
int   se_ui_grain_on(const se_ui *u) { return setval(u, SE_SET_GRAIN); }
int   se_ui_source(const se_ui *u)   { return setval(u, SE_SET_SOURCE); }

static void setting_label(int i, char *dst)
{
    static const char *n[SE_SET_COUNT] = { "SOURCE", "RESOLUTION", "GRAIN" };
    put_str(dst, (i >= 0 && i < SE_SET_COUNT) ? n[i] : "?");
}

static void setting_value(const se_ui *u, int i, char *dst)
{
    char *d = dst;
    int   v = setval(u, i);
    switch (i) {
    case SE_SET_SOURCE:
        put_str(d, g_src_name[v]);
        break;
    case SE_SET_RES:
        d = put_str(d, g_res_name[v]);
        d = put_str(d, "  ");
        d = put_int(d, g_res_px[se_ui_source(u)][v]);
        put_str(d, " PX");
        break;
    case SE_SET_GRAIN:
        put_str(d, v ? "AS THE STOCK SAYS" : "OFF");
        break;
    default:
        put_str(d, "?");
        break;
    }
}

/* ====================================================== state ========= */

void se_ui_init(se_ui *u)
{
    memset(u, 0, sizeof *u);
    u->screen = SE_SCREEN_STOCK;
    u->stock  = 0;
    u->stock_leader_colour = SE_C_RUST;
    u->value[SE_SET_SOURCE] = SE_SOURCE_PREVIEW;
    u->value[SE_SET_RES]    = 1;   /* HALF while we are testing */
    u->value[SE_SET_GRAIN]  = 1;
    u->stage = "";
}

void se_ui_set_stocks(se_ui *u, const se_ui_stock *list, int n)
{
    u->stocks  = list;
    u->nstocks = n < 0 ? 0 : n;
    if (u->stock >= u->nstocks) u->stock = 0;
}

void se_ui_key_press(se_ui *u, se_ui_key k)
{
    int n = u->nstocks;
    if (n < 1) n = 1;

    switch (u->screen) {
    case SE_SCREEN_STOCK:
        if      (k == SE_KEY_LEFT)  u->stock = (u->stock + n - 1) % n;
        else if (k == SE_KEY_RIGHT) u->stock = (u->stock + 1) % n;
        else if (k == SE_KEY_MENU)  u->screen = SE_SCREEN_SETTINGS;
        else if (k == SE_KEY_SET)   u->screen = SE_SCREEN_WORKING;
        break;

    case SE_SCREEN_SETTINGS:
        if      (k == SE_KEY_UP)    u->setting = (u->setting + SE_SET_COUNT - 1) % SE_SET_COUNT;
        else if (k == SE_KEY_DOWN)  u->setting = (u->setting + 1) % SE_SET_COUNT;
        else if (k == SE_KEY_LEFT)  { int *v = &u->value[u->setting];
                                      *v = (*v > 0) ? *v - 1 : g_set_max[u->setting]; }
        else if (k == SE_KEY_RIGHT) { int *v = &u->value[u->setting];
                                      *v = (*v < g_set_max[u->setting]) ? *v + 1 : 0; }
        else if (k == SE_KEY_MENU || k == SE_KEY_SET) u->screen = SE_SCREEN_STOCK;
        break;

    case SE_SCREEN_WORKING:
        if (k == SE_KEY_MENU) u->screen = SE_SCREEN_STOCK;   /* cancel */
        break;

    case SE_SCREEN_DONE:
        u->screen = SE_SCREEN_STOCK;
        break;

    default:
        break;
    }
}

/* ====================================================== drawing ======= */
/*
 * A pixel here is twice as TALL as it is wide on the real screen -- the
 * buffer is 720x240 and Magic Lantern's coordinate space for this body is
 * 720x480, halved vertically. So text takes separate scales and a readable
 * line is sx = 2 * sy, and a shape meant to read as square is twice as
 * wide as it is high.
 *
 * WHAT THIS IS FOR, because the first version forgot: you are holding a
 * camera, the screen is 2.5 inches across, and you want to know which film
 * you are about to shoot. One thing matters -- the name -- and it should
 * be readable at arm's length without looking for it.
 *
 * So the hierarchy is the design. The stock name is set enormous and
 * centred; everything else is small, grey and out of the way. There is one
 * piece of colour per screen, a single band carrying the speed, and it is
 * there to tell the stocks apart at a glance rather than to decorate.
 *
 * The version before this one had cartons with corner wedges, colour
 * chips, barcodes, sprocket rows and a dithered paper texture behind all
 * of it. Every piece of that was legible on a desktop monitor at 3x and
 * none of it was legible on the camera. Detail is not the same as craft.
 */

/* ---------------------------------------------------------- the ground */

static void paper_ground(se_ui_fb *fb)
{
    se_ui_clear(fb, SE_C_PAPER);
}

/* A filled panel with a keyline and its corners knocked off. One pixel at
 * each corner is the difference between a printed card and a filled rect,
 * and it is the only ornament left. */
static void plate(se_ui_fb *fb, int x, int y, int w, int h,
                  uint8_t fill, uint8_t edge, uint8_t ground)
{
    se_ui_fill (fb, x, y, w, h, fill);
    if (edge != fill) se_ui_frame(fb, x, y, w, h, edge);
    se_ui_fill(fb, x,         y,         2, 1, ground);
    se_ui_fill(fb, x + w - 2, y,         2, 1, ground);
    se_ui_fill(fb, x,         y + h - 1, 2, 1, ground);
    se_ui_fill(fb, x + w - 2, y + h - 1, 2, 1, ground);
}

/* Wrapper colours that need dark print in the 7D palette. */
static int is_light(uint8_t c)
{
    return c == SE_C_HILITE || c == SE_C_PAPER || c == SE_C_EDGE ||
           c == SE_C_GOLD   || c == SE_C_AMBER || c == SE_C_WHITE ||
           c == SE_C_MID    || c == SE_C_TEAL;
}
/* ------------------------------------------------------ chrome: bars */

static void header(se_ui_fb *fb, const char *title, const char *right)
{
    se_ui_text(fb, 16, 8, title, SE_C_SLATE, 2, 1);
    if (right)
        se_ui_text(fb, SE_UI_W - 16 - se_ui_text_w(right, 2), 8,
                   right, SE_C_SLATE, 2, 1);
    se_ui_fill(fb, 16, 24, SE_UI_W - 32, 2, SE_C_INK);
}

/* Footer hints: "[SET] DEVELOP" draws SET as a key, a grey box with dark
 * text. fb 0 only measures. Returns the width. */
static int hint_line(se_ui_fb *fb, int x, const char *s)
{
    int x0 = x;
    char ch[2] = { 0, 0 };
    while (*s) {
        if (*s == '[') {
            const char *e = s + 1;
            int n = 0;
            while (*e && *e != ']') { e++; n++; }
            if (fb) se_ui_fill(fb, x, 219, n * 12 + 10, 13, SE_C_EDGE);
            x += 6;
            for (s++; s < e; s++) {
                ch[0] = *s;
                if (fb) se_ui_text(fb, x, 222, ch, SE_C_INK, 2, 1);
                x += 12;
            }
            x += 4;
            if (*s) s++;
            continue;
        }
        ch[0] = *s++;
        if (fb) se_ui_text(fb, x, 222, ch, SE_C_MID, 2, 1);
        x += 12;
    }
    return x - x0;
}

static void footer(se_ui_fb *fb, const char *hints)
{
    se_ui_fill(fb, 16, 212, SE_UI_W - 32, 1, SE_C_EDGE);
    hint_line(fb, SE_UI_W / 2 - hint_line(0, 0, hints) / 2, hints);
}

/*
 * The name, set as large as it will go.
 *
 * Scale is chosen to fit rather than fixed, so CINE is not dwarfed and
 * KODACHROME is not clipped. The ladder steps down 7, 6, 5, 4 and then
 * truncates -- at scale 4 a character is 20 wide, so 32 of them fit, and
 * no stock name is anywhere near that.
 */
static void big_name(se_ui_fb *fb, int y, const char *s, uint8_t c)
{
    static const int ladder[4] = { 7, 6, 5, 4 };
    int i, sx = 4;
    for (i = 0; i < 4; i++)
        if (se_ui_text_w(s, ladder[i]) <= SE_UI_W - 120) { sx = ladder[i]; break; }
    se_ui_text_c(fb, SE_UI_W / 2, y, s, c, sx, sx > 5 ? 4 : 3);
}

/* --------------------------------------------- per-stock accent colour */
/*
 * One colour each, chosen by name so the card format does not change.
 * An unrecognised stock still gets one, keyed off its own letters, so it
 * is at least consistently itself.
 */
static int name_is(const char *name, const char *want)
{
    int i;
    for (i = 0; want[i]; i++) {
        char a = name[i], b = want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return 0;
    }
    return 1;
}

typedef struct {
    uint8_t accent;
    const char *speed;
    const char *short_name;
    const char *label;
    const char *code;           /* overrides the speed on the canister */
} se_stock_look;

/*
 * FILM SPEED is the film's rated speed -- Velvia is a 50,
 * Portra a 400 -- and nothing else. The engine never touches the camera's
 * ISO, and must not: Canon's exposure settings are properties, and writing
 * a property is forbidden here because they live in NVRAM.
 *
 * Match the complete stock name so a custom variant keeps its own name.
 */
static const struct { const char *key; se_stock_look lk; } g_looks[] = {
    { "PORTRA 160",      { SE_C_AMBER,  "160",  "P160",      "PORTRA", NULL } },
    { "PORTRA 800",      { SE_C_AMBER,  "800",  "P800",      "PORTRA", NULL } },
    { "EKTAR 100",       { SE_C_RED,    "100",  "EKT 100",   "EKTAR", NULL } },
    { "SUPERIA 400",     { SE_C_OLIVE,  "400",  "SUP 400",   "SUP", NULL } },
    { "ULTRAMAX 400",    { SE_C_GOLD,   "400",  "UMX 400",   "UMX", NULL } },
    { "PRO 400H",        { SE_C_TEAL,   "400",  "400H",      "PRO",    "400H" } },
    { "VISION3 250D",    { SE_C_TEAL,   "250",  "V3 250D",   "V3",     "250D" } },
    { "VISION3 500T",    { SE_C_PLUM,   "500",  "V3 500T",   "V3",     "500T" } },
    { "EKTACHROME E100", { SE_C_TEAL,   "100",  "E100",      "EKTCH",  "E100" } },
    { "KODACHROME 64",   { SE_C_RED,    "64",   "K64",       "KCHRM", NULL } },
    { "HP5 PLUS",        { SE_C_INK,    "400",  "HP5+",      "HP5+", NULL } },
    { "TRI-X 400",       { SE_C_INK,    "400",  "TRI-X",     "TRI-X", NULL } },
    { "T-MAX 400",       { SE_C_SHADOW, "400",  "TMY",       "TMY", NULL } },
    { "DELTA 3200",      { SE_C_INK,    "3200", "D3200",     "DELTA", NULL } },
    { "PROVIA",          { SE_C_SLATE,  "100",  "PROVIA",    "PROVIA", NULL } },
    { "VELVIA",          { SE_C_OLIVE,  "50",   "VELVIA",    "VELVIA", NULL } },
    { "ASTIA",           { SE_C_PLUM,   "100",  "ASTIA",     "ASTIA", NULL } },
    { "CLASSIC CHROME",  { SE_C_MID,    "200",  "CL CHROME", "CL CH", NULL } },
    { "CLASSIC NEG",     { SE_C_TEAL,   "400",  "CL NEG",    "CL NEG", NULL } },
    { "NOSTALGIC NEG",   { SE_C_RUST,   "400",  "NOST NEG",  "NOST", NULL } },
    { "ETERNA",          { SE_C_EDGE,   "250",  "ETERNA",    "ETERNA", NULL } },
    { "BLEACH BYPASS",   { SE_C_SHADOW, "400",  "BLEACH",    "BLEACH", NULL } },
    { "ACROS",           { SE_C_INK,    "100",  "ACROS",     "ACROS", NULL } },
    { "REALA ACE",       { SE_C_HILITE, "100",  "R ACE",     "R ACE", NULL } },
    { "SOUP 800",        { SE_C_RED,    "800",  "SOUP 800",  "SOUP", NULL } },
    { "GOLD",            { SE_C_GOLD,   "200",  "GOLD",      "GOLD", NULL } },
    { "PORTRA",          { SE_C_AMBER,  "400",  "P400",      "PORTRA", NULL } },
};
#define SE_NLOOKS ((int)(sizeof g_looks / sizeof g_looks[0]))

static se_stock_look stock_look(const char *name)
{
    static const uint8_t pick[7] = { SE_C_TEAL, SE_C_PLUM, SE_C_AMBER,
                                     SE_C_OLIVE, SE_C_RUST, SE_C_GOLD,
                                     SE_C_RED };
    se_stock_look lk;
    unsigned h = 2166136261u;
    int i;

    if (!name) name = "?";
    for (i = 0; i < SE_NLOOKS; i++)
        if (name_is(name, g_looks[i].key) && !name[strlen(g_looks[i].key)])
            return g_looks[i].lk;

    for (i = 0; name[i]; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    lk.accent = pick[h % 7u];
    lk.speed = "200";
    lk.short_name = name;
    lk.label = 0;
    lk.code = 0;
    return lk;
}


/* ------------------------------------------------------- stock icons */
/*
 * The roll itself, upright: a 35mm canister (or a round cine can for the
 * motion-picture stocks) in the stock's colours with its speed on the
 * label. The 28x32 roll sprites of the desktop app, drawn from a few
 * bytes per stock rather than stored, with its colours mapped to
 * the sixteen UI colours. Colour schemes only: no logos or trade dress.
 *
 * Each art pixel is drawn TWICE AS WIDE as it is tall, because the
 * overlay is a 720x240 buffer presented over 480 lines -- so a square in
 * memory is a rectangle on the panel, and only a 2:1 block comes out
 * square to the eye. The font scales already do this (3,2 and 7,4).
 */
#define SE_SPR_W 28
#define SE_SPR_H 32

typedef struct {
    const char *key;
    uint8_t     shape_body;   /* 0x10 = cine can, low nibble body colour */
    uint8_t     band_label;   /* band << 4 | label                       */
    char        iso[5];
} se_roll;

#define CAN(b)     (b)
#define CINE(b)    (0x10 | (b))
#define BL(b, l)   ((uint8_t)((b) << 4 | (l)))

static const se_roll g_rolls[] = {
    { "PORTRA 160",  CAN(SE_C_HILITE),  BL(SE_C_AMBER,  SE_C_INK),    "160"  },
    { "PORTRA 800",  CAN(SE_C_HILITE),  BL(SE_C_RED,    SE_C_INK),    "800"  },
    { "PORTRA",      CAN(SE_C_HILITE),  BL(SE_C_AMBER,  SE_C_INK),    "400"  },
    { "EKTAR",       CAN(SE_C_GOLD),    BL(SE_C_RED,    SE_C_INK),    "100"  },
    { "GOLD",        CAN(SE_C_GOLD),    BL(SE_C_RED,    SE_C_INK),    "200"  },
    { "ULTRAMAX",    CAN(SE_C_GOLD),    BL(SE_C_TEAL,   SE_C_INK),    "400"  },
    { "EKTACHROME",  CAN(SE_C_HILITE),  BL(SE_C_GOLD,   SE_C_RED),    "E1"   },
    { "KODACHROME",  CAN(SE_C_GOLD),    BL(SE_C_RED,    SE_C_INK),    "K64"  },
    { "TRI-X",       CAN(SE_C_GOLD),    BL(SE_C_INK,    SE_C_INK),    "400"  },
    { "T-MAX",       CAN(SE_C_HILITE),  BL(SE_C_GOLD,   SE_C_INK),    "TMY"  },
    { "VISION3 250", CINE(SE_C_MID),    BL(SE_C_GOLD,   SE_C_INK),    "250D" },
    { "VISION3 500", CINE(SE_C_MID),    BL(SE_C_TEAL,   SE_C_HILITE), "500T" },
    { "VELVIA",      CAN(SE_C_OLIVE),   BL(SE_C_RED,    SE_C_HILITE), "50"   },
    { "PROVIA",      CAN(SE_C_TEAL),    BL(SE_C_HILITE, SE_C_HILITE), "100"  },
    { "ASTIA",       CAN(SE_C_PLUM),    BL(SE_C_HILITE, SE_C_HILITE), "100"  },
    { "ACROS",       CAN(SE_C_INK),     BL(SE_C_MID,    SE_C_HILITE), "100"  },
    { "CLASSIC CH",  CAN(SE_C_EDGE),    BL(SE_C_SLATE,  SE_C_INK),    "CC"   },
    { "CLASSIC N",   CAN(SE_C_OLIVE),   BL(SE_C_RED,    SE_C_HILITE), "NEG"  },
    { "NOSTALGIC",   CAN(SE_C_AMBER),   BL(SE_C_RUST,   SE_C_HILITE), "NN"   },
    { "REALA",       CAN(SE_C_HILITE),  BL(SE_C_OLIVE,  SE_C_INK),    "ACE"  },
    { "SUPERIA",     CAN(SE_C_OLIVE),   BL(SE_C_HILITE, SE_C_HILITE), "400"  },
    { "PRO 400H",    CAN(SE_C_TEAL),    BL(SE_C_HILITE, SE_C_HILITE), "400"  },
    { "ETERNA",      CINE(SE_C_MID),    BL(SE_C_OLIVE,  SE_C_HILITE), "ETR"  },
    { "BLEACH",      CINE(SE_C_SLATE),  BL(SE_C_EDGE,   SE_C_INK),    "BB"   },
    { "HP5",         CAN(SE_C_HILITE),  BL(SE_C_INK,    SE_C_INK),    "HP5"  },
    { "DELTA",       CAN(SE_C_HILITE),  BL(SE_C_SHADOW, SE_C_RED),    "3K2"  },
    { "SOUP 800",    CAN(SE_C_INK),     BL(SE_C_RED,    SE_C_AMBER),  "800"  },
};
/* anything the card carries that this file has never heard of */
static const se_roll g_roll_unknown =
    { "", CAN(SE_C_MID), BL(SE_C_SLATE, SE_C_HILITE), "?" };

static const se_roll *stock_roll(const char *name)
{
    int i;
    if (name)
        for (i = 0; i < (int)(sizeof g_rolls / sizeof g_rolls[0]); i++)
            if (name_is(name, g_rolls[i].key)) return &g_rolls[i];
    return &g_roll_unknown;
}

/* The desktop app shades by 0.6 / 1.25; here each colour's darker and lighter
 * neighbour among the sixteen, as dark << 4 | light. */
static const uint8_t g_shade[16] = {
    0x00, 0x12, 0x13, 0x24, 0x35, 0x46, 0x57, 0x5F,
    0x19, 0x8A, 0x8B, 0xA7, 0x25, 0x25, 0x85, 0x5F,
};
#define DARK(c)  (g_shade[(c) & 15] >> 4)
#define LIGHT(c) (g_shade[(c) & 15] & 15)

/* One sprite pixel block: 2s wide, s tall (see above). */
static void spr_rect(se_ui_fb *fb, int x, int y, int s,
                     int x0, int y0, int x1, int y1, uint8_t c)
{
    se_ui_fill(fb, x + x0 * 2 * s, y + y0 * s,
               (x1 - x0 + 1) * 2 * s, (y1 - y0 + 1) * s, c);
}

static void draw_roll(se_ui_fb *fb, int x, int y, int s, const se_roll *r)
{
    uint8_t body = r->shape_body & 15;
    uint8_t band = r->band_label >> 4, label = r->band_label & 15;
    int tx, len = 0, i, j;

    while (r->iso[len]) len++;

    if (!(r->shape_body & 0x10)) {
        /* film leader with sprocket holes */
        spr_rect(fb, x, y, s, 22, 9, 27, 24, SE_C_SLATE);
        for (j = 10; j <= 23; j += 3) spr_rect(fb, x, y, s, 26, j, 26, j, SE_C_INK);
        spr_rect(fb, x, y, s, 22, 9, 22, 24, SE_C_SHADOW);
        /* spool and caps */
        spr_rect(fb, x, y, s, 9, 0, 15, 2, SE_C_MID);
        spr_rect(fb, x, y, s, 10, 0, 11, 1, SE_C_EDGE);
        spr_rect(fb, x, y, s, 3, 3, 21, 5, SE_C_SHADOW);
        spr_rect(fb, x, y, s, 3, 27, 21, 29, SE_C_SHADOW);
        spr_rect(fb, x, y, s, 4, 3, 20, 3, SE_C_SLATE);
        spr_rect(fb, x, y, s, 4, 29, 20, 29, SE_C_INK);
        /* body, band, label */
        spr_rect(fb, x, y, s, 2, 6, 22, 26, body);
        spr_rect(fb, x, y, s, 2, 6, 2, 26, DARK(body));
        spr_rect(fb, x, y, s, 22, 6, 22, 26, DARK(body));
        spr_rect(fb, x, y, s, 4, 6, 4, 26, LIGHT(body));
        spr_rect(fb, x, y, s, 2, 8, 22, 12, band);
        spr_rect(fb, x, y, s, 4, 8, 4, 12, LIGHT(band));
        spr_rect(fb, x, y, s, 2, 24, 22, 24, DARK(band));
        tx = 12 - (len * 6 - 1) / 2;
        se_ui_text(fb, x + tx * 2 * s, y + 16 * s, r->iso, label, 2 * s, s);
    } else {
        /* round cine can from above, in doubled coordinates so the circle
         * needs no square root: (2x-27)^2 + (2y-31)^2 against 4 d^2 */
        for (j = 0; j < SE_SPR_H; j++)
            for (i = 0; i < SE_SPR_W; i++) {
                int dx = 2 * i - 27, dy = 2 * j - 31, d = dx * dx + dy * dy;
                uint8_t c;
                if (d > 729) continue;
                c = d > 625 ? DARK(body) : d > 484 ? LIGHT(body) : body;
                if (d <= 36) c = DARK(body);
                spr_rect(fb, x, y, s, i, j, i, j, c);
            }
        spr_rect(fb, x, y, s, 2, 19, 25, 26, band);
        spr_rect(fb, x, y, s, 3, 19, 24, 19, LIGHT(band));
        for (j = 0; j < 4; j++)
            spr_rect(fb, x, y, s, j & 1 ? 21 : 6, j & 2 ? 14 : 6,
                     j & 1 ? 21 : 6, j & 2 ? 14 : 6, DARK(body));
        tx = 13 - (len * 6 - 1) / 2;
        se_ui_text(fb, x + tx * 2 * s, y + 20 * s, r->iso, label, 2 * s, s);
    }
}

enum {
    CANISTER_WIDTH = 188, CANISTER_AXIS = 132,
    CANISTER_RADIUS = 55, CANISTER_CAP_DEPTH = 10,
    CANISTER_LEADER_WIDTH = 60
};

/* Print counterclockwise. Slightly widen the dots across the label. */
static void draw_axial_text(se_ui_fb *fb, int cx, int cy, const char *line,
                            uint8_t colour)
{
    const int dot_width = 3;
    int y = cy + se_ui_text_w(line, 1) / 2;

    for (; *line; line++, y -= SE_FONT_W + 1) {
        const uint8_t *glyph = se_ui_font[glyph_index((unsigned char)*line)];
        int col, row;
        for (col = 0; col < SE_FONT_W; col++)
            for (row = 0; row < SE_FONT_H; row++)
                if (glyph[col] & (1u << row))
                    se_ui_fill(fb, cx + dot_width * row - SE_FONT_H * dot_width / 2,
                               y - col, dot_width, 1, colour);
    }
}

/* Draw the cap and spindle on individual framebuffer rows. */
static void canister_oval(se_ui_fb *fb, int x, int y, int rx, int ry,
                          uint8_t colour)
{
    int half = rx, dx_squared = rx * rx, dy_squared = ry * ry;
    int limit = dx_squared * dy_squared;
    int dy;

    for (dy = 0; dy <= ry; dy++) {
        while (half * half * dy_squared + dy * dy * dx_squared > limit)
            half--;
        se_ui_fill(fb, x - half, y - dy, 2 * half + 1, 1, colour);
        if (dy)
            se_ui_fill(fb, x - half, y + dy, 2 * half + 1, 1, colour);
    }
}

/* Front half of the same ellipse used to draw both metal end caps. */
static int canister_front_arc(int dx)
{
    const int radius_sq = CANISTER_RADIUS * CANISTER_RADIUS;
    const int depth_sq = CANISTER_CAP_DEPTH * CANISTER_CAP_DEPTH;
    int available = (radius_sq - dx * dx) * depth_sq;
    int arc = CANISTER_CAP_DEPTH;

    while (arc * arc * radius_sq > available) arc--;
    return arc;
}

static void draw_leader_perforation(se_ui_fb *fb, int x, int y, int right)
{
    se_ui_fill(fb, x, y + 1, SE_MIN(8, right - x), 2, SE_C_PAPER);
    se_ui_fill(fb, x + 1, y, SE_MIN(6, right - x - 1), 4, SE_C_PAPER);
}

static void draw_film_leader(se_ui_fb *fb, int x, int y, int inset, uint8_t colour)
{
    const int right = x + CANISTER_LEADER_WIDTH;
    int row, hole;

    if (inset >= CANISTER_LEADER_WIDTH) return;
    x += inset;
    /* Move the taper and perforations together under the stationary outlet. */
    for (row = 0; row < 50; row++) {
        int left = x + SE_MAX(0, row - 13);
        se_ui_fill(fb, left, y + 16 + row, right - left, 1, colour);
    }
    for (hole = 8; hole <= 50; hole += 12)
        draw_leader_perforation(fb, x + hole, y + 20, right);
    for (hole = 39; hole <= 51; hole += 12)
        draw_leader_perforation(fb, x + hole, y + 59, right);
}

/* A 35 mm cartridge with the wrapper continuing onto the tangent lip. */
static void draw_canister(se_ui_fb *fb, int x, int y, const se_roll *r,
                          const se_stock_look *look, int leader_inset, uint8_t leader_colour)
{
    uint8_t body = r->shape_body & 15;
    uint8_t band = r->band_label >> 4;
    uint8_t ink = is_light(body) ? SE_C_INK : SE_C_HILITE;
    int axis_x = x + CANISTER_AXIS;
    int label_y = y + 44;
    int col;

    draw_film_leader(fb, x, y, leader_inset, leader_colour);
    canister_oval(fb, axis_x, y + 71, CANISTER_RADIUS, CANISTER_CAP_DEPTH, SE_C_INK);

    /* The label follows the projected cylinder. The outlet continues the
     * label from the left tangent, with no seam through the colour bands. */
    se_ui_fill(fb, x + 55, y + 15, 3, 55, SE_C_INK);
    for (col = 58; col < CANISTER_WIDTH - 2; col += 2) {
        int arc = canister_front_arc(SE_MAX(col, 86) - CANISTER_AXIS);
        int top = y + 10 + arc;
        int left = x + col;
        uint8_t shade = col >= 178 ? DARK(body) : body;

        se_ui_fill(fb, left, top, 2, 55, SE_C_INK);
        se_ui_fill(fb, left, top + 1, 2, 53, band);
        se_ui_fill(fb, left, top + 7, 2, 39, shade);
        if (col >= CANISTER_AXIS - CANISTER_RADIUS) {
            uint8_t rim = col < 86 || col >= 178 ? SE_C_MID : SE_C_EDGE;
            se_ui_fill(fb, left, top + 55, 2, 5, rim);
        }
    }
    se_ui_fill(fb, x + CANISTER_WIDTH - 2, y + 10, 2, 62, SE_C_INK);

    /* The hub is a shallow ring above the metal cap, with a dark bore. */
    canister_oval(fb, axis_x, y + 10, CANISTER_RADIUS, CANISTER_CAP_DEPTH, SE_C_INK);
    canister_oval(fb, axis_x, y + 11, 52, 8, SE_C_SLATE);
    canister_oval(fb, axis_x, y + 8, 51, 8, SE_C_EDGE);
    canister_oval(fb, axis_x, y + 8, 48, 7, SE_C_HILITE);
    canister_oval(fb, axis_x, y + 7, 44, 6, SE_C_MID);
    canister_oval(fb, axis_x, y + 8, 22, 6, SE_C_SHADOW);
    canister_oval(fb, axis_x, y + 6, 21, 6, SE_C_INK);
    canister_oval(fb, axis_x, y + 5, 18, 4, SE_C_SLATE);
    canister_oval(fb, axis_x, y + 5, 13, 3, SE_C_INK);

    if (look->label) {
        draw_axial_text(fb, axis_x - 13, label_y, look->label, ink);
        draw_axial_text(fb, axis_x + 13, label_y,
                         look->code ? look->code : look->speed, ink);
    } else {
        draw_axial_text(fb, axis_x, label_y, "35MM", ink);
    }
}

/* A favourite's star, stuck on the roll's top-right corner: gold, with
 * an ink outline so it reads over any canister colour. 9x9 sprite pixels. */
static const uint16_t g_star[9] = {          /* bit 8 = leftmost column */
    0x010, 0x010, 0x038, 0x1FF, 0x0FE, 0x07C, 0x06C, 0x0C6, 0x082,
};
static void draw_star(se_ui_fb *fb, int x, int y, int s)
{
    int k, r, c;
    /* the eight neighbour offsets in ink make the outline, then the fill */
    for (k = 0; k < 10; k++) {
        int dx = k == 9 ? 0 : (k % 3) - 1, dy = k == 9 ? 0 : (k / 3) - 1;
        if (k == 4) continue;          /* (0,0) is the fill, drawn last */
        for (r = 0; r < 9; r++)
            for (c = 0; c < 9; c++)
                if (g_star[r] >> (8 - c) & 1)
                    spr_rect(fb, x, y, s, c + dx + SE_SPR_W - 9, r + dy,
                             c + dx + SE_SPR_W - 9, r + dy,
                             k == 9 ? SE_C_GOLD : SE_C_INK);
    }
}

/*
 * The roll and the name side by side, centred as one unit on the line at
 * y. The name takes the largest size that still fits beside the roll.
 * At scale 2 the roll is 64 rows, which fits between the header rule and
 * the speed band with the name centred on it.
 */
static void roll_and_name(se_ui_fb *fb, int y, const char *nm, uint8_t ink,
                          int fav)
{
    static const int ladder[4] = { 7, 6, 5, 4 };
    const int s = 2, gap = 18, iw = SE_SPR_W * 2 * s;
    int sx = 4, sy, tw, x0, i;

    for (i = 0; i < 4; i++)
        if (se_ui_text_w(nm, ladder[i]) <= SE_UI_W - 120 - iw - gap) {
            sx = ladder[i]; break;
        }
    sy = sx > 5 ? 4 : 3;
    tw = se_ui_text_w(nm, sx);
    x0 = (SE_UI_W - (iw + gap + tw)) / 2;

    /* centred on the name's own height, not on the line */
    draw_roll(fb, x0, y + (SE_FONT_H * sy - SE_SPR_H * s) / 2, s, stock_roll(nm));
    if (fav) draw_star(fb, x0, y + (SE_FONT_H * sy - SE_SPR_H * s) / 2, s);
    se_ui_text(fb, x0 + iw + gap, y, nm, ink, sx, sy);
}

typedef struct {
    se_stock_look look;
    char speed[20];
    int width;
} se_stock_label;

enum { STOCK_ART_Y = SE_UI_STOCK_ART_TOP + 2, STOCK_LABEL_GAP = 18, STOCK_SIDE_MARGIN = 48 };

static void stock_label_init(se_stock_label *label, const se_ui_stock *stock)
{
    label->look = stock_look(stock->name);
    put_str(put_str(label->speed, "FILM SPEED "), label->look.speed);
    label->width = CANISTER_WIDTH + STOCK_LABEL_GAP +
                   SE_MAX(se_ui_text_w(label->look.short_name, 3),
                          se_ui_text_w(label->speed, 2) + 12);
}

static void draw_stock_label(se_ui_fb *fb, const se_ui_stock *stock,
                             const se_stock_label *label, int offset, int leader_inset,
                             uint8_t leader_colour)
{
    const int y = STOCK_ART_Y, s = 2;
    int x = (SE_UI_W - label->width) / 2 + offset;
    int label_x = x + CANISTER_WIDTH + STOCK_LABEL_GAP;

    draw_canister(fb, x, y, stock_roll(stock->name), &label->look, leader_inset, leader_colour);
    if (stock->fav) draw_star(fb, x + CANISTER_WIDTH - SE_SPR_W * 2 * s, y, s);
    se_ui_text(fb, label_x, 73, label->look.short_name, SE_C_INK, 3, 2);
    se_ui_fill(fb, label_x, 97, 5, SE_FONT_H, label->look.accent);
    se_ui_text(fb, label_x + 12, 97, label->speed, SE_C_SLATE, 2, 1);
}

static void draw_stock_selection(const se_ui *u, se_ui_fb *fb, int cur)
{
    int from = u->stock_slide_from;
    int leader_inset = (CANISTER_LEADER_WIDTH * u->stock_leader_retract + 128) / 256;
    uint8_t leader_colour = u->stock_leader_colour;
    se_stock_label current;

    stock_label_init(&current, &u->stocks[cur]);
    if (u->stock_slide && from >= 0 && from < u->nstocks && from != cur) {
        const int margin = STOCK_SIDE_MARGIN / 2;  /* two pixels per byte */
        const int paper = SE_C_PAPER | (SE_C_PAPER << 4);
        se_stock_label outgoing;
        int spacing, offset, y;

        stock_label_init(&outgoing, &u->stocks[from]);
        spacing = SE_UI_W / 2 - STOCK_SIDE_MARGIN +
                  (SE_MAX(current.width, outgoing.width) + 1) / 2 + 16;
        offset = spacing * u->stock_slide / 256;
        if (u->stock_slide < 0) spacing = -spacing;
        draw_stock_label(fb, &u->stocks[from], &outgoing, offset - spacing, leader_inset, leader_colour);
        draw_stock_label(fb, &u->stocks[cur], &current, offset, leader_inset, leader_colour);

        /* Keep the arrows and their gutters outside the sliding artwork. */
        for (y = SE_UI_STOCK_ART_TOP; y < SE_UI_STOCK_ART_TOP + SE_UI_STOCK_ART_ROWS; y++) {
            uint8_t *row = fb->px + y * SE_UI_PITCH;
            memset(row, paper, margin);
            memset(row + SE_UI_PITCH - margin, paper, margin);
        }
    } else {
        draw_stock_label(fb, &u->stocks[cur], &current, 0, leader_inset, leader_colour);
    }
}

static void draw_stock_art(const se_ui *u, se_ui_fb *fb, int cur)
{
    draw_stock_selection(u, fb, cur);
    if (u->nstocks > 1) {
        se_ui_text(fb, 16, 114, SE_SYM_LEFT, SE_C_MID, 3, 2);
        se_ui_text(fb, SE_UI_W - 32, 114, SE_SYM_RIGHT, SE_C_MID, 3, 2);
    }
}

void se_ui_redraw_stock_art(const se_ui *u, se_ui_fb *fb)
{
    const int paper = SE_C_PAPER | (SE_C_PAPER << 4);
    int n = u->nstocks;

    if (n <= 0) return;
    memset(fb->px + SE_UI_STOCK_ART_TOP * SE_UI_PITCH, paper,
           SE_UI_STOCK_ART_ROWS * SE_UI_PITCH);
    draw_stock_art(u, fb, (u->stock % n + n) % n);
}

/* --------------------------------------------------------- the screens */
static void draw_stock(const se_ui *u, se_ui_fb *fb)
{
    int n = u->nstocks, i, cur = 0;
    const char *dsc;
    char buf[64], *d;

    paper_ground(fb);

    if (n <= 0) {
        header(fb, "SOUP", NULL);
        if (!u->ready) {
            big_name(fb, 92, "LOADING", SE_C_MID);
        } else {
            big_name(fb, 80, "NO FILM", SE_C_RED);
            se_ui_text_c(fb, SE_UI_W / 2, 132,
                         "PUT .FS FILES IN ML/FILM ON THE CARD",
                         SE_C_SLATE, 2, 1);
        }
        footer(fb, "[DEL] CLOSE");
        return;
    }

    cur = (u->stock % n + n) % n;
    dsc = u->stocks[cur].desc ? u->stocks[cur].desc : "";

    /* the header carries the count, which is the only number worth having
     * up there: it says the card was read and how much is on it */
    d = buf;
    d = put_int(d, cur + 1);
    d = put_str(d, " OF ");
    put_int(d, n);
    header(fb, "SOUP", buf);

    draw_stock_art(u, fb, cur);

    /* what the stock does, in its own words, up to the first colon */
    d = buf;
    for (i = 0; dsc[i] && dsc[i] != ':' && i < 44; i++) *d++ = dsc[i];
    *d = 0;
    se_ui_text_c(fb, SE_UI_W / 2, 154, buf, SE_C_SLATE, 2, 1);

    /*
     * Where you are in the list, as plain ticks. One per stock up to
     * twenty-four, after which they would stop being countable and are
     * dropped -- the "N OF M" in the header still says.
     */
    /* 7D port: upstream dropped the ticks above 24 stocks. 14 px per tick
     * fits 48 across the screen; beyond that the pitch shrinks to fit. */
    if (n > 1) {
        int pitch = 14, tw, w, x0;
        if (n * pitch - 6 > SE_UI_W - 40) pitch = (SE_UI_W - 40) / n;
        if (pitch < 3) pitch = 3;
        tw = pitch > 6 ? pitch - 6 : pitch - 1;
        w = n * pitch - (pitch - tw);
        x0 = (SE_UI_W - w) / 2;
        if (x0 < 0) x0 = 0;
        for (i = 0; i < n; i++) {
            if (i == cur) se_ui_fill(fb, x0 + i * pitch, 176, tw, 4, SE_C_INK);
            else if (u->stocks[i].fav)
                          se_ui_fill(fb, x0 + i * pitch, 177, tw, 3, SE_C_GOLD);
            else          se_ui_fill(fb, x0 + i * pitch, 178, tw, 2, SE_C_EDGE);
        }
    }

    /*
     * The source belongs on this screen, not only in settings. It is the
     * difference between a frame arriving in seconds and one arriving in
     * minutes, and between Canon's colour and ours.
     */
    /*
     * "SOURCE: RAW", with the value in ink and the label grey.
     *
     * Two colours rather than one sentence. The label is the same every
     * time and the value is the only part that changes, so the eye should
     * land on the value. Centred as a unit, which needs both widths first.
     *
     * Grain is gone from this line: the payload never reads the UI's grain
     * setting -- grain comes from the stock file's own GRAIN line and is
     * part of what a stock IS -- so saying "GRAIN ON" here was reporting a
     * switch that is not wired to anything.
     */
    {
        const char *lab = u->info_label ? u->info_label : "SOURCE: ";
        const char *val = u->info_value ? u->info_value
                                        : g_src_name[se_ui_source(u)];
        int lw = se_ui_text_w(lab, 2);
        int vw = se_ui_text_w(val, 2);
        int x  = (SE_UI_W - (lw + vw)) / 2;
        se_ui_text(fb, x,      194, lab, SE_C_MID, 2, 1);
        se_ui_text(fb, x + lw, 194, val,
                   u->info_colour ? u->info_colour : SE_C_INK, 2, 1);
    }

    /*
     * What the body actually does, not what se_ui_key_press does. The
     * camera never routes buttons through this state machine -- se_key in
     * the payload calls se_next_stock and se_toggle_source directly -- so
     * this line names the camera's controls.
     */
    footer(fb, u->stock_hint ? u->stock_hint
                             : "[\x01\x02] SOURCE   [SET] CHOOSE   [DEL] CLOSE");
}

static void draw_settings(const se_ui *u, se_ui_fb *fb)
{
    int i, cur;
    const int row_y = 48, row_h = 42;

    paper_ground(fb);
    header(fb, u->settings_title ? u->settings_title : "SETTINGS",
           u->settings_right);

    if (u->set_n > 0) {
        int n = SE_CLAMP(u->set_n, 1, SE_UI_PORT_SETTINGS);
        cur = SE_CLAMP(u->setting, 0, n - 1);
        for (i = 0; i < n; i++) {
            int y = row_y + i * row_h, sel = (i == cur);
            const char *lab = u->set_label[i] ? u->set_label[i] : "?";
            const char *val = u->set_value[i] ? u->set_value[i] : "";
            if (sel) se_ui_fill(fb, 16, y - 6, 6, 28, SE_C_AMBER);
            se_ui_text(fb, 36, y, lab, sel ? SE_C_INK : SE_C_MID, 3, 2);
            se_ui_text(fb, SE_UI_W - 36 - se_ui_text_w(val, 2), y + 4,
                       val, sel ? SE_C_INK : SE_C_MID, 2, 1);
            if (i < n - 1)
                se_ui_fill(fb, 36, y + row_h - 16, SE_UI_W - 72, 1, SE_C_EDGE);
        }
        if (u->set_help[cur])
            se_ui_text_c(fb, SE_UI_W / 2, 190, u->set_help[cur], SE_C_SLATE, 2, 1);
        footer(fb, u->settings_hint ? u->settings_hint
                                    : "[\x01\x02] CHANGE   [MENU] BACK");
        return;
    }

    /* clamp for display: an out-of-range value must still show a selected
     * row rather than silently highlighting nothing */
    cur = SE_CLAMP(u->setting, 0, SE_SET_COUNT - 1);

    for (i = 0; i < SE_SET_COUNT; i++) {
        int y = row_y + i * row_h;
        int sel = (i == cur);
        char label[32], value[48];

        setting_label(i, label);
        setting_value(u, i, value);

        /* the selected row is marked by a bar and by weight, not by a
         * filled block -- reversed type is harder to read, and this screen
         * is nothing but reading */
        if (sel) se_ui_fill(fb, 16, y - 6, 6, 28, SE_C_AMBER);
        se_ui_text(fb, 36, y, label, sel ? SE_C_INK : SE_C_MID, 3, 2);
        se_ui_text(fb, SE_UI_W - 36 - se_ui_text_w(value, 2), y + 4,
                   value, sel ? SE_C_INK : SE_C_MID, 2, 1);
        if (i < SE_SET_COUNT - 1)
            se_ui_fill(fb, 36, y + row_h - 16, SE_UI_W - 72, 1, SE_C_EDGE);
    }

    {
        static const char *help[SE_SET_COUNT] = {
            "SECONDS AND CANON S COLOUR, OR MINUTES AND OURS",
            "OUTPUT SIZE OF THE DEVELOPED JPEG",
            "EACH STOCK CARRIES ITS OWN GRAIN - THIS TURNS IT OFF"
        };
        se_ui_text_c(fb, SE_UI_W / 2, 190, help[cur], SE_C_SLATE, 2, 1);
    }

    footer(fb, "[\x01\x02] CHANGE   [MENU] BACK");
}

static void draw_working(const se_ui *u, se_ui_fb *fb)
{
    int n = u->nstocks;
    const char *nm = (n > 0) ? u->stocks[(u->stock % n + n) % n].name : "?";
    char buf[64], *d;
    const int bar_x = 110, bar_w = SE_UI_W - 220, bar_y = 130, bar_h = 20;
    int filled = (bar_w - 4) * SE_CLAMP(u->percent, 0, 100) / 100;

    paper_ground(fb);
    header(fb, "DEVELOPING", u->file[0] ? u->file : NULL);

    roll_and_name(fb, 54, nm ? nm : "?", SE_C_INK,
                  n > 0 && u->stocks[(u->stock % n + n) % n].fav);

    d = buf;
    d = put_str(d, u->stage && u->stage[0] ? u->stage : "WORKING");
    d = put_str(d, "   ");
    d = put_int(d, u->percent);
    put_str(d, "%");
    se_ui_text_c(fb, SE_UI_W / 2, 108, buf, SE_C_SLATE, 2, 1);

    /* a plain bar. It is a progress bar; it does not need to be film */
    plate(fb, bar_x, bar_y, bar_w, bar_h, SE_C_PAPER, SE_C_INK, SE_C_PAPER);
    if (filled > 0)
        se_ui_fill(fb, bar_x + 2, bar_y + 2, filled, bar_h - 4, SE_C_AMBER);

    d = buf;
    d = put_str(d, "FRAME ");
    d = put_int(d, u->done + 1);
    d = put_str(d, " OF ");
    put_int(d, u->total);
    se_ui_text_c(fb, SE_UI_W / 2, 164, buf, SE_C_MID, 2, 1);

    d = buf;
    d = put_str(d, "SOURCE ");
    d = put_str(d, g_src_name[se_ui_source(u)]);
    d = put_str(d, "   ");
    d = put_str(d, g_res_name[setval(u, SE_SET_RES)]);
    se_ui_text_c(fb, SE_UI_W / 2, 190, buf, SE_C_MID, 2, 1);

    footer(fb, "[MENU] CANCEL");
}

static void draw_done(const se_ui *u, se_ui_fb *fb)
{
    char buf[64], *d;

    paper_ground(fb);
    header(fb, "SOUP", NULL);

    big_name(fb, 62, u->done_title ? u->done_title : "DEVELOPED",
             u->done_colour ? u->done_colour : SE_C_INK);
    se_ui_fill(fb, (SE_UI_W - 300) / 2, 116, 300, 4, SE_C_OLIVE);

    d = buf;
    d = put_int(d, u->done);
    d = put_str(d, u->done == 1 ? " FRAME IN " : " FRAMES IN ");
    d = put_int(d, u->last_seconds);
    put_str(d, " S");
    se_ui_text_c(fb, SE_UI_W / 2, 138, buf, SE_C_INK, 3, 2);

    se_ui_text_c(fb, SE_UI_W / 2, 178,
                 u->done_note ? u->done_note
                              : "JPEGS WRITTEN NEXT TO THE RAW FILES",
                 SE_C_SLATE, 2, 1);

    footer(fb, "ANY KEY TO CONTINUE");
}

void se_ui_chrome(se_ui_fb *fb, const char *title, const char *right,
                  const char *hints)
{
    paper_ground(fb);
    header(fb, title, right);
    if (hints) footer(fb, hints);
}

void se_ui_draw(const se_ui *u, se_ui_fb *fb)
{
    switch (u->screen) {
    case SE_SCREEN_STOCK:    draw_stock(u, fb);    break;
    case SE_SCREEN_SETTINGS: draw_settings(u, fb); break;
    case SE_SCREEN_WORKING:  draw_working(u, fb);  break;
    case SE_SCREEN_DONE:     draw_done(u, fb);     break;
    default:                 paper_ground(fb); break;
    }
}
