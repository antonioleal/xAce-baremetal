/* Emulation of the Z80 CPU with hooks into the other parts of xace.
 * Copyright (C) 1994 Ian Collier.
 * Z81 changes (C) 1995 Russell Marks.
 * xace changes (C) 1997 Edward Patel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>

#include "z80.h"

extern unsigned long tstates;

/* The screen (2400h-27FFh) and character set (2C00h-2FFFh) copies used by the
 * ROM give the video circuit priority: a CPU access while the picture is being
 * drawn waits until the end of that line's picture (the copies at 2000h and
 * 2800h give the CPU priority instead).
 * Timing as in the MiSTer Ace core: 312 lines of 208 T-states, the picture in
 * the first 128 T-states of lines 0-191, and the interrupt (vsync) at T-state 154
 * of line 248, where tstates counts from. */
#define LINE_TSTATES   208
#define FRAME_LINES    312
#define PICTURE_LINES  192
#define PICTURE_RIGHT  128
#define INT_TSTATE     (248*LINE_TSTATES+154)

/* INT is the vsync signal, low for 8 lines from there: an interrupt can only be
 * taken then (missed if interrupts stay disabled, taken again if a handler
 * re-enables them while it is still low; the ROM's handler is longer) */
#define INT_LOW_TSTATES (8*LINE_TSTATES)

static unsigned
video_wait_tstates(void) {
  unsigned long t=(tstates+INT_TSTATE)%(FRAME_LINES*LINE_TSTATES);
  unsigned long line=t/LINE_TSTATES;
  unsigned pos=t%LINE_TSTATES;
  if(line>=PICTURE_LINES || pos>=PICTURE_RIGHT)
    return 0;
  return PICTURE_RIGHT-pos;
}

#define video_wait(x) (((x)&0xf400)==0x2400 ? (void)(tstates+=video_wait_tstates()) : (void)0)

#undef fetch
#define fetch(x) (video_wait(x), memptr[(unsigned short)((x)&0xe000)>>13][(x)&0x1fff])
#undef store
#define store(x,y) do { video_wait(x); storefunc(x,y); } while(0)
#undef store2b
#define store2b(x,hi,lo) do { video_wait(x); store2func(x,hi,lo); } while(0)

#define parity(a) (partable[a])

unsigned char partable[256] = {
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
  4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4
};


void
mainloop(void) {
  unsigned char a, f, b, c, d, e, h, l;
  unsigned char r, a1, f1, b1, c1, d1, e1, h1, l1, i, iff1, iff2, im;
  unsigned short pc;
  unsigned short ix, iy, sp;
  extern unsigned long tstates,tsmax;
  unsigned int radjust;
  unsigned char ixoriy, new_ixoriy;
  unsigned char intsample;
  unsigned char halted=0;      /* on a HALT instruction, waiting for an interrupt */
  unsigned char op;
  
  a=f=b=c=d=e=h=l=a1=f1=b1=c1=d1=e1=h1=l1=i=r=iff1=iff2=im=0;
  ixoriy=new_ixoriy=0;
  ix=iy=sp=pc=0;
  tstates=radjust=0;
  while(1) {
    ixoriy=new_ixoriy;
    new_ixoriy=0;
    intsample=1;
    op=fetch(pc);
    pc++;
    radjust++;

    switch(op) {
      #include "z80ops.c"
    }

    if(tstates>tsmax)
      fix_tstates();

    /* checked before the interrupt so a reset never pushes onto
     * freshly cleared RAM */
    if (reset_ace) {
      /* actually a kludge to let us do a reset */
      a=f=b=c=d=e=h=l=a1=f1=b1=c1=d1=e1=h1=l1=i=r=iff1=iff2=im=0;
      ixoriy=new_ixoriy=0;
      ix=iy=sp=pc=0;
      tstates=radjust=0;
      halted=0;
      reset_ace = 0;
    }

    /* .ace snapshot loaded (F5): take its registers */
    if (load_snapshot) {
      a=snapshot.reg_af>>8; f=snapshot.reg_af;
      b=snapshot.reg_bc>>8; c=snapshot.reg_bc;
      d=snapshot.reg_de>>8; e=snapshot.reg_de;
      h=snapshot.reg_hl>>8; l=snapshot.reg_hl;
      a1=snapshot.reg_af1>>8; f1=snapshot.reg_af1;
      b1=snapshot.reg_bc1>>8; c1=snapshot.reg_bc1;
      d1=snapshot.reg_de1>>8; e1=snapshot.reg_de1;
      h1=snapshot.reg_hl1>>8; l1=snapshot.reg_hl1;
      ix=snapshot.reg_ix; iy=snapshot.reg_iy; pc=snapshot.reg_pc;
      if (snapshot.set_sp)
        sp=snapshot.reg_sp;
      i=snapshot.reg_i;
      r=radjust=snapshot.reg_r;
      im=snapshot.reg_im==0 ? 0 : snapshot.reg_im==1 ? 2 : 3;  /* as set by ED 46 / 56 / 5E */
      iff1=snapshot.reg_iff1; iff2=snapshot.reg_iff2;
      ixoriy=new_ixoriy=0;
      halted=0;
      load_snapshot = 0;
    }

    /* IM 1 (the Ace's): interrupts off until the handler's EI, RST 38h, 13 T-states */
    if(tstates < INT_LOW_TSTATES && intsample && iff1) {
      if(halted) {
        pc++;                  /* return after the HALT */
        halted=0;
      }
      do_interrupt();
      iff1=iff2=0;
      push2(pc);
      pc=0x38;
      tstates+=13;
    }
  }
}


