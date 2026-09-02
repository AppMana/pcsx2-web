#ifndef FIXTURE_TTY_H
#define FIXTURE_TTY_H

#include <tamtypes.h>

/* Output goes to the EE SIO transmit FIFO at 0x1000F180 through libkernel's
 * sio_putc, which is the byte stream PCSX2 line buffers into its EE console
 * (pcsx2/HwWrite.cpp, _hwWrite8, SIO_TXFIFO case). Every line has the form
 * KEY=VALUE so a harness can filter the fixture's own lines out of anything
 * the BIOS prints on the same channel. */
void fixture_tty_init(void);
void fixture_tty_line(const char *line);
void fixture_tty_kv_str(const char *key, const char *value);
void fixture_tty_kv_hex(const char *key, u32 value);
void fixture_tty_kv_dec(const char *key, u32 value);

#endif
