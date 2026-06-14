/*
  +----------------------------------------------------------------------+
  | php-temporal — native async Temporal transport for PHP TrueAsync     |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License").       |
  +----------------------------------------------------------------------+

  Shared declarations across the Zend-facing translation units (temporal.c,
  temporal_client.c). Not part of the public surface.
*/

#ifndef PHP_TEMPORAL_INTERNAL_H
#define PHP_TEMPORAL_INTERNAL_H

#include <stddef.h>

#include "php.h"

/* Process-wide Temporal core runtime (Tokio); created in MINIT. */
extern void *temporal_runtime_handle;

/* Class entries, registered in MINIT. */
extern zend_class_entry *temporal_ce_temporal_exception;
extern zend_class_entry *temporal_ce_connection_exception;
extern zend_class_entry *temporal_ce_service_exception;
extern zend_class_entry *temporal_ce_connection;
extern zend_class_entry *temporal_ce_worker;

/* Core\Connection object: owns one core connection handle (the transport). */
typedef struct {
	void        *connection;
	zend_object  std;
} temporal_connection_obj;

static zend_always_inline temporal_connection_obj *temporal_connection_from_obj(zend_object *obj)
{
	return (temporal_connection_obj *) ((char *) obj - offsetof(temporal_connection_obj, std));
}

/* Core\Worker object: owns one core worker handle + a ref to its connection. */
typedef struct {
	void        *worker;
	zend_object *connection;   /* the Core\Connection object, ref held */
	bool         finalized;    /* finalizeShutdown ran: the core handle is spent */
	zend_object  std;
} temporal_worker_obj;

static zend_always_inline temporal_worker_obj *temporal_worker_from_obj(zend_object *obj)
{
	return (temporal_worker_obj *) ((char *) obj - offsetof(temporal_worker_obj, std));
}

/* Registered by temporal_client.c, called from MINIT. */
void temporal_register_objects(void);

#endif /* PHP_TEMPORAL_INTERNAL_H */
