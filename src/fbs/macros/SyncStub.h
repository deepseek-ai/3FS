#include "common/utils/Result.h"

#ifdef DEFINE_FBS_SERVICE
#undef DEFINE_FBS_SERVICE
#endif

#define DEFINE_FBS_SERVICE(name, fbsns)                   \
  template <typename Ctx>                                 \
  class name##ServiceStub : public I##name##ServiceStub { \
   public:                                                \
    using ContextType = Ctx;                              \
    using name##Client = ::fbsns::name##Client<Ctx>;      \
                                                          \
    explicit name##ServiceStub(Ctx ctx)                   \
        : client_(std::move(ctx)) {}

#ifdef FINISH_FBS_SERVICE
#undef FINISH_FBS_SERVICE
#endif

#define FINISH_FBS_SERVICE(name, fbsns) \
 private:                               \
  name##Client client_;                 \
  }

#ifdef DEFINE_FBS_SERVICE_METHOD
#undef DEFINE_FBS_SERVICE_METHOD
#endif

#define DEFINE_FBS_SERVICE_METHOD(svc, name, reqtype, rsptype, flatns) \
  hf3fs::Result<flatns::rsptype> name(const flatns::reqtype &req) override

#ifdef DEFINE_FBS_SERVICE_METHOD_VOID
#undef DEFINE_FBS_SERVICE_METHOD_VOID
#endif

#define DEFINE_FBS_SERVICE_METHOD_VOID(svc, name, reqtype, rsptype, flatns) \
  hf3fs::Result<Void> name(const flatns::reqtype &req) override

#ifdef DEFINE_FBS_SERVICE_METHOD_RETURNS
#undef DEFINE_FBS_SERVICE_METHOD_RETURNS
#endif

#define DEFINE_FBS_SERVICE_METHOD_RETURNS(svc, name, reqtype, rsptype, flatns, returns, rtype) \
  hf3fs::Result<rtype> name(const flatns::reqtype &req) override
