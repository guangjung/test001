/*
 * private.h
 */

#ifndef _plugins_klish_h
#define _plugins_klish_h

#include <faux/faux.h>
#include <klish/kcontext_base.h>


C_DECL_BEGIN

// Misc
int klish_nop(kcontext_t *context);
int klish_tsym(kcontext_t *context);

// PTYPEs
int klish_ptype_COMMAND(kcontext_t *context);
int klish_ptype_COMMAND_CASE(kcontext_t *context);

// Navigation
int klish_nav(kcontext_t *context);

C_DECL_END


#endif
