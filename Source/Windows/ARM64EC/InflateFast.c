// SPDX-License-Identifier: MIT
// FEX's own inflate_fast (FEX_INFLATE_FAST, from build 158), included by NativeZlib.c after zlib's inffast.c, whose
// headers (zutil.h, inftrees.h, inflate.h) it uses. It does the same job with the same contract: inflate() calls it with
// avail_in >= 6 and avail_out >= 258, it decodes literals and matches until the block ends, the data is bad, or input or
// output runs short, and it leaves state->hold and state->bits as zlib's own does (fewer than 8 bits, whole bytes given
// back to the input).
//
// Why: in the build 157 run the game unpacked 1.52 GB during play in 6.9 s of CPU (4.5 ms per MB), and one 93 ms frame
// (t=301) was 38 ms of it on the main thread. zlib's loop reads the input 1-2 bytes at a time and copies every match one
// byte at a time. This one, while there are at least 8 bytes of input and 274 bytes of output space left:
// - fills a 64-bit bit buffer with one 8-byte load per code (at least 56 bits, enough for a length, a distance and their
//   extra bits, 48 bits at most, or for three literals), instead of 2-byte steps with a check before each field;
// - copies a match that starts 16 or more bytes back in 16-byte pieces, 8-15 back in 8-byte pieces, and 1-7 back by
//   repeating a 16-byte pattern. The pieces may write up to 15 bytes past the match, never past the output space
//   inflate() gave (avail_out): later output overwrites them, as in Chromium's zlib.
// The last few bytes of input or output, and matches that reach back into zlib's window (output from earlier inflate()
// calls), go through zlib's own byte by byte code, below.
//
// Bad data ends the same way as in zlib (state->mode = BAD and the same message). One difference: zlib, when an
// application turned off its distance check with inflateUndermine, would read in front of its window; here a distance
// too far back is always bad data.

#include <string.h>

// Little-endian only (ARM64EC and the host test).
static inline uint64_t FEXInflateLoad64(const unsigned char* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// For a match 1-7 bytes back: how far a 16-byte copy of the repeating pattern can move on, a multiple of the distance.
static const unsigned char FEXInflatePatternStep[8] = {16, 16, 16, 15, 16, 15, 12, 14};

#define FEX_INFLATE_FAST_IN 8
#define FEX_INFLATE_FAST_OUT (258 + 16)

static void FEXInflateFastChunk(z_streamp strm, unsigned start) {
  struct inflate_state FAR* state = (struct inflate_state FAR*)strm->state;
  z_const unsigned char FAR* in = strm->next_in;
  z_const unsigned char FAR* const in_end = in + strm->avail_in;
  unsigned char FAR* out = strm->next_out;
  unsigned char FAR* const out_end = out + strm->avail_out;
  unsigned char FAR* const beg = out - (start - strm->avail_out); // inflate()'s first output byte in this call
  const unsigned wsize = state->wsize;
  const unsigned whave = state->whave;
  const unsigned wnext = state->wnext;
  unsigned char FAR* const window = state->window;
  const code FAR* const lcode = state->lencode;
  const code FAR* const dcode = state->distcode;
  const unsigned lmask = (1U << state->lenbits) - 1;
  const unsigned dmask = (1U << state->distbits) - 1;
  // Bits above 'bits' are either zero or the low bits of *in (from the last 8-byte load), so new input is ORed in.
  uint64_t hold = state->hold;
  unsigned bits = state->bits;
  code here;
  unsigned op;
  unsigned len;
  unsigned dist;
  unsigned char FAR* from;

  // Fast part.
  while ((size_t)(in_end - in) >= FEX_INFLATE_FAST_IN && (size_t)(out_end - out) >= FEX_INFLATE_FAST_OUT) {
    hold |= FEXInflateLoad64(in) << bits;
    in += (63 - bits) >> 3;
    bits |= 56; // 56-63 bits now
    here = lcode[hold & lmask];
  fast_len:
    op = here.bits;
    hold >>= op;
    bits -= op;
    op = here.op;
    if (op == 0) { // literal
      *out++ = (unsigned char)here.val;
      // Up to two more literals without a refill: each takes 15 bits at most, so 41 and then 26 are still there.
      here = lcode[hold & lmask];
      if (here.op == 0) {
        hold >>= here.bits;
        bits -= here.bits;
        *out++ = (unsigned char)here.val;
        here = lcode[hold & lmask];
        if (here.op == 0) {
          hold >>= here.bits;
          bits -= here.bits;
          *out++ = (unsigned char)here.val;
        }
      }
      continue;
    }
    if (op & 16) { // length base
      len = here.val;
      op &= 15; // extra bits
      len += (unsigned)hold & ((1U << op) - 1);
      hold >>= op;
      bits -= op;
      here = dcode[hold & dmask];
    fast_dist:
      op = here.bits;
      hold >>= op;
      bits -= op;
      op = here.op;
      if (op & 16) { // distance base
        dist = here.val;
        op &= 15; // extra bits
        dist += (unsigned)hold & ((1U << op) - 1);
        hold >>= op;
        bits -= op;
        op = (unsigned)(out - beg); // the farthest back this call's own output reaches
        if (dist > op) {
          // From zlib's window: its own code, byte by byte (there are at least 274 bytes of space).
          op = dist - op; // distance back in the window
          if (op > whave) {
            strm->msg = (char*)"invalid distance too far back";
            state->mode = BAD;
            break;
          }
          from = window;
          if (wnext == 0) { // very common case
            from += wsize - op;
            if (op < len) { // some from window
              len -= op;
              do {
                *out++ = *from++;
              } while (--op);
              from = out - dist; // rest from output
            }
          } else if (wnext < op) { // wrap around window
            from += wsize + wnext - op;
            op -= wnext;
            if (op < len) { // some from end of window
              len -= op;
              do {
                *out++ = *from++;
              } while (--op);
              from = window;
              if (wnext < len) { // some from start of window
                op = wnext;
                len -= op;
                do {
                  *out++ = *from++;
                } while (--op);
                from = out - dist; // rest from output
              }
            }
          } else { // contiguous in window
            from += wnext - op;
            if (op < len) { // some from window
              len -= op;
              do {
                *out++ = *from++;
              } while (--op);
              from = out - dist; // rest from output
            }
          }
          do {
            *out++ = *from++;
          } while (--len);
          continue;
        }
        // From this call's output, in pieces (see above). Everything in front of out is final output.
        unsigned char FAR* const stop = out + len;
        from = out - dist;
        if (dist >= 16) {
          do {
            memcpy(out, from, 16);
            out += 16;
            from += 16;
          } while (out < stop);
        } else if (dist >= 8) {
          do {
            memcpy(out, from, 8);
            out += 8;
            from += 8;
          } while (out < stop);
        } else {
          unsigned char pattern[16];
          if (dist == 1) {
            memset(pattern, *from, 16);
          } else {
            unsigned i = 0;
            for (; i < dist; i++) {
              pattern[i] = from[i];
            }
            for (; i < 16; i++) {
              pattern[i] = pattern[i - dist];
            }
          }
          const unsigned step = FEXInflatePatternStep[dist];
          do {
            memcpy(out, pattern, 16);
            out += step;
          } while (out < stop);
        }
        out = stop;
        continue;
      }
      if ((op & 64) == 0) { // 2nd level distance code
        here = dcode[here.val + (hold & ((1U << op) - 1))];
        goto fast_dist;
      }
      strm->msg = (char*)"invalid distance code";
      state->mode = BAD;
      break;
    }
    if ((op & 64) == 0) { // 2nd level length code
      here = lcode[here.val + (hold & ((1U << op) - 1))];
      goto fast_len;
    }
    if (op & 32) { // end-of-block
      state->mode = TYPE;
      break;
    }
    strm->msg = (char*)"invalid literal/length code";
    state->mode = BAD;
    break;
  }

  // The rest: zlib's own loop (input 6 bytes and output 258 bytes at a time), unless the block already ended or the data
  // was bad. On entry from inflate() it runs at least once, as zlib's would.
  if (state->mode != TYPE && state->mode != BAD) {
    z_const unsigned char FAR* const last = in_end - 5;
    unsigned char FAR* const end = out_end - 257;
    while (in < last && out < end) {
      if (bits < 15) {
        hold |= (uint64_t)(*in++) << bits;
        bits += 8;
        hold |= (uint64_t)(*in++) << bits;
        bits += 8;
      }
      here = lcode[hold & lmask];
    slow_len:
      op = here.bits;
      hold >>= op;
      bits -= op;
      op = here.op;
      if (op == 0) { // literal
        *out++ = (unsigned char)here.val;
      } else if (op & 16) { // length base
        len = here.val;
        op &= 15; // number of extra bits
        if (op) {
          if (bits < op) {
            hold |= (uint64_t)(*in++) << bits;
            bits += 8;
          }
          len += (unsigned)hold & ((1U << op) - 1);
          hold >>= op;
          bits -= op;
        }
        if (bits < 15) {
          hold |= (uint64_t)(*in++) << bits;
          bits += 8;
          hold |= (uint64_t)(*in++) << bits;
          bits += 8;
        }
        here = dcode[hold & dmask];
      slow_dist:
        op = here.bits;
        hold >>= op;
        bits -= op;
        op = here.op;
        if (op & 16) { // distance base
          dist = here.val;
          op &= 15; // number of extra bits
          if (bits < op) {
            hold |= (uint64_t)(*in++) << bits;
            bits += 8;
            if (bits < op) {
              hold |= (uint64_t)(*in++) << bits;
              bits += 8;
            }
          }
          dist += (unsigned)hold & ((1U << op) - 1);
          hold >>= op;
          bits -= op;
          op = (unsigned)(out - beg); // max distance in output
          if (dist > op) { // see if copy from window
            op = dist - op; // distance back in window
            if (op > whave) {
              strm->msg = (char*)"invalid distance too far back";
              state->mode = BAD;
              break;
            }
            from = window;
            if (wnext == 0) { // very common case
              from += wsize - op;
              if (op < len) { // some from window
                len -= op;
                do {
                  *out++ = *from++;
                } while (--op);
                from = out - dist; // rest from output
              }
            } else if (wnext < op) { // wrap around window
              from += wsize + wnext - op;
              op -= wnext;
              if (op < len) { // some from end of window
                len -= op;
                do {
                  *out++ = *from++;
                } while (--op);
                from = window;
                if (wnext < len) { // some from start of window
                  op = wnext;
                  len -= op;
                  do {
                    *out++ = *from++;
                  } while (--op);
                  from = out - dist; // rest from output
                }
              }
            } else { // contiguous in window
              from += wnext - op;
              if (op < len) { // some from window
                len -= op;
                do {
                  *out++ = *from++;
                } while (--op);
                from = out - dist; // rest from output
              }
            }
            while (len > 2) {
              *out++ = *from++;
              *out++ = *from++;
              *out++ = *from++;
              len -= 3;
            }
            if (len) {
              *out++ = *from++;
              if (len > 1) {
                *out++ = *from++;
              }
            }
          } else {
            from = out - dist; // copy direct from output
            do {               // minimum length is three
              *out++ = *from++;
              *out++ = *from++;
              *out++ = *from++;
              len -= 3;
            } while (len > 2);
            if (len) {
              *out++ = *from++;
              if (len > 1) {
                *out++ = *from++;
              }
            }
          }
        } else if ((op & 64) == 0) { // 2nd level distance code
          here = dcode[here.val + (hold & ((1U << op) - 1))];
          goto slow_dist;
        } else {
          strm->msg = (char*)"invalid distance code";
          state->mode = BAD;
          break;
        }
      } else if ((op & 64) == 0) { // 2nd level length code
        here = lcode[here.val + (hold & ((1U << op) - 1))];
        goto slow_len;
      } else if (op & 32) { // end-of-block
        state->mode = TYPE;
        break;
      } else {
        strm->msg = (char*)"invalid literal/length code";
        state->mode = BAD;
        break;
      }
    }
  }

  // Give back whole unused bytes (on entry bits < 8, so in won't go back past where it started) and drop the rest.
  len = bits >> 3;
  in -= len;
  bits -= len << 3;
  hold &= (UINT64_C(1) << bits) - 1;

  strm->next_in = in;
  strm->next_out = out;
  strm->avail_in = (unsigned)(in_end - in);
  strm->avail_out = (unsigned)(out_end - out);
  state->hold = (unsigned long)hold;
  state->bits = bits;
}
