#include <stdint.h>
#include "main.h"
#include "rk356x.h"

int rk356x_format_u64(uint64_t value, char *buffer,
		unsigned int buffer_size, unsigned int base) {
	static const char digits[] = "0123456789abcdef";
	char reverse[64];
	unsigned int length = 0;

	if (!buffer || !buffer_size)
		return -1;
	buffer[0] = '\0';
	if (base < 2 || base > 16)
		return -1;
	do {
		reverse[length++] = digits[value % base];
		value /= base;
	} while (value);
	if (length + 1 > buffer_size) {
		return -1;
	}
	for (unsigned int i = 0; i < length; i++)
		buffer[i] = reverse[length - i - 1];
	buffer[length] = '\0';
	return (int)length;
}

static void write_text(const char *text) {
	while (*text)
		uart_chr(*text++);
}

void rk356x_debug_u64(const char *label, uint64_t value) {
	char hex[17];
	char bits[65];

	if (rk356x_format_u64(value, hex, sizeof(hex), 16) < 0 ||
		rk356x_format_u64(value, bits, sizeof(bits), 2) < 0)
		return;
	write_text("[UART] ");
	write_text(label);
	write_text(hex);
	write_text(" (0b");
	write_text(bits);
	write_text(")\n\r");
}
