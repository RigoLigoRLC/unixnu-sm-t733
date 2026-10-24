#ifndef UNIXNU_EARLY_ENTRY_H
#define UNIXNU_EARLY_ENTRY_H

#if XNU_EARLY_ENTRY_MARKER
void arm_early_stage(unsigned int stage, unsigned int color, const char *message);
void arm_early_value(const char *name, unsigned long value);
void arm_early_end(void);
#else
#define arm_early_stage(stage, color, message) ((void)0)
#define arm_early_value(name, value) ((void)0)
#define arm_early_end() ((void)0)
#endif

#endif
