/*
 * VSCode3DS 1.0 -- one source, two targets. SPDX-License-Identifier: GPL-3.0-or-later
 * SDK baseline: TeamAuroraOS/AuroraOS 8123fd90, native SDK 1.2.
 * HOST (macOS/Linux): cc -std=c11 -O2 -Wall -Wextra -DVS3DS_HOST VSCode3DS.c -o vs3ds-host
 * TEST: cc -std=c11 -g -DVS3DS_TEST VSCode3DS.c -o vs3ds-test && ./vs3ds-test
 * 3DS: python3 sdk/aurcc.py build /absolute/path/VSCode3DS.c -o VSCode3DS.bin
 * HOST RUN: ./vs3ds-host --root /absolute/path/AuroraOS --token YOUR_SECRET_16_PLUS --bind
 * 192.168.1.2 Python3 and devkitARM must be on the host PATH. Run only on a trusted LAN: AuroraOS
 * net_tcp_exchange has no encryption. Never expose the host publicly. No external UI assets.
 * Offline SDK and Auric reference are embedded below. 3DS controls: Dpad cursor (hold repeats),
 * A+Dpad select, B backspace, X save, Y build, L/R previous/next document, START command list,
 * SELECT keyboard page. Circle pad scroll. SELECT+A select all.
 * System L+Down+Select is reserved for AuroraOS. HOME checkpoints via atexit.
 * Limits: 6 documents, 512KiB/document, 64 history edits/document,
 * 2MiB total undo payload, 64 files/build, 4MiB project upload, 15MiB binary.
 * UTF-8 is preserved and navigated by codepoint; unsupported glyphs display '?'.
 * No fake local compiler/debugger: compilation and semantic diagnostics run on host.
 */
#if defined(VS3DS_HOST) || defined(VS3DS_TEST)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION "1.0.0"
#define DOC_MAX 6
#define TEXT_MAX (512u * 1024u)
#define HIST_MAX 64
#define HIST_BUDGET (2u * 1024u * 1024u)
#define FILE_MAX 64
#define PROJECT_MAX (4u * 1024u * 1024u)
#define BIN_MAX (15u * 1024u * 1024u)
#define REL_MAX 160
#define PATH_CAP 1024
#define WIRE_CHUNK 1200u
#define REPLY_MAX 8192u
#define NONE ((size_t)-1)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void strcopy(char *d, size_t cap, const char *s) {
    if (cap) {
        size_t n = MIN(strlen(s), cap - 1);
        memmove(d, s, n);
        d[n] = 0;
    }
}
static bool fmtpath(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n >= 0 && (size_t)n < cap;
}
static bool suffix(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    if (a < b)
        return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)end[i]))
            return false;
    return true;
}
static bool relvalid(const char *s) {
    if (!s || !*s || strlen(s) >= REL_MAX || *s == '/' || *s == '.')
        return false;
    size_t part = 0;
    for (const unsigned char *p = (const unsigned char *)s;; p++) {
        if (!*p || *p == '/') {
            if (!part)
                return false;
            part = 0;
            if (!*p)
                break;
            if (p[1] == '.')
                return false;
        } else {
            if (*p < 32 || *p > 126 || strchr("\\:|*?<>\"", *p))
                return false;
            ++part;
        }
    }
    return true;
}
static uint32_t hashstep(uint32_t h, const void *v, size_t n) {
    const unsigned char *p = v;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}
static uint32_t hashdata(const void *p, size_t n) { return hashstep(2166136261u, p, n); }
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool package_valid(const unsigned char *p, size_t n, bool c) {
    if (n < 40 || n > BIN_MAX || memcmp(p, c ? "AURC" : "AUR1", 4))
        return false;
    uint32_t off = le32(p + 4), sz = le32(p + 8), load = le32(p + 12), entry = le32(p + 16);
    return off == 36 && sz > 0 && (uint64_t)off + sz == n && load == 0x22000000u &&
           (uint64_t)load + sz <= 0x22f00000u && entry >= load &&
           (uint64_t)entry < ((uint64_t)load + sz) && !(entry & 3u) && !le32(p + 20) &&
           !le32(p + 24) && !le32(p + 28) && !le32(p + 32);
}
static unsigned char *read_bytes(const char *path, size_t *length, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }
    long z = ftell(f);
    if (z < 0 || (uint64_t)z > limit || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    unsigned char *p = malloc((size_t)z + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(p, 1, (size_t)z, f);
    bool bad = ferror(f) != 0;
    int cl = fclose(f);
    if (got != (size_t)z || bad || cl) {
        free(p);
        return NULL;
    }
    p[got] = 0;
    *length = got;
    return p;
}
/* Recoverable replacement: never remove an old backup before a new file exists.
 * Backup is retained after success; FAT rename is not a power-loss transaction. */
#if !defined(VS3DS_HOST)
static bool atomic_bytes(const char *path, const void *data, size_t n) {
    char tmp[PATH_CAP], bak[PATH_CAP];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
        snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (fflush(f))
        ok = false;
    if (fclose(f))
        ok = false;
    if (!ok)
        return false;
    struct stat st;
    bool existed = stat(path, &st) == 0;
    if (existed) {
        if (remove(bak) != 0 && errno != ENOENT)
            return false;
        if (rename(path, bak))
            return false;
    }
    if (rename(tmp, path)) {
        if (existed)
            (void)rename(bak, path);
        return false;
    }
    return true;
}

/* -------------------- portable editor model -------------------- */
typedef struct {
    size_t pos, oldn, newn, before, after;
    char *old, *newtext;
    uint32_t stamp;
    int kind;
} Edit;
typedef struct {
    char name[REL_MAX];
    char *text;
    size_t len, cap, cursor, anchor, scroll, hscroll, goal;
    size_t *lines, nlines, linecap;
    unsigned char *lex;
    bool crlf, changed_index, recovered;
    uint32_t saved_hash, revision;
    Edit history[HIST_MAX];
    int hcount, hpos;
    bool boundary;
} Document;
static size_t history_bytes;
static size_t prevpos(const Document *d, size_t p) {
    if (!p)
        return 0;
    --p;
    while (p && ((unsigned char)d->text[p] & 0xc0) == 0x80)
        --p;
    return p;
}
static size_t nextpos(const Document *d, size_t p) {
    if (p >= d->len)
        return d->len;
    ++p;
    while (p < d->len && ((unsigned char)d->text[p] & 0xc0) == 0x80)
        ++p;
    return p;
}
static bool utf8valid(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++], v, minv;
        int k;
        if (!c)
            return false;
        if (c < 128)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            v = c & 31;
            k = 1;
            minv = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            v = c & 15;
            k = 2;
            minv = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            v = c & 7;
            k = 3;
            minv = 0x10000;
        } else
            return false;
        if (i + (size_t)k > n)
            return false;
        while (k--) {
            c = p[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            v = (v << 6) | (c & 63);
        }
        if (v < minv || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
            return false;
    }
    return true;
}
static bool reserve(Document *d, size_t n) {
    if (n > TEXT_MAX)
        return false;
    if (n + 1 <= d->cap)
        return true;
    size_t cap = d->cap ? d->cap : 256;
    while (cap < n + 1)
        cap *= 2;
    if (cap > TEXT_MAX + 1)
        cap = TEXT_MAX + 1;
    char *p = realloc(d->text, cap);
    if (!p)
        return false;
    d->text = p;
    d->cap = cap;
    return true;
}
static bool line_storage(Document *d, size_t n) {
    if (n > 65536)
        return false;
    if (n + 1 <= d->linecap)
        return true;
    size_t cap = 128;
    while (cap < n + 1)
        cap *= 2;
    if (cap > 65537)
        cap = 65537;
    size_t *lines = malloc(cap * sizeof(size_t));
    unsigned char *lex = malloc(cap);
    if (!lines || !lex) {
        free(lines);
        free(lex);
        return false;
    }
    free(d->lines);
    free(d->lex);
    d->lines = lines;
    d->lex = lex;
    d->linecap = cap;
    d->changed_index = true;
    return true;
}
static size_t count_lines(const char *s, size_t n) {
    size_t lines = 1;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n')
            lines++;
    return lines;
}
static void edit_free(Edit *e) {
    history_bytes -= e->oldn + e->newn;
    free(e->old);
    free(e->newtext);
    memset(e, 0, sizeof(*e));
}
static void history_clear(Document *d) {
    for (int i = 0; i < d->hcount; i++)
        edit_free(&d->history[i]);
    d->hcount = d->hpos = 0;
    d->boundary = true;
}
static void doc_free(Document *d) {
    history_clear(d);
    free(d->text);
    free(d->lines);
    free(d->lex);
    memset(d, 0, sizeof(*d));
}
static bool doc_set(Document *d, const char *s, size_t n) {
    if (!utf8valid((const unsigned char *)s, n) || !reserve(d, n) ||
        !line_storage(d, count_lines(s, n)))
        return false;
    memcpy(d->text, s, n);
    d->text[n] = 0;
    d->len = n;
    d->cursor = 0;
    d->anchor = NONE;
    d->goal = NONE;
    d->changed_index = true;
    d->saved_hash = hashdata(s, n);
    d->revision++;
    d->boundary = true;
    return true;
}
static bool doc_dirty(const Document *d) {
    return d->recovered || hashdata(d->text, d->len) != d->saved_hash;
}
static bool bounds(const Document *d, size_t *a, size_t *b) {
    *a = *b = d->cursor;
    if (d->anchor != NONE) {
        *a = MIN(d->anchor, d->cursor);
        *b = d->anchor > d->cursor ? d->anchor : d->cursor;
    }
    return *a != *b;
}
static bool splice(Document *d, size_t a, size_t b, const char *s, size_t n) {
    if (a > b || b > d->len || n > TEXT_MAX - (d->len - (b - a)) ||
        !reserve(d, d->len - (b - a) + n))
        return false;
    size_t rows = count_lines(d->text, d->len) - (count_lines(d->text + a, b - a) - 1) +
                  (count_lines(s, n) - 1);
    if (!line_storage(d, rows))
        return false;
    memmove(d->text + a + n, d->text + b, d->len - b + 1);
    if (n)
        memcpy(d->text + a, s, n);
    d->len = d->len - (b - a) + n;
    d->cursor = a + n;
    d->anchor = NONE;
    d->goal = NONE;
    d->changed_index = true;
    d->revision++;
    return true;
}
/* All allocations precede mutation. History exhaustion rejects the edit instead
 * of silently discarding the user's ability to undo it. Group typing <=700ms. */
static bool replace(Document *d, size_t a, size_t b, const char *s, size_t n, uint32_t now,
                    int kind) {
    if (a > b || b > d->len || n > TEXT_MAX - (d->len - (b - a)) || (!n && a == b))
        return false;
    size_t cost = b - a + n;
    if (cost > HIST_BUDGET)
        return false;
    while (d->hpos < d->hcount)
        edit_free(&d->history[--d->hcount]);
    while (d->hcount && (history_bytes + cost > HIST_BUDGET || d->hcount == HIST_MAX)) {
        edit_free(&d->history[0]);
        memmove(d->history, d->history + 1, (size_t)(--d->hcount) * sizeof(Edit));
        d->hpos--;
    }
    if (history_bytes + cost > HIST_BUDGET)
        return false;
    Edit *last = d->hpos ? &d->history[d->hpos - 1] : NULL;
    if (kind == 1 && !d->boundary && last && last->kind == 1 && !last->oldn && a == b &&
        a == last->pos + last->newn && (uint32_t)(now - last->stamp) < 700 &&
        last->newn + n <= 128 && !memchr(s, '\n', n)) {
        char *joined = malloc(last->newn + n);
        if (!joined)
            return false;
        memcpy(joined, last->newtext, last->newn);
        memcpy(joined + last->newn, s, n);
        if (!splice(d, a, b, s, n)) {
            free(joined);
            return false;
        }
        free(last->newtext);
        last->newtext = joined;
        last->newn += n;
        history_bytes += n;
        last->after = d->cursor;
        last->stamp = now;
        return true;
    }
    Edit e;
    memset(&e, 0, sizeof(e));
    e.pos = a;
    e.oldn = b - a;
    e.newn = n;
    e.before = d->cursor;
    e.stamp = now;
    e.kind = kind;
    if (e.oldn) {
        e.old = malloc(e.oldn);
        if (!e.old)
            return false;
        memcpy(e.old, d->text + a, e.oldn);
    }
    if (n) {
        e.newtext = malloc(n);
        if (!e.newtext) {
            free(e.old);
            return false;
        }
        memcpy(e.newtext, s, n);
    }
    if (!splice(d, a, b, s, n)) {
        free(e.old);
        free(e.newtext);
        return false;
    }
    e.after = d->cursor;
    d->history[d->hcount++] = e;
    d->hpos = d->hcount;
    history_bytes += cost;
    d->boundary = false;
    return true;
}
static bool undo(Document *d, bool redo) {
    if ((redo && d->hpos == d->hcount) || (!redo && !d->hpos))
        return false;
    Edit *e = &d->history[redo ? d->hpos : d->hpos - 1];
    if (!splice(d, e->pos, e->pos + (redo ? e->oldn : e->newn), redo ? e->newtext : e->old,
                redo ? e->newn : e->oldn))
        return false;
    d->cursor = redo ? e->after : e->before;
    d->hpos += redo ? 1 : -1;
    d->boundary = true;
    return true;
}
/* Cache line boundaries and multiline C comment state only after edits. */
static bool index_doc(Document *d) {
    if (!d->changed_index && d->lines)
        return true;
    size_t n = 1;
    for (size_t i = 0; i < d->len; i++)
        if (d->text[i] == '\n')
            n++;
    if (!line_storage(d, n))
        return false;
    size_t *ls = d->lines;
    unsigned char *lex = d->lex;
    memset(lex, 0, n + 1);
    size_t row = 0;
    ls[0] = 0;
    bool block = false;
    char quote = 0;
    bool escape = false, linecomment = false;
    for (size_t i = 0; i < d->len; i++) {
        char c = d->text[i], v = i + 1 < d->len ? d->text[i + 1] : 0;
        if (c == '\n') {
            ls[++row] = i + 1;
            lex[row] = (unsigned char)block;
            quote = 0;
            escape = false;
            linecomment = false;
            continue;
        }
        if (linecomment)
            continue;
        if (block) {
            if (c == '*' && v == '/') {
                block = false;
                i++;
            }
            continue;
        }
        if (quote) {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '/' && v == '*') {
            block = true;
            i++;
        } else if (c == '/' && v == '/')
            linecomment = true;
        else if (c == '"' || c == '\'')
            quote = c;
    }
    ls[n] = d->len;
    d->nlines = n;
    d->changed_index = false;
    return true;
}
static size_t row_at(Document *d, size_t p) {
    if (!index_doc(d))
        return 0;
    size_t a = 0, b = d->nlines;
    while (a + 1 < b) {
        size_t m = (a + b) / 2;
        if (d->lines[m] <= p)
            a = m;
        else
            b = m;
    }
    return a;
}
static size_t row_end(const Document *d, size_t row) {
    size_t e = d->lines[row + 1];
    return e && d->text[e - 1] == '\n' ? e - 1 : e;
}
static size_t column(const Document *d, size_t p, unsigned tab) {
    size_t a = p;
    while (a && d->text[a - 1] != '\n')
        --a;
    size_t col = 0;
    while (a < p) {
        if (d->text[a] == '\t')
            col += tab - col % tab;
        else
            col++;
        a = nextpos(d, a);
    }
    return col;
}
static size_t colpos(const Document *d, size_t row, size_t col, unsigned tab) {
    size_t p = d->lines[row], e = row_end(d, row), c = 0;
    while (p < e) {
        size_t next = d->text[p] == '\t' ? c + tab - c % tab : c + 1;
        if (next > col)
            break;
        c = next;
        p = nextpos(d, p);
    }
    return p;
}
static bool wordchar(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }
static bool match_at(const Document *d, size_t p, const char *s, size_t n, bool sensitive,
                     bool whole) {
    if (!n || p > d->len || n > d->len - p)
        return false;
    for (size_t j = 0; j < n; j++) {
        unsigned char a = (unsigned char)d->text[p + j], b = (unsigned char)s[j];
        if (sensitive ? a != b : tolower(a) != tolower(b))
            return false;
    }
    return !whole || ((!p || !wordchar((unsigned char)d->text[p - 1])) &&
                      (p + n == d->len || !wordchar((unsigned char)d->text[p + n])));
}
static size_t find_text(const Document *d, const char *s, size_t start, bool backwards,
                        bool sensitive, bool whole) {
    size_t n = strlen(s);
    if (!n)
        return NONE;
    size_t p = MIN(start, d->len);
    for (size_t k = 0; k <= d->len; k++) {
        if (match_at(d, p, s, n, sensitive, whole))
            return p;
        if (backwards)
            p = p ? p - 1 : d->len;
        else
            p = p == d->len ? 0 : p + 1;
    }
    return NONE;
}
static bool replace_all(Document *d, const char *s, const char *to, bool sensitive, bool whole,
                        uint32_t stamp) {
    size_t n = strlen(s), m = strlen(to), count = 0;
    if (!n)
        return false;
    for (size_t i = 0; i < d->len;)
        if (match_at(d, i, s, n, sensitive, whole)) {
            count++;
            i += n;
        } else
            i++;
    if (!count || (m > n && count > (TEXT_MAX - d->len) / (m - n)))
        return false;
    size_t z = d->len - count * n + count * m;
    char *out = malloc(z + 1);
    if (!out)
        return false;
    size_t w = 0;
    for (size_t i = 0; i < d->len;)
        if (match_at(d, i, s, n, sensitive, whole)) {
            memcpy(out + w, to, m);
            w += m;
            i += n;
        } else
            out[w++] = d->text[i++];
    out[w] = 0;
    bool ok = replace(d, 0, d->len, out, w, stamp, 0);
    free(out);
    return ok;
}

#endif /* portable editor is not linked into host */
#if defined(VS3DS_HOST)
/* -------------------- authenticated LAN build host -------------------- */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
typedef struct {
    char path[REL_MAX];
    size_t total, received;
    uint32_t hash;
    bool verified;
} Upload;
typedef struct {
    char id[9], dir[PATH_CAP], entry[REL_MAX];
    Upload files[FILE_MAX];
    int count;
    size_t bytes;
    bool c, locked, finished;
    pid_t pid;
    int result;
    time_t started;
    size_t binsize;
    uint32_t binhash;
} Job;
static Job jobs[8];
static char root[PATH_CAP], secret[96], bindip[64] = "127.0.0.1";
static int port = 45321;
static volatile sig_atomic_t running = 1;
static void stop_signal(int s) {
    (void)s;
    running = 0;
}
static bool sendall(int fd, const void *v, size_t n) {
    const unsigned char *p = v;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}
static bool recvall(int fd, void *v, size_t n) {
    unsigned char *p = v;
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}
static void respond(int fd, const char *status, const void *v, size_t n) {
    char h[100];
    int k = snprintf(h, sizeof(h), "V3S2|%s|%lu\n", status, (unsigned long)n);
    if (k > 0 && sendall(fd, h, (size_t)k) && n)
        (void)sendall(fd, v, n);
}
static void message(int fd, const char *st, const char *text) {
    respond(fd, st, text, strlen(text));
}
static bool mkdir_host(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0)
        return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    return mkdir(path, 0700) == 0;
}
/* Work tree is private (0700); reject all symlinks at every traversed component. */
static bool upload_path(Job *j, const char *rel, char out[PATH_CAP], bool parents) {
    if (!relvalid(rel) || !fmtpath(out, PATH_CAP, j->dir, "work"))
        return false;
    char copy[REL_MAX];
    strcopy(copy, sizeof(copy), rel);
    char *save = NULL, *part = strtok_r(copy, "/", &save);
    while (part) {
        char next[PATH_CAP];
        if (!fmtpath(next, sizeof(next), out, part))
            return false;
        strcopy(out, PATH_CAP, next);
        part = strtok_r(NULL, "/", &save);
        struct stat st;
        if (lstat(out, &st) == 0) {
            if (S_ISLNK(st.st_mode) || (part && !S_ISDIR(st.st_mode)) ||
                (!part && !S_ISREG(st.st_mode)))
                return false;
        } else if (errno != ENOENT || (part && (!parents || !mkdir_host(out))))
            return false;
    }
    return true;
}
static bool safeid(const char *id) {
    if (strlen(id) != 8)
        return false;
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)id[i]) || isupper((unsigned char)id[i]))
            return false;
    return true;
}
static Job *job_find(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(jobs); i++)
        if (!strcmp(jobs[i].id, id))
            return &jobs[i];
    return NULL;
}
static void job_tick(void) {
    for (size_t i = 0; i < ARRAY_LEN(jobs); i++) {
        Job *j = &jobs[i];
        if (j->pid <= 0)
            continue;
        int st;
        pid_t r = waitpid(j->pid, &st, WNOHANG);
        if (r == 0 && time(NULL) - j->started > 180) {
            kill(-j->pid, SIGKILL);
            while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR) {
            }
            j->result = 124;
            j->pid = 0;
            j->finished = true;
        } else if (r > 0 || (r < 0 && errno != EINTR)) {
            j->result = r < 0 ? 128 : WIFEXITED(st) ? WEXITSTATUS(st) : 128;
            j->pid = 0;
            j->finished = true;
        }
    }
}
static bool check_binary(Job *j) {
    char p[PATH_CAP];
    if (!fmtpath(p, sizeof(p), j->dir, "app.bin"))
        return false;
    size_t n;
    unsigned char *data = read_bytes(p, &n, BIN_MAX);
    if (!data)
        return false;
    bool ok = package_valid(data, n, j->c);
    if (ok) {
        j->binsize = n;
        j->binhash = hashdata(data, n);
    }
    free(data);
    return ok;
}
static bool start_job(Job *j) {
    for (size_t i = 0; i < ARRAY_LEN(jobs); i++)
        if (jobs[i].pid > 0)
            return false;
    char log[PATH_CAP], out[PATH_CAP], src[PATH_CAP], work[PATH_CAP], cache[PATH_CAP],
        script[PATH_CAP], icon[PATH_CAP];
    if (!fmtpath(log, sizeof(log), j->dir, "build.log") ||
        !fmtpath(out, sizeof(out), j->dir, "app.bin") || !upload_path(j, j->entry, src, false) ||
        !fmtpath(work, sizeof(work), j->dir, "work") ||
        !fmtpath(cache, sizeof(cache), j->dir, "cache") ||
        !fmtpath(script, sizeof(script), root, "sdk/aurcc.py") ||
        !fmtpath(icon, sizeof(icon), work, "app.icon"))
        return false;
    int fd = open(log, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    unlink(out);
    pid_t p = fork();
    if (p < 0) {
        close(fd);
        return false;
    }
    if (!p) {
        setpgid(0, 0);
        if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(126);
        close(fd);
        char cwd[PATH_CAP];
        if (!fmtpath(cwd, sizeof(cwd), root, "auric-lang") || chdir(cwd))
            _exit(126);
        setenv("PYTHONUNBUFFERED", "1", 1);
        setenv("PYTHONIOENCODING", "utf-8", 1);
        setenv("LC_ALL", "C", 1);
        if (j->c) {
            if (access(icon, R_OK) == 0)
                execlp("python3", "python3", script, "build", work, "-I", work, "-o", out,
                       "--build-dir", cache, "--icon", icon, (char *)NULL);
            else
                execlp("python3", "python3", script, "build", work, "-I", work, "-o", out,
                       "--build-dir", cache, (char *)NULL);
        } else {
            if (access(icon, R_OK) == 0)
                execlp("python3", "python3", "-m", "compiler.aurc", "build", src, "-o", out,
                       "--build-dir", cache, "--icon", icon, (char *)NULL);
            else
                execlp("python3", "python3", "-m", "compiler.aurc", "build", src, "-o", out,
                       "--build-dir", cache, (char *)NULL);
        }
        perror("python3");
        _exit(127);
    }
    close(fd);
    (void)setpgid(p, p);
    j->pid = p;
    j->started = time(NULL);
    j->locked = true;
    return true;
}
static void chunk_file(int fd, const char *path, size_t offset) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        message(fd, "ERR", "File unavailable");
        return;
    }
    unsigned char b[4096];
    if (offset > BIN_MAX || fseek(f, (long)offset, SEEK_SET)) {
        fclose(f);
        message(fd, "ERR", "Offset invalid");
        return;
    }
    size_t n = fread(b, 1, sizeof(b), f);
    bool ok = !ferror(f);
    fclose(f);
    if (ok)
        respond(fd, "OK", b, n);
    else
        message(fd, "ERR", "Read failed");
}
static void client(int fd) {
    char h[256];
    size_t hn = 0;
    while (hn < sizeof(h) - 1) {
        if (!recvall(fd, h + hn, 1))
            return;
        if (h[hn++] == '\n')
            break;
    }
    if (!hn || h[hn - 1] != '\n') {
        message(fd, "ERR", "Header too long");
        return;
    }
    h[hn - 1] = 0;
    char proto[8], token[96], cmd[16], id[9];
    unsigned long off, total, n;
    int used = 0;
    if (sscanf(h, "%7[^|]|%95[^|]|%15[^|]|%8[^|]|%lu|%lu|%lu%n", proto, token, cmd, id, &off,
               &total, &n, &used) != 7 ||
        h[used] || strcmp(proto, "V3S2") || n > WIRE_CHUNK || off > BIN_MAX || total > BIN_MAX) {
        message(fd, "ERR", "Malformed request");
        return;
    }
    unsigned diff = (unsigned)(strlen(token) ^ strlen(secret));
    for (size_t i = 0; i < strlen(secret); i++)
        diff |= (unsigned char)secret[i] ^ (i < strlen(token) ? (unsigned char)token[i] : 0);
    if (diff) {
        message(fd, "ERR", "Authentication failed");
        return;
    }
    unsigned char body[WIRE_CHUNK + 1];
    if (!recvall(fd, body, n))
        return;
    body[n] = 0;
    if (!strcmp(cmd, "PING")) {
        message(fd, "OK", "VSCode3DS " VERSION " | Auric+C | protocol V3S2");
        return;
    }
    if (!safeid(id)) {
        message(fd, "ERR", "Session invalid");
        return;
    }
    Job *j = job_find(id);
    job_tick();
    if (!strcmp(cmd, "RESET")) {
        if (j) {
            char manifest[REL_MAX + 4];
            snprintf(manifest, sizeof(manifest), "%c|%s", j->c ? 'C' : 'A', j->entry);
            if (!j->locked && strlen(manifest) == n && !memcmp(manifest, body, n)) {
                message(fd, "OK", "Existing upload session");
                return;
            }
            message(fd, "ERR", "Session already exists; use a new id");
            return;
        }
        char lang = (char)body[0];
        char entry[REL_MAX];
        strcopy(entry, sizeof(entry), n > 2 ? (char *)body + 2 : "");
        if (n < 3 || body[1] != '|' || strlen(entry) != n - 2 || !relvalid(entry) ||
            (lang != 'C' && lang != 'A') ||
            !(lang == 'C' ? suffix(entry, ".c") : suffix(entry, ".aur"))) {
            message(fd, "ERR", "Invalid entry/language");
            return;
        }
        for (size_t i = 0; i < ARRAY_LEN(jobs); i++)
            if (!jobs[i].id[0] || jobs[i].finished) {
                j = &jobs[i];
                break;
            }
        if (!j) {
            message(fd, "ERR", "No session slots");
            return;
        }
        memset(j, 0, sizeof(*j));
        strcopy(j->id, sizeof(j->id), id);
        strcopy(j->entry, sizeof(j->entry), entry);
        j->c = lang == 'C';
        char base[PATH_CAP], work[PATH_CAP];
        if (!fmtpath(base, sizeof(base), root, ".vs3ds") || !mkdir_host(base) ||
            !fmtpath(j->dir, sizeof(j->dir), base, id) || mkdir(j->dir, 0700) ||
            !fmtpath(work, sizeof(work), j->dir, "work") || !mkdir_host(work)) {
            memset(j, 0, sizeof(*j));
            message(fd, "ERR", "Cannot create private session");
            return;
        }
        message(fd, "OK", "Session ready");
        return;
    }
    if (!j) {
        message(fd, "ERR", "Unknown session");
        return;
    }
    if (!strcmp(cmd, "PUT") || !strcmp(cmd, "VERIFY")) {
        if (j->locked) {
            message(fd, "ERR", "Build snapshot locked");
            return;
        }
        unsigned char *nl = memchr(body, '\n', n);
        if (!nl || memchr(body, 0, (size_t)(nl - body))) {
            message(fd, "ERR", "Missing path");
            return;
        }
        *nl = 0;
        char *rel = (char *)body;
        size_t bytes = n - (size_t)(nl - body) - 1;
        char path[PATH_CAP];
        if (!upload_path(j, rel, path, true) || total > TEXT_MAX || off > total ||
            (!strcmp(cmd, "PUT") && bytes > total - off)) {
            message(fd, "ERR", "Invalid path/range");
            return;
        }
        Upload *u = NULL;
        for (int i = 0; i < j->count; i++)
            if (!strcmp(j->files[i].path, rel))
                u = &j->files[i];
        if (!u) {
            if (strcmp(cmd, "PUT") || off || j->count == FILE_MAX ||
                j->bytes + total > PROJECT_MAX) {
                message(fd, "ERR", "Upload limit or sequence");
                return;
            }
            u = &j->files[j->count++];
            strcopy(u->path, sizeof(u->path), rel);
            u->total = total;
            u->hash = 2166136261u;
            j->bytes += total;
        }
        if (u->total != total) {
            message(fd, "ERR", "Size changed");
            return;
        }
        if (!strcmp(cmd, "VERIFY")) {
            char hex[9];
            snprintf(hex, sizeof(hex), "%08x", u->hash);
            if (bytes != 8 || u->received != u->total || memcmp(nl + 1, hex, 8)) {
                message(fd, "ERR", "Checksum/length mismatch");
                return;
            }
            u->verified = true;
            message(fd, "OK", "Verified");
            return;
        }
        if (off < u->received && off + bytes <= u->received) {
            unsigned char old[WIRE_CHUNK];
            FILE *f = fopen(path, "rb");
            bool same = f && fseek(f, (long)off, SEEK_SET) == 0 &&
                        fread(old, 1, bytes, f) == bytes && !memcmp(old, nl + 1, bytes);
            if (f)
                fclose(f);
            if (same) {
                message(fd, "OK", "Duplicate chunk already stored");
                return;
            }
        }
        if (u->verified || off != u->received) {
            message(fd, "ERR", "Upload offset mismatch");
            return;
        }
        int f = open(path, O_CREAT | O_WRONLY | O_NOFOLLOW | (off ? 0 : O_TRUNC), 0600);
        if (f < 0) {
            message(fd, "ERR", "Open failed");
            return;
        }
        bool ok = lseek(f, (off_t)off, SEEK_SET) >= 0;
        size_t w = 0;
        while (ok && w < bytes) {
            ssize_t z = write(f, nl + 1 + w, bytes - w);
            if (z < 0 && errno == EINTR)
                continue;
            if (z <= 0) {
                ok = false;
                break;
            }
            w += (size_t)z;
        }
        if (close(f))
            ok = false;
        if (!ok) {
            message(fd, "ERR", "Write failed; restart session");
            return;
        }
        u->received += bytes;
        u->hash = hashstep(u->hash, nl + 1, bytes);
        message(fd, "OK", "Stored");
        return;
    }
    if (!strcmp(cmd, "BUILD")) {
        if (j->locked && (j->pid > 0 || j->finished)) {
            message(fd, "BUILDING", "Existing build; request STATUS");
            return;
        }
        bool entry = false, valid = j->count > 0;
        for (int i = 0; i < j->count; i++) {
            if (!j->files[i].verified)
                valid = false;
            if (!strcmp(j->entry, j->files[i].path))
                entry = true;
        }
        if (j->locked || !entry || !valid || !start_job(j)) {
            message(fd, "ERR", "Build busy or unverified project");
            return;
        }
        message(fd, "BUILDING", "Started");
        return;
    }
    if (!strcmp(cmd, "CANCEL")) {
        if (j->pid > 0) {
            kill(-j->pid, SIGKILL);
            int st;
            while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR) {
            }
            j->pid = 0;
        }
        j->locked = true;
        j->finished = true;
        j->result = 130;
        message(fd, "OK", "Cancelled");
        return;
    }
    if (!strcmp(cmd, "STATUS")) {
        if (j->pid > 0) {
            message(fd, "BUILDING", "Compiling");
            return;
        }
        if (!j->finished) {
            message(fd, "ERR", "Build not started");
            return;
        }
        if (j->result || !check_binary(j)) {
            char s[64];
            snprintf(s, sizeof(s), "Compiler exit %d; read build log", j->result);
            message(fd, "FAILED", s);
            return;
        }
        char s[80];
        snprintf(s, sizeof(s), "%lu|%08x|%c", (unsigned long)j->binsize, j->binhash,
                 j->c ? 'C' : 'A');
        message(fd, "OK", s);
        return;
    }
    if (!strcmp(cmd, "LOG")) {
        char path[PATH_CAP];
        if (fmtpath(path, sizeof(path), j->dir, "build.log"))
            chunk_file(fd, path, off);
        else
            message(fd, "ERR", "Path too long");
        return;
    }
    if (!strcmp(cmd, "FETCH")) {
        if (!j->finished || j->result || !j->binsize || off > j->binsize) {
            message(fd, "ERR", "No verified successful build");
            return;
        }
        char path[PATH_CAP];
        if (fmtpath(path, sizeof(path), j->dir, "app.bin"))
            chunk_file(fd, path, off);
        else
            message(fd, "ERR", "Path too long");
        return;
    }
    message(fd, "ERR", "Unknown command");
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--root"))
            strcopy(root, sizeof(root), argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--token")) {
            if (strlen(argv[i + 1]) >= sizeof(secret)) {
                fprintf(stderr, "Token too long (maximum 95 characters).\n");
                return 2;
            }
            strcopy(secret, sizeof(secret), argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--bind"))
            strcopy(bindip, sizeof(bindip), argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--port")) {
            char *e;
            long p = strtol(argv[++i], &e, 10);
            if (*e || p < 1024 || p > 65535)
                return 2;
            port = (int)p;
        } else {
            fprintf(stderr,
                    "Usage: %s --root AuroraOS --token SECRET16+ [--bind LAN_IP] [--port 45321]\n",
                    argv[0]);
            return 2;
        }
    }
    if (strlen(secret) < 16 || strpbrk(secret, "|\r\n") || !root[0]) {
        fprintf(stderr, "Root and token (16..95 characters) required.\n");
        return 2;
    }
    char *resolved = realpath(root, NULL), p[PATH_CAP];
    if (!resolved) {
        perror("root");
        return 2;
    }
    if (strlen(resolved) >= sizeof(root)) {
        free(resolved);
        fprintf(stderr, "Repository path too long.\n");
        return 2;
    }
    strcopy(root, sizeof(root), resolved);
    free(resolved);
    if (!fmtpath(p, sizeof(p), root, "sdk/aurcc.py") || access(p, R_OK) ||
        !fmtpath(p, sizeof(p), root, "auric-lang/compiler/aurc.py") || access(p, R_OK)) {
        fprintf(stderr, "--root must be the AuroraOS repository root.\n");
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    if (s < 0)
        return 1;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bindip, &a.sin_addr) != 1 || bind(s, (struct sockaddr *)&a, sizeof(a)) ||
        listen(s, 8)) {
        perror("listen");
        close(s);
        return 1;
    }
    fprintf(stderr, "VSCode3DS %s on %s:%d. Plain TCP: trusted LAN only. Ctrl-C stops.\n", VERSION,
            bindip, port);
    while (running) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        struct timeval t = {1, 0};
        int ready = select(s + 1, &r, NULL, NULL, &t);
        job_tick();
        if (ready <= 0)
            continue;
        int fd = accept(s, NULL, NULL);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        struct timeval timeout = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        client(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    for (size_t i = 0; i < ARRAY_LEN(jobs); i++)
        if (jobs[i].pid > 0) {
            kill(-jobs[i].pid, SIGKILL);
            while (waitpid(jobs[i].pid, NULL, 0) < 0 && errno == EINTR) {
            }
        }
    close(s);
    return 0;
}
#elif defined(VS3DS_TEST)
#include <assert.h>
int main(void) {
    Document d = {0};
    assert(doc_set(&d, "a b\na", 5));
    assert(replace_all(&d, "a", "alphabet", true, false, 1));
    assert(!strcmp(d.text, "alphabet b\nalphabet"));
    assert(undo(&d, false));
    assert(!strcmp(d.text, "a b\na"));
    assert(!doc_dirty(&d));
    assert(undo(&d, true));
    assert(doc_dirty(&d));
    assert(index_doc(&d) && d.nlines == 2);
    doc_free(&d);
    assert(doc_set(&d, "", 0));
    assert(replace(&d, 0, 0, "a", 1, 100, 1));
    assert(replace(&d, 1, 1, "b", 1, 200, 1));
    assert(d.hcount == 1);
    assert(undo(&d, false) && d.len == 0);
    assert(undo(&d, true) && !strcmp(d.text, "ab"));
    doc_free(&d);
    assert(doc_set(&d, "x\xc3\xa8\tZ\nfoo", 9));
    assert(nextpos(&d, 1) == 3 && prevpos(&d, 3) == 1);
    assert(index_doc(&d));
    assert(column(&d, 5, 4) == 5);
    assert(find_text(&d, "FOO", 0, false, false, true) == 6);
    doc_free(&d);
    assert(!utf8valid((const unsigned char *)"\xc0\x80", 2));
    assert(!relvalid("../x.c"));
    assert(!relvalid("a/../x.c"));
    assert(!relvalid("x|y"));
    assert(relvalid("src/main.c"));
    assert(history_bytes == 0);
    unsigned char pkg[40] = {0};
    memcpy(pkg, "AURC", 4);
    pkg[4] = 36;
    pkg[8] = 4;
    pkg[15] = 0x22;
    pkg[19] = 0x22;
    assert(package_valid(pkg, 40, true));
    pkg[16] = 1;
    assert(!package_valid(pkg, 40, true));
    puts("VSCode3DS model tests passed");
    return 0;
}
#else
/* -------------------- AuroraOS handheld application -------------------- */
#include <aurora_app.h>
typedef enum {
    P_EDIT,
    P_MENU,
    P_FILES,
    P_LOG,
    P_ERRORS,
    P_HELP,
    P_COMPLETE,
    P_RECENT,
    P_BUILD
} Panel;
typedef struct {
    char host[64], token[96], project[240], entry[REL_MAX], output[48];
    int port, tab, scale, theme, wrap, spaces, pairs, sensitive, whole, overwrite;
} Settings;
typedef struct {
    char file[REL_MAX], message[192];
    unsigned row, col;
    bool warning;
} Diagnostic;
typedef struct {
    char rel[REL_MAX];
    size_t size;
} ProjectFile;
typedef enum {
    B_IDLE,
    B_CONNECT,
    B_RESET,
    B_FILE,
    B_PUT,
    B_VERIFY,
    B_START,
    B_STATUS,
    B_LOG,
    B_FETCH,
    B_CANCEL
} BuildState;
typedef struct {
    BuildState state;
    char id[9];
    ProjectFile files[FILE_MAX];
    int count, index, retries;
    size_t total, offset, got, binsize;
    uint32_t binhash, hash, next;
    unsigned char *data;
    char status[40], error[160], temp[280], dest[280];
    FILE *out;
    bool c, success, logtruncated, paused, transport_error;
} Build;
static Document docs[DOC_MAX];
static int ndoc, active;
static Settings cfg;
static Build build;
static Panel panel = P_EDIT;
static int menu_scroll, file_selected, help_scroll, log_scroll, error_selected, complete_selected;
static char browser[REL_MAX], statusline[160] = "Ready", input[256], prompt_label[80], query[128],
                              replacement[128], clipboard[TEXT_MAX + 1];
static char recent[12][REL_MAX];
static int nrecent;
static char logtext[65537];
static size_t loglen;
static Diagnostic diagnostics[128];
static int ndiag;
static char entries[256][REL_MAX];
static bool entrydir[256];
static int nentries;
static size_t browser_page;
static bool browser_more;
static char completions[48][80];
static int ncomplete;
static size_t complete_start;
static int prompt_action;
static bool redraw = true, shift, caps, keyboard = true, code_bottom, manual_scroll;
static int keypage;
static uint32_t repeat_at, repeat_keys, last_checkpoint, blink_at;
static bool blink = true;
static size_t last_tap = NONE;
static uint32_t tap_at;
static unsigned char replybuf[REPLY_MAX];
static Document *doc(void) { return ndoc ? &docs[active] : NULL; }
static bool busy(void) { return build.state != B_IDLE; }
static uint32_t now(void) { return sys_millis(); }
static void status(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(statusline, sizeof(statusline), fmt, a);
    va_end(a);
    redraw = true;
}
static uint32_t bg(void) { return cfg.theme ? RGB(239, 242, 248) : RGB(16, 20, 29); }
static uint32_t fg(void) { return cfg.theme ? RGB(22, 30, 45) : RGB(225, 232, 242); }
static uint32_t box(void) { return cfg.theme ? RGB(211, 222, 236) : RGB(34, 44, 61); }
static uint32_t accent(void) { return cfg.theme ? RGB(0, 95, 86) : RGB(100, 232, 200); }
static uint32_t muted(void) { return cfg.theme ? RGB(75, 89, 110) : RGB(143, 158, 182); }
static void text(Screen s, int x, int y, uint32_t color, const char *fmt, ...) {
    char b[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    gfx_text(s, x, y, color, "%s", b);
}
static bool project_path(char *out, size_t cap, const char *rel) {
    return relvalid(rel) && fmtpath(out, cap, cfg.project, rel) && strlen(out) < 256;
}
static bool make_parents(const char *path) {
    char p[280];
    strcopy(p, sizeof(p), path);
    char *s = strrchr(p, '/');
    if (!s)
        return true;
    *s = 0;
    return fs_mkdir(p);
}
static void remember(const char *name) {
    int at = -1;
    for (int i = 0; i < nrecent; i++)
        if (!strcmp(recent[i], name))
            at = i;
    if (at < 0) {
        if (nrecent < 12)
            nrecent++;
        at = nrecent - 1;
    }
    for (int i = at; i > 0; i--)
        strcopy(recent[i], sizeof(recent[i]), recent[i - 1]);
    strcopy(recent[0], sizeof(recent[0]), name);
}
static void recent_save(void) {
    char path[280], data[2400];
    if (!fmtpath(path, sizeof(path), app_dir(), "recent.txt"))
        return;
    size_t w = (size_t)snprintf(data, sizeof(data), "%s\n", cfg.project);
    for (int i = 0; i < nrecent; i++) {
        int n = snprintf(data + w, sizeof(data) - w, "%s\n", recent[i]);
        if (n < 0 || (size_t)n >= sizeof(data) - w)
            return;
        w += (size_t)n;
    }
    (void)atomic_bytes(path, data, w);
}
static void recent_load(void) {
    char path[280], line[256];
    if (!fmtpath(path, sizeof(path), app_dir(), "recent.txt"))
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, cfg.project))
            while (nrecent < 12 && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (relvalid(line))
                    strcopy(recent[nrecent++], sizeof(recent[0]), line);
            }
    }
    fclose(f);
}
static void config_save(void) {
    char p[280], s[1200];
    if (!fmtpath(p, sizeof(p), app_dir(), "settings.txt"))
        return;
    int n = snprintf(
        s, sizeof(s),
        "host=%s\ntoken=%s\nproject=%s\nentry=%s\noutput=%s\nport=%d\ntab=%d\nscale=%d\ntheme=%"
        "d\nwrap=%d\nspaces=%d\npairs=%d\nsensitive=%d\nwhole=%d\noverwrite=%d\n",
        cfg.host, cfg.token, cfg.project, cfg.entry, cfg.output, cfg.port, cfg.tab, cfg.scale,
        cfg.theme, cfg.wrap, cfg.spaces, cfg.pairs, cfg.sensitive, cfg.whole, cfg.overwrite);
    if (n < 0 || (size_t)n >= sizeof(s) || !atomic_bytes(p, s, (size_t)n))
        status("Settings could not be saved");
}
static void config_load(void) {
    memset(&cfg, 0, sizeof(cfg));
    strcopy(cfg.host, sizeof(cfg.host), "192.168.1.2");
    cfg.port = 45321;
    cfg.tab = 4;
    cfg.scale = 1;
    cfg.spaces = 1;
    cfg.pairs = 1;
    cfg.sensitive = 1;
    cfg.overwrite = 1;
    strcopy(cfg.entry, sizeof(cfg.entry), "main.aur");
    strcopy(cfg.output, sizeof(cfg.output), "MyApp");
    snprintf(cfg.project, sizeof(cfg.project), "%s/projects/Default", app_dir());
    char p[280], line[400];
    fmtpath(p, sizeof(p), app_dir(), "settings.txt");
    FILE *f = fopen(p, "rb");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        if (!strcmp(line, "host"))
            strcopy(cfg.host, sizeof(cfg.host), eq);
        else if (!strcmp(line, "token"))
            strcopy(cfg.token, sizeof(cfg.token), eq);
        else if (!strcmp(line, "project") && eq[0] == '/' && strlen(eq) < 220)
            strcopy(cfg.project, sizeof(cfg.project), eq);
        else if (!strcmp(line, "entry") && relvalid(eq))
            strcopy(cfg.entry, sizeof(cfg.entry), eq);
        else if (!strcmp(line, "output") && relvalid(eq) && !strchr(eq, '/'))
            strcopy(cfg.output, sizeof(cfg.output), eq);
        else {
            int v = atoi(eq);
            if (!strcmp(line, "port") && v >= 1024 && v <= 65535)
                cfg.port = v;
            else if (!strcmp(line, "tab") && (v == 2 || v == 4 || v == 8))
                cfg.tab = v;
            else if (!strcmp(line, "scale"))
                cfg.scale = v == 2 ? 2 : 1;
            else if (!strcmp(line, "theme"))
                cfg.theme = !!v;
            else if (!strcmp(line, "wrap"))
                cfg.wrap = !!v;
            else if (!strcmp(line, "spaces"))
                cfg.spaces = !!v;
            else if (!strcmp(line, "pairs"))
                cfg.pairs = !!v;
            else if (!strcmp(line, "sensitive"))
                cfg.sensitive = !!v;
            else if (!strcmp(line, "whole"))
                cfg.whole = !!v;
            else if (!strcmp(line, "overwrite"))
                cfg.overwrite = !!v;
        }
    }
    fclose(f);
}
static bool normalize(Document *d, const unsigned char *s, size_t n) {
    if (!utf8valid(s, n))
        return false;
    char *p = malloc(n + 1);
    if (!p)
        return false;
    size_t w = 0;
    bool crlf = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') {
                i++;
                crlf = true;
            }
            p[w++] = '\n';
        } else
            p[w++] = (char)s[i];
    }
    bool ok = doc_set(d, p, w);
    free(p);
    d->crlf = crlf;
    return ok;
}
static bool open_document(const char *rel) {
    if (!relvalid(rel)) {
        status("Invalid relative file name");
        return false;
    }
    for (int i = 0; i < ndoc; i++)
        if (!strcmp(docs[i].name, rel)) {
            active = i;
            panel = P_EDIT;
            redraw = true;
            return true;
        }
    if (ndoc == DOC_MAX) {
        status("Six documents open: close one first");
        return false;
    }
    char p[280];
    if (!project_path(p, sizeof(p), rel)) {
        status("SDK path limit: 255 bytes");
        return false;
    }
    size_t n;
    unsigned char *b = read_bytes(p, &n, TEXT_MAX);
    if (!b) {
        status("Open failed or file exceeds 512 KiB");
        return false;
    }
    Document d = {0};
    bool ok = normalize(&d, b, n);
    free(b);
    if (!ok) {
        doc_free(&d);
        status("Requires valid UTF-8 text; NUL/binary rejected");
        return false;
    }
    strcopy(d.name, sizeof(d.name), rel);
    docs[ndoc] = d;
    active = ndoc++;
    remember(rel);
    panel = P_EDIT;
    redraw = true;
    return true;
}
static bool save_doc(Document *d) {
    if (busy()) {
        status("Build uses saved snapshot; save after build");
        return false;
    }
    char p[280];
    if (!project_path(p, sizeof(p), d->name) || !make_parents(p)) {
        status("Cannot create parent folder / path too long");
        return false;
    }
    char *bytes = d->text;
    size_t n = d->len;
    if (d->crlf) {
        size_t count = 0;
        for (size_t i = 0; i < n; i++)
            if (bytes[i] == '\n')
                count++;
        bytes = malloc(n + count + 1);
        if (!bytes) {
            status("Memory exhausted");
            return false;
        }
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            if (d->text[i] == '\n')
                bytes[w++] = '\r';
            bytes[w++] = d->text[i];
        }
        n = w;
    }
    if (n > TEXT_MAX) {
        if (bytes != d->text)
            free(bytes);
        status("CRLF output exceeds 512 KiB; choose LF or split file");
        return false;
    }
    bool ok = atomic_bytes(p, bytes, n);
    if (bytes != d->text)
        free(bytes);
    if (ok) {
        d->saved_hash = hashdata(d->text, d->len);
        d->recovered = false;
        d->boundary = true;
        remember(d->name);
        status("Saved %s", d->name);
    } else
        status("Save failed. Check .tmp / .bak; recovery retained");
    return ok;
}
static bool save_all(void) {
    for (int i = 0; i < ndoc; i++)
        if (doc_dirty(&docs[i]) && !save_doc(&docs[i]))
            return false;
    return true;
}
/* A single atomic session file checkpoints document names, bytes and cursors.
 * Kept independently from user files; it never auto-overwrites original sources. */
static void checkpoint(void) {
    size_t cap = 1024;
    for (int i = 0; i < ndoc; i++)
        cap += docs[i].len + 400;
    char *out = malloc(cap);
    if (!out)
        return;
    int k = snprintf(out, cap, "VS3DS_SESSION_1\n%s\n%d\n%d\n", cfg.project, ndoc, active);
    if (k < 0) {
        free(out);
        return;
    }
    size_t w = (size_t)k;
    for (int i = 0; i < ndoc; i++) {
        Document *d = &docs[i];
        k = snprintf(out + w, cap - w, "%s\n%lu %lu %u %d %d\n", d->name, (unsigned long)d->len,
                     (unsigned long)d->cursor, ((unsigned)d->saved_hash), d->crlf, d->recovered);
        if (k < 0 || (size_t)k >= cap - w) {
            free(out);
            return;
        }
        w += (size_t)k;
        memcpy(out + w, d->text, d->len);
        w += d->len;
        out[w++] = '\n';
    }
    uint32_t integrity = hashdata(out, w);
    k = snprintf(out + w, cap - w, "CHECKSUM %08lx\n", (unsigned long)integrity);
    if (k != 18) {
        free(out);
        return;
    }
    w += (size_t)k;
    char p[280];
    if (fmtpath(p, sizeof(p), app_dir(), "session.dat"))
        (void)atomic_bytes(p, out, w);
    recent_save();
    free(out);
    last_checkpoint = now();
}
static bool line_read(FILE *f, char *s, size_t cap) {
    if (!fgets(s, (int)cap, f))
        return false;
    size_t n = strcspn(s, "\r\n");
    if (!s[n] && !feof(f))
        return false;
    s[n] = 0;
    return true;
}
static bool restore_session_path(const char *p) {
    size_t size = 0;
    unsigned char *raw = read_bytes(p, &size, DOC_MAX * (TEXT_MAX + 400u) + 1024u);
    if (!raw)
        return false;
    bool integrity = size >= 18 && !memcmp(raw + size - 18, "CHECKSUM ", 9);
    if (integrity) {
        char *end;
        unsigned long expected = strtoul((char *)raw + size - 9, &end, 16);
        integrity =
            end == (char *)raw + size - 1 && *end == '\n' && expected == hashdata(raw, size - 18);
    }
    free(raw);
    if (!integrity)
        return false;
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    char line[400];
    int count = 0, selected = 0;
    bool ok = line_read(f, line, sizeof(line)) && !strcmp(line, "VS3DS_SESSION_1") &&
              line_read(f, line, sizeof(line)) && !strcmp(line, cfg.project);
    if (ok) {
        ok = line_read(f, line, sizeof(line));
        count = atoi(line);
        ok = ok && count >= 0 && count <= DOC_MAX && line_read(f, line, sizeof(line));
        selected = atoi(line);
    }
    Document temp[DOC_MAX];
    memset(temp, 0, sizeof(temp));
    for (int i = 0; ok && i < count; i++) {
        char name[REL_MAX];
        unsigned long n = 0, cur = 0;
        unsigned saved = 0;
        int crlf = 0, rec = 0;
        ok = line_read(f, name, sizeof(name)) && relvalid(name) &&
             line_read(f, line, sizeof(line)) &&
             sscanf(line, "%lu %lu %u %d %d", &n, &cur, &saved, &crlf, &rec) == 5 &&
             n <= TEXT_MAX && cur <= n;
        if (!ok)
            break;
        char *bytes = malloc(n + 1);
        if (!bytes) {
            ok = false;
            break;
        }
        ok = fread(bytes, 1, n, f) == n && fgetc(f) == '\n';
        if (ok)
            ok = doc_set(&temp[i], bytes, n);
        free(bytes);
        if (ok) {
            strcopy(temp[i].name, sizeof(temp[i].name), name);
            temp[i].cursor = cur;
            while (temp[i].cursor && temp[i].cursor < n &&
                   ((unsigned char)temp[i].text[temp[i].cursor] & 0xc0) == 0x80)
                temp[i].cursor--;
            temp[i].saved_hash = saved;
            temp[i].crlf = !!crlf;
            temp[i].recovered = !!rec;
        }
    }
    fclose(f);
    if (ok) {
        for (int i = 0; i < ndoc; i++)
            doc_free(&docs[i]);
        memcpy(docs, temp, sizeof(temp));
        ndoc = count;
        active = selected >= 0 && selected < count ? selected : 0;
        for (int i = 0; i < ndoc; i++)
            remember(docs[i].name);
        status("Session restored; unsaved buffers recovered");
        return true;
    }
    for (int i = 0; i < DOC_MAX; i++)
        doc_free(&temp[i]);
    return false;
}
static void restore_session(void) {
    char p[280];
    fmtpath(p, sizeof(p), app_dir(), "session.dat");
    if (restore_session_path(p))
        return;
    strncat(p, ".bak", sizeof(p) - strlen(p) - 1);
    (void)restore_session_path(p);
}
static void editor_exit_checkpoint(void) { checkpoint(); }
static void prompt(int action, const char *label, const char *initial) {
    prompt_action = action;
    strcopy(prompt_label, sizeof(prompt_label), label);
    strcopy(input, sizeof(input), initial ? initial : "");
    keyboard = true;
    redraw = true;
}
static void new_document(const char *name, const char *initial) {
    if (ndoc == DOC_MAX) {
        status("Close a document first");
        return;
    }
    if (!relvalid(name)) {
        status("Invalid name");
        return;
    }
    for (int i = 0; i < ndoc; i++)
        if (!strcmp(docs[i].name, name)) {
            status("Already open");
            return;
        }
    char p[280];
    if (!project_path(p, sizeof(p), name) || fs_exists(p)) {
        status("File exists or path too long");
        return;
    }
    Document d = {0};
    if (!doc_set(&d, initial, strlen(initial))) {
        doc_free(&d);
        status("Out of memory");
        return;
    }
    strcopy(d.name, sizeof(d.name), name);
    d.recovered = true;
    docs[ndoc] = d;
    active = ndoc++;
    panel = P_EDIT;
    redraw = true;
}
static const char aur_template[] =
    "fn main() {\n    buffered(true);\n    while true {\n        clear(BLACK);\n        "
    "print(\"Hello from VSCode3DS!\", 8, 8, AURORA);\n        present();\n        if (keys_down() "
    "& KEY_A) != 0 { break; }\n    }\n}\n";
static const char c_template[] =
    "#include <aurora_app.h>\n\nint main(void) {\n    while (app_loop()) {\n        "
    "gfx_clear(SCREEN_TOP, COLOR_BLACK);\n        gfx_text(SCREEN_TOP, 8, 8, COLOR_WHITE, \"Hello "
    "from C!\");\n        if (hid_keys_down() & KEY_START) break;\n    }\n    return 0;\n}\n";

/* -------------------- project/file browser -------------------- */
static void refresh_files(void) {
    nentries = 0;
    browser_more = false;
    char path[280];
    if (browser[0]) {
        if (!project_path(path, sizeof(path), browser))
            return;
    } else
        strcopy(path, sizeof(path), cfg.project);
    FsDir *dir = fs_dir_open(path);
    if (!dir) {
        status("Cannot read project folder");
        return;
    }
    FsEntry e;
    size_t skipped = 0;
    while (fs_dir_read(dir, &e)) {
        if (e.name[0] == '.')
            continue;
        if (skipped++ < browser_page * 256)
            continue;
        if (nentries == 256) {
            browser_more = true;
            break;
        }
        strcopy(entries[nentries], sizeof(entries[0]), e.name);
        entrydir[nentries++] = e.is_dir;
    }
    fs_dir_close(dir);
    for (int i = 1; i < nentries; i++) {
        char name[REL_MAX];
        strcopy(name, sizeof(name), entries[i]);
        bool isdir = entrydir[i];
        int j = i;
        while (j > 0 && ((isdir && !entrydir[j - 1]) ||
                         (isdir == entrydir[j - 1] && strcmp(name, entries[j - 1]) < 0))) {
            strcopy(entries[j], sizeof(entries[j]), entries[j - 1]);
            entrydir[j] = entrydir[j - 1];
            j--;
        }
        strcopy(entries[j], sizeof(entries[j]), name);
        entrydir[j] = isdir;
    }
    file_selected = 0;
    redraw = true;
}
static bool selected_rel(char out[REL_MAX]) {
    if (file_selected < 0 || file_selected >= nentries)
        return false;
    return browser[0] ? fmtpath(out, REL_MAX, browser, entries[file_selected])
                      : (strcopy(out, REL_MAX, entries[file_selected]), true);
}
static void browser_enter(void) {
    char rel[REL_MAX];
    if (!selected_rel(rel))
        return;
    if (entrydir[file_selected]) {
        strcopy(browser, sizeof(browser), rel);
        browser_page = 0;
        refresh_files();
    } else
        (void)open_document(rel);
}
static void browser_up(void) {
    char *s = strrchr(browser, '/');
    if (s)
        *s = 0;
    else
        browser[0] = 0;
    browser_page = 0;
    refresh_files();
}
static bool collect_files(const char *rel, int depth) {
    if (depth > 8) {
        status("Project folder depth exceeds 8");
        return false;
    }
    char path[280];
    if (rel[0]) {
        if (!project_path(path, sizeof(path), rel))
            return false;
    } else
        strcopy(path, sizeof(path), cfg.project);
    FsDir *dir = fs_dir_open(path);
    if (!dir)
        return false;
    FsEntry e;
    bool ok = true;
    while (ok && fs_dir_read(dir, &e)) {
        if (e.name[0] == '.' || !strcmp(e.name, "build") || suffix(e.name, ".bak") ||
            suffix(e.name, ".tmp") || suffix(e.name, ".bin") || suffix(e.name, ".log") ||
            suffix(e.name, ".elf") || suffix(e.name, ".o"))
            continue;
        char child[REL_MAX];
        if (rel[0])
            ok = fmtpath(child, sizeof(child), rel, e.name);
        else {
            strcopy(child, sizeof(child), e.name);
            ok = strlen(e.name) < sizeof(child);
        }
        if (!ok || !relvalid(child)) {
            ok = false;
            break;
        }
        if (e.is_dir)
            ok = collect_files(child, depth + 1);
        else {
            if (build.count == FILE_MAX || e.size > TEXT_MAX ||
                build.total + e.size > PROJECT_MAX) {
                ok = false;
                break;
            }
            ProjectFile *f = &build.files[build.count++];
            strcopy(f->rel, sizeof(f->rel), child);
            f->size = e.size;
            build.total += e.size;
        }
    }
    fs_dir_close(dir);
    return ok;
}
static void parse_diagnostics(void) {
    ndiag = 0;
    const char *p = logtext;
    while (*p && ndiag < (int)ARRAY_LEN(diagnostics)) {
        const char *end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        char line[800];
        size_t n = MIN((size_t)(end - p), sizeof(line) - 1);
        memcpy(line, p, n);
        line[n] = 0;
        for (char *colon = strchr(line, ':'); colon; colon = strchr(colon + 1, ':')) {
            unsigned row = 0, col = 1;
            int used = 0;
            if (sscanf(colon + 1, "%u:%u:%n", &row, &col, &used) < 2 || !used) {
                used = 0;
                if (sscanf(colon + 1, "%u:%n", &row, &used) != 1 || !used)
                    continue;
            }
            char *msg = colon + 1 + used;
            while (*msg == ' ')
                msg++;
            char file[PATH_CAP];
            size_t z = (size_t)(colon - line);
            memcpy(file, line, z);
            file[z] = 0;
            char *relative = strstr(file, "/work/");
            relative = relative ? relative + 6 : file;
            if (!relvalid(relative) || !row)
                continue;
            Diagnostic *d = &diagnostics[ndiag++];
            strcopy(d->file, sizeof(d->file), relative);
            d->row = row;
            d->col = col;
            d->warning = strstr(msg, "warning") != NULL;
            strcopy(d->message, sizeof(d->message), msg);
            break;
        }
        /* Auric: /path/work/main.aur: type error (line N, col M): message */
        char *where = strstr(line, " (line "), *aurfile = strstr(line, ".aur:");
        if (where && aurfile && ndiag < (int)ARRAY_LEN(diagnostics)) {
            unsigned row = 0, col = 1;
            if (sscanf(where, " (line %u, col %u)", &row, &col) >= 1 && row) {
                char file[REL_MAX];
                const char *relative = strstr(line, "/work/");
                relative = relative ? relative + 6 : line;
                size_t fn = (size_t)(aurfile + 4 - relative);
                if (fn < sizeof(file)) {
                    memcpy(file, relative, fn);
                    file[fn] = 0;
                    if (relvalid(file)) {
                        Diagnostic *e = &diagnostics[ndiag++];
                        memset(e, 0, sizeof(*e));
                        strcopy(e->file, sizeof(e->file), file);
                        e->row = row;
                        e->col = col;
                        strcopy(e->message, sizeof(e->message), aurfile + 6);
                    }
                }
            }
        }
        p = *end ? end + 1 : end;
    }
    error_selected = 0;
}
static void goto_diagnostic(void) {
    if (!ndiag)
        return;
    Diagnostic *e = &diagnostics[error_selected];
    if (open_document(e->file)) {
        Document *d = doc();
        if (index_doc(d)) {
            size_t row = MIN((size_t)e->row - 1, d->nlines - 1);
            d->cursor = colpos(d, row, e->col ? e->col - 1 : 0, (unsigned)cfg.tab);
            d->anchor = NONE;
            status("%s", e->message);
        }
    }
}

/* -------------------- frame-driven remote builds -------------------- */
static bool rpc(const char *cmd, size_t offset, size_t total, const void *payload, size_t n,
                size_t *length) {
    char h[2048];
    int k = snprintf(h, sizeof(h), "V3S2|%s|%s|%s|%lu|%lu|%lu\n", cfg.token, cmd, build.id,
                     (unsigned long)offset, (unsigned long)total, (unsigned long)n);
    if (k < 0 || n > WIRE_CHUNK || (size_t)k + n > sizeof(h))
        return false;
    if (n)
        memcpy(h + k, payload, n);
    int got = net_tcp_exchange(cfg.host, cfg.port, h, (size_t)k + n, replybuf, sizeof(replybuf));
    if (got < 0) {
        build.transport_error = true;
        strcopy(build.error, sizeof(build.error), net_error());
        return false;
    }
    unsigned char *nl = memchr(replybuf, '\n', (size_t)got);
    if (!nl || memchr(replybuf, 0, (size_t)(nl - replybuf)))
        return false;
    size_t head = (size_t)(nl - replybuf);
    if (head >= 120)
        return false;
    char line[128];
    memcpy(line, replybuf, head);
    line[head] = 0;
    unsigned long len;
    int used = 0;
    if (sscanf(line, "V3S2|%39[^|]|%lu%n", build.status, &len, &used) != 2 || line[used] ||
        len != (size_t)got - head - 1 || len >= sizeof(replybuf))
        return false;
    memmove(replybuf, nl + 1, len);
    replybuf[len] = 0;
    *length = len;
    if (!strcmp(build.status, "ERR")) {
        strcopy(build.error, sizeof(build.error), (char *)replybuf);
        return false;
    }
    return true;
}
static void network_wait(uint32_t ms) {
    gfx_clear(SCREEN_BOTTOM, bg());
    text(SCREEN_BOTTOM, 8, 30, accent(), "Network request: %lu s", (unsigned long)ms / 1000);
    text(SCREEN_BOTTOM, 8, 52, fg(), "Waiting for Aurora Wi-Fi service");
    text(SCREEN_BOTTOM, 8, 76, muted(), "HOME handled when SDK call returns");
    gfx_present();
}
static void build_end(const char *message) {
    if (build.out) {
        fclose(build.out);
        build.out = NULL;
    }
    free(build.data);
    build.data = NULL;
    build.state = B_IDLE;
    net_on_wait(NULL);
    parse_diagnostics();
    status("%s", message);
    panel = ndiag ? P_ERRORS : P_LOG;
    redraw = true;
    checkpoint();
}
static void build_fail(void) {
    char msg[180];
    snprintf(msg, sizeof(msg), "Build stopped: %.140s",
             build.error[0] ? build.error : "invalid or incomplete host reply");
    build_end(msg);
}
static bool start_build(void) {
    if (busy())
        return false;
    if (strlen(cfg.token) < 16) {
        prompt(10, "Host token (16..95 characters)", cfg.token);
        return false;
    }
    if (!save_all())
        return false;
    checkpoint();
    memset(&build, 0, sizeof(build));
    build.c = suffix(cfg.entry, ".c");
    if (!build.c && !suffix(cfg.entry, ".aur")) {
        status("Entry must be .aur or .c");
        return false;
    }
    if (!collect_files("", 0)) {
        status("Project exceeds 64 files / 4 MiB / depth 8, or invalid path");
        return false;
    }
    bool entry = false;
    for (int i = 0; i < build.count; i++)
        if (!strcmp(cfg.entry, build.files[i].rel))
            entry = true;
    if (!entry) {
        status("Entry file is missing: set Project entry");
        return false;
    }
    uint32_t id = (uint32_t)sys_micros() ^ now() ^ (uint32_t)time(NULL);
    snprintf(build.id, sizeof(build.id), "%08lx", (unsigned long)id);
    build.state = B_CONNECT;
    panel = P_BUILD;
    loglen = 0;
    logtext[0] = 0;
    ndiag = 0;
    net_on_wait(network_wait);
    redraw = true;
    return true;
}
static bool install_target(void) {
    if (!relvalid(cfg.output) || strchr(cfg.output, '/'))
        return false;
    const char *folder = build.c ? "/Aurora/Apps/C" : "/Aurora/Apps";
    if (!fs_mkdir(folder))
        return false;
    char base[280];
    snprintf(base, sizeof(base), "%s/%s.bin", folder, cfg.output);
    if (cfg.overwrite || !fs_exists(base))
        strcopy(build.dest, sizeof(build.dest), base);
    else {
        bool found = false;
        for (int i = 2; i < 1000; i++) {
            snprintf(build.dest, sizeof(build.dest), "%s/%s_%d.bin", folder, cfg.output, i);
            if (!fs_exists(build.dest)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    int n = snprintf(build.temp, sizeof(build.temp), "%s.part", build.dest);
    if (n < 0 || (size_t)n >= sizeof(build.temp))
        return false;
    build.out = fopen(build.temp, "wb");
    return build.out != NULL;
}
static bool finish_install(void) {
    if (!build.out)
        return false;
    bool ok = fflush(build.out) == 0;
    if (fclose(build.out))
        ok = false;
    build.out = NULL;
    if (!ok)
        return false;
    /* Verify downloaded bytes, address bounds and all nine header fields. */
    FILE *f = fopen(build.temp, "rb");
    if (!f)
        return false;
    unsigned char header[40];
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n < 40)
        return false;
    /* package_valid only inspects the header and the supplied complete size. */
    if (!package_valid(header, build.binsize, build.c) || build.hash != build.binhash)
        return false;
    char bak[300];
    snprintf(bak, sizeof(bak), "%s.bak", build.dest);
    if (fs_exists(build.dest)) {
        if (remove(bak) && errno != ENOENT)
            return false;
        if (rename(build.dest, bak))
            return false;
    }
    if (rename(build.temp, build.dest)) {
        if (fs_exists(bak))
            (void)rename(bak, build.dest);
        return false;
    }
    /* Deploy assets/ relative to the app data folder, preserving the tree. */
    const char *base = strrchr(build.dest, '/');
    base = base ? base + 1 : build.dest;
    char name[80];
    strcopy(name, sizeof(name), base);
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    char datafolder[280];
    snprintf(datafolder, sizeof(datafolder), build.c ? "/Aurora/Apps/C/%s" : "/Aurora/Apps/%s",
             name);
    if (!fs_mkdir(datafolder))
        return false;
    for (int i = 0; i < build.count; i++)
        if (!strncmp(build.files[i].rel, "assets/", 7)) {
            char src[280], dst[280];
            size_t sz;
            if (!project_path(src, sizeof(src), build.files[i].rel) ||
                !fmtpath(dst, sizeof(dst), datafolder, build.files[i].rel + 7) ||
                strlen(dst) >= 256 || !make_parents(dst))
                return false;
            unsigned char *bytes = read_bytes(src, &sz, TEXT_MAX);
            if (!bytes)
                return false;
            ok = atomic_bytes(dst, bytes, sz);
            free(bytes);
            if (!ok)
                return false;
        }
    return true;
}
static void build_step(void) {
    if (!busy() || build.paused || (int32_t)(now() - build.next) < 0)
        return;
    size_t n = 0;
    bool ok = true;
    build.error[0] = 0;
    build.transport_error = false;
    switch (build.state) {
    case B_CONNECT:
        if (!net_available() || !net_connect()) {
            strcopy(build.error, sizeof(build.error), net_error());
            ok = false;
        } else
            build.state = B_RESET;
        break;
    case B_RESET: {
        char manifest[REL_MAX + 4];
        int k = snprintf(manifest, sizeof(manifest), "%c|%s", build.c ? 'C' : 'A', cfg.entry);
        ok = rpc("RESET", 0, 0, manifest, (size_t)k, &n) && !strcmp(build.status, "OK");
        if (ok)
            build.state = B_FILE;
        break;
    }
    case B_FILE:
        if (build.index == build.count) {
            build.state = B_START;
            break;
        } else {
            char p[280];
            ProjectFile *f = &build.files[build.index];
            if (!project_path(p, sizeof(p), f->rel)) {
                ok = false;
                break;
            }
            size_t z;
            build.data = read_bytes(p, &z, TEXT_MAX);
            ok = build.data && z == f->size;
            if (ok) {
                build.offset = 0;
                build.hash = hashdata(build.data, z);
                build.state = B_PUT;
            }
            break;
        }
    case B_PUT: {
        ProjectFile *f = &build.files[build.index];
        unsigned char b[WIRE_CHUNK];
        size_t prefix = strlen(f->rel) + 1;
        memcpy(b, f->rel, prefix - 1);
        b[prefix - 1] = '\n';
        size_t chunk = MIN(f->size - build.offset, WIRE_CHUNK - prefix);
        if (chunk)
            memcpy(b + prefix, build.data + build.offset, chunk);
        ok =
            rpc("PUT", build.offset, f->size, b, prefix + chunk, &n) && !strcmp(build.status, "OK");
        if (ok) {
            build.offset += chunk;
            if (build.offset == f->size)
                build.state = B_VERIFY;
        }
        break;
    }
    case B_VERIFY: {
        ProjectFile *f = &build.files[build.index];
        char b[REL_MAX + 12];
        int k = snprintf(b, sizeof(b), "%s\n%08x", f->rel, ((unsigned)build.hash));
        ok = rpc("VERIFY", 0, f->size, b, (size_t)k, &n) && !strcmp(build.status, "OK");
        if (ok) {
            free(build.data);
            build.data = NULL;
            build.index++;
            build.state = B_FILE;
        }
        break;
    }
    case B_START:
        ok = rpc("BUILD", 0, 0, NULL, 0, &n) && !strcmp(build.status, "BUILDING");
        if (ok) {
            build.state = B_STATUS;
            build.next = now() + 700;
        }
        break;
    case B_STATUS:
        ok = rpc("STATUS", 0, 0, NULL, 0, &n);
        if (!ok)
            break;
        if (!strcmp(build.status, "BUILDING")) {
            build.next = now() + 700;
            break;
        }
        build.success = !strcmp(build.status, "OK");
        if (build.success) {
            unsigned long sz;
            unsigned hash;
            char lang;
            int used = 0;
            ok = sscanf((char *)replybuf, "%lu|%x|%c%n", &sz, &hash, &lang, &used) == 3 &&
                 (size_t)used == n && sz >= 40 && sz <= BIN_MAX && lang == (build.c ? 'C' : 'A');
            if (!ok)
                break;
            build.binsize = sz;
            build.binhash = hash;
        }
        build.state = B_LOG;
        build.offset = 0;
        break;
    case B_LOG:
        ok = rpc("LOG", build.offset, 0, NULL, 0, &n);
        if (!ok)
            break;
        {
            size_t add = MIN(n, sizeof(logtext) - 1 - loglen);
            memcpy(logtext + loglen, replybuf, add);
            loglen += add;
            logtext[loglen] = 0;
            build.offset += n;
            if (add < n)
                build.logtruncated = true;
            if (!n || build.offset >= sizeof(logtext) - 1) {
                if (build.logtruncated || n) {
                    const char *mark = "\n[Log capped at 64 KiB; full log on host]\n";
                    size_t m = strlen(mark);
                    memcpy(logtext + sizeof(logtext) - 1 - m, mark, m + 1);
                    loglen = strlen(logtext);
                }
                if (!build.success) {
                    build_end("Compilation failed: select an error or read log");
                    return;
                }
                build.offset = 0;
                build.hash = 2166136261u;
                ok = install_target();
                if (ok)
                    build.state = B_FETCH;
            }
        }
        break;
    case B_FETCH:
        ok = rpc("FETCH", build.offset, 0, NULL, 0, &n);
        if (ok) {
            if (!n || n > build.binsize - build.offset) {
                ok = false;
                break;
            }
            ok = fwrite(replybuf, 1, n, build.out) == n;
            if (ok) {
                build.hash = hashstep(build.hash, replybuf, n);
                build.offset += n;
                if (build.offset == build.binsize) {
                    ok = finish_install();
                    if (ok) {
                        char msg[160];
                        snprintf(msg, sizeof(msg), "Installed %.120s; launch from Home",
                                 build.dest);
                        build_end(msg);
                        return;
                    }
                }
            }
        }
        break;
    case B_CANCEL:
        if (net_connected())
            (void)rpc("CANCEL", 0, 0, NULL, 0, &n);
        build_end("Build cancelled; partial download kept as .part");
        return;
    default:
        break;
    }
    if (!ok) {
        if (build.transport_error) {
            build.retries++;
            if (build.retries < 3) {
                build.next = now() + 1000;
                status("Network retry %d/3", build.retries);
            } else {
                build.paused = true;
                panel = P_BUILD;
                status("Connection lost: A retries, B cancels");
            }
            redraw = true;
            return;
        }
        build_fail();
        return;
    }
    build.retries = 0;
    redraw = true;
}
/* -------------------- editing commands -------------------- */
static void insert_text(const char *s, size_t n, int kind) {
    Document *d = doc();
    if (!d)
        return;
    size_t a, b;
    bounds(d, &a, &b);
    if (!replace(d, a, b, s, n, now(), kind))
        status("Edit exceeds memory or 512 KiB limit");
    redraw = true;
}
static void type_char(char c) {
    Document *d = doc();
    if (!d)
        return;
    if (cfg.pairs && strchr(")]}'\"", c) && d->cursor < d->len && d->text[d->cursor] == c &&
        d->anchor == NONE) {
        d->cursor = nextpos(d, d->cursor);
        d->boundary = true;
        redraw = true;
        return;
    }
    const char *opens = "([{\"'";
    const char *at = strchr(opens, c);
    if (cfg.pairs && at) {
        const char closing[] = ")]}\"'";
        char close = closing[at - opens];
        size_t a, b;
        bounds(d, &a, &b);
        char *s = malloc(b - a + 3);
        if (!s)
            return;
        s[0] = c;
        memcpy(s + 1, d->text + a, b - a);
        s[b - a + 1] = close;
        bool ok = replace(d, a, b, s, b - a + 2, now(), 0);
        free(s);
        if (ok) {
            d->cursor = a + 1;
            if (b > a) {
                d->anchor = a + 1;
                d->cursor = b + 1;
            }
        }
        redraw = true;
        return;
    }
    insert_text(&c, 1, 1);
}
static void erase_text(bool forward) {
    Document *d = doc();
    if (!d)
        return;
    size_t a, b;
    if (!bounds(d, &a, &b)) {
        if (forward)
            b = nextpos(d, b);
        else
            a = prevpos(d, a);
        if (cfg.pairs && a < d->cursor && b < d->len && strchr("([{\"'", d->text[a])) {
            char c = d->text[a], e = d->text[b];
            if ((c == '(' && e == ')') || (c == '[' && e == ']') || (c == '{' && e == '}') ||
                (c == e))
                b = nextpos(d, b);
        }
    }
    if (a != b && !replace(d, a, b, "", 0, now(), 0))
        status("Delete failed");
    redraw = true;
}
static void copy_text(bool cut) {
    Document *d = doc();
    if (!d)
        return;
    size_t a, b;
    if (!bounds(d, &a, &b)) {
        size_t row = row_at(d, d->cursor);
        a = d->lines[row];
        b = d->lines[row + 1];
    }
    size_t n = b - a;
    memcpy(clipboard, d->text + a, n);
    clipboard[n] = 0;
    if (cut && n)
        (void)replace(d, a, b, "", 0, now(), 0);
    status(cut ? "Cut to clipboard" : "Copied to clipboard");
}
static void newline_indent(void) {
    Document *d = doc();
    if (!d)
        return;
    size_t a, b;
    bounds(d, &a, &b);
    size_t row = row_at(d, a), ls = d->lines[row], p = ls;
    while (p < a && (d->text[p] == ' ' || d->text[p] == '\t'))
        p++;
    size_t len = p - ls;
    char *s = malloc(len + 2 * (size_t)cfg.tab + 5);
    if (!s)
        return;
    size_t w = 0;
    s[w++] = '\n';
    memcpy(s + w, d->text + ls, len);
    w += len;
    bool brace = a && d->text[a - 1] == '{';
    if (brace) {
        if (cfg.spaces) {
            memset(s + w, ' ', (size_t)cfg.tab);
            w += (size_t)cfg.tab;
        } else
            s[w++] = '\t';
    }
    size_t cursor = a + w;
    if (brace && b < d->len && d->text[b] == '}') {
        s[w++] = '\n';
        memcpy(s + w, d->text + ls, len);
        w += len;
    }
    (void)replace(d, a, b, s, w, now(), 0);
    d->cursor = cursor;
    free(s);
    redraw = true;
}
static void move_cursor(int dir, bool select, bool word) {
    Document *d = doc();
    if (!d || !index_doc(d))
        return;
    d->boundary = true;
    if (select) {
        if (d->anchor == NONE)
            d->anchor = d->cursor;
    } else if (d->anchor != NONE) {
        size_t a, b;
        if (bounds(d, &a, &b) && (dir == -1 || dir == 1)) {
            d->cursor = dir < 0 ? a : b;
            d->anchor = NONE;
            redraw = true;
            return;
        }
        d->anchor = NONE;
    }
    size_t row = row_at(d, d->cursor);
    if (dir == -1 || dir == 1) {
        size_t p = d->cursor;
        if (word) {
            if (dir < 0) {
                while (p && !wordchar((unsigned char)d->text[prevpos(d, p)]))
                    p = prevpos(d, p);
                while (p && wordchar((unsigned char)d->text[prevpos(d, p)]))
                    p = prevpos(d, p);
            } else {
                while (p < d->len && wordchar((unsigned char)d->text[p]))
                    p = nextpos(d, p);
                while (p < d->len && !wordchar((unsigned char)d->text[p]))
                    p = nextpos(d, p);
            }
        } else
            p = dir < 0 ? prevpos(d, p) : nextpos(d, p);
        d->cursor = p;
        d->goal = NONE;
    } else {
        if (d->goal == NONE)
            d->goal = column(d, d->cursor, (unsigned)cfg.tab);
        int delta = dir == 2 ? 1 : dir == -2 ? -1 : dir > 0 ? 20 : -20;
        size_t dest = delta < 0 ? (row < (size_t)-delta ? 0 : row - (size_t)-delta)
                                : MIN(row + (size_t)delta, d->nlines - 1);
        d->cursor = colpos(d, dest, d->goal, (unsigned)cfg.tab);
    }
    redraw = true;
}
static void edit_lines(int op) {
    Document *d = doc();
    if (!d || !index_doc(d))
        return;
    size_t a, b;
    bounds(d, &a, &b);
    size_t first = row_at(d, a), last = row_at(d, b);
    if (b > a && b == d->lines[last] && last)
        last--;
    a = d->lines[first];
    b = d->lines[last + 1];
    if (op == 3) {
        if (b > a)
            (void)replace(d, a, b, "", 0, now(), 0);
        redraw = true;
        return;
    }
    if (op == 4) {
        size_t n = b - a;
        char *s = malloc(n + 1);
        if (!s)
            return;
        memcpy(s, d->text + a, n);
        bool add = n && s[n - 1] != '\n';
        if (add)
            s[n++] = '\n';
        (void)replace(d, a, a, s, n, now(), 0);
        free(s);
        redraw = true;
        return;
    }
    if (op == 5 || op == 6) {
        size_t x = a, y = b;
        if (op == 5) {
            if (!first)
                return;
            x = d->lines[first - 1];
        } else {
            if (last + 1 >= d->nlines)
                return;
            y = d->lines[last + 2];
        }
        size_t len = y - x;
        char *s = malloc(len + 2);
        if (!s)
            return;
        size_t split = op == 5 ? a : b, part = y - split;
        memcpy(s, d->text + split, part);
        size_t w = part;
        if (w && s[w - 1] != '\n' && split > x)
            s[w++] = '\n';
        memcpy(s + w, d->text + x, split - x);
        w += split - x;
        if (w > len && w && s[w - 1] == '\n')
            w--;
        bool ok = replace(d, x, y, s, w, now(), 0);
        free(s);
        if (ok)
            d->cursor = op == 5 ? x : a + (y - b);
        redraw = true;
        return;
    }
    bool uncomment = op == 2;
    for (size_t r = first; uncomment && r <= last; r++) {
        size_t p = d->lines[r], end = row_end(d, r);
        while (p < end && (d->text[p] == ' ' || d->text[p] == '\t'))
            p++;
        if (p + 1 >= end || d->text[p] != '/' || d->text[p + 1] != '/')
            uncomment = false;
    }
    size_t extra = (last - first + 1) * (size_t)(cfg.tab + 3);
    if (extra > TEXT_MAX || b - a > TEXT_MAX - extra) {
        status("Selection too large");
        return;
    }
    char *s = malloc(b - a + extra + 1);
    if (!s)
        return;
    size_t w = 0;
    for (size_t r = first; r <= last; r++) {
        size_t p = d->lines[r], end = d->lines[r + 1];
        if (op == 0) {
            if (cfg.spaces) {
                memset(s + w, ' ', (size_t)cfg.tab);
                w += (size_t)cfg.tab;
            } else
                s[w++] = '\t';
        } else if (op == 1) {
            if (p < end && d->text[p] == '\t')
                p++;
            else
                for (int k = 0; k < cfg.tab && p < end && d->text[p] == ' '; k++)
                    p++;
        } else if (op == 2) {
            size_t q = p;
            while (q < end && (d->text[q] == ' ' || d->text[q] == '\t'))
                q++;
            memcpy(s + w, d->text + p, q - p);
            w += q - p;
            p = q;
            if (uncomment) {
                p += 2;
                if (p < end && d->text[p] == ' ')
                    p++;
            } else {
                s[w++] = '/';
                s[w++] = '/';
                s[w++] = ' ';
            }
        }
        memcpy(s + w, d->text + p, end - p);
        w += end - p;
    }
    bool ok = replace(d, a, b, s, w, now(), 0);
    free(s);
    if (ok) {
        d->anchor = a;
        d->cursor = a + w;
    }
    redraw = true;
}
static void search_next(bool backwards) {
    Document *d = doc();
    if (!d || !query[0])
        return;
    size_t a, b;
    bool selected = bounds(d, &a, &b);
    size_t start = backwards
                       ? (selected ? (a ? a - 1 : d->len) : (d->cursor ? d->cursor - 1 : d->len))
                       : d->cursor;
    size_t p = find_text(d, query, start, backwards, cfg.sensitive, cfg.whole);
    if (p == NONE) {
        status("No match");
        return;
    }
    d->anchor = p;
    d->cursor = p + strlen(query);
    d->goal = NONE;
    manual_scroll = false;
    panel = P_EDIT;
    status("Match at line %lu", (unsigned long)row_at(d, p) + 1);
}
/* Built-in identifiers and full signatures are extracted from embedded docs.
 * Project identifiers supplement them without claiming semantic completion. */
static void add_completion(const char *s, size_t n, const char *prefix, size_t pn) {
    if (n <= pn || n >= sizeof(completions[0]) || ncomplete == 48 || memcmp(s, prefix, pn))
        return;
    for (int i = 0; i < ncomplete; i++)
        if (strlen(completions[i]) == n && !memcmp(completions[i], s, n))
            return;
    memcpy(completions[ncomplete], s, n);
    completions[ncomplete++][n] = 0;
}
static void scan_completion(const char *s, size_t n, const char *prefix, size_t pn) {
    for (size_t p = 0; p < n;) {
        if (isalpha((unsigned char)s[p]) || s[p] == '_') {
            size_t a = p++;
            while (p < n && (isalnum((unsigned char)s[p]) || s[p] == '_'))
                p++;
            add_completion(s + a, p - a, prefix, pn);
        } else
            p++;
    }
}
static void complete(void);
static void accept_completion(void) {
    Document *d = doc();
    if (d && ncomplete) {
        (void)replace(d, complete_start, d->cursor, completions[complete_selected],
                      strlen(completions[complete_selected]), now(), 0);
    }
    panel = P_EDIT;
    redraw = true;
}
typedef enum {
    PA_NEW_A = 1,
    PA_NEW_C,
    PA_NEW_TEXT,
    PA_SAVEAS,
    PA_FIND,
    PA_REPLACE_FIND,
    PA_REPLACE_WITH,
    PA_GOTO,
    PA_HOST,
    PA_TOKEN,
    PA_PORT,
    PA_ENTRY,
    PA_OUTPUT,
    PA_PROJECT,
    PA_RENAME,
    PA_MKDIR,
    PA_DELETE,
    PA_CLOSE,
    PA_QUIT,
    PA_REPLACE_ONE
} PromptAction;
static const char *commands[] = {"New Auric file",
                                 "New C file",
                                 "New text/header file",
                                 "Save",
                                 "Save as",
                                 "Save all",
                                 "Close document",
                                 "Recent files",
                                 "Project explorer",
                                 "Undo",
                                 "Redo",
                                 "Copy (selection or line)",
                                 "Cut (selection or line)",
                                 "Paste",
                                 "Select all",
                                 "Select word",
                                 "Select line",
                                 "Find",
                                 "Find next",
                                 "Find previous",
                                 "Replace all",
                                 "Replace selection/match",
                                 "Search: case sensitive",
                                 "Search: whole words",
                                 "Go to line",
                                 "Start of line",
                                 "End of line",
                                 "Start of file",
                                 "End of file",
                                 "Page up",
                                 "Page down",
                                 "Previous word",
                                 "Next word",
                                 "Indent lines",
                                 "Unindent lines",
                                 "Toggle line comment",
                                 "Duplicate lines",
                                 "Delete lines",
                                 "Move lines up",
                                 "Move lines down",
                                 "Complete identifier",
                                 "Snippets",
                                 "Build project",
                                 "Cancel build",
                                 "Build output",
                                 "Errors / warnings",
                                 "Project folder",
                                 "Project entry file",
                                 "Output app name",
                                 "Install: replace/copy",
                                 "Host address",
                                 "Host token",
                                 "Host port",
                                 "Test connection",
                                 "SDK + Auric reference",
                                 "Font size 8/16",
                                 "Light/dark theme",
                                 "Soft wrap",
                                 "Tabs: 2/4/8",
                                 "Indent: spaces/tabs",
                                 "Auto pairs",
                                 "Caps lock",
                                 "Bottom code / keyboard",
                                 "Rename selected file",
                                 "Create folder",
                                 "Delete selected entry",
                                 "Next browser page",
                                 "Previous browser page",
                                 "Toggle LF/CRLF",
                                 "Quit (save first)"};
static bool file_op_blocked(void) {
    if (busy()) {
        status("Wait for build or cancel it before changing files");
        return true;
    }
    return false;
}
static void do_command(int c) {
    Document *d = doc();
    size_t a = 0, b = 0;
    redraw = true;
    switch (c) {
    case 0:
        if (!file_op_blocked())
            prompt(PA_NEW_A, "New Auric path relative to project", "main.aur");
        break;
    case 1:
        if (!file_op_blocked())
            prompt(PA_NEW_C, "New C path relative to project", "main.c");
        break;
    case 2:
        if (!file_op_blocked())
            prompt(PA_NEW_TEXT, "New text/header relative path", "include/helpers.h");
        break;
    case 3:
        if (d)
            (void)save_doc(d);
        panel = P_EDIT;
        break;
    case 4:
        if (d && !file_op_blocked())
            prompt(PA_SAVEAS, "Save as (existing file is rejected)", d->name);
        break;
    case 5:
        (void)save_all();
        panel = P_EDIT;
        break;
    case 6:
        if (d)
            prompt(PA_CLOSE,
                   doc_dirty(d) ? "Unsaved: type SAVE or DISCARD" : "Close document? Type CLOSE",
                   "");
        break;
    case 7:
        panel = P_RECENT;
        file_selected = 0;
        break;
    case 8:
        panel = P_FILES;
        browser_page = 0;
        refresh_files();
        break;
    case 9:
        if (d && !undo(d, false))
            status("Nothing to undo");
        panel = P_EDIT;
        break;
    case 10:
        if (d && !undo(d, true))
            status("Nothing to redo");
        panel = P_EDIT;
        break;
    case 11:
        copy_text(false);
        panel = P_EDIT;
        break;
    case 12:
        copy_text(true);
        panel = P_EDIT;
        break;
    case 13:
        insert_text(clipboard, strlen(clipboard), 0);
        panel = P_EDIT;
        break;
    case 14:
        if (d) {
            d->anchor = 0;
            d->cursor = d->len;
        }
        panel = P_EDIT;
        break;
    case 15:
        if (d) {
            a = d->cursor;
            while (a && wordchar((unsigned char)d->text[prevpos(d, a)]))
                a = prevpos(d, a);
            b = d->cursor;
            while (b < d->len && wordchar((unsigned char)d->text[b]))
                b = nextpos(d, b);
            d->anchor = a;
            d->cursor = b;
        }
        panel = P_EDIT;
        break;
    case 16:
        if (d) {
            a = row_at(d, d->cursor);
            d->anchor = d->lines[a];
            d->cursor = d->lines[a + 1];
        }
        panel = P_EDIT;
        break;
    case 17:
        prompt(PA_FIND, "Find text", query);
        break;
    case 18:
        search_next(false);
        break;
    case 19:
        search_next(true);
        break;
    case 20:
        prompt(PA_REPLACE_FIND, "Replace all: find text", query);
        break;
    case 21:
        prompt(PA_REPLACE_ONE, "Replace current matching selection", replacement);
        break;
    case 22:
        cfg.sensitive = !cfg.sensitive;
        config_save();
        status("Case sensitive: %s", cfg.sensitive ? "on" : "off");
        break;
    case 23:
        cfg.whole = !cfg.whole;
        config_save();
        status("Whole words: %s", cfg.whole ? "on" : "off");
        break;
    case 24:
        prompt(PA_GOTO, "Line number (1-based)", "");
        break;
    case 25:
    case 26:
    case 27:
    case 28:
        if (d) {
            size_t row = row_at(d, d->cursor);
            d->cursor = c == 25 ? d->lines[row] : c == 26 ? row_end(d, row) : c == 27 ? 0 : d->len;
            d->anchor = NONE;
            d->boundary = true;
        }
        panel = P_EDIT;
        break;
    case 29:
    case 30:
        move_cursor(c == 29 ? -20 : 20, false, false);
        panel = P_EDIT;
        break;
    case 31:
    case 32:
        move_cursor(c == 31 ? -1 : 1, false, true);
        panel = P_EDIT;
        break;
    case 33:
    case 34:
    case 35:
        edit_lines(c - 33);
        panel = P_EDIT;
        break;
    case 36:
        edit_lines(4);
        panel = P_EDIT;
        break;
    case 37:
        edit_lines(3);
        panel = P_EDIT;
        break;
    case 38:
        edit_lines(5);
        panel = P_EDIT;
        break;
    case 39:
        edit_lines(6);
        panel = P_EDIT;
        break;
    case 40:
        complete();
        break;
    case 41:
        keypage = 2;
        keyboard = true;
        panel = P_EDIT;
        break;
    case 42:
        (void)start_build();
        break;
    case 43:
        if (busy()) {
            build.paused = false;
            build.state = B_CANCEL;
            build.next = 0;
            panel = P_BUILD;
        } else
            status("No running build");
        break;
    case 44:
        panel = P_LOG;
        log_scroll = 0;
        break;
    case 45:
        panel = P_ERRORS;
        break;
    case 46:
        if (!file_op_blocked())
            prompt(PA_PROJECT, "Project SD folder (absolute /...)", cfg.project);
        break;
    case 47:
        if (!file_op_blocked())
            prompt(PA_ENTRY, "Project entry (.aur or .c)", cfg.entry);
        break;
    case 48:
        if (!file_op_blocked())
            prompt(PA_OUTPUT, "Installed app name (without .bin)", cfg.output);
        break;
    case 49:
        cfg.overwrite = !cfg.overwrite;
        config_save();
        status("Install: %s", cfg.overwrite ? "replace with .bak" : "numbered copy");
        break;
    case 50:
        if (!file_op_blocked())
            prompt(PA_HOST, "Host IPv4 or DNS name", cfg.host);
        break;
    case 51:
        if (!file_op_blocked())
            prompt(PA_TOKEN, "Host token (16..95 characters)", cfg.token);
        break;
    case 52:
        if (!file_op_blocked()) {
            char s[16];
            snprintf(s, sizeof(s), "%d", cfg.port);
            prompt(PA_PORT, "Host port (1024..65535)", s);
        }
        break;
    case 53:
        if (!file_op_blocked()) {
            net_on_wait(network_wait);
            if (net_available() && net_connect()) {
                size_t n;
                strcopy(build.id, sizeof(build.id), "00000000");
                if (rpc("PING", 0, 0, NULL, 0, &n))
                    status("%s", replybuf);
                else
                    status("Host: %s", build.error);
            } else
                status("Wi-Fi: %s", net_error());
            net_on_wait(NULL);
        }
        break;
    case 54:
        panel = P_HELP;
        help_scroll = 0;
        break;
    case 55:
        cfg.scale = cfg.scale == 1 ? 2 : 1;
        config_save();
        break;
    case 56:
        cfg.theme = !cfg.theme;
        config_save();
        break;
    case 57:
        cfg.wrap = !cfg.wrap;
        config_save();
        if (d)
            d->scroll = 0;
        break;
    case 58:
        cfg.tab = cfg.tab == 2 ? 4 : cfg.tab == 4 ? 8 : 2;
        config_save();
        break;
    case 59:
        cfg.spaces = !cfg.spaces;
        config_save();
        break;
    case 60:
        cfg.pairs = !cfg.pairs;
        config_save();
        break;
    case 61:
        caps = !caps;
        break;
    case 62:
        code_bottom = !code_bottom;
        keyboard = !code_bottom;
        panel = P_EDIT;
        break;
    case 63:
    case 65:
        if (!file_op_blocked()) {
            char rel[REL_MAX];
            if (!selected_rel(rel)) {
                status("Open explorer and select an entry first");
                break;
            }
            if (c == 63)
                prompt(PA_RENAME, "New leaf name (current selection)", entries[file_selected]);
            else
                prompt(PA_DELETE, "Type DELETE to remove selected entry", "");
        }
        break;
    case 64:
        if (!file_op_blocked())
            prompt(PA_MKDIR, "New folder name in explorer", "");
        break;
    case 66:
        if (browser_more) {
            browser_page++;
            panel = P_FILES;
            refresh_files();
        } else
            status("Last browser page");
        break;
    case 67:
        if (browser_page) {
            browser_page--;
            panel = P_FILES;
            refresh_files();
        }
        break;
    case 68:
        if (d) {
            d->crlf = !d->crlf;
            d->recovered = true;
            status("Save line endings: %s", d->crlf ? "CRLF" : "LF");
        }
        break;
    case 69:
        prompt(PA_QUIT, "Quit: type SAVE or RECOVER", "");
        break;
    default:
        break;
    }
}
static void submit_prompt(void) {
    int action = prompt_action;
    Document *d = doc();
    char p[280];
    if (strpbrk(input, "|\r\n")) {
        status("Reserved character in input");
        return;
    }
    if (action == PA_NEW_A || action == PA_NEW_C || action == PA_NEW_TEXT) {
        if ((action == PA_NEW_A && !suffix(input, ".aur")) ||
            (action == PA_NEW_C && !suffix(input, ".c")) || !relvalid(input)) {
            status("Invalid name or extension");
            return;
        }
        new_document(input, action == PA_NEW_A   ? aur_template
                            : action == PA_NEW_C ? c_template
                                                 : "");
    } else if (action == PA_SAVEAS) {
        if (!d)
            return;
        if (!project_path(p, sizeof(p), input) || fs_exists(p)) {
            status("Invalid path or file already exists");
            return;
        }
        char old[REL_MAX];
        strcopy(old, sizeof(old), d->name);
        strcopy(d->name, sizeof(d->name), input);
        if (!save_doc(d)) {
            strcopy(d->name, sizeof(d->name), old);
            return;
        }
        panel = P_EDIT;
    } else if (action == PA_FIND) {
        if (!input[0])
            return;
        strcopy(query, sizeof(query), input);
        search_next(false);
    } else if (action == PA_REPLACE_FIND) {
        if (!input[0])
            return;
        strcopy(query, sizeof(query), input);
        prompt(PA_REPLACE_WITH, "Replace all: replacement (may be empty)", replacement);
        return;
    } else if (action == PA_REPLACE_WITH) {
        strcopy(replacement, sizeof(replacement), input);
        if (!d || !replace_all(d, query, replacement, cfg.sensitive, cfg.whole, now()))
            status("No matches or memory/size limit");
        else
            status("Replaced all matches (one undo)");
        panel = P_EDIT;
    } else if (action == PA_REPLACE_ONE) {
        strcopy(replacement, sizeof(replacement), input);
        size_t a, b;
        if (!d || !bounds(d, &a, &b) || b - a != strlen(query) ||
            !match_at(d, a, query, strlen(query), cfg.sensitive, cfg.whole)) {
            status("Select a search result first");
            return;
        }
        insert_text(replacement, strlen(replacement), 0);
        panel = P_EDIT;
    } else if (action == PA_GOTO) {
        char *end;
        unsigned long row = strtoul(input, &end, 10);
        if (!d || *end || !row || !index_doc(d) || row > d->nlines) {
            status("Line number out of range");
            return;
        }
        d->cursor = d->lines[row - 1];
        d->anchor = NONE;
        panel = P_EDIT;
    } else if (action == PA_HOST) {
        if (!input[0] || strlen(input) >= sizeof(cfg.host) || strpbrk(input, " /\\:")) {
            status("Use a hostname or IPv4 address");
            return;
        }
        strcopy(cfg.host, sizeof(cfg.host), input);
        config_save();
        panel = P_EDIT;
    } else if (action == PA_TOKEN) {
        if (strlen(input) < 16 || strlen(input) >= sizeof(cfg.token)) {
            status("Token length: 16..95");
            return;
        }
        strcopy(cfg.token, sizeof(cfg.token), input);
        config_save();
        panel = P_EDIT;
    } else if (action == PA_PORT) {
        char *end;
        long v = strtol(input, &end, 10);
        if (!input[0] || *end || v < 1024 || v > 65535) {
            status("Port out of range");
            return;
        }
        cfg.port = (int)v;
        config_save();
        panel = P_EDIT;
    } else if (action == PA_ENTRY) {
        if (!relvalid(input) || (!suffix(input, ".aur") && !suffix(input, ".c"))) {
            status("Entry must end in .aur or .c");
            return;
        }
        strcopy(cfg.entry, sizeof(cfg.entry), input);
        config_save();
        panel = P_EDIT;
    } else if (action == PA_OUTPUT) {
        if (strlen(input) >= sizeof(cfg.output) || !relvalid(input) || strchr(input, '/')) {
            status("Invalid app name");
            return;
        }
        strcopy(cfg.output, sizeof(cfg.output), input);
        config_save();
        panel = P_EDIT;
    } else if (action == PA_PROJECT) {
        if (input[0] != '/' || strstr(input, "..") || strlen(input) >= 220) {
            status("Use an absolute SD path under 220 bytes");
            return;
        }
        if (!save_all())
            return;
        checkpoint();
        if (!fs_mkdir(input)) {
            status("Cannot create/open project folder");
            return;
        }
        for (int i = 0; i < ndoc; i++)
            doc_free(&docs[i]);
        ndoc = active = nrecent = 0;
        strcopy(cfg.project, sizeof(cfg.project), input);
        config_save();
        browser[0] = 0;
        browser_page = 0;
        panel = P_FILES;
        refresh_files();
    } else if (action == PA_RENAME || action == PA_DELETE) {
        char rel[REL_MAX];
        if (!selected_rel(rel) || !project_path(p, sizeof(p), rel))
            return;
        for (int i = 0; i < ndoc; i++)
            if (!strcmp(docs[i].name, rel) ||
                (!strncmp(docs[i].name, rel, strlen(rel)) && docs[i].name[strlen(rel)] == '/')) {
                if (action == PA_DELETE) {
                    status("Close affected documents before deleting");
                    return;
                }
            }
        if (action == PA_DELETE) {
            if (strcmp(input, "DELETE")) {
                status("Type DELETE exactly, or B to cancel");
                return;
            }
            if ((entrydir[file_selected] ? rmdir(p) : remove(p)) != 0) {
                status("Delete failed; folders must be empty");
                return;
            }
            status("Entry removed; no trash on FAT");
        } else {
            if (!relvalid(input) || strchr(input, '/')) {
                status("Enter a valid leaf name");
                return;
            }
            char newrel[REL_MAX], dest[280];
            if (browser[0]) {
                if (!fmtpath(newrel, sizeof(newrel), browser, input))
                    return;
            } else
                strcopy(newrel, sizeof(newrel), input);
            if (!project_path(dest, sizeof(dest), newrel) || fs_exists(dest)) {
                status("Destination exists or too long");
                return;
            }
            size_t oldlen = strlen(rel);
            for (int i = 0; i < ndoc; i++)
                if (!strcmp(docs[i].name, rel) ||
                    (!strncmp(docs[i].name, rel, oldlen) && docs[i].name[oldlen] == '/'))
                    if (strlen(newrel) + strlen(docs[i].name + oldlen) >= REL_MAX) {
                        status("Open document path would be too long");
                        return;
                    }
            if (rename(p, dest)) {
                status("Rename failed");
                return;
            }
            for (int i = 0; i < ndoc; i++)
                if (!strcmp(docs[i].name, rel) ||
                    (!strncmp(docs[i].name, rel, oldlen) && docs[i].name[oldlen] == '/')) {
                    char changed[REL_MAX];
                    snprintf(changed, sizeof(changed), "%s%s", newrel, docs[i].name + oldlen);
                    strcopy(docs[i].name, sizeof(docs[i].name), changed);
                }
            if (!strcmp(cfg.entry, rel) ||
                (!strncmp(cfg.entry, rel, oldlen) && cfg.entry[oldlen] == '/')) {
                char changed[REL_MAX];
                snprintf(changed, sizeof(changed), "%s%s", newrel, cfg.entry + oldlen);
                strcopy(cfg.entry, sizeof(cfg.entry), changed);
                config_save();
            }
            status("Renamed");
        }
        panel = P_FILES;
        refresh_files();
        checkpoint();
    } else if (action == PA_MKDIR) {
        char rel[REL_MAX];
        if (!relvalid(input) || strchr(input, '/')) {
            status("Enter a valid leaf folder name");
            return;
        }
        if (browser[0]) {
            if (!fmtpath(rel, sizeof(rel), browser, input))
                return;
        } else
            strcopy(rel, sizeof(rel), input);
        if (!project_path(p, sizeof(p), rel) || fs_exists(p) || !fs_mkdir(p)) {
            status("Folder exists or cannot be created");
            return;
        }
        panel = P_FILES;
        refresh_files();
    } else if (action == PA_CLOSE) {
        if (!d)
            return;
        if (doc_dirty(d)) {
            if (!strcmp(input, "SAVE")) {
                if (!save_doc(d))
                    return;
            } else if (strcmp(input, "DISCARD")) {
                status("Type SAVE or DISCARD, or B to cancel");
                return;
            }
        } else if (strcmp(input, "CLOSE")) {
            status("Type CLOSE, or B to cancel");
            return;
        }
        doc_free(d);
        memmove(d, d + 1, (size_t)(ndoc - active - 1) * sizeof(Document));
        memset(&docs[--ndoc], 0, sizeof(Document));
        if (active >= ndoc)
            active = ndoc ? ndoc - 1 : 0;
        panel = P_EDIT;
        checkpoint();
    } else if (action == PA_QUIT) {
        if (!strcmp(input, "SAVE")) {
            if (!save_all())
                return;
        } else if (strcmp(input, "RECOVER")) {
            status("Type SAVE or RECOVER, or B to cancel");
            return;
        }
        checkpoint();
        app_quit();
    }
    prompt_action = 0;
    input[0] = 0;
    redraw = true;
}

/* -------------------- renderer and touch keyboard -------------------- */
static bool keyword(const char *p, size_t n, bool c) {
    static const char *aur =
        "fn let if else while break continue return true false int float bool string ";
    static const char *ck =
        "auto break case char const continue default do double else enum extern float for goto if "
        "inline int long register restrict return short signed sizeof static struct switch typedef "
        "union unsigned void volatile while _Bool _Complex _Atomic bool true false NULL size_t "
        "uint32_t ";
    const char *s = c ? ck : aur;
    while (*s) {
        const char *e = strchr(s, ' ');
        if (!e)
            break;
        if ((size_t)(e - s) == n && !memcmp(s, p, n))
            return true;
        s = e + 1;
    }
    return false;
}
static uint32_t syntax_color(Document *d, size_t *tokenend, uint32_t *cached, size_t p, size_t end,
                             bool *block, char *quote, bool *escape, bool *comment, bool *pre) {
    char c = d->text[p], v = p + 1 < end ? d->text[p + 1] : 0;
    if (*comment || *pre)
        return muted();
    if (*block) {
        if (p && d->text[p - 1] == '*' && c == '/')
            *block = false;
        return muted();
    }
    if (*quote) {
        if (*escape)
            *escape = false;
        else if (c == '\\')
            *escape = true;
        else if (c == *quote)
            *quote = 0;
        return cfg.theme ? RGB(35, 108, 32) : RGB(150, 218, 139);
    }
    if (c == '/' && v == '/') {
        *comment = true;
        return muted();
    }
    if (c == '/' && v == '*') {
        *block = true;
        return muted();
    }
    if (c == '#') {
        *pre = true;
        return cfg.theme ? RGB(141, 60, 15) : RGB(238, 184, 111);
    }
    if (c == '"' || c == '\'') {
        *quote = c;
        return cfg.theme ? RGB(35, 108, 32) : RGB(150, 218, 139);
    }
    if (p < *tokenend)
        return *cached;
    if (isalpha((unsigned char)c) || c == '_') {
        size_t q = p + 1;
        while (q < end && (isalnum((unsigned char)d->text[q]) || d->text[q] == '_'))
            q++;
        *tokenend = q;
        *cached = keyword(d->text + p, q - p, suffix(d->name, ".c") || suffix(d->name, ".h"))
                      ? (cfg.theme ? RGB(124, 38, 150) : RGB(199, 160, 255))
                      : fg();
        return *cached;
    }
    if (isdigit((unsigned char)c)) {
        size_t q = p + 1;
        while (q < end && (isalnum((unsigned char)d->text[q]) || d->text[q] == '.'))
            q++;
        *tokenend = q;
        *cached = cfg.theme ? RGB(160, 81, 7) : RGB(255, 197, 120);
        return *cached;
    }
    return fg();
}
static size_t visual_rows(Document *d, size_t row, size_t cols) {
    size_t end = row_end(d, row), width = column(d, end, (unsigned)cfg.tab);
    return width / cols + 1;
}
static size_t visual_cursor(Document *d, size_t row, size_t cols) {
    size_t v = 0;
    for (size_t r = 0; r < row; r++)
        v += visual_rows(d, r, cols);
    return v + column(d, d->cursor, (unsigned)cfg.tab) / cols;
}
static void render_code(Screen s, int y, int height, int scale, bool follow) {
    Document *d = doc();
    int width = s == SCREEN_TOP ? 400 : 320, cell = 8 * scale, gutter = scale == 1 ? 40 : 48;
    size_t cols = (size_t)(width - gutter) / cell;
    int rows = height / cell;
    if (!d) {
        text(s, 8, y + 16, muted(), "START: commands / new file");
        return;
    }
    if (!index_doc(d)) {
        text(s, 8, y, fg(), "Line index: memory exhausted");
        return;
    }
    size_t current = row_at(d, d->cursor), col = column(d, d->cursor, (unsigned)cfg.tab),
           vcur = cfg.wrap ? visual_cursor(d, current, cols) : current;
    if (follow && !manual_scroll) {
        if (vcur < d->scroll)
            d->scroll = vcur;
        if (vcur >= d->scroll + (size_t)rows)
            d->scroll = vcur - (size_t)rows + 1;
        if (!cfg.wrap) {
            if (col < d->hscroll)
                d->hscroll = col;
            if (col >= d->hscroll + cols)
                d->hscroll = col - cols + 1;
        }
    }
    size_t row = cfg.wrap ? 0 : MIN(d->scroll, d->nlines - 1), segment = 0;
    if (cfg.wrap) {
        size_t v = d->scroll;
        while (row + 1 < d->nlines && v >= visual_rows(d, row, cols)) {
            v -= visual_rows(d, row, cols);
            row++;
        }
        segment = v;
    }
    size_t sa, sb;
    bool selected = bounds(d, &sa, &sb);
    size_t qn = strlen(query);
    for (int out = 0; out < rows && row < d->nlines; out++) {
        int yy = y + out * cell;
        size_t from = cfg.wrap ? segment * cols : d->hscroll, to = from + cols, p = d->lines[row],
               end = row_end(d, row), vc = 0;
        bool block = d->lex[row] != 0, comment = false, pre = false, escape = false;
        char quote = 0;
        size_t tokenend = 0, match_end = 0;
        uint32_t cached = fg();
        if (!segment) {
            char number[16];
            snprintf(number, sizeof(number), "%4lu", (unsigned long)row + 1);
            gfx_text(s, 0, yy, muted(), "%s", number);
        }
        gfx_rect(s, gutter - 3, yy, 1, cell, box());
        while (p < end) {
            size_t next = nextpos(d, p);
            uint32_t color = syntax_color(d, &tokenend, &cached, p, end, &block, &quote, &escape,
                                          &comment, &pre);
            size_t cells = d->text[p] == '\t' ? (size_t)cfg.tab - vc % (size_t)cfg.tab : 1;
            bool match = qn && match_at(d, p, query, qn, cfg.sensitive, cfg.whole);
            if (match)
                match_end = p + qn;
            for (size_t k = 0; k < cells; k++) {
                size_t ccol = vc + k;
                if (ccol >= from && ccol < to) {
                    int x = gutter + (int)(ccol - from) * cell;
                    bool sel = selected && p < sb && next > sa;
                    bool found = p < match_end;
                    if (sel || found)
                        gfx_rect(s, x, yy, cell, cell, sel ? RGB(25, 100, 100) : RGB(105, 80, 25));
                    char ch = d->text[p] == '\t'                 ? ' '
                              : (unsigned char)d->text[p] >= 128 ? '?'
                                                                 : d->text[p];
                    if (scale == 1)
                        text(s, x, yy, color, "%c", ch);
                    else
                        gfx_text_scaled(s, x, yy, scale, color, "%c", ch);
                }
            }
            if (blink && p == d->cursor && vc >= from && vc < to)
                gfx_rect(s, gutter + (int)(vc - from) * cell, yy, 1, cell, accent());
            vc += cells;
            p = next;
            if (vc >= to)
                break;
        }
        if (blink && d->cursor == end && vc >= from && vc < to)
            gfx_rect(s, gutter + (int)(vc - from) * cell, yy, 1, cell, accent());
        if (selected && end >= sa && end < sb && vc >= from && vc < to)
            gfx_rect(s, gutter + (int)(vc - from) * cell, yy, cell, cell, RGB(25, 100, 100));
        if (cfg.wrap && segment + 1 < visual_rows(d, row, cols))
            segment++;
        else {
            row++;
            segment = 0;
        }
    }
}
static void button(int x, int y, int w, int h, const char *label, bool selected) {
    gfx_round_rect(SCREEN_BOTTOM, x + 1, y + 1, w - 2, h - 2, 3,
                   selected ? RGB(25, 100, 100) : box());
    text(SCREEN_BOTTOM, x + 4, y + (h - 8) / 2, selected ? COLOR_WHITE : fg(), "%s", label);
}
static void render_top(void) {
    gfx_clear(SCREEN_TOP, bg());
    gfx_rect(SCREEN_TOP, 0, 0, 400, 20, box());
    Document *d = doc();
    text(SCREEN_TOP, 4, 6, accent(), "VS3DS");
    if (d) {
        char leaf[40];
        const char *p = strrchr(d->name, '/');
        strcopy(leaf, sizeof(leaf), p ? p + 1 : d->name);
        text(SCREEN_TOP, 58, 6, fg(), "%.28s%s", leaf, doc_dirty(d) ? " *" : "");
        text(SCREEN_TOP, 330, 6, muted(), "%d/%d", active + 1, ndoc);
    }
    render_code(SCREEN_TOP, 24, 200, cfg.scale, true);
    gfx_rect(SCREEN_TOP, 0, 228, 400, 12, box());
    if (d)
        text(SCREEN_TOP, 2, 230, muted(), "L%lu C%lu %luB %s %s %s",
             (unsigned long)row_at(d, d->cursor) + 1,
             (unsigned long)column(d, d->cursor, (unsigned)cfg.tab) + 1, (unsigned long)d->len,
             d->crlf ? "CRLF" : "LF", cfg.wrap ? "WRAP" : "SCROLL",
             suffix(d->name, ".aur") ? "AUR" : "C/TXT");
}
static const char *alpha_rows[] = {"qwertyuiop", "asdfghjkl_", "zxcvbnm,.;", "0123456789"};
static const char *symbol_rows[] = {"(){}[]<>;:", "+-*/%=!&|^", "\"'\\#?@$~`_", "0123456789"};
static const char *snippet_names[] = {"if", "while", "for C", "function", "main A", "main C",
                                      "->", "==",    "!=",    "&&",       "||",     "//"};
static const char *snippet_texts[] = {"if condition {\n    \n}",
                                      "while condition {\n    \n}",
                                      "for (int i = 0; i < count; ++i) {\n    \n}",
                                      "fn name() {\n    \n}",
                                      aur_template,
                                      c_template,
                                      "->",
                                      "==",
                                      "!=",
                                      "&&",
                                      "||",
                                      "// "};
static void render_keyboard(void) {
    if (keypage == 2) {
        for (int i = 0; i < 12; i++)
            button((i % 3) * 106, 112 + (i / 3) * 25, 106, 25, snippet_names[i], false);
    } else
        for (int r = 0; r < 4; r++) {
            const char *s = keypage ? symbol_rows[r] : alpha_rows[r];
            int n = (int)strlen(s), w = 320 / n;
            for (int c = 0; c < n; c++) {
                char label[2] = {s[c], 0};
                if ((shift != caps) && isalpha((unsigned char)label[0]))
                    label[0] = (char)toupper((unsigned char)label[0]);
                button(c * w, 112 + r * 25, w, 25, label, false);
            }
        }
    button(0, 214, 53, 26, shift ? "SHIFT" : "Shift", shift);
    button(53, 214, 53, 26, "Space", false);
    button(106, 214, 53, 26, "Bksp", false);
    button(159, 214, 53, 26, "Enter", false);
    button(212, 214, 53, 26, "Del", false);
    button(265, 214, 55, 26, "Tab", false);
}
static const char *skip_display(const char *s, size_t rows, size_t width) {
    while (*s && rows--) {
        size_t n = 0;
        while (*s && *s != '\n' && n < width) {
            s++;
            n++;
        }
        if (*s == '\n')
            s++;
    }
    return s;
}
static void wrapped_text(const char *s, int first, int top, int rows) {
    s = skip_display(s, (size_t)(first < 0 ? 0 : first), 39);
    for (int r = 0; r < rows && *s; r++) {
        char line[40];
        size_t n = 0;
        while (*s && *s != '\n' && n < 39) {
            unsigned char ch = (unsigned char)*s++;
            line[n++] = ch < 128 ? (char)ch : '?';
        }
        if (*s == '\n')
            s++;
        line[n] = 0;
        text(SCREEN_BOTTOM, 4, top + r * 9, fg(), "%s", line);
    }
}
static const char *reference_text(void);
static void render_bottom(void) {
    gfx_clear(SCREEN_BOTTOM, bg());
    if (prompt_action) {
        text(SCREEN_BOTTOM, 4, 4, accent(), "%.39s", prompt_label);
        char display[256];
        strcopy(display, sizeof(display), input);
        if (prompt_action == PA_TOKEN)
            for (size_t i = 0; display[i]; i++)
                display[i] = '*';
        size_t len = strlen(display);
        text(SCREEN_BOTTOM, 4, 30, fg(), "> %.37s", display + (len > 37 ? len - 37 : 0));
        text(SCREEN_BOTTOM, 4, 52, muted(), "A/Enter confirm; B cancels");
        text(SCREEN_BOTTOM, 4, 78, muted(), "%.39s", statusline);
        render_keyboard();
        return;
    }
    if (panel == P_EDIT) {
        button(0, 0, 64, 24, "Save", false);
        button(64, 0, 64, 24, "Build", busy());
        button(128, 0, 64, 24, "Files", false);
        button(192, 0, 64, 24, "Menu", false);
        button(256, 0, 64, 24, "Hint", false);
        if (code_bottom) {
            render_code(SCREEN_BOTTOM, 27, 180, 1, true);
            button(0, 213, 106, 27, "Keyboard", false);
            button(106, 213, 106, 27, "Left", false);
            button(212, 213, 108, 27, "Right", false);
            return;
        }
        button(0, 26, 64, 24, "Undo", false);
        button(64, 26, 64, 24, "Redo", false);
        button(128, 26, 64, 24, "Copy", false);
        button(192, 26, 64, 24, "Paste", false);
        button(256, 26, 64, 24, "Find", false);
        button(0, 52, 64, 24, "Left", false);
        button(64, 52, 64, 24, "Right", false);
        button(128, 52, 64, 24, "Up", false);
        button(192, 52, 64, 24, "Down", false);
        button(256, 52, 64, 24, keypage ? "ABC" : "Symbols", false);
        text(SCREEN_BOTTOM, 3, 80, muted(), "%.39s", statusline);
        text(SCREEN_BOTTOM, 3, 96, muted(), "A+Dpad select | START commands");
        if (keyboard)
            render_keyboard();
        return;
    }
    if (panel == P_BUILD) {
        text(SCREEN_BOTTOM, 5, 8, accent(), "PROJECT BUILD - %s", build.c ? "C SDK" : "Auric");
        text(SCREEN_BOTTOM, 5, 32, fg(), "Stage %d / 11  File %d/%d", build.state, build.index + 1,
             build.count);
        if (build.index < build.count)
            text(SCREEN_BOTTOM, 5, 54, fg(), "%.38s", build.files[build.index].rel);
        text(SCREEN_BOTTOM, 5, 80, fg(), "%lu / %lu bytes", (unsigned long)build.offset,
             (unsigned long)(build.state == B_FETCH      ? build.binsize
                             : build.index < build.count ? build.files[build.index].size
                                                         : build.total));
        text(SCREEN_BOTTOM, 5, 112, muted(), "Each network call may briefly wait.");
        button(0, 210, 160, 30, "B: Cancel", false);
        button(160, 210, 160, 30, build.paused ? "A: Retry" : "Editor", false);
        return;
    }
    const char *title = panel == P_MENU       ? "COMMANDS"
                        : panel == P_FILES    ? "PROJECT EXPLORER"
                        : panel == P_LOG      ? "BUILD LOG"
                        : panel == P_ERRORS   ? "ERRORS / WARNINGS"
                        : panel == P_HELP     ? "OFFLINE SDK / AURIC"
                        : panel == P_COMPLETE ? "COMPLETIONS"
                                              : "RECENT FILES";
    text(SCREEN_BOTTOM, 4, 5, accent(), "%s", title);
    if (panel == P_MENU) {
        int first = (menu_scroll / 9) * 9;
        for (int r = 0; r < 9 && first + r < (int)ARRAY_LEN(commands); r++) {
            int i = first + r;
            button(0, 27 + r * 20, 320, 20, commands[i], i == menu_scroll);
        }
        button(0, 211, 106, 29, "Prev", false);
        button(106, 211, 106, 29, "Next", false);
        button(212, 211, 108, 29, "Close", false);
    } else if (panel == P_FILES) {
        text(SCREEN_BOTTOM, 4, 20, muted(), "/%.29s p%lu", browser,
             (unsigned long)browser_page + 1);
        int first = (file_selected / 8) * 8;
        for (int r = 0; r < 8 && first + r < nentries; r++) {
            int i = first + r;
            char label[44];
            snprintf(label, sizeof(label), "%s %.35s", entrydir[i] ? "[D]" : "   ", entries[i]);
            button(0, 39 + r * 20, 320, 20, label, i == file_selected);
        }
        button(0, 211, 80, 29, "Up", false);
        button(80, 211, 80, 29, "Prev", false);
        button(160, 211, 80, 29, "Next", false);
        button(240, 211, 80, 29, "Menu", false);
    } else if (panel == P_LOG) {
        wrapped_text(logtext, log_scroll, 25, 20);
        button(0, 211, 106, 29, "Prev", false);
        button(106, 211, 106, 29, "Next", false);
        button(212, 211, 108, 29, "Close", false);
    } else if (panel == P_HELP) {
        wrapped_text(reference_text(), help_scroll, 25, 20);
        button(0, 211, 106, 29, "Prev", false);
        button(106, 211, 106, 29, "Next", false);
        button(212, 211, 108, 29, "Close", false);
    } else if (panel == P_ERRORS) {
        int first = (error_selected / 6) * 6;
        if (!ndiag)
            text(SCREEN_BOTTOM, 4, 35, muted(), "No parsed diagnostics; read full log");
        for (int r = 0; r < 6 && first + r < ndiag; r++) {
            Diagnostic *e = &diagnostics[first + r];
            char label[48];
            snprintf(label, sizeof(label), "%c %.23s:%u", e->warning ? 'W' : 'E', e->file, e->row);
            button(0, 28 + r * 29, 320, 29, label, first + r == error_selected);
        }
        button(0, 211, 106, 29, "Prev", false);
        button(106, 211, 106, 29, "Next", false);
        button(212, 211, 108, 29, "Close", false);
    } else if (panel == P_COMPLETE || panel == P_RECENT) {
        int count = panel == P_COMPLETE ? ncomplete : nrecent,
            sel = panel == P_COMPLETE ? complete_selected : file_selected, first = (sel / 8) * 8;
        for (int r = 0; r < 8 && first + r < count; r++)
            button(0, 30 + r * 22, 320, 22,
                   panel == P_COMPLETE ? completions[first + r] : recent[first + r],
                   sel == first + r);
        button(0, 211, 106, 29, "Prev", false);
        button(106, 211, 106, 29, "Next", false);
        button(212, 211, 108, 29, "Close", false);
    }
}
static void keyboard_key(int x, int y) {
    if (y >= 214) {
        int c = x / 53;
        if (c == 0) {
            shift = !shift;
            redraw = true;
            return;
        }
        if (c == 3) {
            if (prompt_action)
                submit_prompt();
            else
                newline_indent();
            return;
        }
        if (c == 2 || c == 4) {
            if (prompt_action) {
                size_t n = strlen(input);
                if (n)
                    input[n - 1] = 0;
                redraw = true;
            } else
                erase_text(c == 4);
            return;
        }
        const char *s = c == 1       ? " "
                        : cfg.spaces ? (cfg.tab == 2   ? "  "
                                        : cfg.tab == 8 ? "        "
                                                       : "    ")
                                     : "\t";
        if (prompt_action) {
            size_t n = strlen(input), m = strlen(s);
            if (n + m < sizeof(input)) {
                memcpy(input + n, s, m + 1);
            }
        } else
            insert_text(s, strlen(s), 0);
        redraw = true;
        return;
    }
    if (y < 112 || y >= 212)
        return;
    int r = (y - 112) / 25;
    if (keypage == 2) {
        int c = x / 106, i = r * 3 + c;
        if (i < 12 && !prompt_action)
            insert_text(snippet_texts[i], strlen(snippet_texts[i]), 0);
        return;
    }
    const char *s = keypage ? symbol_rows[r] : alpha_rows[r];
    int w = 320 / (int)strlen(s), c = x / w;
    if (c >= (int)strlen(s))
        return;
    char ch = s[c];
    if ((shift != caps) && isalpha((unsigned char)ch))
        ch = (char)toupper((unsigned char)ch);
    if (prompt_action) {
        size_t n = strlen(input);
        if (n + 1 < sizeof(input)) {
            input[n] = ch;
            input[n + 1] = 0;
        }
    } else
        type_char(ch);
    shift = false;
    redraw = true;
}
static void tap_code(int x, int y) {
    Document *d = doc();
    if (!d || !index_doc(d))
        return;
    size_t cols = 35, row = cfg.wrap ? 0 : MIN(d->scroll, d->nlines - 1), segment = 0;
    if (cfg.wrap) {
        size_t v = d->scroll;
        while (row + 1 < d->nlines && v >= visual_rows(d, row, cols)) {
            v -= visual_rows(d, row, cols);
            row++;
        }
        segment = v;
    }
    int display = (y - 27) / 8;
    while (display-- > 0) {
        if (cfg.wrap && segment + 1 < visual_rows(d, row, cols))
            segment++;
        else {
            if (row + 1 < d->nlines)
                row++;
            segment = 0;
        }
    }
    size_t columnx = x < 40 ? 0 : (size_t)(x - 40) / 8,
           offset = cfg.wrap ? segment * cols : d->hscroll;
    size_t p = colpos(d, row, offset + columnx, (unsigned)cfg.tab);
    d->cursor = p;
    d->anchor = NONE;
    d->boundary = true;
    if (last_tap == p && (uint32_t)(now() - tap_at) < 400)
        do_command(15);
    last_tap = p;
    tap_at = now();
    redraw = true;
}
static void handle_touch(void) {
    static uint32_t repeat_touch;
    int x = hid_touch_x(), y = hid_touch_y();
    bool down = hid_touch_down();
    if (down)
        repeat_touch = now() + 350;
    else {
        bool repeatable = (prompt_action || (panel == P_EDIT && keyboard && !code_bottom)) &&
                          y >= 214 && ((x >= 106 && x < 159) || (x >= 212 && x < 265));
        if (!repeatable || !hid_touch_held() || (int32_t)(now() - repeat_touch) < 0)
            return;
        repeat_touch = now() + 65;
    }
    redraw = true;
    if (prompt_action) {
        keyboard_key(x, y);
        return;
    }
    if (panel == P_EDIT) {
        if (y < 24) {
            int c = x / 64;
            do_command(c == 0 ? 3 : c == 1 ? 42 : c == 2 ? 8 : c == 3 ? -1 : 40);
            if (c == 3) {
                panel = P_MENU;
                menu_scroll = 0;
            }
            return;
        }
        if (code_bottom) {
            if (y >= 213) {
                if (x < 106) {
                    code_bottom = false;
                    keyboard = true;
                } else
                    move_cursor(x < 212 ? -1 : 1, false, false);
            } else if (y >= 27 && y < 207)
                tap_code(x, y);
            return;
        }
        if (y >= 26 && y < 50) {
            int c = x / 64;
            do_command(c == 0 ? 9 : c == 1 ? 10 : c == 2 ? 11 : c == 3 ? 13 : 17);
            return;
        }
        if (y >= 52 && y < 76) {
            int c = x / 64;
            if (c < 4)
                move_cursor(c == 0 ? -1 : c == 1 ? 1 : c == 2 ? -2 : 2, false, false);
            else
                keypage = keypage ? 0 : 1;
            return;
        }
        if (keyboard)
            keyboard_key(x, y);
        return;
    }
    if (panel == P_BUILD) {
        if (y >= 210) {
            if (x < 160) {
                build.paused = false;
                build.state = B_CANCEL;
                build.next = 0;
            } else if (build.paused) {
                build.paused = false;
                build.retries = 0;
                build.next = 0;
            } else
                panel = P_EDIT;
        }
        return;
    }
    if (y >= 211) {
        int c = x / 106;
        if (panel == P_FILES) {
            if (x < 80)
                browser_up();
            else if (x < 160)
                do_command(67);
            else if (x < 240)
                do_command(66);
            else
                panel = P_MENU;
            return;
        }
        if (c >= 2) {
            panel = P_EDIT;
            return;
        }
        int delta = c ? 1 : -1;
        if (panel == P_MENU) {
            menu_scroll += delta * 9;
            if (menu_scroll < 0)
                menu_scroll = 0;
            if (menu_scroll >= (int)ARRAY_LEN(commands))
                menu_scroll = (int)ARRAY_LEN(commands) - 1;
        } else if (panel == P_HELP) {
            help_scroll += delta * 18;
            if (help_scroll < 0)
                help_scroll = 0;
        } else if (panel == P_LOG) {
            log_scroll += delta * 18;
            if (log_scroll < 0)
                log_scroll = 0;
        } else if (panel == P_ERRORS) {
            error_selected += delta * 6;
            if (error_selected < 0)
                error_selected = 0;
            if (error_selected >= ndiag)
                error_selected = ndiag ? ndiag - 1 : 0;
        } else if (panel == P_COMPLETE) {
            complete_selected += delta * 8;
            if (complete_selected < 0)
                complete_selected = 0;
            if (complete_selected >= ncomplete)
                complete_selected = ncomplete ? ncomplete - 1 : 0;
        } else {
            file_selected += delta * 8;
            if (file_selected < 0)
                file_selected = 0;
            if (file_selected >= nrecent)
                file_selected = nrecent ? nrecent - 1 : 0;
        }
        return;
    }
    if (panel == P_MENU && y >= 27) {
        int i = (menu_scroll / 9) * 9 + (y - 27) / 20;
        if (i < (int)ARRAY_LEN(commands)) {
            menu_scroll = i;
            do_command(i);
        }
    } else if (panel == P_FILES && y >= 39) {
        int i = (file_selected / 8) * 8 + (y - 39) / 20;
        if (i < nentries) {
            if (file_selected == i)
                browser_enter();
            else
                file_selected = i;
        }
    } else if (panel == P_ERRORS && y >= 28) {
        int i = (error_selected / 6) * 6 + (y - 28) / 29;
        if (i < ndiag) {
            if (error_selected == i)
                goto_diagnostic();
            else
                error_selected = i;
        }
    } else if ((panel == P_COMPLETE || panel == P_RECENT) && y >= 30) {
        int *selected = panel == P_COMPLETE ? &complete_selected : &file_selected,
            count = panel == P_COMPLETE ? ncomplete : nrecent,
            i = (*selected / 8) * 8 + (y - 30) / 22;
        if (i < count) {
            if (*selected == i) {
                if (panel == P_COMPLETE)
                    accept_completion();
                else {
                    char name[REL_MAX];
                    strcopy(name, sizeof(name), recent[i]);
                    open_document(name);
                }
            } else
                *selected = i;
        }
    }
}
static void handle_buttons(void) {
    uint32_t down = hid_keys_down(), held = hid_keys_held(), r = down;
    uint32_t navigation = held & (KEY_DPAD | (panel == P_EDIT && !prompt_action ? KEY_B : 0));
    if (navigation != repeat_keys) {
        repeat_keys = navigation;
        repeat_at = now() + 350;
    } else if (navigation && (int32_t)(now() - repeat_at) >= 0) {
        r |= navigation;
        repeat_at = now() + 65;
    }
    if (!down && !r)
        return;
    redraw = true;
    if (prompt_action) {
        if (down & KEY_B) {
            prompt_action = 0;
            input[0] = 0;
        } else if (down & KEY_A)
            submit_prompt();
        else if (down & KEY_SELECT)
            keypage = keypage == 1 ? 0 : 1;
        return;
    }
    if (down & KEY_START) {
        panel = panel == P_MENU ? P_EDIT : P_MENU;
        return;
    }
    if (panel == P_BUILD) {
        if (down & KEY_B) {
            build.paused = false;
            build.state = B_CANCEL;
            build.next = 0;
        } else if ((down & KEY_A) && build.paused) {
            build.paused = false;
            build.retries = 0;
            build.next = 0;
        }
        return;
    }
    if (panel != P_EDIT) {
        if (down & KEY_B) {
            panel = P_EDIT;
            return;
        }
        int delta = (r & KEY_UP) ? -1 : (r & KEY_DOWN) ? 1 : 0;
        if (panel == P_MENU) {
            menu_scroll += delta;
            if (r & KEY_LEFT)
                menu_scroll -= 9;
            if (r & KEY_RIGHT)
                menu_scroll += 9;
            if (menu_scroll < 0)
                menu_scroll = 0;
            if (menu_scroll >= (int)ARRAY_LEN(commands))
                menu_scroll = (int)ARRAY_LEN(commands) - 1;
            if (down & KEY_A)
                do_command(menu_scroll);
        } else if (panel == P_FILES) {
            file_selected += delta;
            if (file_selected < 0)
                file_selected = 0;
            if (file_selected >= nentries)
                file_selected = nentries ? nentries - 1 : 0;
            if (down & KEY_A)
                browser_enter();
            if (down & KEY_LEFT)
                browser_up();
            if (down & KEY_X)
                do_command(65);
            if (down & KEY_Y)
                do_command(63);
            if (down & KEY_R)
                do_command(66);
            if (down & KEY_L)
                do_command(67);
        } else if (panel == P_LOG || panel == P_HELP) {
            int *scroll = panel == P_LOG ? &log_scroll : &help_scroll;
            *scroll += delta;
            if (r & KEY_LEFT)
                *scroll -= 18;
            if (r & KEY_RIGHT)
                *scroll += 18;
            if (*scroll < 0)
                *scroll = 0;
        } else if (panel == P_ERRORS) {
            error_selected += delta;
            if (error_selected < 0)
                error_selected = 0;
            if (error_selected >= ndiag)
                error_selected = ndiag ? ndiag - 1 : 0;
            if (down & KEY_A)
                goto_diagnostic();
        } else if (panel == P_COMPLETE) {
            complete_selected += delta;
            if (complete_selected < 0)
                complete_selected = 0;
            if (complete_selected >= ncomplete)
                complete_selected = ncomplete ? ncomplete - 1 : 0;
            if (down & KEY_A)
                accept_completion();
        } else if (panel == P_RECENT) {
            file_selected += delta;
            if (file_selected < 0)
                file_selected = 0;
            if (file_selected >= nrecent)
                file_selected = nrecent ? nrecent - 1 : 0;
            if ((down & KEY_A) && nrecent) {
                char name[REL_MAX];
                strcopy(name, sizeof(name), recent[file_selected]);
                open_document(name);
            }
        }
        return;
    }
    if ((down & KEY_SELECT) && !(held & KEY_L))
        keypage = keypage == 1 ? 0 : 1;
    if (held & KEY_SELECT) {
        if (down & KEY_A)
            do_command(14);
        else if (down & KEY_LEFT)
            do_command(9);
        else if (down & KEY_RIGHT)
            do_command(10);
        else if (down & KEY_X)
            copy_text(false);
        else if (down & KEY_Y)
            do_command(13);
        else if (down & KEY_UP)
            do_command(29);
        else if (down & KEY_DOWN)
            do_command(30);
        return;
    }
    if (down & KEY_L) {
        if (ndoc)
            active = (active + ndoc - 1) % ndoc;
    }
    if (down & KEY_R) {
        if (ndoc)
            active = (active + 1) % ndoc;
    }
    if (down & KEY_X)
        do_command(3);
    if (down & KEY_Y)
        do_command(42);
    if (r & KEY_B)
        erase_text(false);
    bool select = (held & KEY_A) != 0;
    if (r & KEY_LEFT)
        move_cursor(-1, select, false);
    if (r & KEY_RIGHT)
        move_cursor(1, select, false);
    if (r & KEY_UP)
        move_cursor(-2, select, false);
    if (r & KEY_DOWN)
        move_cursor(2, select, false);
}
static void cpad_scroll(void) {
    static uint32_t next;
    int x, y;
    hid_cpad(&x, &y);
    if ((abs(x) < 40 && abs(y) < 40) || (int32_t)(now() - next) < 0)
        return;
    next = now() + 60;
    manual_scroll = true;
    Document *d = doc();
    if (!d || !index_doc(d))
        return;
    size_t total = d->nlines;
    if (cfg.wrap) {
        size_t cols = (400 - ((cfg.scale == 1) ? 40 : 48)) / (8 * (size_t)cfg.scale);
        total = 0;
        for (size_t row = 0; row < d->nlines; row++)
            total += visual_rows(d, row, cols);
    }
    if (y > 40) {
        if (d->scroll)
            d->scroll--;
    } else if (y < -40 && d->scroll + 1 < total)
        d->scroll++;
    if (!cfg.wrap) {
        if (x > 40 && d->hscroll < TEXT_MAX)
            d->hscroll++;
        else if (x < -40 && d->hscroll)
            d->hscroll--;
    }
    redraw = true;
}
static const char offline_reference[] =
    "VSCode3DS 1.0\n"
    "Use START for commands, SELECT for symbols; A+D-pad selects.\n"
    "X saves; Y builds; L/R change document. Circle pad scrolls.\n"
    "Build host needs Python3, devkitARM, complete AuroraOS repo.\n"
    "app.icon embeds an icon. assets/ deploys into app data folder.\n"
    "Launch installed apps from Aurora Home. SDK offers no app launch API.\n"
    "Recovery on HOME and every 15 seconds preserves unsaved buffers.\n"
    "FAT replacement is recoverable via .tmp/.bak, not crash atomic.\n"
    "Completions are lexical. Compiler provides semantic diagnostics.\n"
    "No interactive debugger, Git client, regex or language server.\n"
    "6 documents, 512 KiB each, 64 edits/document, 2 MiB history budget.\n"
    "64 files/build, 4 MiB upload, depth 8, 15 MiB output.\n"
    "Host stores session folders under .vs3ds; clean old ones when idle.\n"
    "Transport is plain TCP: trusted LAN only. No port forwarding.\n"
    "\n"
    "SDK + AURIC SOURCE REFERENCE (GPL-3.0)\n"
    "/* aurora_app.h - the AuroraOS native app SDK.\n"
    " *\n"
    " * Everything an app written in C needs from AuroraOS: both screens and\n"
    " * drawing, buttons, touch and the circle pad, sound, files, time, system\n"
    " * information and the network (HTTP). The C standard library (devkitARM's\n"
    " * newlib) works alongside it: printf() and friends write to an on-screen\n"
    " * console, malloc() has a 16 MB heap, fopen() and the rest of stdio reach the\n"
    " * SD card, and time() reads the console's clock.\n"
    " *\n"
    " * A minimal app:\n"
    " *\n"
    " *   #include <aurora_app.h>\n"
    " *\n"
    " *   int main(void) {\n"
    " *     while (app_loop()) {\n"
    " *       gfx_clear(SCREEN_TOP, COLOR_BLACK);\n"
    " *       gfx_text(SCREEN_TOP, 8, 8, COLOR_WHITE, \"Hello from C!\");\n"
    " *       if (hid_keys_down() & KEY_START)\n"
    " *         break;\n"
    " *     }\n"
    " *     return 0;\n"
    " *   }\n"
    " *\n"
    " * Returning from main(), calling exit() or pressing HOME goes back to the Home\n"
    " * Menu. L + Down + Select opens the system menu in an app. Build with\n"
    " * sdk/aurcc.py; see sdk/README.md. */\n"
    "#ifndef AURORA_APP_H\n"
    "#define AURORA_APP_H\n"
    "\n"
    "#include <stdbool.h>\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "\n"
    "#ifdef __cplusplus\n"
    "extern \"C\" {\n"
    "#endif\n"
    "\n"
    "#define AURORA_SDK_VERSION \"1.2\"\n"
    "\n"
    "#define AURORA_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))\n"
    "\n"
    "#ifndef AURORA_H\n"
    "typedef uint8_t u8;\n"
    "typedef uint16_t u16;\n"
    "typedef uint32_t u32;\n"
    "typedef uint64_t u64;\n"
    "typedef int8_t s8;\n"
    "typedef int16_t s16;\n"
    "typedef int32_t s32;\n"
    "typedef int64_t s64;\n"
    "#endif\n"
    "\n"
    "/* One pass of a frame loop: shows what was drawn since the last call, waits\n"
    " * for the next frame (60 per second), then reads the buttons, touch and\n"
    " * circle pad. HOME goes back to the Home Menu; L + Down + Select opens the\n"
    " * system menu. Returns false once app_quit() has been called.\n"
    " *\n"
    " *   while (app_loop()) { read input, update, draw }                       */\n"
    "bool app_loop(void);\n"
    "\n"
    "/* Makes the next app_loop() return false. */\n"
    "void app_quit(void);\n"
    "\n"
    "/* Frames shown per second, measured over the last second of app_loop(). */\n"
    "int app_fps(void);\n"
    "\n"
    "/* True when the app was started from the Home Menu, the Files app or the\n"
    " * Terminal. Only then are the GPU, sound, touch and the circle pad there\n"
    " * (the ARM11 core that runs them belongs to the OS), and only then does\n"
    " * HOME (or leaving the app) return to the Home Menu. An app booted directly\n"
    " * by the launcher as AURORAOS.BIN draws with the CPU and stays silent. */\n"
    "bool app_from_home(void);\n"
    "\n"
    "/* The app's file, e.g. \"/Aurora/Apps/C/Snake.bin\", and the folder for its own\n"
    " * data, named after it: \"/Aurora/Apps/C/Snake\", wherever the file is. Aurora\n"
    " * makes the folder when it starts the app, so files can be saved there at\n"
    " * once (an app built with --magic AUR1 or AOS1 makes it with fs_mkdir()). */\n"
    "const char *app_path(void);\n"
    "const char *app_dir(void);\n"
    "\n"
    "/* Leaving: return from main() or call exit(). Functions given to atexit()\n"
    " * run first, also when the user presses HOME, so that is the place to save.\n"
    " * Open stdio files are flushed and closed either way. */\n"
    "\n"
    "typedef enum {\n"
    "  SCREEN_TOP = 0,    /* 400 x 240 */\n"
    "  SCREEN_BOTTOM = 1, /* 320 x 240, the touchscreen */\n"
    "} Screen;\n"
    "\n"
    "#define SCREEN_TOP_WIDTH    400\n"
    "#define SCREEN_BOTTOM_WIDTH 320\n"
    "#define SCREEN_HEIGHT       240\n"
    "\n"
    "/* Colours are 0xRRGGBB. */\n"
    "#define RGB(r, g, b) \\\n"
    "  ((uint32_t)((((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF)))\n"
    "/* Image pixels carry alpha in the top byte: 0 clear, 255 solid. */\n"
    "#define RGBA(r, g, b, a) (RGB(r, g, b) | ((uint32_t)((a) & 0xFF) << 24))\n"
    "\n"
    "#ifndef AURORA_APP_NO_COLORS\n"
    "#define COLOR_BLACK      0x000000u\n"
    "#define COLOR_WHITE      0xFFFFFFu\n"
    "#define COLOR_GRAY       0xA0A0A0u\n"
    "#define COLOR_DARK_GRAY  0x505050u\n"
    "#define COLOR_RED        0xFF0000u\n"
    "#define COLOR_GREEN      0x00FF00u\n"
    "#define COLOR_BLUE       0x0000FFu\n"
    "#define COLOR_CYAN       0x00FFFFu\n"
    "#define COLOR_MAGENTA    0xFF00FFu\n"
    "#define COLOR_YELLOW     0xFFFF00u\n"
    "#define COLOR_ORANGE     0xFFA000u\n"
    "#define COLOR_AURORA     0x64E8C8u /* the OS's default accent */\n"
    "#endif\n"
    "\n"
    "/* Drawing goes to an off-screen copy of each screen and shows at the next\n"
    " * app_loop() (or gfx_present()), so a frame never appears half drawn. Every\n"
    " * function clips to the screen. Both screens start black. */\n"
    "\n"
    "int gfx_width(Screen s); /* 400 or 320 */\n"
    "\n"
    "void gfx_clear(Screen s, uint32_t color);\n"
    "void gfx_pixel(Screen s, int x, int y, uint32_t color);\n"
    "uint32_t gfx_get_pixel(Screen s, int x, int y);\n"
    "void gfx_rect(Screen s, int x, int y, int w, int h, uint32_t color);\n"
    "/* alpha 0..255: how much of `color` covers what is there. */\n"
    "void gfx_rect_blend(Screen s, int x, int y, int w, int h, uint32_t color,\n"
    "                    int alpha);\n"
    "/* A frame `t` pixels thick, inside the rectangle. */\n"
    "void gfx_rect_outline(Screen s, int x, int y, int w, int h, int t,\n"
    "                      uint32_t color);\n"
    "/* Corners of radius r, smoothed. */\n"
    "void gfx_round_rect(Screen s, int x, int y, int w, int h, int r,\n"
    "                    uint32_t color);\n"
    "/* From `top` at the first row to `bottom` at the last. */\n"
    "void gfx_gradient(Screen s, int x, int y, int w, int h, uint32_t top,\n"
    "                  uint32_t bottom);\n"
    "void gfx_line(Screen s, int x0, int y0, int x1, int y1, uint32_t color);\n"
    "void gfx_circle(Screen s, int cx, int cy, int r, uint32_t color);\n"
    "void gfx_circle_outline(Screen s, int cx, int cy, int r, uint32_t color);\n"
    "void gfx_triangle(Screen s, int x0, int y0, int x1, int y1, int x2, int y2,\n"
    "                  uint32_t color);\n"
    "\n"
    "/* Text. FONT_MONO is the built-in 8x8 font and is always there; the others\n"
    " * are the OS's Figtree from SD:/Aurora/assets.pak (Latin-1, UTF-8 strings)\n"
    " * and fall back to FONT_MONO without it. Text is printf-formatted, has a\n"
    " * clear background, and \"\\n\" starts a new line under the first. The\n"
    " * functions return the width drawn, in pixels. */\n"
    "typedef enum {\n"
    "  FONT_MONO = 0,\n"
    "  FONT_SMALL,   /* about 11 px high */\n"
    "  FONT_REGULAR, /* about 14 px */\n"
    "  FONT_BOLD,    /* about 14 px, bold */\n"
    "  FONT_TITLE,   /* about 20 px, bold */\n"
    "} GfxFont;\n"
    "\n"
    "/* (x, y) is the top left of the text in FONT_MONO. */\n"
    "int gfx_text(Screen s, int x, int y, uint32_t color, const char *fmt, ...)\n"
    "    AURORA_PRINTF(5, 6);\n"
    "/* FONT_MONO with every pixel `scale` x `scale`. */\n"
    "int gfx_text_scaled(Screen s, int x, int y, int scale, uint32_t color,\n"
    "                    const char *fmt, ...) AURORA_PRINTF(6, 7);\n"
    "/* Any font; (x, y) is the top left of the first line. */\n"
    "int gfx_print(Screen s, int x, int y, GfxFont font, uint32_t color,\n"
    "              const char *fmt, ...) AURORA_PRINTF(6, 7);\n"
    "/* The widest line of `text`, and the distance between lines. */\n"
    "int gfx_text_width(GfxFont font, const char *text);\n"
    "int gfx_line_height(GfxFont font);\n"
    "/* False when the Figtree fonts could not be loaded. */\n"
    "bool gfx_fonts_loaded(void);\n"
    "\n"
    "/* Images: pixels row by row, each 0xAARRGGBB. Load PNG (with alpha), JPEG\n"
    " * (baseline) and BMP files, or make an empty one and fill `pixels`. */\n"
    "typedef struct {\n"
    "  int w, h;\n"
    "  uint32_t *pixels;\n"
    "} GfxImage;\n"
    "\n"
    "/* NULL on failure; gfx_image_error() says why. Free with gfx_image_free(). */\n"
    "GfxImage *gfx_image_load(const char *path);\n"
    "/* Every pixel clear (0). */\n"
    "GfxImage *gfx_image_new(int w, int h);\n"
    "void gfx_image_free(GfxImage *img);\n"
    "const char *gfx_image_error(void);\n"
    "\n"
    "/* Draws with alpha blending, at (x, y) for the top left. */\n"
    "void gfx_image(Screen s, const GfxImage *img, int x, int y);\n"
    "/* The (sx, sy, sw, sh) part of the image only, e.g. a sprite from a sheet. */\n"
    "void gfx_image_part(Screen s, const GfxImage *img, int sx, int sy, int sw,\n"
    "                    int sh, int x, int y);\n"
    "/* Stretched or shrunk to w x h, nearest pixel. */\n"
    "void gfx_image_scaled(Screen s, const GfxImage *img, int x, int y, int w,\n"
    "                      int h);\n"
    "\n"
    "/* Shows both screens now; app_loop() does this itself. */\n"
    "void gfx_present(void);\n"
    "\n"
    "/* For drawing your own way: the off-screen copy of a screen, 3 bytes per\n"
    " * pixel in B, G, R order, stored by column and turned: pixel (x, y) is at\n"
    " * ((x * 240) + (239 - y)) * 3. Call gfx_mark(s) after writing to it. */\n"
    "uint8_t *gfx_framebuffer(Screen s);\n"
    "void gfx_mark(Screen s);\n"
    "\n"
    "/* The console starts on the top screen at the first output. It draws into the\n"
    " * screen like the gfx_ functions (so gfx_clear() erases it) and shows at the\n"
    " * next app_loop(), hid_wait() or gfx_present(). It wraps long lines and\n"
    " * scrolls; \"\\f\" clears it. stdin reads nothing. */\n"
    "\n"
    "/* Puts the console on a screen, cleared, cursor top left. */\n"
    "void con_init(Screen s);\n"
    "void con_clear(void);\n"
    "/* Colours for the text written from now on; the background also fills the\n"
    " * screen on con_clear() and the line a scroll opens. */\n"
    "void con_color(uint32_t fg, uint32_t bg);\n"
    "/* Moves the cursor to a column and row (from 0). */\n"
    "void con_move(int col, int row);\n"
    "int con_cols(void); /* 50 on the top screen, 40 on the bottom */\n"
    "int con_rows(void); /* 30 */\n"
    "\n"
    "#define KEY_A      (1u << 0)\n"
    "#define KEY_B      (1u << 1)\n"
    "#define KEY_SELECT (1u << 2)\n"
    "#define KEY_START  (1u << 3)\n"
    "#define KEY_RIGHT  (1u << 4)\n"
    "#define KEY_LEFT   (1u << 5)\n"
    "#define KEY_UP     (1u << 6)\n"
    "#define KEY_DOWN   (1u << 7)\n"
    "#define KEY_R      (1u << 8)\n"
    "#define KEY_L      (1u << 9)\n"
    "#define KEY_X      (1u << 10)\n"
    "#define KEY_Y      (1u << 11)\n"
    "/* Touching the bottom screen. */\n"
    "#define KEY_TOUCH  (1u << 20)\n"
    "/* The circle pad pushed most of the way in a direction. */\n"
    "#define KEY_CPAD_RIGHT (1u << 28)\n"
    "#define KEY_CPAD_LEFT  (1u << 29)\n"
    "#define KEY_CPAD_UP    (1u << 30)\n"
    "#define KEY_CPAD_DOWN  (1u << 31)\n"
    "#define KEY_DPAD (KEY_RIGHT | KEY_LEFT | KEY_UP | KEY_DOWN)\n"
    "#define KEY_CPAD (KEY_CPAD_RIGHT | KEY_CPAD_LEFT | KEY_CPAD_UP | KEY_CPAD_DOWN)\n"
    "\n"
    "/* Reads every input once. app_loop() calls it; a loop of your own should\n"
    " * call it once per pass, which also keeps HOME working. */\n"
    "void hid_scan(void);\n"
    "\n"
    "/* As of the last scan: pressed since the scan before, held down, and let go\n"
    " * since the scan before. */\n"
    "uint32_t hid_keys_down(void);\n"
    "uint32_t hid_keys_held(void);\n"
    "uint32_t hid_keys_up(void);\n"
    "\n"
    "/* Touch, in bottom-screen pixels (x 0..319, y 0..239), calibrated as in\n"
    " * Settings > Touch Calibration. A touch already on the screen when the app\n"
    " * starts is ignored until it lifts. */\n"
    "bool hid_touch_held(void);\n"
    "bool hid_touch_down(void);\n"
    "bool hid_touch_up(void);\n"
    "/* Where the stylus is, or where it last was; -1 before the first touch. */\n"
    "int hid_touch_x(void);\n"
    "int hid_touch_y(void);\n"
    "bool hid_touch_in(int x, int y, int w, int h);\n"
    "\n"
    "/* The circle pad, -127..127 on each axis, +x right and +y up, 0 inside a\n"
    " * small dead zone. */\n"
    "void hid_cpad(int *x, int *y);\n"
    "\n"
    "/* Shows the screens, then waits until one of `keys` is pressed (or touched,\n"
    " * with KEY_TOUCH) and returns which. HOME still works while it waits. */\n"
    "uint32_t hid_wait(uint32_t keys);\n"
    "\n"
    "/* WAV files, 8 or 16-bit PCM, any rate (above 32 kHz they load at half\n"
    " * rate), mono or stereo (mixed to mono). Sounds stay loaded until\n"
    " * snd_unload_all() and share a 10 MB pool. Sound needs app_from_home(). */\n"
    "\n"
    "/* A handle for the play functions, or -1 when the file cannot be read, is\n"
    " * not PCM, does not fit, or nothing can play it. */\n"
    "int snd_load(const char *path);\n"
    "/* Once, on whichever of the seven effect voices is free. volume 0..100. */\n"
    "void snd_play(int sound);\n"
    "void snd_play_volume(int sound, int volume);\n"
    "/* Looped on the music voice, replacing what was playing there. */\n"
    "void snd_music(int sound);\n"
    "void snd_music_volume(int sound, int volume);\n"
    "void snd_stop_music(void);\n"
    "/* Silences the effect voices; the music keeps playing. */\n"
    "void snd_stop_sounds(void);\n"
    "/* Stops everything and frees the pool; earlier handles become invalid. */\n"
    "void snd_unload_all(void);\n"
    "bool snd_available(void);\n"
    "\n"
    "/* Paths are on the SD card: \"/Aurora/x.txt\", \"Aurora/x.txt\", \"sdmc:/...\" and\n"
    " * \"0:/...\" all mean the same file, and \".\" and \"..\" work. The card is mounted\n"
    " * at the first use. fopen(), remove(), rename(), stat(), mkdir() and rmdir()\n"
    " * from the C library use these too. */\n"
    "\n"
    "#define FS_NAME_MAX 256\n"
    "\n"
    "typedef struct {\n"
    "  char name[FS_NAME_MAX]; /* UTF-8 */\n"
    "  uint32_t size;          /* bytes; 0 for a folder */\n"
    "  bool is_dir;\n"
    "} FsEntry;\n"
    "\n"
    "typedef struct FsDir FsDir;\n"
    "\n"
    "/* The entries of a folder, unsorted, without \".\" and \"..\". */\n"
    "FsDir *fs_dir_open(const char *path);\n"
    "bool fs_dir_read(FsDir *dir, FsEntry *entry); /* false at the end */\n"
    "void fs_dir_close(FsDir *dir);\n"
    "\n"
    "/* A whole file in memory from malloc(), with a 0 byte after the end so text\n"
    " * can be used as a string. NULL when it cannot be read. */\n"
    "void *fs_read_file(const char *path, size_t *size);\n"
    "/* Creates or replaces the file. */\n"
    "bool fs_write_file(const char *path, const void *data, size_t size);\n"
    "bool fs_exists(const char *path);\n"
    "bool fs_is_dir(const char *path);\n"
    "/* Creates the folder and any missing parents; true if it exists after. */\n"
    "bool fs_mkdir(const char *path);\n"
    "/* True when the SD card can be read. */\n"
    "bool fs_available(void);\n"
    "\n"
    "/* Since the app started. */\n"
    "uint32_t sys_millis(void);\n"
    "uint64_t sys_micros(void);\n"
    "/* Waits, keeping HOME working; it does not show the screens. */\n"
    "void sys_sleep(uint32_t ms);\n"
    "\n"
    "typedef struct {\n"
    "  int year;    /* e.g. 2026 */\n"
    "  int month;   /* 1..12 */\n"
    "  int day;     /* 1..31 */\n"
    "  int hour;    /* 0..23 */\n"
    "  int minute;\n"
    "  int second;\n"
    "  int weekday; /* 0 Sunday .. 6 Saturday */\n"
    "} SysTime;\n"
    "\n"
    "/* The clock as the Home Menu shows it (Settings > Clock included). False if\n"
    " * the clock could not be read. time() and localtime() give the same. */\n"
    "bool sys_time(SysTime *t);\n"
    "\n"
    "/* Battery charge 0..100, or -1 when unknown. */\n"
    "int sys_battery(void);\n"
    "/* The same in tenths of a percent (0..1000), or -1. */\n"
    "int sys_battery_tenths(void);\n"
    "bool sys_charging(void);\n"
    "/* The battery's temperature in degrees C: the only temperature the 3DS\n"
    " * reports (it has no CPU sensor software can read). SYS_TEMP_UNKNOWN when\n"
    " * the power chip does not answer. */\n"
    "#define SYS_TEMP_UNKNOWN (-1000)\n"
    "int sys_battery_temperature(void);\n"
    "/* The system voltage in millivolts (measured on the load side, so a little\n"
    " * under the battery's own), or -1. */\n"
    "int sys_battery_millivolts(void);\n"
    "/* New 3DS, New 3DS XL or New 2DS XL. Needs app_from_home(). */\n"
    "bool sys_is_new3ds(void);\n"
    "\n"
    "typedef enum {\n"
    "  SYS_LANG_ENGLISH = 0,\n"
    "  SYS_LANG_SPANISH = 1,\n"
    "  SYS_LANG_FRENCH = 2,\n"
    "} SysLanguage;\n"
    "\n"
    "/* From the user's settings (the setup wizard and Settings). */\n"
    "const char *sys_user_name(void); /* \"\" when not set */\n"
    "SysLanguage sys_language(void);\n"
    "uint32_t sys_accent(void); /* the accent colour, 0xRRGGBB */\n"
    "\n"
    "void sys_power_off(void) __attribute__((noreturn));\n"
    "void sys_reboot(void) __attribute__((noreturn));\n"
    "\n"
    "/* The network, over the Wi-Fi network saved in Settings > Wi-Fi. Plain HTTP\n"
    " * only: AuroraOS has no TLS yet, so https:// addresses fail. Every call waits\n"
    " * until it is done. The first net_connect() takes about 25 seconds, a request\n"
    " * usually well under one (at most 15). Meanwhile the screens keep their last\n"
    " * frame and the core can play no new sound or read touch and the circle pad,\n"
    " * unless a net_on_wait() handler draws something. HOME leaves once the call is\n"
    " * over. Needs app_from_home() and an AuroraOS as new as this SDK (ARM11\n"
    " * core 115 or later).\n"
    " *\n"
    " *   HttpResponse *r = net_connect() ? http_get(\"http://example.com/\") : NULL;\n"
    " *   if (r) {\n"
    " *     printf(\"%d, %u bytes\\n\", r->status, (unsigned)r->length);\n"
    " *     http_free(r);\n"
    " *   } else {\n"
    " *     printf(\"%s\\n\", net_error());\n"
    " *   }                                                                    */\n"
    "\n"
    "/* Whether this app can use the network at all; net_error() says why not. */\n"
    "bool net_available(void);\n"
    "/* Joins the saved network, unless the console is on it already (the Home\n"
    " * Menu, the Terminal or another app may have joined it). */\n"
    "bool net_connect(void);\n"
    "/* Asks the Wi-Fi chip whether the link is still up. */\n"
    "bool net_connected(void);\n"
    "/* Why the last call failed, in a short line: \"no network saved: pick one in\n"
    " * Settings > Wi-Fi\", \"example.com did not answer in time\", ... */\n"
    "const char *net_error(void);\n"
    "/* The console's address, \"192.168.1.20\", or \"\" when not connected. */\n"
    "const char *net_address(void);\n"
    "/* A host name to its address in text (\"93.184.215.14\"); `ip` holds 16. */\n"
    "bool net_resolve(const char *host, char ip[16]);\n"
    "/* One ping: the round trip in milliseconds, or -1. */\n"
    "int net_ping(const char *host);\n"
    "/* Called about ten times a second while a call waits, with the milliseconds\n"
    " * waited so far. It may draw and gfx_present() (the CPU copies the frames\n"
    " * then) and call hid_scan() for the buttons, but not make network calls or\n"
    " * play sounds. NULL: none. */\n"
    "void net_on_wait(void (*fn)(uint32_t ms));\n"
    "\n"
    "/* Plain TCP, once: connects to host:port, sends `length` bytes (2 KB at\n"
    " * most) and reads what comes back until the server closes the connection.\n"
    " * Copies up to `max` bytes to `reply` and returns how many came (at most\n"
    " * 1020 KB), or -1. */\n"
    "int net_tcp_exchange(const char *host, int port, const void *data,\n"
    "                     size_t length, void *reply, size_t max);\n"
    "\n"
    "typedef struct {\n"
    "  int status;     /* 200, 404, ...: whatever the server answered */\n"
    "  char *body;     /* `length` bytes, with a 0 byte after them */\n"
    "  size_t length;\n"
    "  char *headers;  /* the status line and headers, \"\\r\\n\" between lines */\n"
    "} HttpResponse;\n"
    "\n"
    "/* One HTTP request to an http:// address (a port and a query may follow the\n"
    " * host). Redirects are followed, up to five; one to https:// comes back as it\n"
    " * is, with net_error() saying so. NULL when no reply came. The request, with\n"
    " * its headers and body, must fit in 2 KB; the reply in 1020 KB (use\n"
    " * http_download() for bigger files). Free the result with http_free(). */\n"
    "HttpResponse *http_get(const char *url);\n"
    "/* `content_type` NULL is a form, \"application/x-www-form-urlencoded\". */\n"
    "HttpResponse *http_post(const char *url, const char *content_type,\n"
    "                        const void *data, size_t length);\n"
    "/* Any method. `headers` (may be NULL) are extra lines such as\n"
    " * \"Authorization: Bearer x\\r\\n\"; Content-Length is added for a body. */\n"
    "HttpResponse *http_request(const char *method, const char *url,\n"
    "                           const char *headers, const void *data,\n"
    "                           size_t length);\n"
    "/* A header's value (name without the colon, any case) copied into `value`. */\n"
    "bool http_header(const HttpResponse *r, const char *name, char *value,\n"
    "                 size_t max);\n"
    "void http_free(HttpResponse *r);\n"
    "\n"
    "/* Saves what `url` gives to a file, of any size: in pieces of up to 512 KB\n"
    " * when the server takes Range requests, as most do (otherwise the file must\n"
    " * fit in 1020 KB). It goes to `path`.part first and replaces `path` at the\n"
    " * end. `progress` (may be NULL) is called after each piece, with total 0 while\n"
    " * the size is unknown; it may draw and present. */\n"
    "bool http_download(const char *url, const char *path,\n"
    "                   void (*progress)(size_t done, size_t total));\n"
    "\n"
    "/* `text` percent-encoded for a query or a form (\"a b&c\" -> \"a%20b%26c\");\n"
    " * returns the length written, cut to fit `max` with its 0 byte. */\n"
    "size_t http_url_encode(char *out, size_t max, const char *text);\n"
    "\n"
    "#ifdef __cplusplus\n"
    "}\n"
    "#endif\n"
    "\n"
    "#endif\n"
    "# Auric v0.5: language reference\n"
    "\n"
    "*\"Coding too hard? Try Auric!\"*\n"
    "\n"
    "Auric is a tiny, statically-typed language that **transpiles to freestanding C**\n"
    "and compiles into a bootable app for [AuroraOS](../../README.md) on the Nintendo\n"
    "3DS. This document is the complete v0.5 spec, the language is deliberately\n"
    "small.\n"
    "\n"
    "## A whole program\n"
    "\n"
    "```auric\n"
    "fn main() {\n"
    "    clear(BLACK);\n"
    "    print(\"Coding too hard? Try Auric!\", 40, 108, AURORA);\n"
    "    print(\"Press A to exit.\", 40, 140, WHITE);\n"
    "    wait_key(KEY_A);\n"
    "}\n"
    "```\n"
    "\n"
    "Every program needs a `fn main()` that takes no parameters and returns nothing;\n"
    "it is the entry point the runtime calls.\n"
    "\n"
    "## Types\n"
    "\n"
    "| Type      | Meaning                     | Lowers to C     |\n"
    "|-----------|-----------------------------|-----------------|\n"
    "| `int`     | 32-bit signed integer       | `int`           |\n"
    "| `float`   | 32-bit float (soft-float)   | `float`         |\n"
    "| `bool`    | `true` / `false`            | `int` (0 or 1)  |\n"
    "| `string`  | immutable text literal      | `const char *`  |\n"
    "| `T[N]`    | fixed-length array of `T`   | `T name[N]`     |\n"
    "\n"
    "There are no implicit conversions: you cannot mix `int` and `float` in one\n"
    "expression, and conditions must be `bool`.\n"
    "\n"
    "## Globals\n"
    "\n"
    "A `let` outside any function is a global, visible to every function:\n"
    "\n"
    "```auric\n"
    "let score = 0;\n"
    "let board: int[200];\n"
    "\n"
    "fn bump() { score = score + 1; }\n"
    "```\n"
    "\n"
    "A global's initializer must be a **constant expression**: literals, predefined\n"
    "constants, and arithmetic on them. It becomes a C file-scope initializer, so\n"
    "`let n = some_call();` at global scope is rejected.\n"
    "\n"
    "## Arrays\n"
    "\n"
    "```auric\n"
    "let board: int[200];    // always zero-filled; there is no initializer syntax\n"
    "board[3] = 7;\n"
    "let v = board[3];\n"
    "```\n"
    "\n"
    "Arrays may be global or local, and the index must be `int`. **Arrays cannot be\n"
    "passed to functions**, declare them global and let functions reach them\n"
    "directly. There is no bounds checking: indexing past the end corrupts memory,\n"
    "exactly as in C.\n"
    "\n"
    "## Functions\n"
    "\n"
    "```auric\n"
    "fn name(param: type, ...) -> ret_type {\n"
    "    ...\n"
    "    return value;\n"
    "}\n"
    "```\n"
    "\n"
    "* The `-> ret_type` is omitted for functions that return nothing (void).\n"
    "* Functions may be called before they are defined (forward-declared for you).\n"
    "* Recursion is allowed.\n"
    "\n"
    "## Statements\n"
    "\n"
    "```auric\n"
    "let x = 1 + 2;        // type inferred (int)\n"
    "let y: float = 3.5;   // explicit type\n"
    "x = x + 1;            // assignment (variable must already exist)\n"
    "\n"
    "if cond { ... } else if other { ... } else { ... }\n"
    "\n"
    "while cond { ... }\n"
    "break;                // leave the innermost loop\n"
    "continue;             // next iteration of the innermost loop\n"
    "\n"
    "return;               // in a void function\n"
    "return expr;          // in a value-returning function\n"
    "```\n"
    "\n"
    "Conditions are **not** parenthesized (`if x < 3 { ... }`). Blocks always use\n"
    "braces. Each `{ ... }` block is its own scope.\n"
    "\n"
    "## Operators\n"
    "\n"
    "| Category   | Operators               | Operand types                             | Result "
    "|\n"
    "|------------|-------------------------|-------------------------------------------|--------|"
    "\n"
    "| Arithmetic | `+` `-` `*` `/` `%`     | int x int or float x float (`%` int only) | same   "
    "|\n"
    "| Bitwise    | `&` `\\|` `^` `<<` `>>`   | int x int                                 | `int`  "
    "|\n"
    "| Comparison | `<` `<=` `>` `>=`       | int x int or float x float                | `bool` "
    "|\n"
    "| Equality   | `==` `!=`               | any matching type; strings by their text  | `bool` "
    "|\n"
    "| Logical    | `&&` `\\|\\|` `!`           | `bool`                                    | "
    "`bool` |\n"
    "| Unary      | `-` (negate), `!` (not) | number / bool                             | same   "
    "|\n"
    "\n"
    "Precedence (low -> high): `||`, `&&`, `|`, `^`, `&`, equality, comparison,\n"
    "shifts, `+ -`, `* / %`, unary, the same order as C, so `a & b == c` means\n"
    "`a & (b == c)`. Parenthesize bit tests: `(flags & MASK) != 0`.\n"
    "\n"
    "## Built-in functions\n"
    "\n"
    "These map one-to-one onto AuroraOS's API through the runtime shim\n"
    "([`../runtime`](../runtime)). Drawing goes to the screen picked with\n"
    "`screen()`: the **top screen** (400x240) unless the app asks for the bottom one\n"
    "(320x240). Coordinates start at the top-left corner; text uses the 8x8 font.\n"
    "\n"
    "| Built-in                          | Does                                             | "
    "AuroraOS call     |\n"
    "|-----------------------------------|--------------------------------------------------|------"
    "-------------|\n"
    "| `screen(which)`                   | draw on `TOP` or `BOTTOM` from now on             | "
    "`VRAM_TOP_LA` / `VRAM_BOT_A` |\n"
    "| `print(text, x, y, color)`        | draw `text` at pixel `(x, y)`                     | "
    "`draw_string`     |\n"
    "| `print_int(value, x, y, color)`   | draw a signed decimal number                      | "
    "`draw_string`     |\n"
    "| `clear(color)`                    | fill the screen; remembers `color` as its text bg | "
    "`clear_screen`    |\n"
    "| `fill_rect(x, y, w, h, color)`    | fill a rectangle (use it to draw bands/shapes)    | "
    "`draw_filled_rect`|\n"
    "| `wait_key(button)`                | block until `button` is newly pressed             | "
    "`get_keys_down`   |\n"
    "| `keys_down() -> int`              | buttons newly pressed since the last call         | "
    "`get_keys_down`   |\n"
    "| `keys_held() -> int`              | buttons currently held                            | "
    "`get_keys`        |\n"
    "| `touch_down() -> bool`            | a touch began since the last call                 | "
    "`touch_read`      |\n"
    "| `touch_held() -> bool`            | the bottom screen is being touched                | "
    "`touch_read`      |\n"
    "| `touch_up() -> bool`              | the stylus lifted since the last call             | "
    "`touch_read`      |\n"
    "| `touch_x() -> int`                | x of the touch, or of the last one (-1: none yet) | "
    "`touch_read`      |\n"
    "| `touch_y() -> int`                | y of the touch, or of the last one (-1: none yet) | "
    "`touch_read`      |\n"
    "| `touch_in(x, y, w, h) -> bool`    | that position lies inside the rectangle           | "
    "`touch_in`        |\n"
    "| `buffered(on)`                    | draw off-screen until `present()`                 | "
    "`screen_use_backbuffer` |\n"
    "| `present()`                       | show the off-screen frame on both screens         | "
    "`screen_present_top` / `_bottom` |\n"
    "| `rand(n) -> int`                  | pseudo-random int in `[0, n)`                     | "
    "seeded from the RTC |\n"
    "| `millis() -> int`                 | milliseconds since the app started                 | "
    "ARM9 hardware timer |\n"
    "| `delay(cycles)`                   | busy-wait ~`cycles` iterations                     | "
    "`delay`           |\n"
    "| `load_sound(path) -> int`         | read a WAV from the SD card; `-1` if it cannot     | "
    "`wav_load`        |\n"
    "| `play_sound(handle)`              | play once, on a free effect voice                  | "
    "ARM11 voice       |\n"
    "| `play_music(handle)`              | loop on the music voice                            | "
    "ARM11 voice       |\n"
    "| `stop_music()`                    | silence the music                                  | "
    "ARM11 voice       |\n"
    "| `stop_sounds()`                   | silence every effect                               | "
    "ARM11 voice       |\n"
    "| `net_connect() -> bool`           | join the Wi-Fi network saved in Settings           | "
    "ARM11 Wi-Fi       |\n"
    "| `net_online() -> bool`            | the network link is up                             | "
    "ARM11 Wi-Fi       |\n"
    "| `net_error() -> string`           | why the last network call failed                   | "
    "`src/os/AppNet.c` |\n"
    "| `net_address() -> string`         | the console's address on the network               | "
    "ARM11 Wi-Fi       |\n"
    "| `http_get(url) -> int`            | an HTTP GET; the reply's status, 0 if none         | "
    "ARM11 TCP         |\n"
    "| `http_post(url, body) -> int`     | an HTTP POST                                       | "
    "ARM11 TCP         |\n"
    "| `http_param(name, value)`         | a parameter for the next request (`_int`: a number) | "
    "`src/os/AppNet.c` |\n"
    "| `http_header(name, value)`        | a header for the next request                      | "
    "`src/os/AppNet.c` |\n"
    "| `http_text() -> string`           | the last reply's body                              | in "
    "place          |\n"
    "| `http_length() -> int`            | its size in bytes                                  | in "
    "place          |\n"
    "| `http_lines() -> int`             | how many lines it has                              | in "
    "place          |\n"
    "| `http_line(n) -> string`          | line `n`, from 0                                   | in "
    "place          |\n"
    "| `http_save(path) -> bool`         | write the last reply to the SD card                | "
    "FatFs             |\n"
    "| `http_download(url, path) -> bool` | save a file of any size, in pieces                | "
    "ARM11 TCP, FatFs  |\n"
    "| `json_string(path) -> string`     | a value of the last reply, read as JSON            | "
    "`src/os/Json.c`   |\n"
    "| `json_int` / `json_float` / `json_bool(path)` | the same as a number or a truth value | "
    "`src/os/Json.c`   |\n"
    "| `json_has(path) -> bool`          | the value is there                                 | "
    "`src/os/Json.c`   |\n"
    "| `json_count(path) -> int`         | the elements of an array, members of an object     | "
    "`src/os/Json.c`   |\n"
    "| `json_index(n)`                   | what `#` stands for in the paths after it          |    "
    "               |\n"
    "\n"
    "### Animation and games\n"
    "\n"
    "`wait_key` blocks, which is fine for a slideshow but useless for a game. Poll\n"
    "with `keys_down()` (edge, fires once per press) and `keys_held()` (level, true\n"
    "while held), testing the `KEY_*` masks with `(k & KEY_A) != 0`.\n"
    "\n"
    "Call `buffered(true)` once at startup and `present()` at the end of each frame.\n"
    "Without it every drawing call goes straight to the panel and the player watches\n"
    "the frame being built. When the app was launched from the Home Menu, `present()`\n"
    "is a **GPU blit**; the ARM11 core is still resident, so a full-screen present\n"
    "costs one DMA rather than a 288 KB CPU copy. A directly booted app falls back to\n"
    "the CPU copy.\n"
    "\n"
    "Buffered mode also means **the backbuffer keeps what you drew last frame**, so\n"
    "draw fixed chrome once and repaint only what moves.\n"
    "\n"
    "**Never pace a game with `delay()`.** Drive it with `millis()`, which reads an\n"
    "ARM9 hardware timer:\n"
    "\n"
    "```auric\n"
    "let next_step = millis() + 500;\n"
    "while true {\n"
    "    let now = millis();\n"
    "    if now >= next_step {\n"
    "        next_step = now + 500;\n"
    "        // ... one step of the simulation ...\n"
    "    }\n"
    "    draw();\n"
    "    present();\n"
    "}\n"
    "```\n"
    "\n"
    "That way the speed of the game is fixed in real time and the frame rate only\n"
    "affects how smooth it looks. See `../../Games/Tetris_Source/Tetris.aur` for a\n"
    "complete game built this way.\n"
    "\n"
    "### Two screens\n"
    "\n"
    "An app starts out drawing on the top screen. `screen(BOTTOM)` sends every\n"
    "drawing built-in after it (`clear`, `print`, `print_int`, `fill_rect`) to the\n"
    "bottom screen, and `screen(TOP)` sends them back:\n"
    "\n"
    "```auric\n"
    "screen(TOP);\n"
    "clear(BLACK);\n"
    "print(\"Score\", 8, 8, WHITE);\n"
    "\n"
    "screen(BOTTOM);\n"
    "clear(DARK_GRAY);\n"
    "fill_rect(120, 100, 80, 40, AURORA);   // a button\n"
    "print(\"Start\", 140, 116, BLACK);\n"
    "```\n"
    "\n"
    "* The bottom screen is 320x240, so a line of the 8x8 font holds 40 characters\n"
    "  there instead of 50. Drawing past an edge is clipped, on either screen.\n"
    "* Each screen keeps its own text background: `clear()` sets it for the screen\n"
    "  it fills.\n"
    "* Until an app draws on the bottom screen, it keeps the Home Menu's last\n"
    "  picture (the \"Launch app\" card). Clear it at startup if the app uses it.\n"
    "* In buffered mode `present()` shows both screens, but copies only the ones\n"
    "  drawn on since the previous `present()`: a game that redraws only the top\n"
    "  each frame pays for one copy.\n"
    "\n"
    "### Touch\n"
    "\n"
    "The bottom screen is a touchscreen. Positions are in its pixels, `x` from 0 to\n"
    "319 and `y` from 0 to 239, corrected by the user's Settings > Touch Calibration.\n"
    "\n"
    "`touch_down()` and `touch_up()` are edges, like `keys_down()`: each is true once\n"
    "when a touch begins or the stylus lifts, counted since that built-in was last\n"
    "called. `touch_held()` is the level: true while the stylus is down.\n"
    "`touch_x()` and `touch_y()` give where the stylus is, or where it was when it\n"
    "lifted (both `-1` before the first touch), and `touch_in(x, y, w, h)` tests\n"
    "that position against a rectangle:\n"
    "\n"
    "```auric\n"
    "// A button that acts on the tap:\n"
    "if touch_down() && touch_in(120, 100, 80, 40) {\n"
    "    start_game();\n"
    "}\n"
    "\n"
    "// Or only when the stylus lifts on it, so a touch can slide off to cancel:\n"
    "if touch_up() && touch_in(120, 100, 80, 40) {\n"
    "    start_game();\n"
    "}\n"
    "\n"
    "// Drawing while the stylus is down:\n"
    "if touch_held() {\n"
    "    screen(BOTTOM);\n"
    "    fill_rect(touch_x() - 1, touch_y() - 1, 3, 3, WHITE);\n"
    "}\n"
    "```\n"
    "\n"
    "* A touch already on the screen when the app first asks is ignored until the\n"
    "  stylus lifts, so the tap that launched the app does not press anything.\n"
    "* The touchscreen is sampled, so a quick stroke arrives as points some pixels\n"
    "  apart. Join them to draw a line, as `../examples/paint.aur` does.\n"
    "* The first touch built-in an app calls reads the calibration from\n"
    "  `SD:/Aurora/USER.dat`, once.\n"
    "* Like sound, touch needs the ARM11 core, so it works only when the app was\n"
    "  launched from the Home Menu. In a directly booted app the screen is never\n"
    "  touched: `touch_held()` stays false and `touch_x()` stays `-1`.\n"
    "\n"
    "`../examples/paint.aur` is a small drawing app that uses both screens and the\n"
    "stylus.\n"
    "\n"
    "### Sound\n"
    "\n"
    "Sounds are WAV files on the SD card, loaded once and then played by handle:\n"
    "\n"
    "```auric\n"
    "let jump = -1;\n"
    "let theme = -1;\n"
    "\n"
    "fn main() {\n"
    "    jump = load_sound(\"Aurora/Apps/MYGAME/JUMP.WAV\");\n"
    "    theme = load_sound(\"Aurora/Apps/MYGAME/THEME.WAV\");\n"
    "    play_music(theme);\n"
    "    // ... and on an event:\n"
    "    play_sound(jump);\n"
    "}\n"
    "```\n"
    "\n"
    "* The path is counted from the root of the card. Long file names work, and\n"
    "  letter case does not matter. Keep an app's files in a folder beside it, named\n"
    "  after it (see `docs/apps.md`).\n"
    "* Any uncompressed 8 or 16-bit PCM WAV works, mono or stereo. Stereo is mixed to\n"
    "  mono, and a file above 32 kHz loads at half its rate, which halves the memory\n"
    "  it takes. `tools/sound_prep.py` converts files ahead of time to mono 22,050 Hz,\n"
    "  which also makes them quicker to load.\n"
    "* Every sound shares one 10 MB pool, nearly four minutes of 22 kHz mono, and at\n"
    "  most 32 can be loaded.\n"
    "* `load_sound` returns `-1` when a file is missing or unusable, and playing `-1`\n"
    "  does nothing, so a game still runs without its sounds.\n"
    "* Up to seven effects play at once alongside the music; another takes over one\n"
    "  of their voices. `play_music` loops, replacing any music already playing, at\n"
    "  about 60% volume so effects carry over it.\n"
    "* Sound needs the ARM11 core, so, like the GPU present, it works only when the\n"
    "  app was launched from the Home Menu. In a directly booted app every\n"
    "  `load_sound` returns `-1`.\n"
    "* HOME stops every sound before the Home Menu comes back.\n"
    "\n"
    "Loading reads the whole file, so a long track takes a moment: draw a message and\n"
    "`present()` first, as `Games/Tetris_Source/Tetris.aur` does.\n"
    "\n"
    "### The network\n"
    "\n"
    "An app can reach the internet over the Wi-Fi network saved in Settings > Wi-Fi.\n"
    "It speaks plain HTTP: AuroraOS has no TLS yet, so `https://` addresses do not\n"
    "work (many sites send `http://` visitors to `https://`; such a reply comes back\n"
    "as its 301 or 302, with `net_error()` saying so).\n"
    "\n"
    "```auric\n"
    "fn main() {\n"
    "    print(\"Connecting...\", 8, 8, WHITE);\n"
    "    if !net_connect() {\n"
    "        print(net_error(), 8, 24, RED);\n"
    "        wait_key(KEY_A);\n"
    "        return;\n"
    "    }\n"
    "    if http_get(\"http://ip-api.com/json/\") == 200 {\n"
    "        print(json_string(\"city\"), 8, 24, WHITE);\n"
    "    } else {\n"
    "        print(net_error(), 8, 24, RED);\n"
    "    }\n"
    "    wait_key(KEY_A);\n"
    "}\n"
    "```\n"
    "\n"
    "* `net_connect()` joins the saved network, unless the console is on it\n"
    "  already (aShop, the Terminal or another app may have joined it, and a join\n"
    "  stays up until the console is turned off or the network drops). The first\n"
    "  join takes about 25\n"
    "  seconds, so draw a message before it. It returns `false` when it cannot, and\n"
    "  `net_error()` says why, e.g. \"no network saved: pick one in Settings >\n"
    "  Wi-Fi\".\n"
    "* Every network built-in waits until it is done, and the screens keep their\n"
    "  last frame meanwhile. A request usually takes well under a second, and at\n"
    "  most 15.\n"
    "* `http_get(url)` and `http_post(url, body)` return the reply's status (200,\n"
    "  404, ...), or `0` when no reply came (not connected, no such host, no\n"
    "  answer; `net_error()` says which). Redirects are followed. The address may\n"
    "  carry a port and a query: `\"http://example.com:8080/api?q=1\"`.\n"
    "* The reply stays until the next request: `http_text()` is its body as text,\n"
    "  `http_length()` its size in bytes, `http_lines()` and `http_line(n)` its\n"
    "  lines (from 0, without their line ends), and `http_save(path)` writes it to\n"
    "  the SD card. A reply can be up to 1020 KB. `http_download(url, path)` saves\n"
    "  a file of any size, in pieces, without keeping it as the reply.\n"
    "* `http_param(name, value)` and `http_param_int(name, value)` add a parameter\n"
    "  to the next request's address (`?name=value&...`, encoded for you), and\n"
    "  `http_header(name, value)` a header (an API key, say). They count for that\n"
    "  one request. `http_post` with an empty body sends the parameters as the\n"
    "  body, as a web form does; a body that starts with `{` or `[` is sent as\n"
    "  JSON, anything else as a form, unless a `Content-Type` header says\n"
    "  otherwise.\n"
    "* A request, with its address, headers and body, must fit in 2 KB.\n"
    "* `net_online()` asks whether the link is still up, and `net_address()` is\n"
    "  the console's address on the network (`\"\"` when not connected).\n"
    "* Like sound, the network needs the ARM11 core, so it works only in an app\n"
    "  launched from the Home Menu. HOME pressed during a network built-in takes\n"
    "  effect when it is done.\n"
    "\n"
    "#### JSON\n"
    "\n"
    "Most web APIs answer in JSON. `json_string(path)`, `json_int(path)`,\n"
    "`json_float(path)`, `json_bool(path)`, `json_has(path)` and `json_count(path)`\n"
    "read the last reply as JSON. A path names members of objects and elements of\n"
    "arrays, separated by dots: `\"items.0.name\"` is the `name` of the first element\n"
    "of `items`, and `\"\"` is the whole reply. A value that is not there gives `\"\"`,\n"
    "`0`, `0.0` or `false` (and `json_has` says `false`).\n"
    "\n"
    "```auric\n"
    "// {\"city\": \"Paris\", \"temp\": 21.5, \"items\": [{\"name\": \"one\"}, {\"name\": "
    "\"two\"}]}\n"
    "print(json_string(\"city\"), 8, 8, WHITE);\n"
    "let t = json_float(\"temp\");\n"
    "let n = json_count(\"items\");\n"
    "let i = 0;\n"
    "while i < n {\n"
    "    json_index(i);\n"
    "    print(json_string(\"items.#.name\"), 8, 24 + i * 10, WHITE);\n"
    "    i = i + 1;\n"
    "}\n"
    "```\n"
    "\n"
    "* `json_index(n)` sets what a `#` in the paths after it stands for, to walk\n"
    "  through an array.\n"
    "* `json_int` rounds, and reads a number written as a string (`\"17\"`) too.\n"
    "  `json_string` of a number, `true` or a whole object gives it as written.\n"
    "* Text outside ASCII shows as `?`, as the 8x8 font has no other letters.\n"
    "* **Strings from a reply** (`http_text`, `http_line`, `json_string`,\n"
    "  `net_error`) **change at the next request**, even one kept in a variable:\n"
    "  print or compare them before asking again, and keep numbers with `json_int`.\n"
    "\n"
    "`../examples/net.aur` joins, asks ip-api.com where the console is online and\n"
    "sends a score to httpbin.org with `http_param` and `http_post`.\n"
    "\n"
    "### The HOME button\n"
    "\n"
    "You don't handle HOME yourself: the runtime checks it during built-in calls\n"
    "(at least every 12 ms) and, when launched from the AuroraOS home menu, returns\n"
    "there on a press. Keep a `delay(...)` (or another built-in) in any long-running\n"
    "loop so HOME stays responsive. See `../../sample/rainbow.aur`. Returning from\n"
    "`fn main` also goes back to the home menu.\n"
    "\n"
    "L + Down + Select opens AuroraOS's system menu over the app (drawn by the OS,\n"
    "not by your program). While it is open, your program waits at its next button\n"
    "read; its Return to Home leaves the app like HOME. Rebuild older apps with this\n"
    "runtime for that. `examples/system_menu.aur` uses Down and Select itself to\n"
    "show that the chord still reaches the menu.\n"
    "\n"
    "### App icons\n"
    "\n"
    "Every compiled app embeds a 32x32 icon that the home menu shows. Supply your own\n"
    "with `aurc build ... --icon my.icon` (a text bitmap: 32 lines, `#` = on); without\n"
    "`--icon`, a default icon is used.\n"
    "\n"
    "## Predefined constants\n"
    "\n"
    "**Colours** (packed `0xRRGGBB`, unpacked to an AuroraOS `Color` by the shim):\n"
    "\n"
    "`BLACK` `WHITE` `RED` `GREEN` `BLUE` `CYAN` `MAGENTA` `YELLOW` `ORANGE`\n"
    "`AURORA` `GRAY` `DARK_GRAY`\n"
    "\n"
    "A colour is just an `int`, so you can also pass a raw literal like `0xFF8800`.\n"
    "\n"
    "**Buttons** (HID bitmasks, matching `BUTTON_*` in `include/aurora.h`):\n"
    "\n"
    "`KEY_A` `KEY_B` `KEY_X` `KEY_Y` `KEY_SELECT` `KEY_START` `KEY_UP` `KEY_DOWN`\n"
    "`KEY_LEFT` `KEY_RIGHT` `KEY_L` `KEY_R`\n"
    "\n"
    "HOME is not in this list: the runtime handles it for you (see above).\n"
    "\n"
    "**Screens** (for `screen()`): `TOP` `BOTTOM`\n"
    "\n"
    "You cannot declare a variable or function that reuses a predefined name.\n"
    "\n"
    "## Comments\n"
    "\n"
    "```auric\n"
    "// line comment\n"
    "/* block comment */\n"
    "```\n"
    "\n"
    "## What Auric still leaves out\n"
    "\n"
    "Structs, pointers, `for` loops, string operations (joining, slicing; `==`\n"
    "compares), float<->int conversion, multi-dimensional arrays, and passing\n"
    "arrays to functions. The scope stays\n"
    "deliberately small; these are candidates for later versions.\n"
    "\n"
    "Flatten a 2D grid by hand until then, `board[row * WIDTH + col]`, as\n"
    "`Games/Tetris_Source/Tetris.aur` does.\n"
    "\n";
static const char *reference_text(void) { return offline_reference; }
static void complete(void) {
    Document *d = doc();
    if (!d)
        return;
    size_t a = d->cursor;
    while (a && wordchar((unsigned char)d->text[prevpos(d, a)]))
        a = prevpos(d, a);
    size_t n = d->cursor - a;
    if (!n) {
        status("Type an identifier prefix first");
        return;
    }
    complete_start = a;
    ncomplete = complete_selected = 0;
    scan_completion(offline_reference, sizeof(offline_reference) - 1, d->text + a, n);
    for (int i = 0; i < ndoc; i++)
        scan_completion(docs[i].text, docs[i].len, d->text + a, n);
    if (!ncomplete) {
        status("No identifier suggestions");
        return;
    }
    panel = P_COMPLETE;
    redraw = true;
}
int main(void) {
    config_load();
    recent_load();
    if (!fs_mkdir(app_dir()) || !fs_mkdir(cfg.project)) {
        con_init(SCREEN_TOP);
        printf("Cannot open SD/project folder.\n");
        hid_wait(KEY_A);
        return 1;
    }
    atexit(editor_exit_checkpoint);
    restore_session();
    if (!ndoc) {
        char p[280];
        if (project_path(p, sizeof(p), cfg.entry) && fs_exists(p))
            open_document(cfg.entry);
        else
            new_document(cfg.entry, suffix(cfg.entry, ".c") ? c_template : aur_template);
    }
    last_checkpoint = now();
    uint32_t observed_revision = 0;
    int observed_doc = -1;
    size_t observed_cursor = NONE;
    while (app_loop()) {
        handle_buttons();
        handle_touch();
        cpad_scroll();
        build_step();
        Document *d = doc();
        if (active != observed_doc ||
            (d && (observed_revision != d->revision || observed_cursor != d->cursor))) {
            manual_scroll = false;
            observed_doc = active;
            observed_revision = d ? d->revision : 0;
            observed_cursor = d ? d->cursor : NONE;
            redraw = true;
            blink = true;
            blink_at = now();
        }
        if ((uint32_t)(now() - blink_at) > 500) {
            blink = !blink;
            blink_at = now();
            redraw = true;
        }
        if ((uint32_t)(now() - last_checkpoint) > 15000)
            checkpoint();
        if (redraw) {
            render_top();
            render_bottom();
            redraw = false;
        }
    }
    return 0;
}
#endif
