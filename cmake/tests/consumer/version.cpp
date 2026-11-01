#include "grpc_version.h"
#include <cstring>

int main() {
  return std::strcmp(grpc_version(), GRPC_VERSION_STRING);
}
