#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "hid_keyboard.h"
#include "input.h"
#include "ohci.h"
#include "usb.h"

static void report(uint8_t modifier, uint8_t usage) {
	uint8_t data[8] = { modifier, 0, usage, 0, 0, 0, 0, 0 };
	hid_keyboard_report(data);
}

static void release_keys(void) {
	uint8_t data[8] = { 0 };
	hid_keyboard_report(data);
}

static void test_hid(void) {
	input_reset(); hid_keyboard_reset();
	report(0, 0x04); assert(input_get_char() == 'a');
	report(0, 0x04); assert(!input_available());
	release_keys(); report(0x02, 0x04); assert(input_get_char() == 'A');
	release_keys(); report(0, 0x39); release_keys();
	report(0, 0x04); assert(input_get_char() == 'A');
	release_keys(); report(0x02, 0x1e); assert(input_get_char() == '!');
	release_keys(); report(0, 0x28); assert(input_get_char() == '\r');
	release_keys(); report(0, 0x2a); assert(input_get_char() == '\b');
	release_keys(); report(0, 0x2b); assert(input_get_char() == '\t');
	release_keys(); report(0, 0x29); assert(input_get_char() == 0x1b);
	release_keys(); report(0, 0x4f); assert(!input_available());
	release_keys(); report(0, 0x3a); assert(!input_available());
	release_keys(); report(0x04, 0x04); assert(!input_available());
	release_keys();
	uint8_t rollover[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
	hid_keyboard_report(rollover); assert(!input_available());

	input_reset();
	for (unsigned int i = 0; i < 63; i++) assert(input_enqueue('x'));
	assert(!input_enqueue('y'));
	for (unsigned int i = 0; i < 32; i++) assert(input_get_char() == 'x');
	for (unsigned int i = 0; i < 31; i++) assert(input_enqueue('z'));
	for (unsigned int i = 0; i < 31; i++) assert(input_get_char() == 'x');
	for (unsigned int i = 0; i < 31; i++) assert(input_get_char() == 'z');
	assert(!input_available());
}

static void test_usb(void) {
	uint8_t composite[] = {
		9, USB_DT_CONFIG, 34, 0, 2, 1, 0, 0x80, 50,
		9, USB_DT_INTERFACE, 0, 0, 1, 8, 6, 80, 0,
		9, USB_DT_INTERFACE, 2, 0, 1, 3, 1, 1, 0,
		7, USB_DT_ENDPOINT, 0x83, 3, 8, 0, 10,
	};
	struct UsbBootKeyboard keyboard;
	assert(!usb_find_boot_keyboard(composite, sizeof(composite), &keyboard));
	assert(keyboard.interface_number == 2 && keyboard.endpoint == 3);
	assert(keyboard.max_packet == 8 && keyboard.interval == 10);
	composite[9 + 9 + 9 + 6] = 0;
	assert(usb_find_boot_keyboard(composite, sizeof(composite), &keyboard));
	composite[9 + 9 + 9 + 6] = 10;
	composite[9 + 9 + 3] = 1;
	assert(usb_find_boot_keyboard(composite, sizeof(composite), &keyboard));
	composite[9 + 9 + 3] = 0;
	composite[9 + 9 + 7] = 2;
	assert(usb_find_boot_keyboard(composite, sizeof(composite), &keyboard));

	static uint8_t arena[1024 + 256];
	uintptr_t start = (uintptr_t)arena;
	ohci_dma_configure(start, start + sizeof(arena));
	void *a = ohci_dma_alloc(17, 16);
	void *b = ohci_dma_alloc(256, 256);
	assert(a && b && !((uintptr_t)a & 15) && !((uintptr_t)b & 255));
	assert(ohci_dma_contains((uintptr_t)a, 17));
	assert(!ohci_dma_contains(start - 1, 1));
	assert(ohci_dma_contains(start + sizeof(arena) - 1, 1));
	assert(!ohci_dma_contains(start + sizeof(arena) - 1, 2));
	assert(!ohci_dma_alloc(sizeof(arena), 16));
}

int main(void) {
	test_hid();
	test_usb();
	puts("RK356x host unit tests passed");
	return 0;
}
