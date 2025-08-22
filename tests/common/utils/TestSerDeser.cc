#include <common/utils/SerDeser.h>
#include <gtest/gtest.h>

namespace hf3fs::tests {
namespace {
TEST(Serializer, testPutChar) {
  String s;
  Serializer ser(s);
  ser.putChar('a');
  ser.putChar('b');
  ser.putChar('c');
  ASSERT_EQ(s, "abc");
}

TEST(Serializer, testPutIntAsChar) {
  String s;
  Serializer ser(s);
  ser.putIntAsChar<int16_t>('a');
  ser.putIntAsChar<int32_t>('b');
  ser.putIntAsChar<int64_t>('c');
  ASSERT_EQ(s, "abc");
}

TEST(Serializer, testPutShortString) {
  String s;
  Serializer ser(s);
  ser.putShortString("abc");
  ASSERT_EQ(s.size(), 4);
  ASSERT_EQ(s[0], 3);
  ASSERT_EQ(s.substr(1), "abc");
}

TEST(Deserializer, testGetChar) {
  String s("abc");
  Deserializer des(s);
  String res;
  for (auto c = des.getChar(); c.hasValue(); c = des.getChar()) {
    res.push_back(c.value());
  }
  ASSERT_TRUE(des.reachEnd());
  ASSERT_TRUE(!des.getChar().hasValue());
  ASSERT_EQ(res, "abc");
}

TEST(Deserializer, testGetIntFromChar) {
  String s;
  Serializer ser(s);
  ser.putIntAsChar<int>(100);
  ser.putIntAsChar<int16_t>(-50);
  ser.putIntAsChar<uint32_t>(50);

  Deserializer des(s);
  auto c0 = des.getIntFromChar<int>();
  ASSERT_TRUE(c0.hasValue());
  ASSERT_EQ(c0.value(), 100);
  auto c1 = des.getIntFromChar<int16_t>();
  ASSERT_TRUE(c1.hasValue());
  ASSERT_EQ(c1.value(), -50);
  auto c2 = des.getIntFromChar<uint32_t>();
  ASSERT_TRUE(c2.hasValue());
  ASSERT_EQ(c2.value(), 50);

  ASSERT_TRUE(des.reachEnd());
  ASSERT_TRUE(!des.getIntFromChar<int8_t>().hasValue());
}

TEST(Deserializer, testGetShortString) {
  String s;
  Serializer ser(s);
  ser.putShortString("abc");

  Deserializer des(s);
  ASSERT_EQ(des.getShortString().value(), "abc");
  ASSERT_TRUE(des.reachEnd());

  ASSERT_TRUE(!des.getShortString().hasValue());
}

TEST(Deserializer, testGet) {
  struct T {
    int x;
    char y;
  };
  T t = {1, 2};
  String s;
  Serializer ser(s);
  ser.put(t);

  Deserializer des(s);
  auto res = des.get<T>();
  ASSERT_TRUE(res.hasValue());
  ASSERT_TRUE(des.reachEnd());
  ASSERT_EQ(res.value().x, 1);
  ASSERT_EQ(res.value().y, 2);

  ASSERT_TRUE(!des.get<T>().hasValue());
}

TEST(Deserializer, testGetRaw) {
  String s = "abcdefg";
  Deserializer des(s);
  auto res = des.getRaw(1);
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(res.value(), "a");

  res = des.getRaw(2);
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(res.value(), "bc");

  res = des.getRaw(5);
  ASSERT_TRUE(!res.hasValue());

  res = des.getRaw(1);
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(res.value(), "d");

  res = des.getRawUntilEnd();
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(res.value(), "efg");

  res = des.getRawUntilEnd();
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(res.value(), "");

  ASSERT_TRUE(des.reachEnd());
  ASSERT_TRUE(!des.getRaw(1).hasValue());
}

}  // namespace
}  // namespace hf3fs::tests
