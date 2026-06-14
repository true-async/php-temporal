/*
  +----------------------------------------------------------------------+
  | php-temporal — native async Temporal client for PHP TrueAsync        |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License").       |
  +----------------------------------------------------------------------+
  | Author: Edmond                                                        |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_TEMPORAL_H
#define PHP_TEMPORAL_H

extern zend_module_entry temporal_module_entry;
#define phpext_temporal_ptr &temporal_module_entry

#define PHP_TEMPORAL_VERSION "0.1.0-dev"

#ifdef ZTS
#include "TSRM.h"
#endif

#endif /* PHP_TEMPORAL_H */
