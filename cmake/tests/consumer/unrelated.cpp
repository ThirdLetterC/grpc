#ifdef _CRT_SECURE_NO_WARNINGS
#error grpc compiler definitions leaked to an unrelated target
#endif

int main() {
  auto value = 0;
  return value;
}
