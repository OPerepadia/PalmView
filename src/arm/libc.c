/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The C library parts the ARM module needs beyond newlib's string
 * functions and musl's math: a heap over an arena the 68K app provides,
 * errno, abort, number parsing and formatting, and stubs for I/O and time
 * functions that FFmpeg references but never calls here.
 *
 * Newlib headers are not included on purpose: these definitions only need
 * to match by name and calling convention. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "libc_arm.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Heap: boundary-tag blocks with a first-fit free list. */

#define HEAP_ALIGN 16U
#define HEAP_HEADER 16U
#define HEAP_MIN_BLOCK 32U
#define USED 1U

typedef struct Block {
    uint32_t size;          /* whole block in bytes; bit 0 marks it used */
    uint32_t prevSize;      /* size of the block before, 0 for the first */
    struct Block *nextFree;
    struct Block *prevFree;
} Block;

static Block *gFreeList;
static uint32_t gHeapSize;
static uint32_t gHeapUsed;
static uint32_t gHeapPeak;

static uint32_t BlockSize(const Block *b) { return b->size & ~USED; }
static Block *NextBlock(Block *b) { return (Block *)((uint8_t *)b + BlockSize(b)); }
static Block *PrevBlock(Block *b) { return (Block *)((uint8_t *)b - b->prevSize); }

static void Unlink(Block *b)
{
    if (b->prevFree != 0) b->prevFree->nextFree = b->nextFree;
    else gFreeList = b->nextFree;
    if (b->nextFree != 0) b->nextFree->prevFree = b->prevFree;
}

static void Link(Block *b)
{
    b->prevFree = 0;
    b->nextFree = gFreeList;
    if (gFreeList != 0) gFreeList->prevFree = b;
    gFreeList = b;
}

void ArmHeapInit(void *arenaP, uint32_t size)
{
    gFreeList = 0;
    gHeapSize = gHeapUsed = gHeapPeak = 0;
    ArmHeapAdd(arenaP, size);
}

/* Each region ends in a used sentinel and starts with prevSize 0, so
 * blocks never merge across regions. */
void ArmHeapAdd(void *regionP, uint32_t size)
{
    uintptr_t start = ((uintptr_t)regionP + HEAP_ALIGN - 1) & ~(uintptr_t)(HEAP_ALIGN - 1);
    uintptr_t end = ((uintptr_t)regionP + size) & ~(uintptr_t)(HEAP_ALIGN - 1);
    Block *first, *sentinel;
    if (end <= start + 2 * HEAP_MIN_BLOCK) return;
    first = (Block *)start;
    sentinel = (Block *)(end - HEAP_HEADER);
    first->size = (uint32_t)((uintptr_t)sentinel - start);
    first->prevSize = 0;
    sentinel->size = USED;            /* zero-length, always used */
    sentinel->prevSize = first->size;
    Link(first);
    gHeapSize += (uint32_t)(end - start);
}

void ArmHeapStats(uint32_t *usedP, uint32_t *peakP, uint32_t *freeP)
{
    *usedP = gHeapUsed;
    *peakP = gHeapPeak;
    *freeP = gHeapSize - gHeapUsed;
}

/* Splits b so it is exactly need bytes, returning the rest to the list. */
static void Split(Block *b, uint32_t need)
{
    uint32_t size = BlockSize(b);
    if (size - need >= HEAP_MIN_BLOCK) {
        Block *rest = (Block *)((uint8_t *)b + need);
        rest->size = size - need;
        rest->prevSize = need;
        NextBlock(rest)->prevSize = rest->size;
        b->size = need | (b->size & USED);
        Link(rest);
    }
}

static uint32_t Need(size_t n)
{
    uint32_t need;
    if (n > 0x7FFFFFF0U) return 0;
    need = (uint32_t)((n + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1)) + HEAP_HEADER;
    return need < HEAP_MIN_BLOCK ? HEAP_MIN_BLOCK : need;
}

static void Account(int32_t delta)
{
    gHeapUsed += (uint32_t)delta;
    if (gHeapUsed > gHeapPeak) gHeapPeak = gHeapUsed;
}

void *malloc(size_t n)
{
    uint32_t need = Need(n);
    Block *b;
    if (need == 0) return 0;
    for (b = gFreeList; b != 0; b = b->nextFree) {
        if (BlockSize(b) >= need) {
            Unlink(b);
            b->size |= USED;
            Split(b, need);
            Account((int32_t)BlockSize(b));
            return (uint8_t *)b + HEAP_HEADER;
        }
    }
    return 0;
}

void free(void *p)
{
    Block *b, *next;
    if (p == 0) return;
    b = (Block *)((uint8_t *)p - HEAP_HEADER);
    Account(-(int32_t)BlockSize(b));
    b->size &= ~USED;
    next = NextBlock(b);
    if ((next->size & USED) == 0) {
        Unlink(next);
        b->size += next->size;
        NextBlock(b)->prevSize = b->size;
    }
    if (b->prevSize != 0) {
        Block *prev = PrevBlock(b);
        if ((prev->size & USED) == 0) {
            prev->size += b->size;
            NextBlock(prev)->prevSize = prev->size;
            return;
        }
    }
    Link(b);
}

void *calloc(size_t count, size_t size)
{
    void *p;
    if (size != 0 && count > 0x7FFFFFF0U / size) return 0;
    p = malloc(count * size);
    if (p != 0) memset(p, 0, count * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    Block *b, *next;
    uint32_t need = Need(n), size;
    void *q;
    if (p == 0) return malloc(n);
    if (n == 0) {
        free(p);
        return 0;
    }
    if (need == 0) return 0;
    b = (Block *)((uint8_t *)p - HEAP_HEADER);
    size = BlockSize(b);
    if (need <= size) return p;
    next = NextBlock(b);
    if ((next->size & USED) == 0 && size + next->size >= need) {
        Unlink(next);
        Account((int32_t)next->size);
        b->size += next->size;
        NextBlock(b)->prevSize = BlockSize(b);
        return p;
    }
    q = malloc(n);
    if (q == 0) return 0;
    memcpy(q, p, size - HEAP_HEADER);
    free(p);
    return q;
}

void *memalign(size_t alignment, size_t n)
{
    uint8_t *p, *aligned;
    Block *b, *back;
    uint32_t gap;
    if (alignment <= HEAP_ALIGN) return malloc(n);
    if ((alignment & (alignment - 1)) != 0) return 0;
    p = malloc(n + alignment + HEAP_MIN_BLOCK);
    if (p == 0 || ((uintptr_t)p & (alignment - 1)) == 0) return p;
    aligned = (uint8_t *)(((uintptr_t)p + HEAP_MIN_BLOCK + alignment - 1) &
        ~(uintptr_t)(alignment - 1));
    gap = (uint32_t)(aligned - p);
    /* Give the front of the block back as its own free block. */
    /* Through uintptr_t: the header sits before the pointer malloc gave. */
    b = (Block *)((uintptr_t)p - HEAP_HEADER);
    back = (Block *)((uintptr_t)aligned - HEAP_HEADER);
    back->size = (BlockSize(b) - gap) | USED;
    back->prevSize = gap;
    NextBlock(back)->prevSize = BlockSize(back);
    b->size = gap | USED;
    free(p);
    return aligned;
}

int posix_memalign(void **resultP, size_t alignment, size_t n)
{
    void *p = memalign(alignment, n);
    if (p == 0) return 12;  /* ENOMEM */
    *resultP = p;
    return 0;
}

/* errno and abort */

static int gErrno;
void *gArmAbortJump[5];
int gArmAborted;

int *__errno(void)
{
    return &gErrno;
}

void abort(void)
{
    gArmAborted = 1;
    __builtin_longjmp(gArmAbortJump, 1);
}

/* Searching and parsing */

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const uint8_t *lo = (const uint8_t *)base;
    while (count != 0) {
        const uint8_t *mid = lo + (count / 2) * size;
        int c = compare(key, mid);
        if (c == 0) return (void *)mid;
        if (c > 0) {
            lo = mid + size;
            count -= count / 2 + 1;
        } else {
            count /= 2;
        }
    }
    return 0;
}

static int IsSpace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int DigitValue(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Parses an unsigned magnitude; *negP gets the sign. */
static unsigned long long ParseInteger(const char *s, char **endP, int base,
                                       int *negP)
{
    const char *start = s;
    unsigned long long value = 0;
    int any = 0;
    while (IsSpace(*s)) s++;
    *negP = 0;
    if (*s == '+' || *s == '-') *negP = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    for (; DigitValue(*s) < base; s++, any = 1)
        value = value * (unsigned)base + (unsigned)DigitValue(*s);
    if (endP != 0) *endP = (char *)(any ? s : start);
    return value;
}

unsigned long long strtoull(const char *s, char **endP, int base)
{
    int neg;
    unsigned long long v = ParseInteger(s, endP, base, &neg);
    return neg ? -v : v;
}

long long strtoll(const char *s, char **endP, int base)
{
    int neg;
    unsigned long long v = ParseInteger(s, endP, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long strtoul(const char *s, char **endP, int base)
{
    return (unsigned long)strtoull(s, endP, base);
}

long strtol(const char *s, char **endP, int base)
{
    return (long)strtoll(s, endP, base);
}

double strtod(const char *s, char **endP)
{
    const char *p = s;
    double value = 0, scale = 1;
    int neg = 0, any = 0, exponent = 0, expNeg = 0;
    while (IsSpace(*p)) p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    for (; *p >= '0' && *p <= '9'; p++, any = 1) value = value * 10 + (*p - '0');
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++, any = 1) {
            scale /= 10;
            value += (*p - '0') * scale;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *e = p + 1;
        if (*e == '+' || *e == '-') expNeg = *e++ == '-';
        if (*e >= '0' && *e <= '9') {
            for (; *e >= '0' && *e <= '9'; e++) exponent = exponent * 10 + (*e - '0');
            p = e;
            while (exponent-- > 0) value = expNeg ? value / 10 : value * 10;
        }
    }
    if (endP != 0) *endP = (char *)(any ? p : s);
    return neg ? -value : value;
}

/* Formatting: integers, strings and characters. Floats print as "?";
 * nothing on this path formats floats except log messages. */

typedef struct Out {
    char *bufferP;
    size_t capacity;
    size_t length;
} Out;

static void Put(Out *o, char c)
{
    if (o->length + 1 < o->capacity) o->bufferP[o->length] = c;
    o->length++;
}

static void PutPadded(Out *o, const char *textP, size_t length, int width,
                      int left, char pad)
{
    int fill = width > (int)length ? width - (int)length : 0;
    if (!left) while (fill-- > 0) Put(o, pad);
    while (length-- > 0) Put(o, *textP++);
    if (left) while (fill-- > 0) Put(o, ' ');
}

int vsnprintf(char *bufferP, size_t capacity, const char *format, va_list args)
{
    Out o;
    o.bufferP = bufferP;
    o.capacity = capacity;
    o.length = 0;
    while (*format != '\0') {
        char digits[24];
        const char *textP = digits;
        size_t length = 0;
        int left = 0, zero = 0, width = 0, precision = -1, longs = 0;
        unsigned long long value;
        int base = 10, negative = 0, upper = 0;
        char c = *format++;
        if (c != '%') {
            Put(&o, c);
            continue;
        }
        for (;; format++) {
            if (*format == '-') left = 1;
            else if (*format == '0') zero = 1;
            else if (*format != '+' && *format != ' ' && *format != '#') break;
        }
        if (*format == '*') { width = va_arg(args, int); format++; }
        else while (*format >= '0' && *format <= '9') width = width * 10 + (*format++ - '0');
        if (*format == '.') {
            format++;
            precision = 0;
            if (*format == '*') { precision = va_arg(args, int); format++; }
            else while (*format >= '0' && *format <= '9')
                precision = precision * 10 + (*format++ - '0');
        }
        for (;; format++) {
            if (*format == 'l') longs++;
            else if (*format == 'z' || *format == 'j' || *format == 't') longs = 1;
            else if (*format != 'h') break;
        }
        c = *format++;
        switch (c) {
            case 'd': case 'i':
                {
                    long long v = longs >= 2 ? va_arg(args, long long)
                        : longs == 1 ? va_arg(args, long) : va_arg(args, int);
                    negative = v < 0;
                    value = negative ? (unsigned long long)-v : (unsigned long long)v;
                }
                goto number;
            case 'u': case 'x': case 'X': case 'o': case 'p':
                if (c == 'p') { value = (uintptr_t)va_arg(args, void *); base = 16; }
                else {
                    value = longs >= 2 ? va_arg(args, unsigned long long)
                        : longs == 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned);
                    base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
                    upper = c == 'X';
                }
            number:
                {
                    char reversed[24];
                    size_t n = 0;
                    do {
                        unsigned d = (unsigned)(value % (unsigned)base);
                        reversed[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
                        value /= (unsigned)base;
                    } while (value != 0 && n < sizeof(reversed));
                    while ((int)n < precision && n < sizeof(reversed)) reversed[n++] = '0';
                    if (negative) digits[length++] = '-';
                    while (n != 0) digits[length++] = reversed[--n];
                }
                PutPadded(&o, digits, length, width, left, zero && !left ? '0' : ' ');
                break;
            case 'c':
                digits[0] = (char)va_arg(args, int);
                PutPadded(&o, digits, 1, width, left, ' ');
                break;
            case 's':
                textP = va_arg(args, const char *);
                if (textP == 0) textP = "(null)";
                while (textP[length] != '\0' && (precision < 0 || (int)length < precision))
                    length++;
                PutPadded(&o, textP, length, width, left, ' ');
                break;
            case 'f': case 'g': case 'e': case 'G': case 'E':
                (void)va_arg(args, double);
                PutPadded(&o, "?", 1, width, left, ' ');
                break;
            case '%':
                Put(&o, '%');
                break;
            default:
                if (c == '\0') format--;
                break;
        }
    }
    if (o.capacity != 0)
        o.bufferP[o.length < o.capacity ? o.length : o.capacity - 1] = '\0';
    return (int)o.length;
}

int snprintf(char *bufferP, size_t capacity, const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(bufferP, capacity, format, args);
    va_end(args);
    return n;
}

/* Never reached in this module; present so FFmpeg links. */

void *_impure_ptr;
int sscanf(const char *s, const char *format, ...) { (void)s; (void)format; return 0; }
int fprintf(void *f, const char *format, ...) { (void)f; (void)format; return 0; }
int fputs(const char *s, void *f) { (void)s; (void)f; return 0; }
size_t fread(void *p, size_t size, size_t count, void *f) { (void)p; (void)size; (void)count; (void)f; return 0; }
void *fdopen(int fd, const char *mode) { (void)fd; (void)mode; return 0; }
int fclose(void *f) { (void)f; return -1; }
int setvbuf(void *f, char *b, int mode, size_t size) { (void)f; (void)b; (void)mode; (void)size; return -1; }
int isatty(int fd) { (void)fd; return 0; }
int open(const char *path, int flags, ...) { (void)path; (void)flags; return -1; }
int fcntl(int fd, int command, ...) { (void)fd; (void)command; return -1; }
char *getenv(const char *name) { (void)name; return 0; }
int gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return -1; }
long clock(void) { return -1; }
void *localtime_r(const void *t, void *tm) { (void)t; (void)tm; return 0; }
void *gmtime_r(const void *t, void *tm) { (void)t; (void)tm; return 0; }
long mktime(void *tm) { (void)tm; return -1; }
size_t strftime(char *s, size_t n, const char *format, const void *tm)
{
    (void)format; (void)tm;
    if (n != 0) s[0] = '\0';
    return 0;
}
int __xpg_strerror_r(int error, char *bufferP, size_t capacity)
{
    snprintf(bufferP, capacity, "error %d", error);
    return 0;
}
