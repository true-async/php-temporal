/* This is a generated file, edit temporal.stub.php instead.
 * Stub hash: 6d3fc18d5e116fe8ab1be5b9809adff736bb3a39 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Connection___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, address, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, identity, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, apiKey, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tlsServerRootCaCert, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tlsClientCert, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tlsClientPrivateKey, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tlsServerName, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Connection_rpcCall, 0, 3, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, service, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, metadata, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Worker___construct, 0, 0, 2)
	ZEND_ARG_OBJ_INFO(0, connection, TrueAsync\\Temporal\\Core\\Connection, 0)
	ZEND_ARG_TYPE_INFO(0, taskQueue, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, namespace, IS_STRING, 0, "\'default\'")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxConcurrentActivities, IS_LONG, 0, "100")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Worker_pollActivityTask, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Worker_completeActivityTask, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, completion, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Worker_recordActivityHeartbeat, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, heartbeat, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_Temporal_Core_Worker_pollWorkflowActivation arginfo_class_TrueAsync_Temporal_Core_Worker_pollActivityTask

#define arginfo_class_TrueAsync_Temporal_Core_Worker_completeWorkflowActivation arginfo_class_TrueAsync_Temporal_Core_Worker_completeActivityTask

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Temporal_Core_Worker_initiateShutdown, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_Temporal_Core_Worker_finalizeShutdown arginfo_class_TrueAsync_Temporal_Core_Worker_initiateShutdown

ZEND_METHOD(TrueAsync_Temporal_Core_Connection, __construct);
ZEND_METHOD(TrueAsync_Temporal_Core_Connection, rpcCall);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, __construct);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, pollActivityTask);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, completeActivityTask);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, recordActivityHeartbeat);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, pollWorkflowActivation);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, completeWorkflowActivation);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, initiateShutdown);
ZEND_METHOD(TrueAsync_Temporal_Core_Worker, finalizeShutdown);

static const zend_function_entry class_TrueAsync_Temporal_Core_Connection_methods[] = {
	ZEND_ME(TrueAsync_Temporal_Core_Connection, __construct, arginfo_class_TrueAsync_Temporal_Core_Connection___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Connection, rpcCall, arginfo_class_TrueAsync_Temporal_Core_Connection_rpcCall, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_TrueAsync_Temporal_Core_Worker_methods[] = {
	ZEND_ME(TrueAsync_Temporal_Core_Worker, __construct, arginfo_class_TrueAsync_Temporal_Core_Worker___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, pollActivityTask, arginfo_class_TrueAsync_Temporal_Core_Worker_pollActivityTask, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, completeActivityTask, arginfo_class_TrueAsync_Temporal_Core_Worker_completeActivityTask, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, recordActivityHeartbeat, arginfo_class_TrueAsync_Temporal_Core_Worker_recordActivityHeartbeat, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, pollWorkflowActivation, arginfo_class_TrueAsync_Temporal_Core_Worker_pollWorkflowActivation, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, completeWorkflowActivation, arginfo_class_TrueAsync_Temporal_Core_Worker_completeWorkflowActivation, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, initiateShutdown, arginfo_class_TrueAsync_Temporal_Core_Worker_initiateShutdown, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Temporal_Core_Worker, finalizeShutdown, arginfo_class_TrueAsync_Temporal_Core_Worker_finalizeShutdown, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_Temporal_TemporalException(zend_class_entry *class_entry_RuntimeException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync\\Temporal", "TemporalException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_RuntimeException, 0);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_Temporal_ConnectionException(zend_class_entry *class_entry_TrueAsync_Temporal_TemporalException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync\\Temporal", "ConnectionException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_Temporal_TemporalException, 0);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_Temporal_ServiceException(zend_class_entry *class_entry_TrueAsync_Temporal_TemporalException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync\\Temporal", "ServiceException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_Temporal_TemporalException, 0);

	zval property_statusDetails_default_value;
	ZVAL_NULL(&property_statusDetails_default_value);
	zend_string *property_statusDetails_name = zend_string_init("statusDetails", sizeof("statusDetails") - 1, true);
	zend_declare_typed_property(class_entry, property_statusDetails_name, &property_statusDetails_default_value, ZEND_ACC_PUBLIC, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release_ex(property_statusDetails_name, true);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_Temporal_Core_Connection(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync\\Temporal\\Core", "Connection", class_TrueAsync_Temporal_Core_Connection_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_Temporal_Core_Worker(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync\\Temporal\\Core", "Worker", class_TrueAsync_Temporal_Core_Worker_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}
