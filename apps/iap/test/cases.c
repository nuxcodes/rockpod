/***************************************************************************
 * Test registry.
 *
 * Add new cases here. The runner in iap_test.c calls iaptest_init() before
 * each one, so every case starts from a freshly connected accessory.
 ****************************************************************************/

#include "config.h"     /* CONFIG_TUNER, which cases.def switches on */
#include "iap_test.h"

#define CASE(fn) void fn(void);
#include "cases.def"
#undef CASE

const struct iaptest_case iaptest_cases[] = {
#define CASE(fn) { #fn, fn },
#include "cases.def"
#undef CASE
};

const int iaptest_case_count =
    (int)(sizeof(iaptest_cases) / sizeof(iaptest_cases[0]));
