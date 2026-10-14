// FUN_00462020, rewritten from the machine code (0x462020-0x4627c5).
//
// It builds the 16-bit colour table for a golfer (or, from 0x9c up, another
// character) by copying ranges of palette entries out of the skin, hair,
// shirt and trouser palettes into the shared palette object. Ghidra lost the
// arguments of all its virtual calls and mixed up its locals, so the
// translation indexed the palette table at 0x81ca6c with a stack address:
// whether that read stayed inside memory depended on where the stack
// happened to be (it broke once the desktop build's static data grew).
//
// The palettes are graphsy objects: +0x10 GetEntries(rgb, first, count)
// writes count RGB triplets for entries 0..count-1, +0x14 SetEntries(rgb,
// first, count) takes them from rgb, which points at entry `first`.

#include "golf_protos.h"
#include "port/runtime.h"

#define PAL_TABLE 0x81ca6cu   // palette object pointers, 0x58 bytes apart
#define ALT_TABLE 0x81fa34u   // the same for the alternative set
#define ALT_EXTRA 0x81fa8cu
#define DST_HOLDER 0x81ca10u  // holder object; +4 is the shared palette
#define DST 0x81ca14u

static uint32_t u32_at(uint32_t a) { return *(uint32_t *)(uintptr_t)a; }

static uint32_t vcall(uint32_t obj, uint32_t slot, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t fn = u32_at(u32_at(obj) + slot);
  return icall_this(fn, obj, a, b, c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// Reads entries 0..count-1 of a palette, when it exists.
static void get(uint32_t pal, uint8_t *rgb, uint32_t count) {
  if (pal) vcall(pal, 0x10, (uint32_t)(uintptr_t)rgb, 0, count);
}

// Copies entries first..first+count-1 to the shared palette, creating it first if need be.
static uint32_t set(uint8_t *rgb, uint32_t first, uint32_t count) {
  if (!u32_at(DST)) icall_this(u32_at(u32_at(DST_HOLDER)), DST_HOLDER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  return vcall(u32_at(DST), 0x14, (uint32_t)(uintptr_t)(rgb + first * 3), first, count);
}

static uint32_t pal(int index) { return u32_at(PAL_TABLE + (uint32_t)index * 0x58); }

// @override FUN_00462020
undefined4 FUN_00462020(int param_1) {
  uint8_t rgb[0x2f0];
  int who = param_1;
  // [esp+0x10], 0x14, 0x18, 0x1c, 0x20 and ebx in the original: palette indices
  int a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
  int fixed = 0;   // [esp+0x24]: don't store the choice in the character record
  int plain;       // [esp+0x28]: 0 adds 0x8b to every index (the alternative set)

  if (who < 0x9c) {
    plain = (int)FUN_0046c940(who);
    if (who == 0xff) who = 0;
    int x = *(int16_t *)(uintptr_t)(0x57956eu + ((uint32_t)who << 8));
    uint8_t flags = *(uint8_t *)(uintptr_t)(0x5794d1u + ((uint32_t)who << 8));
    d = x % 10;
    a = (x * 3) % 10;
    int t = *(int8_t *)(uintptr_t)(0x4d60aau + (uint32_t)x * 0x230);
    c = *(int8_t *)(uintptr_t)((plain ? 0x4d55e8u : 0x4d5b38u) + (uint32_t)t * 0x44);
    f = (flags & 4) ? a : (plain ? 0 : 9);
    b = (flags & 2) ? 4 : c;
    if (!plain) a = (x * 2) % 3 + 1;

    // 0x462454
    uint32_t rec = (uint32_t)who << 8;
    x = *(int16_t *)(uintptr_t)(0x57956eu + rec);
    uint32_t s = (uint32_t)x * 0x230;
    uint32_t k = FUN_0046c940(who);
    e = *(int8_t *)(uintptr_t)(0x4d55ebu + (uint32_t)((k ? 0 : 0x14) + *(int8_t *)(uintptr_t)(0x4d60aau + s)) * 0x44);
    if (*(int32_t *)(uintptr_t)(0x4d60b4u + s)) {
      a = *(int8_t *)(uintptr_t)(0x4d60acu + s);
      d = *(int8_t *)(uintptr_t)(0x4d60adu + s);
      c = *(int8_t *)(uintptr_t)(0x4d60afu + s);
      f = *(uint8_t *)(uintptr_t)(0x4d60abu + s) & 0xf;
      b = *(int8_t *)(uintptr_t)(0x4d60aeu + s);
      e = *(int8_t *)(uintptr_t)(0x4d60b0u + s);
    }
    if (*(uint8_t *)(uintptr_t)(0x5794d0u + rec) == 0x20) {
      b = 4;
      plain = 1;
      uint32_t o = (uint32_t)*(uint8_t *)(uintptr_t)(0x579573u + rec) * 56;
      f = *(int8_t *)(uintptr_t)(0x58dd72u + o);
      c = *(int8_t *)(uintptr_t)(0x58dd71u + o);
      a = *(int8_t *)(uintptr_t)(0x58dd73u + o);
      d = *(int8_t *)(uintptr_t)(0x58dd74u + o);
    }
    if (!fixed) {
      uint8_t *r = (uint8_t *)(uintptr_t)(0x4d60abu + s);
      r[0] = (uint8_t)((r[0] & 0xf0) | (uint8_t)f);
      r[1] = (uint8_t)a;
      r[2] = (uint8_t)d;
      r[4] = (uint8_t)c;
      r[3] = (uint8_t)b;
      r[5] = (uint8_t)e;
    }
    if (!plain) {
      a += 0x8b; b += 0x8b; c += 0x8b; d += 0x8b; e += 0x8b; f += 0x8b;
      get(u32_at(ALT_TABLE), rgb, 0x78);
      set(rgb, 0, 0x78);
      goto ranges;
    }
  } else {
    // 0x46212f: other characters, one record of 0x25 bytes each
    fixed = 1;
    plain = 1;
    b = 4;
    uint32_t r = (uint32_t)who * 0x25;
    c = *(int8_t *)(uintptr_t)(0x55c0cdu + r);
    f = *(int8_t *)(uintptr_t)(0x55c0ceu + r) + 5;
    a = *(int8_t *)(uintptr_t)(0x55c0cfu + r);
    d = *(int8_t *)(uintptr_t)(0x55c0d0u + r);
  }

  // 0x462178
  get(u32_at(PAL_TABLE), rgb, 0x78);
  set(rgb, 0, 0x78);

ranges:
  // 0x4621ba
  get(pal(a), rgb, 0x14);
  set(rgb, 0, 0x14);
  get(pal(d), rgb, 0x28);
  set(rgb, 0x14, 0x14);
  get(pal(c), rgb, 0x3c);
  set(rgb, 0x28, 0x14);
  get(pal(f), rgb, 0x50);
  set(rgb, 0x3c, 0x14);
  if (plain) {
    get(pal(a), rgb, 0x79);
    set(rgb, 0x6b, 0xf);
    get(pal(d), rgb, 0x83);
    set(rgb, 0x7a, 0xa);
    get(pal(c), rgb, 0x88);
    set(rgb, 0x84, 0x32);
    if (!fixed) {
      get(pal(b), rgb, 0x5a);
      set(rgb, 0x50, 0x7);
    }
  } else {
    // 0x462614
    get(pal(e), rgb, 0x5a);
    set(rgb, 0x50, 0xa);
    get(pal(b), rgb, 0x64);
    set(rgb, 0x5a, 0x7);
    get(u32_at(ALT_TABLE + (uint32_t)(who & 1) * 0x58), rgb, 0xac);
  }

  // 0x4626f0
  if (who >= 0 && who < 0x98) {
    int x = *(int16_t *)(uintptr_t)(0x57956eu + ((uint32_t)who << 8));
    if ((*(uint8_t *)(uintptr_t)(0x5849e2u + (uint32_t)x * 44) & 7) >= 4) {
      get(u32_at(ALT_EXTRA), rgb, 0xac);
      return set(rgb, 0x90, 0x1c);
    }
  }
  get(u32_at(ALT_TABLE), rgb, 0xac);
  return set(rgb, 0x90, 0x1c);
}
