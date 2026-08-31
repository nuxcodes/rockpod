#ifndef KERNEL_H
#define KERNEL_H
void sleep(int ticks);
void tick_add_task(void (*f)(void));
#endif
