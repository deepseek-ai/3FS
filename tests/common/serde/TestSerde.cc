#include <folly/json.h>

#include "common/serde/Serde.h"
#include "common/utils/DownwardBytes.h"
#include "common/utils/Reflection.h"
#include "common/utils/Result.h"
#include "common/utils/RobinHood.h"
#include "common/utils/StrongType.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

bool checkValue(float b) { return b > 10.0; }

struct CheckString {
  constexpr CheckString() = default;
  constexpr bool operator()(const std::string &str) const { return !str.empty(); }
};

struct Demo {
  SERDE_CLASS_FIELD(a, short{10}, [](short a) { return a > 0; });  // checker is lambda expr.
  SERDE_CLASS_FIELD(b, 20.0f, checkValue);                         // checker is function pointer.
  SERDE_CLASS_FIELD(c, std::string{"ok"}, CheckString{});          // checker is function object.
};
static_assert(serde::count<Demo>() == 3);
static_assert(serde::name<Demo, 0>() == "a");
static_assert(serde::name<Demo, 1>() == "b");
static_assert(serde::name<Demo, 2>() == "c");
static_assert(serde::SerdeType<Demo>);
static_assert(!serde::SerdeType<int>);

TEST(TestSerde, Normal) {
  const Demo demo;
  ASSERT_EQ(serde::name<0>(demo), "a");
  ASSERT_EQ(serde::name<1>(demo), "b");
  ASSERT_EQ(serde::name<2>(demo), "c");
  ASSERT_EQ(serde::value<0>(demo), 10);
  ASSERT_EQ(serde::value<1>(demo), 20.0);
  ASSERT_EQ(serde::value<2>(demo), "ok");
  ASSERT_EQ(&serde::value<0>(demo), &demo.a());
  ASSERT_EQ(&serde::value<1>(demo), &demo.b());
  ASSERT_EQ(&serde::value<2>(demo), &demo.c());
}

TEST(TestSerde, Serialize) {
  Demo demo;
  demo.a() = 100;
  demo.b() = 200.f;
  demo.c() = "hello";

  const Demo &d = demo;
  auto out = serde::serialize(d);
  ASSERT_EQ(out.length(), serde::serializeLength(d));
  constexpr auto size = 1 + 2 + 4 + 1 + 5;  // struct size + short + float + str length + str content.
  ASSERT_EQ(out.length(), size);

  XLOGF(INFO, "out is {:02X}", fmt::join(out, ","));

  Demo des;
  ASSERT_EQ(des.a(), 10);
  ASSERT_EQ(des.b(), 20.f);
  ASSERT_EQ(des.c(), "ok");

  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(des.a(), 100);
  ASSERT_EQ(des.b(), 200.f);
  ASSERT_EQ(des.c(), "hello");
}

struct Nested {
  SERDE_CLASS_FIELD(a, Demo{});
  SERDE_CLASS_FIELD(b, Demo{});
};

TEST(TestSerde, Serialize2) {
  Nested nested;
  nested.b().a() = 100;
  nested.b().b() = 200.f;
  nested.b().c() = "hello";

  auto out = serde::serialize(nested);
  ASSERT_EQ(out.length(), serde::serializeLength(nested));
  constexpr auto size = 1 + (1 + 2 + 4 + 1 + 2) + (1 + 2 + 4 + 1 + 5);
  ASSERT_EQ(out.length(), size);

  Nested des;
  ASSERT_EQ(des.a().a(), des.b().a());
  ASSERT_EQ(des.a().b(), des.b().b());
  ASSERT_EQ(des.a().c(), des.b().c());

  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(des.a().a(), 10);
  ASSERT_EQ(des.a().b(), 20.f);
  ASSERT_EQ(des.a().c(), "ok");
  ASSERT_EQ(des.b().a(), 100);
  ASSERT_EQ(des.b().b(), 200.f);
  ASSERT_EQ(des.b().c(), "hello");
}

TEST(TestSerde, Vector) {
  struct Test {
    SERDE_CLASS_FIELD(a, (std::vector<int>{0, 1, 2}));
    SERDE_CLASS_FIELD(b, (std::vector<std::string>{"x", "y"}));
  };

  Test a;
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.size(), 1 + 1 + 4 * 3 + 1 + 2 * (1 + 1));  // struct size + vsize + int * 3 + vsize + 2 * (ssize + c)

  a.a() = std::vector<int>{0, 1, 0, 1};
  a.b() = std::vector<std::string>{"a", "b", "c", "d"};
  out = serde::serialize(a);

  Test b;
  ASSERT_NE(b.a(), a.a());
  ASSERT_NE(b.b(), a.b());
  ASSERT_OK(serde::deserialize(b, out));
  ASSERT_EQ(b.a(), a.a());
  ASSERT_EQ(b.b(), a.b());
}

TEST(TestSerde, Vector2) {
  struct Item {
    SERDE_STRUCT_FIELD(a, uint32_t{});
    SERDE_STRUCT_FIELD(b, std::string{});
  };
  struct Vector {
    SERDE_STRUCT_FIELD(vec, std::vector<Item>{});
  };

  Vector vec;
  vec.vec.push_back({1, "one"});
  vec.vec.push_back({2, "two"});
  XLOGF(INFO, "vec is {}", vec);
}

TEST(TestSerde, Vector3) {
  struct Item {
    SERDE_STRUCT_FIELD(a, std::vector<uint32_t>{});
  };

  Item item;
  item.a.push_back(2);
  item.a.push_back(0);
  item.a.push_back(2);
  item.a.push_back(3);
  auto out = serde::serialize(item);

  ASSERT_EQ(out.size(), 18);
  ASSERT_EQ(out[0], 17);
  ASSERT_EQ(out[1], 4);

  Item des;
  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(item.a, des.a);
}

TEST(TestSerde, Set) {
  struct Test {
    SERDE_CLASS_FIELD(a, (std::set<int>{0, 1, 2}));
    SERDE_CLASS_FIELD(b, (std::set<std::string>{"x", "y"}));
  };

  Test a;
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.size(), 1 + 1 + 4 * 3 + 1 + 2 * (1 + 1));

  a.a() = std::set<int>{0, 1, 0, 1};
  a.b() = std::set<std::string>{"a", "b", "c", "d"};
  out = serde::serialize(a);

  Test b;
  ASSERT_NE(b.a(), a.a());
  ASSERT_NE(b.b(), a.b());
  ASSERT_OK(serde::deserialize(b, out));
  ASSERT_EQ(b.a(), a.a());
  ASSERT_EQ(b.b(), a.b());
}

TEST(TestSerde, Map) {
  struct Test {
    SERDE_CLASS_FIELD(a, (std::map<int, int>{}));
    SERDE_CLASS_FIELD(b, (robin_hood::unordered_map<std::string, std::string>{}));
  };

  Test a;
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.size(), 1 + 1 + 1);

  a.a()[1] = 2;
  a.a()[3] = 4;
  a.b()["hello"] = "world";
  a.b()["nice to"] = "meet you";
  out = serde::serialize(a);
  ASSERT_EQ(out.size(), 1 + 1 + 4 * 4 + 1 + 4 * 1 + 5 + 5 + 7 + 8);

  Test b;
  ASSERT_NE(b.a(), a.a());
  ASSERT_NE(b.b(), a.b());
  ASSERT_OK(serde::deserialize(b, out));
  ASSERT_EQ(b.a(), a.a());
  ASSERT_EQ(b.b(), a.b());
}

TEST(TestSerde, Optional) {
  struct Test {
    SERDE_CLASS_FIELD(a, std::optional<std::string>{});
    SERDE_CLASS_FIELD(b, std::optional<std::string>{"OK"});
  };

  Test a;
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.size(), 1 + 1 + 1 + 1 + 2);

  a.a() = "hello";
  a.b() = std::nullopt;
  out = serde::serialize(a);
  ASSERT_EQ(out.size(), 1 + 1 + 1 + 5 + 1);

  Test b;
  ASSERT_NE(b.a(), a.a());
  ASSERT_NE(b.b(), a.b());
  ASSERT_OK(serde::deserialize(b, out));
  ASSERT_EQ(b.a(), a.a());
  ASSERT_EQ(b.b(), a.b());
}

TEST(TestSerde, Variant) {
  struct Test {
    SERDE_CLASS_FIELD(a, (std::variant<int, float, std::string>{}));
  };

  Test a;
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.size(), 1 + 1 + 3 + 4);

  a.a() = "hello";
  out = serde::serialize(a);
  ASSERT_EQ(out.size(), 1 + 1 + 12 + 1 + 5);

  Test b;
  ASSERT_NE(b.a(), a.a());
  ASSERT_OK(serde::deserialize(b, out));
  ASSERT_EQ(b.a(), a.a());
}

TEST(TestSerde, Compatibility) {
  struct Test {
    SERDE_CLASS_FIELD(a, std::optional<std::string>{});
    SERDE_CLASS_FIELD(b, std::vector<int>{});
  };

  Test a;
  a.a() = "ok";
  a.b() = {1, 2, 3};
  auto out = serde::serialize(a);
  ASSERT_EQ(out.length(), serde::serializeLength(a));
  ASSERT_EQ(out.length(), 1 + 1 + 1 + 2 + 1 + 4 * 3);
  XLOGF(INFO, "out is {:02x}", fmt::join(out, ","));

  size_t size = out.length();
  for (size_t i = 1; i <= size; ++i) {
    std::string_view view = out;
    *reinterpret_cast<uint8_t *>(out.data()) = i - 1;
    view = view.substr(0, i);

    Test t;
    if (i == 1) {
      ASSERT_OK(serde::deserialize(t, view));
      ASSERT_FALSE(t.a().has_value());
      ASSERT_TRUE(t.b().empty());
    } else if (i == 5) {
      ASSERT_OK(serde::deserialize(t, view));
      ASSERT_TRUE(t.a().has_value());
      ASSERT_EQ(t.a().value(), "ok");
      ASSERT_TRUE(t.b().empty());
    } else if (i == size) {
      ASSERT_OK(serde::deserialize(t, view));
      ASSERT_TRUE(t.a().has_value());
      ASSERT_EQ(t.a().value(), "ok");
      ASSERT_FALSE(t.b().empty());
    } else {
      XLOGF(INFO, "length is {}", i);
      ASSERT_FALSE(serde::deserialize(a, view));
    }
  }
}

struct Base {
  SERDE_STRUCT_FIELD(x, int{});
  SERDE_STRUCT_FIELD(y, int{});
};

struct Derived : Base {
  SERDE_STRUCT_FIELD(z, int{});
};

struct Grandson : Derived {
  SERDE_STRUCT_FIELD(x, int{});
  SERDE_STRUCT_FIELD(y, int{});
  SERDE_STRUCT_FIELD(z, int{});
};

namespace v1 {
struct Base {
  SERDE_STRUCT_FIELD(x, int{});
};

struct Derived : Base {
  SERDE_STRUCT_FIELD(z, int{});
};

struct Grandson : Derived {
  SERDE_STRUCT_FIELD(x, int{});
  SERDE_STRUCT_FIELD(y, int{});
};
};  // namespace v1

namespace v3 {
struct Base {
  SERDE_STRUCT_FIELD(x, int{});
  SERDE_STRUCT_FIELD(y, int{});
  SERDE_STRUCT_FIELD(z, int{});
};

struct Derived : Base {
  SERDE_STRUCT_FIELD(z, int{});
  SERDE_STRUCT_FIELD(a, int{});
};

struct Grandson : Derived {
  SERDE_STRUCT_FIELD(x, int{});
  SERDE_STRUCT_FIELD(y, int{});
  SERDE_STRUCT_FIELD(z, int{});
  SERDE_STRUCT_FIELD(a, int{});
};
};  // namespace v3

static_assert(serde::count<Base>() == 2, "field count of Base must be 2");
static_assert(serde::count<Derived>() == 3, "field count of Derived must be 3");
static_assert(serde::count<Grandson>() == 6, "field count of Grandson must be 6");
static_assert(serde::count<v1::Grandson>() == 4);

static_assert(serde::getter<Grandson, 0>() != serde::getter<Grandson, 3>());
static_assert(serde::getter<Grandson, 1>() != serde::getter<Grandson, 4>());
static_assert(serde::getter<Grandson, 2>() != serde::getter<Grandson, 5>());

static_assert(serde::name<Base, 0>() == "x");
static_assert(serde::name<Base, 1>() == "y");

static_assert(serde::name<Derived, 0>() == "x");
static_assert(serde::name<Derived, 1>() == "y");
static_assert(serde::name<Derived, 2>() == "z");

static_assert(serde::name<Grandson, 0>() == "x");
static_assert(serde::name<Grandson, 1>() == "y");
static_assert(serde::name<Grandson, 2>() == "z");
static_assert(serde::name<Grandson, 3>() == "x");
static_assert(serde::name<Grandson, 4>() == "y");
static_assert(serde::name<Grandson, 5>() == "z");

TEST(TestSerde, Inherit) {
  Grandson x;
  x.Base::x = 10;
  x.Base::y = 12;
  x.Derived::z = 16;
  x.Grandson::x = 20;
  x.Grandson::y = 24;
  x.Grandson::z = 28;
  ASSERT_EQ(serde::count(x), 6);
  ASSERT_EQ(serde::value<0>(x), 10);
  ASSERT_EQ(serde::value<3>(x), 20);

  int typeCount = 1;
  refl::Helper::iterate<Grandson, true>([](auto t) { XLOGF(INFO, "field name: {}", t.name); },
                                        [&]() {
                                          ++typeCount;
                                          XLOGF(INFO, "type changed");
                                        });
  ASSERT_EQ(typeCount, 3);

  auto out = serde::serialize(x);
  Grandson o;
  ASSERT_OK(serde::deserialize(o, out));
  ASSERT_EQ(x.Base::x, o.Base::x);
  ASSERT_EQ(x.Base::y, o.Base::y);
  ASSERT_EQ(x.Derived::z, o.Derived::z);
  ASSERT_EQ(x.Grandson::x, o.Grandson::x);
  ASSERT_EQ(x.Grandson::y, o.Grandson::y);
  ASSERT_EQ(x.Grandson::z, o.Grandson::z);

  v1::Grandson p;
  ASSERT_OK(serde::deserialize(p, out));
  ASSERT_EQ(x.Base::x, p.Base::x);
  ASSERT_EQ(x.Derived::z, p.Derived::z);
  ASSERT_EQ(x.Grandson::x, p.Grandson::x);
  ASSERT_EQ(x.Grandson::y, p.Grandson::y);

  v3::Grandson q;
  ASSERT_OK(serde::deserialize(q, out));
  ASSERT_EQ(x.Base::x, q.Base::x);
  ASSERT_EQ(x.Base::y, q.Base::y);
  ASSERT_EQ(q.Base::z, 0);
  ASSERT_EQ(x.Derived::z, q.Derived::z);
  ASSERT_EQ(q.Derived::a, 0);
  ASSERT_EQ(x.Grandson::x, q.Grandson::x);
  ASSERT_EQ(x.Grandson::y, q.Grandson::y);
  ASSERT_EQ(x.Grandson::z, q.Grandson::z);
  ASSERT_EQ(q.Grandson::a, 0);
}

struct WithStringView {
  SERDE_STRUCT_FIELD(msg, std::string_view{});
  SERDE_STRUCT_FIELD(test, std::string_view{});
};

TEST(TestSerde, StringView) {
  WithStringView view;
  view.msg = "OK";
  view.test = "test";
  auto out = serde::serialize(view);
  ASSERT_EQ(out.length(), serde::serializeLength(view));

  WithStringView des;
  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(view.msg, des.msg);
  ASSERT_EQ(view.test, des.test);
  ASSERT_LT(out.data(), des.msg.data());
  ASSERT_LT(des.msg.data() + des.msg.length(), des.test.data());
  ASSERT_LE(des.test.data() + des.test.length(), out.data() + out.length());
}

TEST(TestSerde, Result) {
  {
    Result<std::string> ser = "OK";
    XLOGF(INFO, "result: {}", serde::toJsonString(ser));

    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 1 + 2);

    Result<std::string> des = makeError(Status::OK);
    ASSERT_FALSE(des);
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_OK(des);
    ASSERT_EQ(des, ser);
  }

  {
    Result<std::string> ser = makeError(StatusCode::kInvalidArg);
    XLOGF(INFO, "result: {}", serde::toJsonString(ser));

    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 2 + 1);

    Result<std::string> des = makeError(Status::OK);
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_EQ(des.error().code(), StatusCode::kInvalidArg);
    ASSERT_TRUE(des.error().message().empty());
  }

  {
    Result<std::string> ser = makeError(StatusCode::kInvalidArg, "TT");
    XLOGF(INFO, "result: {}", serde::toJsonString(ser));

    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 2 + 1 + 1 + 2);

    Result<std::string> des = makeError(Status::OK);
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_EQ(des.error().code(), StatusCode::kInvalidArg);
    ASSERT_EQ(des.error().message(), "TT");
  }

  {
    Result<Void> ser = Void{};
    XLOGF(INFO, "result: {}", serde::toJsonString(ser));

    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1);

    Result<Void> des = makeError(Status::OK);
    ASSERT_FALSE(des);
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_OK(des);
    ASSERT_EQ(des, ser);
  }

  {
    Result<Void> ser = makeError(StatusCode::kInvalidArg);
    XLOGF(INFO, "result: {}", serde::toJsonString(ser));

    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 2 + 1);

    Result<Void> des = makeError(Status::OK);
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_EQ(des.error().code(), StatusCode::kInvalidArg);
    ASSERT_TRUE(des.error().message().empty());
  }
}

TEST(TestSerde, NewDes) {
  Demo ser;
  ser.a() = 1;
  ser.b() = 2;
  ser.c() = "test";
  auto out = serde::serialize(ser);

  Demo des;
  serde::In<std::string_view> in(out);
  ASSERT_OK(serde::deserialize(des, in));
  ASSERT_EQ(des.a(), 1);
  ASSERT_EQ(des.b(), 2);
  ASSERT_EQ(des.c(), "test");
}

TEST(TestSerde, NewDesJson) {
  auto json = R"({
    "a": 1,
    "b": 2.0,
    "c": "test"
  })";

  Demo des;
  ASSERT_OK(serde::fromJsonString(des, json));
  ASSERT_EQ(des.a(), 1);
  ASSERT_EQ(des.b(), 2);
  ASSERT_EQ(des.c(), "test");
}

TEST(TestSerde, NewDesToml) {
  auto toml = R"(
    a = 1
    b = 2.0
    c = "test"
  )";

  Demo des;
  ASSERT_OK(serde::fromTomlString(des, toml));
  ASSERT_EQ(des.a(), 1);
  ASSERT_EQ(des.b(), 2);
  ASSERT_EQ(des.c(), "test");
}

TEST(TestSerde, NewDesOptional) {
  struct Foo {
    SERDE_STRUCT_FIELD(num, std::optional<float>{});
    SERDE_STRUCT_FIELD(str, std::optional<std::string>{});
  };

  {
    Foo ser;
    auto out = serde::serialize(ser);

    Foo des;
    serde::In<std::string_view> in(out);
    ASSERT_OK(serde::deserialize(des, in));
    ASSERT_FALSE(des.num.has_value());
    ASSERT_FALSE(des.str.has_value());
  }

  {
    Foo ser;
    auto out = serde::toJsonString(ser);
    XLOGF(INFO, "json is {}", out);

    Foo des;
    ASSERT_OK(serde::fromJsonString(des, out));
    ASSERT_FALSE(des.num.has_value());
    ASSERT_FALSE(des.str.has_value());
  }

  {
    Foo ser;
    auto out = serde::toTomlString(ser);
    XLOGF(INFO, "toml is {}", out);

    Foo des;
    ASSERT_OK(serde::fromTomlString(des, out));
    ASSERT_FALSE(des.num.has_value());
    ASSERT_FALSE(des.str.has_value());
  }

  {
    Foo des;
    des.num = 20;
    des.str = "Test";
    ASSERT_OK(serde::fromJsonString(des, "{}"));
    ASSERT_FALSE(des.num.has_value());
    ASSERT_FALSE(des.str.has_value());
  }

  {
    Foo des;
    des.num = 20;
    des.str = "Test";
    ASSERT_OK(serde::fromTomlString(des, ""));
    ASSERT_FALSE(des.num.has_value());
    ASSERT_FALSE(des.str.has_value());
  }

  {
    Foo ser;
    ser.num = 27;
    ser.str = "OK";
    auto out = serde::serialize(ser);

    Foo des;
    serde::In<std::string_view> in(out);
    ASSERT_OK(serde::deserialize(des, in));
    ASSERT_TRUE(des.num.has_value());
    ASSERT_EQ(ser.num, des.num);
    ASSERT_TRUE(des.str.has_value());
    ASSERT_EQ(ser.str, des.str);
  }

  {
    Foo ser;
    ser.num = 27;
    ser.str = "OK";
    auto out = serde::toJsonString(ser);
    XLOGF(INFO, "json is {}", out);

    Foo des;
    ASSERT_OK(serde::fromJsonString(des, out));
    ASSERT_TRUE(des.num.has_value());
    ASSERT_EQ(ser.num, des.num);
    ASSERT_TRUE(des.str.has_value());
    ASSERT_EQ(ser.str, des.str);
  }

  {
    Foo ser;
    ser.num = 27;
    ser.str = "OK";
    auto out = serde::toTomlString(ser);
    XLOGF(INFO, "toml is {}", out);

    Foo des;
    ASSERT_OK(serde::fromTomlString(des, out));
    ASSERT_TRUE(des.num.has_value());
    ASSERT_EQ(ser.num, des.num);
    ASSERT_TRUE(des.str.has_value());
    ASSERT_EQ(ser.str, des.str);
  }
}

TEST(TestSerde, NewDesVariant) {
  struct Foo {
    SERDE_STRUCT_FIELD(num, (std::variant<int, double>{}));
    SERDE_STRUCT_FIELD(val, (std::variant<std::string, Demo>{}));
  };

  {
    Foo ser;
    auto out = serde::serialize(ser);
    Foo des;
    serde::In<std::string_view> in(out);
    ASSERT_OK(serde::deserialize(des, in));
    ASSERT_EQ(des.num.index(), 0);
    ASSERT_EQ(des.val.index(), 0);
  }

  {
    Foo ser;
    ser.num = 20.0;
    ser.val = Demo{};
    auto out = serde::serialize(ser);
    Foo des;
    ASSERT_EQ(des.num.index(), 0);
    ASSERT_EQ(des.val.index(), 0);
    serde::In<std::string_view> in(out);
    ASSERT_OK(serde::deserialize(des, in));
    ASSERT_EQ(des.num.index(), 1);
    ASSERT_EQ(des.val.index(), 1);
  }

  {
    auto json = R"({
      "num": {
        "type": "double",
        "value": 70
      },
      "val": {
        "type": "basic_string",
        "value": "Test"
      }
    })";

    Foo des;
    ASSERT_OK(serde::fromJsonString(des, json));
    ASSERT_EQ(des.num.index(), 1);
    ASSERT_EQ(std::get<1>(des.num), 70.0);
    ASSERT_EQ(des.val.index(), 0);
    ASSERT_EQ(std::get<0>(des.val), "Test");
  }

  {
    Foo ser;
    ser.num = 70.0;
    Demo demo;
    demo.c() = "OK";
    ser.val = demo;
    auto out = serde::toJsonString(ser);
    XLOGF(INFO, "json is {}", out);

    Foo des;
    ASSERT_OK(serde::fromJsonString(des, out));
    ASSERT_EQ(des.num.index(), 1);
    ASSERT_EQ(std::get<1>(des.num), 70.0);
    ASSERT_EQ(des.val.index(), 1);
    ASSERT_EQ(std::get<1>(des.val).c(), "OK");
  }
}

TEST(TestSerde, NewDesContainer) {
  struct Foo {
    SERDE_STRUCT_FIELD(num, (std::vector<int>{}));
    SERDE_STRUCT_FIELD(val, (std::set<std::string>{}));
    SERDE_STRUCT_FIELD(map, (std::map<std::string, std::string>{}));
    SERDE_STRUCT_FIELD(map2, (std::map<int, std::string>{}));
  };

  {
    auto json = R"({
      "num": [1, 2, 3, 4],
      "val": ["A", "B"],
      "map": {
        "A": "1",
        "B": "2"
      },
      "map2": {
        "1": "A",
        "2": "B"
      }
    })";

    Foo des;
    ASSERT_OK(serde::fromJsonString(des, json));
    ASSERT_EQ(des.num.size(), 4);
    ASSERT_EQ(des.val.size(), 2);
    ASSERT_EQ(des.map.size(), 2);
    ASSERT_EQ(des.map["A"], "1");
    ASSERT_EQ(des.map["B"], "2");
    ASSERT_EQ(des.map2.size(), 2);
    ASSERT_EQ(des.map2[1], "A");
    ASSERT_EQ(des.map2[2], "B");
  }
}

TEST(TestSerde, NewDesEnum) {
  enum class Foo { A, B, C };

  Foo foo;
  {
    auto json = serde::toJsonString(Foo::B);
    ASSERT_OK(serde::fromJsonString(foo, json));
    ASSERT_EQ(foo, Foo::B);
  }

  ASSERT_FALSE(serde::fromJsonString(foo, "\"D\""));
}

TEST(TestSerde, UniquePtr) {
  struct Foo {
    SERDE_STRUCT_FIELD(value, std::unique_ptr<std::string>{});
  };
  struct Bar {
    SERDE_STRUCT_FIELD(value, std::unique_ptr<Foo>{});
  };

  Bar bar;
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_EQ(des.value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_EQ(des.value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_EQ(des.value, nullptr);
  }

  bar.value = std::make_unique<Foo>();
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }

  bar.value->value = std::make_unique<std::string>("hello");
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
}

TEST(TestSerde, SharedPtr) {
  struct Foo {
    SERDE_STRUCT_FIELD(value, std::shared_ptr<std::string>{});
  };
  struct Bar {
    SERDE_STRUCT_FIELD(value, std::shared_ptr<Foo>{});
  };

  Bar bar;
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_EQ(des.value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_EQ(des.value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_EQ(des.value, nullptr);
  }

  bar.value = std::make_unique<Foo>();
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_EQ(des.value->value, nullptr);
  }

  bar.value->value = std::make_unique<std::string>("hello");
  XLOGF(INFO, "{}", bar);

  {
    Bar des;
    ASSERT_OK(serde::deserialize(des, serde::serialize(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
  {
    Bar des;
    ASSERT_OK(serde::fromJsonString(des, serde::toJsonString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
  {
    Bar des;
    ASSERT_OK(serde::fromTomlString(des, serde::toTomlString(bar)));
    ASSERT_NE(des.value, nullptr);
    ASSERT_NE(des.value->value, nullptr);
    ASSERT_EQ(*des.value->value, "hello");
  }
}

TEST(TestSerde, AutoFallbackVariant) {
  std::variant<String, Status> src = Status(StatusCode::kUnknown, "Unknown");
  static_assert(!serde::is_auto_fallback_variant_v<decltype(src)>);
  auto s = serde::serialize(src);
  serde::AutoFallbackVariant<String> dst;
  static_assert(serde::is_auto_fallback_variant_v<decltype(dst)>);
  ASSERT_OK(serde::deserialize(dst, s));
  auto *uvt = std::get_if<serde::UnknownVariantType>(&dst);
  ASSERT_TRUE(uvt != nullptr);
  ASSERT_EQ(uvt->type, "Status");
}

TEST(TestSerde, UserBufferAllocator) {
  std::string ser = "hello world!";
  auto len = serde::serializeLength(ser);

  std::string buf(len, '\0');
  serde::serializeToUserBuffer(ser, (uint8_t *)buf.data(), buf.size());

  std::string der;
  serde::deserialize(der, buf);
  ASSERT_EQ(ser, der);
}

}  // namespace
}  // namespace hf3fs::test
