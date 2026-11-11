#ifndef HENGE_TL_APPKIT_MINI_H
#define HENGE_TL_APPKIT_MINI_H

/* A toy AppKit for tests (see appkit_mini.m): the TlHost.shim_lookup for AppKit and Cocoa. */
void *tl_appkit_mini_lookup(const char *install_name, const char *symbol);
/* Whether the real AppKit is in this process. */
int tl_appkit_is_real_loaded(void);

#endif
