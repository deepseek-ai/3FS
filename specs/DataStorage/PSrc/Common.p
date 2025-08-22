fun Min(x: int, y: int): int{
  if (x < y)
      return x;
  else
      return y;
}

fun Max(x: int, y: int): int{
  if (x > y)
      return x;
  else
      return y;
}

fun BitwiseAnd(x: int, y: int): int {
  var n: int;
  var r: int;

  n = 1;
  while (x > 0 && y > 0) {
    if (x % 2 > 0 && y % 2 > 0) {
      r = r + n;
    }
    x = x / 2;
    y = y / 2;
    n = n * 2;
  }

  return r;
}
