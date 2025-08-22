#ifndef DEFINE_SERDE_SERVICE_METHOD_FULL
#define DEFINE_SERDE_SERVICE_METHOD_FULL(ServiceName, methodName, MethodName, MethodId, RequestType, ResponseType)
#endif

#ifndef DEFINE_SERDE_SERVICE_METHOD
#define DEFINE_SERDE_SERVICE_METHOD(ServiceName, methodName, MethodName, MethodId) \
  DEFINE_SERDE_SERVICE_METHOD_FULL(ServiceName, methodName, MethodName, MethodId, MethodName##Req, MethodName##Rsp)
#endif
