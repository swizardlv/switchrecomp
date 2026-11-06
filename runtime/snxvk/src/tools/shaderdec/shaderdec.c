/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* shaderdec: generates the Maxwell instruction decoder from its encodings (src/nx/gpu/shader/maxwell_opcodes.def).
   It checks the spec (row syntax, names, patterns, and that two patterns match the same word only when one fixes
   every bit the other does and more) and writes two files:
   - the opcode enum, O_NONE, then O_<NAME> for each name in the order it first appears, then O__COUNT
   - the decoder's data and matcher, included by decode.c after translator.h: the rows as Op, most fixed bits first
     (so a more specific pattern is tried before the one it narrows), each opcode's first row, and a table indexed
     by the top 12 bits of the word that names the few rows that can match it, which shaderdec_match() checks.
   Nothing is built at run time. It is strict C99 and depends on nothing, so a cross build can build it for the
   machine it runs on.

   Usage: shaderdec --enum OUT.h --decoder OUT.inc SPEC
   Outputs are only rewritten when they change. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS   1024
#define MAX_NAME   32
#define MAX_SHOW   48
#define INDEX_BITS 12
#define INDEX_SIZE (1 << INDEX_BITS)
#define END_MARK   0xFFFFu

typedef struct {
    char name[MAX_NAME];
    char show[MAX_SHOW];
    unsigned mask, val; /* the top 16 bits */
    int bits, line, alias;
} Row;

static Row rows[MAX_ROWS];
static int nrows;
static int order[MAX_ROWS]; /* rows by priority */
static const char* spec_path;

static char* obuf;
static size_t olen, ocap;

static void fail(int line, const char* fmt, ...) {
    va_list ap;
    if (line)
        fprintf(stderr, "%s:%d: ", spec_path, line);
    else
        fprintf(stderr, "shaderdec: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void out(const char* fmt, ...) {
    char line[512];
    size_t n;
    va_list ap;
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    n = strlen(line);
    if (olen + n + 1 > ocap) {
        ocap = (olen + n + 1) * 2;
        obuf = (char*)realloc(obuf, ocap);
        if (!obuf) fail(0, "out of memory");
    }
    memcpy(obuf + olen, line, n + 1);
    olen += n;
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
static void flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)malloc(olen + 1);
        size_t got = old ? fread(old, 1, olen + 1, f) : 0;
        int same = old && got == olen && !memcmp(old, obuf, olen);
        free(old);
        fclose(f);
        if (same) {
            olen = 0;
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(obuf, 1, olen, f) != olen || fclose(f)) fail(0, "cannot write %s", path);
    olen = 0;
}

static const char* skip_space(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* a quoted field into buf; returns the text after it */
static const char* quoted(const char* p, char* buf, size_t size, int line, const char* what) {
    size_t n = 0;
    p = skip_space(p);
    if (*p != '"') fail(line, "expected the %s in quotes", what);
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' || !isprint((unsigned char)*p)) fail(line, "the %s may only hold printable characters", what);
        if (n + 1 >= size) fail(line, "the %s is too long", what);
        buf[n++] = *p;
    }
    if (*p != '"') fail(line, "the %s has no closing quote", what);
    buf[n] = 0;
    if (!n) fail(line, "the %s is empty", what);
    return p + 1;
}

static void parse_row(const char* p, int line) {
    Row* r;
    char pattern[64], word[32];
    size_t n = 0;
    int i, bit;
    if (nrows == MAX_ROWS) fail(line, "more than %d rows", MAX_ROWS);
    r = &rows[nrows];
    memset(r, 0, sizeof *r);
    r->line = line;
    if (!isupper((unsigned char)*p)) fail(line, "a name starts with a capital letter");
    while (isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_') {
        if (n + 1 >= sizeof r->name) fail(line, "the name is too long");
        r->name[n++] = *p++;
    }
    r->name[n] = 0;
    if (*p != ' ' && *p != '\t') fail(line, "a name is capital letters, digits and _");
    if (!strcmp(r->name, "NONE")) fail(line, "NONE is the enum's \"no opcode\" and can't be a row");
    p = quoted(p, r->show, sizeof r->show, line, "display name");
    p = quoted(p, pattern, sizeof pattern, line, "pattern");
    for (i = 0, bit = 15; pattern[i]; i++) {
        char c = pattern[i];
        if (c == ' ') continue;
        if (c != '0' && c != '1' && c != '-') fail(line, "the pattern may only hold 0, 1, - and spaces");
        if (bit < 0) fail(line, "the pattern has more than 16 bits");
        if (c != '-') {
            r->mask |= 1u << bit;
            r->bits++;
            if (c == '1') r->val |= 1u << bit;
        }
        bit--;
    }
    if (bit >= 0) fail(line, "the pattern has fewer than 16 bits");
    p = skip_space(p);
    if (*p) {
        n = 0;
        while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof word) word[n++] = *p++;
        word[n] = 0;
        if (strcmp(word, "alias")) fail(line, "unknown attribute '%s'", word);
        r->alias = 1;
        if (*skip_space(p)) fail(line, "text after the attributes");
    }
    nrows++;
}

static void read_spec(const char* path) {
    char line[512];
    int number = 0;
    FILE* f = fopen(path, "r");
    if (!f) fail(0, "cannot open %s", path);
    while (fgets(line, sizeof line, f)) {
        char* p;
        size_t n = strlen(line);
        number++;
        if (n && line[n - 1] != '\n' && !feof(f)) fail(number, "line too long");
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = 0;
        p = (char*)skip_space(line);
        if (!*p || *p == '#') continue;
        parse_row(p, number);
    }
    fclose(f);
    if (!nrows) fail(0, "%s has no rows", path);
}

/* the first row of name, before row limit, or -1 */
static int first_row(const char* name, int limit) {
    int i;
    for (i = 0; i < limit; i++)
        if (!strcmp(rows[i].name, name)) return i;
    return -1;
}

static void check_spec(void) {
    int i, j;
    for (i = 0; i < nrows; i++) {
        int first = first_row(rows[i].name, i);
        if (first >= 0 && !rows[i].alias)
            fail(rows[i].line, "%s is already a row at line %d: mark a further encoding of it alias", rows[i].name,
                 rows[first].line);
        if (first < 0 && rows[i].alias)
            fail(rows[i].line, "alias %s names no opcode of an earlier row", rows[i].name);
        for (j = 0; j < i; j++) {
            unsigned common = rows[i].mask & rows[j].mask;
            if (!strcmp(rows[i].show, rows[j].show))
                fail(rows[i].line, "display name \"%s\" is also line %d's", rows[i].show, rows[j].line);
            if ((rows[i].val ^ rows[j].val) & common) continue;
            /* both match some word: fine only when one pattern narrows the other */
            if (rows[i].mask != rows[j].mask && (common == rows[i].mask || common == rows[j].mask)) continue;
            fail(rows[i].line, "%s and %s (line %d) both match %04x and neither pattern narrows the other",
                 rows[i].show, rows[j].show, rows[j].line, rows[i].val | rows[j].val);
        }
    }
}

/* most fixed bits first, spec order among equals: a pattern always comes before the ones it narrows */
static void sort_rows(void) {
    int i, j;
    for (i = 0; i < nrows; i++) {
        int r = i;
        for (j = i; j > 0 && rows[order[j - 1]].bits < rows[r].bits; j--) order[j] = order[j - 1];
        order[j] = r;
    }
}

static void write_enum(const char* path) {
    int i;
    out("/* Generated by tools/shaderdec/shaderdec.c from maxwell_opcodes.def: change the spec, not this file. */\n");
    out("#ifndef SNX_MAXWELL_OPCODES_H\n#define SNX_MAXWELL_OPCODES_H\n");
    out("/* Maxwell opcodes, O_NONE for none */\nenum {\n    O_NONE = 0,\n");
    for (i = 0; i < nrows; i++)
        if (!rows[i].alias) out("    O_%s,\n", rows[i].name);
    out("    O__COUNT\n};\n#endif\n");
    flush(path);
}

static void write_decoder(const char* path) {
    static unsigned short lists[INDEX_SIZE * 8], index_of[INDEX_SIZE];
    unsigned short list[MAX_ROWS + 1];
    int nlist = 0, i, k, n, total = 0, longest = 0;
    out("/* Generated by tools/shaderdec/shaderdec.c from maxwell_opcodes.def: change the spec, not this file.\n");
    out("   Included by decode.c after translator.h. */\n");
    out("/* the rows, most fixed bits first */\nstatic const Op shaderdec_ops[%d] = {\n", nrows);
    for (i = 0; i < nrows; i++) {
        const Row* r = &rows[order[i]];
        out("    {O_%s, \"%s\", (u64)0x%04xu << 48, (u64)0x%04xu << 48, %d},\n", r->name, r->show, r->mask, r->val,
            r->bits);
    }
    out("};\n");
    out("/* each opcode's first row in shaderdec_ops */\nstatic const unsigned short shaderdec_row[O__COUNT] = {\n");
    out("    0x%04x, /* O_NONE */\n", END_MARK);
    for (i = 0; i < nrows; i++) {
        if (rows[i].alias) continue;
        for (k = 0; k < nrows && strcmp(rows[order[k]].name, rows[i].name); k++) {
        }
        out("    %d, /* O_%s */\n", k, rows[i].name);
    }
    out("};\n");
    /* each top-12-bit value's candidates, deduplicated; a candidate fixing no bit below the top 12 ends its list */
    for (i = 0; i < INDEX_SIZE; i++) {
        unsigned top = (unsigned)i << (16 - INDEX_BITS), high = 0xFFFFu << (16 - INDEX_BITS) & 0xFFFFu;
        int at = -1;
        n = 0;
        for (k = 0; k < nrows; k++) {
            const Row* r = &rows[order[k]];
            if ((top ^ r->val) & r->mask & high) continue;
            list[n++] = (unsigned short)k;
            if (!(r->mask & ~high)) break;
        }
        list[n++] = END_MARK;
        if (n - 1 > longest) longest = n - 1;
        for (k = 0; k + n <= total && at < 0; k++)
            if (!memcmp(&lists[k], list, n * sizeof *list) && (k == 0 || lists[k - 1] == END_MARK)) at = k;
        if (at < 0) {
            if (total + n > (int)(sizeof lists / sizeof *lists)) fail(0, "the candidate lists overflow");
            at = total;
            memcpy(&lists[total], list, n * sizeof *list);
            total += n;
            nlist++;
        }
        index_of[i] = (unsigned short)at;
    }
    out("#define SHADERDEC_END 0x%04xu\n", END_MARK);
    out("/* %d lists of the rows that can match, at most %d long, each ending with SHADERDEC_END */\n", nlist,
        longest);
    out("static const unsigned short shaderdec_lists[%d] = {", total);
    for (i = 0; i < total; i++) out("%s0x%04x,", i % 12 ? " " : "\n    ", lists[i]);
    out("\n};\n");
    out("/* by the top %d bits of the word: where its list starts */\n", INDEX_BITS);
    out("static const unsigned short shaderdec_index[%d] = {", INDEX_SIZE);
    for (i = 0; i < INDEX_SIZE; i++) out("%s%d,", i % 16 ? " " : "\n    ", index_of[i]);
    out("\n};\n");
    out("/* the row the word matches, or a null pointer */\n");
    out("static const Op* shaderdec_match(u64 w) {\n");
    out("    const unsigned short* k = shaderdec_lists + shaderdec_index[(unsigned)(w >> %d)];\n", 64 - INDEX_BITS);
    out("    for (; *k != SHADERDEC_END; k++)\n");
    out("        if ((w & shaderdec_ops[*k].mask) == shaderdec_ops[*k].val) return &shaderdec_ops[*k];\n");
    out("    return 0;\n}\n");
    flush(path);
}

int main(int argc, char** argv) {
    const char *enum_path = NULL, *decoder_path = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--enum") && i + 1 < argc)
            enum_path = argv[++i];
        else if (!strcmp(argv[i], "--decoder") && i + 1 < argc)
            decoder_path = argv[++i];
        else if (argv[i][0] != '-' && !spec_path)
            spec_path = argv[i];
        else
            spec_path = NULL, i = argc;
    }
    if (!spec_path) {
        fprintf(stderr, "usage: shaderdec [--enum OUT.h] [--decoder OUT.inc] SPEC\n");
        return 2;
    }
    read_spec(spec_path);
    check_spec();
    sort_rows();
    if (enum_path) write_enum(enum_path);
    if (decoder_path) write_decoder(decoder_path);
    free(obuf);
    return 0;
}
