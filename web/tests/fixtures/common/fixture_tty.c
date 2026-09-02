#include <sio.h>
#include "fixture_tty.h"

void fixture_tty_init(void)
{
	sio_init(38400, 0, 0, 0, 0);
}

void fixture_tty_line(const char *line)
{
	sio_puts(line);
}

static char *append(char *dst, const char *src)
{
	while (*src)
		*dst++ = *src++;
	return dst;
}

void fixture_tty_kv_str(const char *key, const char *value)
{
	char buf[96];
	char *p = append(buf, key);
	*p++ = '=';
	p = append(p, value);
	*p = '\0';
	sio_puts(buf);
}

void fixture_tty_kv_hex(const char *key, u32 value)
{
	static const char digits[] = "0123456789abcdef";
	char buf[64];
	char *p = append(buf, key);
	int shift;
	*p++ = '=';
	*p++ = '0';
	*p++ = 'x';
	for (shift = 28; shift >= 0; shift -= 4)
		*p++ = digits[(value >> shift) & 0xf];
	*p = '\0';
	sio_puts(buf);
}

void fixture_tty_kv_dec(const char *key, u32 value)
{
	char tmp[11];
	char buf[64];
	char *p = append(buf, key);
	int n = 0;
	*p++ = '=';
	do {
		tmp[n++] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	while (n)
		*p++ = tmp[--n];
	*p = '\0';
	sio_puts(buf);
}
