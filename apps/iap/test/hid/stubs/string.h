/* usb_iap_hid.c includes "string.h" with quotes, which finds this
 * before the system header. Hand through to the real one. */
#ifndef HIDSTUB_STRING_H
#define HIDSTUB_STRING_H
#include_next <string.h>
#endif
