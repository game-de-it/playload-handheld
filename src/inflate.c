// Raw DEFLATE (RFC 1951) decoder, in the style of zlib's puff.c.
// Playload URLs carry deflate-raw data, so no zlib header or checksum is involved.
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *in;
  size_t inlen, inpos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t *out;
  size_t outlen, outcap, maxout;
  int err;
} State;

typedef struct {
  short count[16];
  short symbol[288];
} Huffman;

static int bits(State *s, int need) {
  uint32_t val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->inpos >= s->inlen) {
      s->err = 1;
      return 0;
    }
    val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = val >> need;
  s->bitcnt -= need;
  return (int)(val & ((1u << need) - 1));
}

static int emit(State *s, uint8_t byte) {
  if (s->outlen == s->outcap) {
    size_t cap = s->outcap ? s->outcap * 2 : 4096;
    if (cap > s->maxout) cap = s->maxout;
    if (cap <= s->outlen) return -1;
    uint8_t *grown = realloc(s->out, cap);
    if (!grown) return -1;
    s->out = grown;
    s->outcap = cap;
  }
  s->out[s->outlen++] = byte;
  return 0;
}

static int decode(State *s, const Huffman *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= bits(s, 1);
    if (s->err) return -1;
    int count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static int construct(Huffman *h, const short *length, int n) {
  short offs[16];
  memset(h->count, 0, sizeof h->count);
  for (int i = 0; i < n; i++) h->count[length[i]]++;
  if (h->count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len < 16; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return -1;
  }
  offs[1] = 0;
  for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
  for (int i = 0; i < n; i++)
    if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
  return left;
}

static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(State *s, const Huffman *lencode, const Huffman *distcode) {
  for (;;) {
    int symbol = decode(s, lencode);
    if (symbol < 0) return -1;
    if (symbol < 256) {
      if (emit(s, (uint8_t)symbol)) return -1;
    } else if (symbol == 256) {
      return 0;
    } else {
      symbol -= 257;
      if (symbol >= 29) return -1;
      int len = lbase[symbol] + bits(s, lext[symbol]);
      int dsym = decode(s, distcode);
      if (dsym < 0 || dsym >= 30) return -1;
      size_t dist = (size_t)(dbase[dsym] + bits(s, dext[dsym]));
      if (s->err || dist > s->outlen) return -1;
      while (len--)
        if (emit(s, s->out[s->outlen - dist])) return -1;
    }
  }
}

static int stored(State *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->inpos + 4 > s->inlen) return -1;
  unsigned len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
  unsigned nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
  s->inpos += 4;
  if (len != (~nlen & 0xffff) || s->inpos + len > s->inlen) return -1;
  while (len--)
    if (emit(s, s->in[s->inpos++])) return -1;
  return 0;
}

static int fixed(State *s) {
  static Huffman lencode, distcode;
  static int built = 0;
  if (!built) {
    short lengths[288];
    int i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    construct(&lencode, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    construct(&distcode, lengths, 30);
    built = 1;
  }
  return codes(s, &lencode, &distcode);
}

static int dynamic(State *s) {
  static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  short lengths[320];
  Huffman lencode, distcode;
  int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
  if (s->err || nlen > 286 || ndist > 30) return -1;
  int index = 0;
  for (; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
  for (; index < 19; index++) lengths[order[index]] = 0;
  if (s->err || construct(&lencode, lengths, 19) != 0) return -1;
  index = 0;
  while (index < nlen + ndist) {
    int symbol = decode(s, &lencode);
    if (symbol < 0) return -1;
    if (symbol < 16) {
      lengths[index++] = (short)symbol;
    } else {
      int len = 0, repeat;
      if (symbol == 16) {
        if (index == 0) return -1;
        len = lengths[index - 1];
        repeat = 3 + bits(s, 2);
      } else if (symbol == 17) {
        repeat = 3 + bits(s, 3);
      } else {
        repeat = 11 + bits(s, 7);
      }
      if (s->err || index + repeat > nlen + ndist) return -1;
      while (repeat--) lengths[index++] = (short)len;
    }
  }
  if (lengths[256] == 0) return -1;
  int err = construct(&lencode, lengths, nlen);
  if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
  err = construct(&distcode, lengths + nlen, ndist);
  if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
  return codes(s, &lencode, &distcode);
}

int raw_inflate(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t maxout) {
  State s = {.in = in, .inlen = inlen, .maxout = maxout};
  int last, err;
  do {
    last = bits(&s, 1);
    int type = bits(&s, 2);
    if (s.err) {
      err = -1;
      break;
    }
    err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
  } while (!err && !last);
  if (err || s.err) {
    free(s.out);
    return -1;
  }
  *out = s.out;
  *outlen = s.outlen;
  return 0;
}
