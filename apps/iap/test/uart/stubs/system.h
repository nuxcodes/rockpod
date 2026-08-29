#ifndef SYSTEM_H
#define SYSTEM_H
unsigned long inl(unsigned long addr);
void outl(unsigned long v, unsigned long addr);

/* The Makefile checks interrupt masking around the divisor latch. */
#define disable_irq_save()  (0)
#define restore_irq(level)  ((void)(level))


#endif
