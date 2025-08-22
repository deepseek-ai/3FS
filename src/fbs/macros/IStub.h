#include "common/utils/Coroutine.h"

#ifdef DEFINE_FBS_SERVICE
#undef DEFINE_FBS_SERVICE
#endif

#define DEFINE_FBS_SERVICE(name, fbsns)         \
  class I##name##ServiceStub {                  \
   public:                                      \
    using InterfaceType = I##name##ServiceStub; \
                                                \
    virtual ~I##name##ServiceStub() = default;

#ifdef FINISH_FBS_SERVICE
#undef FINISH_FBS_SERVICE
#endif

#define FINISH_FBS_SERVICE(name, fbsns) }

#ifdef DEFINE_FBS_SERVICE_METHOD
#undef DEFINE_FBS_SERVICE_METHOD
#endif

#define DEFINE_FBS_SERVICE_METHOD(svc, name, reqtype, rsptype, flatns) \
  virtual CoTryTask<flatns::rsptype> name(const flatns::reqtype &req) = 0

#ifdef DEFINE_FBS_SERVICE_METHOD_VOID
#undef DEFINE_FBS_SERVICE_METHOD_VOID
#endif

#define DEFINE_FBS_SERVICE_METHOD_VOID(svc, name, reqtype, rsptype, flatns) \
  DEFINE_FBS_SERVICE_METHOD(svc, name, reqtype, rsptype, flatns)

#ifdef DEFINE_FBS_SERVICE_METHOD_RETURNS
#undef DEFINE_FBS_SERVICE_METHOD_RETURNS
#endif

#define DEFINE_FBS_SERVICE_METHOD_RETURNS(svc, name, reqtype, rsptype, flatns, returns, rtype) \
  DEFINE_FBS_SERVICE_METHOD(svc, name, reqtype, rsptype, flatns)
