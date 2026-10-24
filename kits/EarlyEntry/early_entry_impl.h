/* The entry log uses bootstrap Device mappings until the VM changes TTBRs.
 * arm_init restores output with an ml_io_map address after arm_vm_init returns.
 */
#include "early_entry.h"

#if XNU_EARLY_ENTRY_MARKER
extern unsigned char iso_font[];
void arm_early_panic(const char *message, uintptr_t caller);

static uintptr_t arm_early_video;
static unsigned long arm_early_stride;
static unsigned long arm_early_width;
static unsigned long arm_early_height;
static unsigned long arm_early_log_y;
static unsigned int arm_early_stage_id;

static void
arm_early_text(const char *text, unsigned long y, unsigned long limit)
{
	unsigned long x = 16;

	for (unsigned long index = 0; index < limit && text[index] != '\0'; index++) {
		unsigned char character = (unsigned char)text[index];

		if (character == '\n' || x + 16 > arm_early_width) {
			x = 16;
			y += 32;
		}
		if (y + 32 > arm_early_height) {
			break;
		}
		if (character == '\n') {
			continue;
		}

		for (unsigned long row = 0; row < 32; row++) {
			volatile uint32_t *pixels = (volatile uint32_t *)(arm_early_video +
			    (y + row) * arm_early_stride + x * 4);
			unsigned char bits = iso_font[character * 16 + row / 2];

			for (unsigned long column = 0; column < 16; column++) {
				pixels[column] = (bits & (1U << (column / 2))) ? 0x00ffffff : 0;
			}
		}
		x += 16;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void
arm_early_stage(unsigned int stage, unsigned int color, const char *message)
{
	if (arm_early_video == 0) {
		return;
	}

	arm_early_stage_id = stage;
	unsigned long top = arm_early_height / 2;
	unsigned long bottom = arm_early_height - 160;
	unsigned long row_words = arm_early_stride / 4;
	volatile uint32_t *frame = (volatile uint32_t *)arm_early_video;

	if (arm_early_log_y + 36 > bottom) {
		unsigned long used_lines = (arm_early_log_y - top) / 36;
		unsigned long removed_lines = used_lines < 8 ? used_lines : 8;
		unsigned long removed_rows = removed_lines * 36;
		unsigned long retained_end = arm_early_log_y - removed_rows;

		/* Eight new lines share one copy of the retained Device framebuffer rows. */
		for (unsigned long row = top; row < retained_end; row++) {
			for (unsigned long column = 0; column < row_words; column++) {
				frame[row * row_words + column] = frame[(row + removed_rows) * row_words + column];
			}
		}

		for (unsigned long row = retained_end; row < arm_early_log_y; row++) {
			for (unsigned long column = 0; column < row_words; column++) {
				frame[row * row_words + column] = 0;
			}
		}
		arm_early_log_y = retained_end;
	}

	for (unsigned long row = 0; row < 36; row++) {
		for (unsigned long column = 0; column < row_words; column++) {
			frame[(arm_early_log_y + row) * row_words + column] = color;
		}
	}
	arm_early_text(message, arm_early_log_y + 2, (arm_early_width - 16) / 16);
	arm_early_log_y += 36;
}

void
arm_early_value(const char *name, unsigned long value)
{
	char line[96];
	const char digits[] = "0123456789abcdef";
	unsigned int length = 0;

	while (length < 64 && name[length] != '\0') {
		line[length] = name[length];
		length++;
	}
	line[length++] = ' ';
	line[length++] = '0';
	line[length++] = 'x';
	for (unsigned int index = 0; index < 16; index++) {
		line[length++] = digits[(value >> ((15 - index) * 4)) & 15];
	}
	line[length] = '\0';
	arm_early_stage(arm_early_stage_id, 0x00181818, line);
}

static void
arm_early_begin(const boot_args *args)
{
	if ((args->Video.v_depth & 0xff) != 32 || args->Video.v_width < 48 ||
	    args->Video.v_height < 512 || args->Video.v_rowBytes < args->Video.v_width * 4) {
		return;
	}

	arm_early_stride = args->Video.v_rowBytes;
	arm_early_width = args->Video.v_width;
	arm_early_height = args->Video.v_height;
	arm_early_video = args->Video.v_baseAddr;
	arm_early_log_y = arm_early_height / 2;

	volatile uint32_t *pixels = (volatile uint32_t *)(arm_early_video +
	    arm_early_log_y * arm_early_stride);
	unsigned long words = (arm_early_height - arm_early_log_y) * arm_early_stride / 4;
	for (unsigned long index = 0; index < words; index++) {
		pixels[index] = 0;
	}

	arm_early_stage(8, 0x00008000, "arm_init C entry");
}

void
arm_early_end(void)
{
	arm_early_video = 0;
}

static void
arm_early_resume(const boot_args *args)
{
	if (arm_early_stride == 0) {
		return;
	}

	arm_early_video = ml_io_map(args->Video.v_baseAddr, arm_early_stride * arm_early_height);
	arm_early_stage(15, 0x00004040, "VM returned; framebuffer Device mapping restored");
	arm_early_value("framebuffer VA", arm_early_video);
}

void
arm_early_panic(const char *message, uintptr_t caller)
{
	if (arm_early_video == 0) {
		return;
	}

	char address[17];
	const char digits[] = "0123456789abcdef";
	for (unsigned int index = 0; index < 16; index++) {
		address[index] = digits[(caller >> ((15 - index) * 4)) & 15];
	}
	address[16] = '\0';

	unsigned long top = arm_early_height - 320;
	volatile uint32_t *pixels = (volatile uint32_t *)(arm_early_video + top * arm_early_stride);
	for (unsigned long index = 0; index < 320 * arm_early_stride / 4; index++) {
		pixels[index] = 0x00800000;
	}
	arm_early_text("EARLY PANIC caller:", top, 32);
	arm_early_text(address, top + 32, 16);
	if (message != NULL) {
		arm_early_text(message, top + 64, 511);
	}
}
#else
#define arm_early_begin(args) ((void)0)
#define arm_early_resume(args) ((void)0)
#endif
