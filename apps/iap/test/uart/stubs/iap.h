#ifndef IAP_H
#define IAP_H
#include <stdbool.h>
/* HAVE_IAP_MULTIPORT is not defined for the Video, so the port
 * argument disappears -- same as the real header. */
#define IF_IAP_MP(x...)
#define IF_IAP_MP_NONVOID(x...) void
bool iap_getc(unsigned char c);
void iap_rx_flush(void);
void iap_reset_state(void);
#endif
