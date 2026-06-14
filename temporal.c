/*
  +----------------------------------------------------------------------+
  | php-temporal — native async Temporal transport for PHP TrueAsync     |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License").       |
  +----------------------------------------------------------------------+
  | Author: Edmond                                                        |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "ext/spl/spl_exceptions.h"
#include "zend_exceptions.h"

#include "php_temporal.h"
#include "temporal_internal.h"
#include "temporal_core.h"
#include "temporal_arginfo.h"

/* Process-wide core runtime (Tokio). One per process, shared by all connections. */
void *temporal_runtime_handle = NULL;

zend_class_entry *temporal_ce_temporal_exception = NULL;
zend_class_entry *temporal_ce_connection_exception = NULL;
zend_class_entry *temporal_ce_service_exception = NULL;
zend_class_entry *temporal_ce_connection = NULL;
zend_class_entry *temporal_ce_worker = NULL;

PHP_MINIT_FUNCTION(temporal)
{
	/* Exceptions (chain under SPL's RuntimeException). */
	temporal_ce_temporal_exception =
		register_class_TrueAsync_Temporal_TemporalException(spl_ce_RuntimeException);
	temporal_ce_connection_exception =
		register_class_TrueAsync_Temporal_ConnectionException(temporal_ce_temporal_exception);
	temporal_ce_service_exception =
		register_class_TrueAsync_Temporal_ServiceException(temporal_ce_temporal_exception);

	/* The transport objects. */
	temporal_ce_connection = register_class_TrueAsync_Temporal_Core_Connection();
	temporal_ce_worker = register_class_TrueAsync_Temporal_Core_Worker();
	temporal_register_objects();

	/* Create the process-wide core runtime. */
	char *err = NULL;
	temporal_runtime_handle = tphp_runtime_new(&err);
	if (temporal_runtime_handle == NULL) {
		zend_error(E_WARNING, "temporal: failed to create core runtime: %s",
		           err != NULL ? err : "unknown error");
		free(err);
		return FAILURE;
	}

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(temporal)
{
	if (temporal_runtime_handle != NULL) {
		tphp_runtime_free(temporal_runtime_handle);
		temporal_runtime_handle = NULL;
	}

	return SUCCESS;
}

PHP_MINFO_FUNCTION(temporal)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "Temporal support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_TEMPORAL_VERSION);
	php_info_print_table_row(2, "Core c-bridge", "temporalio-sdk-core-c-bridge v0.4.0");
	php_info_print_table_end();
}

zend_module_entry temporal_module_entry = {
	STANDARD_MODULE_HEADER,
	"temporal",
	NULL,                       /* functions */
	PHP_MINIT(temporal),
	PHP_MSHUTDOWN(temporal),
	NULL,                       /* RINIT */
	NULL,                       /* RSHUTDOWN */
	PHP_MINFO(temporal),
	PHP_TEMPORAL_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_TEMPORAL
ZEND_GET_MODULE(temporal)
#endif
