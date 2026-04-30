#include <stddef.h>
#pragma code_seg("RESIDENT")
#pragma data_seg("RESIDENT", "CODE")

#include "resident.h"

/* --- Timing constants --- */
#define DELAY_OPL2_ADDRESS  6
#define DELAY_OPL2_DATA     23
#define DELAY_OPL3          6
#define DELAY_SERIAL_DATA   35

/* --- Status register bits --- */
#define STATUS_OPL2_LOW_BITS  0x06
#define STATUS_OPL3_LOW_BITS  0x00

struct config RESIDENT config;

/*
 * opl3Reg, cache_init, shadow_regs are defined in res_glue.s
 * inside the RESIDENT segment so they are protected by _dos_keep().
 * Accessed here via DS (DGROUP), always safe from any code segment.
 */
extern unsigned RESIDENT opl3Reg;
extern int      RESIDENT cache_init;
extern unsigned char RESIDENT shadow_regs[];


/* I/O port access */

static void outp(unsigned port, char value);
#pragma aux outp =               \
  "out dx, al"                   \
  parm [dx] [al]                 \
  modify exact [ax]

static char inp(unsigned port);
#pragma aux inp =                \
  "in al, dx"                    \
  parm [dx]                      \
  modify exact [ax]

#define PP_NOT_STROBE   0x1
#define PP_NOT_AUTOFD   0x2
#define PP_INIT         0x4
#define PP_NOT_SELECT   0x8


/* Debug output */

static void writechar(char x) {
  int COM1 = 0x03F8;
  outp(COM1, x);
}

static void writeln(void) {
  writechar('\r');
  writechar('\n');
}

static void writehex(char x) {
  x = (x & 0xf) + '0';
  if (x > '9') x += 'A' - '9' - 1;
  writechar(x);
}

static void write2hex(char x) {
  writehex(x >> 4);
  writehex(x);
}

static void write4hex(int x) {
  write2hex(x >> 8);
  write2hex(x);
}


/* I/O dispatching */

static porthandler * const adlib_table[4][2] = {
  { emulate_opl2_read, emulate_opl2_write_address },
  { emulate_opl2_read, emulate_opl2_write_data },
  { emulate_opl3_read, emulate_opl3_write_high_address },
  { emulate_opl3_read, emulate_opl3_write_data }
};

static porthandler * const sb_table[16][2] = {
  { emulate_opl3_read, emulate_opl3_write_low_address },
  { emulate_opl3_read, emulate_opl3_write_data },
  { emulate_opl3_read, emulate_opl3_write_high_address },
  { emulate_opl3_read, emulate_opl3_write_data },
  { emulate_ignore, emulate_ignore },
  { emulate_sbdsp_read_data, emulate_sbmixer_data_write },
  { emulate_ignore, emulate_sbdsp_reset },
  { NULL, NULL },
  { emulate_opl2_read, emulate_opl2_write_address },
  { emulate_opl2_read, emulate_opl2_write_data },
  { emulate_sbdsp_read_data, emulate_ignore },
  { NULL, NULL },
  { emulate_sbdsp_write_buffer, emulate_sbdsp_write_data },
  { NULL, NULL },
  { emulate_sbdsp_data_avail, emulate_ignore },
  { NULL, NULL }
};

porthandler *get_port_handler(unsigned port, unsigned flags) {
  porthandler * const (*table)[2];
  int base;
  if ((base = (port & ~0x3)) == 0x388) {
    table = adlib_table;
  }
  else if (config.sb_base && (base = (port & ~0xF)) == config.sb_base) {
    table = sb_table;
  }
  else {
    return 0;
  }
  return table[port & ~base][(flags >> 2) & 1];
}


/* I/O virtualization */

char _WCI86FAR * RESIDENT port_trap_ip;
static short RESIDENT address;
static char  RESIDENT timer_reg;


/* Nuke.YKT: send register address header */
static void WRITE_REG_NUKE(unsigned reg, unsigned bank, unsigned port) {
  char sendBuffer;
  opl3Reg = reg;
  sendBuffer = (char)(0x80 | ((bank & 0x03) << 2) | (address >> 6));
  outp(port, sendBuffer);
}

/* Nuke.YKT: send register value as 2-byte packed serial packet */
static void WRITE_VAL_NUKE(char val, unsigned port) {
  char sendBuffer[2];
  sendBuffer[0] = (char)(((opl3Reg & 0x3f) << 1) | ((val >> 7) & 1));
  sendBuffer[1] = (char)(val & 0x7f);
  /* Wait for Transmitter Holding Register Empty (THRE = LSR bit5) */
  while (!(inp(port + 5) & 0x20));
  outp(port, sendBuffer[0]);
  while (!(inp(port + 5) & 0x20));
  outp(port, sendBuffer[1]);
}


#define WRITE_COM(value, flags)                 \
  do {                                          \
    unsigned port = com_port;                   \
    outp(port, (value));                        \
    port += 2;                                  \
    outp(port, (flags));                        \
    outp(port, (flags) ^ PP_INIT);              \
    outp(port, (flags));                        \
  } while (0)

#ifdef _M_I86
#pragma aux delay parm [dx] [bx] modify exact [ax bx]
#pragma aux cond_delay parm [dx] [bx] modify exact [ax bx]
#else
#pragma aux delay parm [edx] [ebx] modify exact [eax ebx]
#pragma aux cond_delay parm [edx] [ebx] modify exact [eax ebx]
#endif

static void delay(unsigned port, char cnt) {
  do { (volatile) inp(port); } while (--cnt);
}

static void cond_delay(unsigned port, char cnt) {
  if (config.enable_patching) delay(port, cnt);
}


unsigned emulate_opl2_write_address(unsigned ax) {
  unsigned com_port = config.com_port;
  address = ax & 0xFF;
  if (!config.opl3) {
    WRITE_COM(ax, PP_INIT | PP_NOT_SELECT | PP_NOT_STROBE);
    cond_delay(com_port, DELAY_OPL2_ADDRESS);
  }
  else {
    WRITE_REG_NUKE(ax, 0, com_port);
    while (!(inp(com_port + 5) & 0x20));  /* THRE polling */
  }
  return ax;
}

unsigned emulate_opl2_write_data(unsigned ax) {
  static const char delay_cnt[2] = { DELAY_OPL2_DATA, DELAY_OPL3 };
  unsigned com_port = config.com_port;
  if (address == 4) timer_reg = ax;
  if (!config.opl3) {
    WRITE_COM(ax, PP_INIT | PP_NOT_SELECT);
  }
  else {
    /* No shadow cache - send all writes unconditionally.
     * THRE polling in WRITE_VAL_NUKE ensures no data loss. */
    WRITE_VAL_NUKE(ax, com_port);
  }
  cond_delay(com_port, delay_cnt[config.opl3]);
  return ax;
}

unsigned emulate_opl3_write_low_address(unsigned ax) {
  if (!config.opl3) return ax;
  return emulate_opl2_write_address(ax);
}

unsigned emulate_opl3_write_high_address(unsigned ax) {
  unsigned com_port = config.com_port;
  if (!config.opl3) return ax;
  address = 0x100 | (ax & 0xFF);
  WRITE_REG_NUKE(ax, 1, com_port);  /* bank=1 (high bank) */
  while (!(inp(com_port + 5) & 0x20));  /* THRE polling */
  return ax;
}

unsigned emulate_opl3_write_data(unsigned ax) {
  if (!config.opl3) return ax;
  return emulate_opl2_write_data(ax);
}

unsigned emulate_opl2_read(unsigned ax) {
  ax = ax & ~0xFF;
  if ((timer_reg & 0xC1) == 1) ax |= 0xC0;
  if ((timer_reg & 0xA2) == 2) ax |= 0xA0;
  if (config.opl3) ax |= STATUS_OPL3_LOW_BITS;
  else             ax |= STATUS_OPL2_LOW_BITS;

  if (config.enable_patching) {
    if ((address & 0xFF) >= 0x20) {
      char _WCI86FAR *opc = port_trap_ip - 1;
      if (*opc == 0xEC) *opc = 0x90;
    }
  }
  if (config.cpu_type >= 5) {
    (volatile) inp(config.com_port);
  }
  return ax;
}

unsigned emulate_opl3_read(unsigned ax) {
  if (!config.opl3) return 0;
  return emulate_opl2_read(ax);
}


/* ---------------------------------------------------------------
 * CODE segment: hw_reset and init_comport
 *
 * These are called only from adlcom.c as near calls (small model).
 * They live in CODE (non-resident) - same as the original source.
 *
 * RULE: must NOT call any RESIDENT function (would be cross-segment
 * near call with wrong CS -> crash).
 * CAN access RESIDENT data via DS (DS=DGROUP always contains RESIDENT).
 *
 * hw_reset (nuke path): inlines Nuke.YKT protocol directly.
 * hw_reset also resets shadow_regs[] via DS - safe data access.
 * --------------------------------------------------------------- */
#pragma code_seg("CODE")

void hw_reset(unsigned com_port, char nuke) {
  int i;
  char cnt;

  /* Reset shadow register cache.
   * shadow_regs is RESIDENT data in DGROUP - DS access, always safe. */
  {
    int j;
    for (j = 0; j < 512; j++) shadow_regs[j] = 0xFF;
    cache_init = 1;
  }

  for (i = 0x00; i < 0xFF; i++) {
    char val = (i >= 0x40 && i <= 0x55) ? 0x3F : 0x00;

    if (!nuke) {
      /* OPL2 parallel-port path - direct register write */
      outp(com_port, i);
      outp(com_port, val);
    }
    else {
      /*
       * OPL3/Nuke.YKT serial protocol - fully inlined.
       * No calls to RESIDENT functions allowed from here.
       *
       * Header byte: 1_00_aaaaaa  (bank=0, upper addr bits from i)
       * Value bytes: b0 = (reg[5:0]<<1)|(val>>7), b1 = val&0x7F
       *
       * opl3Reg and address are RESIDENT data - DS access, safe.
       */
      char hdr, b0, b1;

      opl3Reg = i;
      address = (short)(i & 0xFF);

      hdr = (char)(0x80 | ((i >> 6) & 0x03));
      outp(com_port, hdr);

      b0 = (char)(((i & 0x3F) << 1) | ((val >> 7) & 1));
      b1 = (char)(val & 0x7F);

      while (!(inp(com_port + 5) & 0x20)); outp(com_port, b0);
      while (!(inp(com_port + 5) & 0x20)); outp(com_port, b1);
    }
  }
}

void init_comport(unsigned com_port) {
  outp(com_port + 1, 0x00);    /* Disable all interrupts            */
  outp(com_port + 3, 0x80);    /* Enable DLAB (set baud rate divisor)*/
  outp(com_port + 0, 0x01);    /* Set divisor to 1 (lo byte) 115200  */
  outp(com_port + 1, 0x00);    /*                  (hi byte)         */
  outp(com_port + 3, 0x03);    /* 8 bits, no parity, one stop bit    */
  outp(com_port + 2, 0xC7);    /* Enable FIFO, clear, 14-byte thr.   */
  outp(com_port + 4, 0x0B);    /* IRQs enabled, RTS/DSR set          */
}

#pragma code_seg("RESIDENT")


/* Sound Blaster emulation */

enum sb_write_state { SB_IDLE, SB_IDENT };

static enum sb_write_state RESIDENT sb_write_state = SB_IDLE;
static char RESIDENT sb_read_buf[2] = { 0xAA, 0xAA };

unsigned emulate_ignore(unsigned ax) {
#ifdef DEBUG
  writechar('I'); writechar('G'); writechar('N'); writeln();
#endif
  return ax;
}

unsigned emulate_sbdsp_reset(unsigned ax) {
#ifdef DEBUG
  writechar('R'); writechar('S'); writechar('T'); writeln();
#endif
  sb_write_state = SB_IDLE;
  sb_read_buf[0] = sb_read_buf[1] = 0xAA;
  return ax;
}

unsigned emulate_sbdsp_read_data(unsigned ax) {
  char result = sb_read_buf[0];
#ifdef DEBUG
  writechar('R'); writechar('D'); writechar(' '); write2hex(result); writeln();
#endif
  sb_read_buf[0] = sb_read_buf[1];
  return (ax & ~0xFF) | result;
}

void int0f();
#ifdef _M_I86
#pragma aux int0f = "int 0x0f" modify exact []
#endif

unsigned emulate_sbdsp_write_data(unsigned ax) {
#ifdef DEBUG
  writechar('W'); writechar('R'); writechar(' '); write2hex(ax); writeln();
#endif
  switch (sb_write_state) {
  case SB_IDENT: {
    char reply = ~ax;
    sb_read_buf[0] = sb_read_buf[1] = reply;
    sb_write_state = SB_IDLE;
    break;
  }
  default:
    switch ((char)ax) {
    case 0xE0: case 0x10: case 0x38: case 0x40:
      sb_write_state = SB_IDENT;
      break;
    case 0xE1:
      if (config.opl3) { sb_read_buf[0] = 3; sb_read_buf[1] = 2; }
      else             { sb_read_buf[0] = 1; sb_read_buf[1] = 5; }
      break;
    case 0xF2:
      int0f();
      break;
    default:
      sb_read_buf[0] = sb_read_buf[1] = 0;
      break;
    }
  }
  return ax;
}

unsigned emulate_sbdsp_write_buffer(unsigned ax) {
#ifdef DEBUG
  writechar('W'); writechar('R'); writechar('?'); writechar(' ');
#endif
  return ax & ~0xFF;
}

unsigned emulate_sbdsp_data_avail(unsigned ax) {
#ifdef DEBUG
  writechar('R'); writechar('D'); writechar('?'); writechar(' ');
#endif
  return ax | 0xFF;
}

unsigned emulate_sbmixer_data_write(unsigned ax) {
#ifdef DEBUG
  writechar('M'); writechar('I'); writechar('X'); write2hex(ax); writeln();
#endif
  /* Reuse DSP read buffer - Miles just wants write & read back */
  sb_read_buf[0] = ax;
  sb_read_buf[1] = ax;
  return ax;
}
