#ifndef RF_TERM_H
#define RF_TERM_H

#include <stddef.h>

int rf_term_setup_stdin(char *err, size_t errsz);
void rf_term_restore_stdin(void);

#endif

