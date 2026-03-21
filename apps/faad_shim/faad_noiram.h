/* Force-included AFTER config.h to override IRAM attributes.
 * Core IRAM is full — libfaad must use regular RAM. */
#ifdef IBSS_ATTR
#undef IBSS_ATTR
#endif
#define IBSS_ATTR

#ifdef ICODE_ATTR
#undef ICODE_ATTR
#endif
#define ICODE_ATTR

#ifdef ICONST_ATTR
#undef ICONST_ATTR
#endif
#define ICONST_ATTR

#ifdef IDATA_ATTR
#undef IDATA_ATTR
#endif
#define IDATA_ATTR

/* Override IMDCT-specific attribute too */
#define ICODE_ATTR_TREMOR_MDCT
