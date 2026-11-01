#include "grpc.h"
#include <string.h>

int main(void) {
  return grpc_get_allocator_name() == nullptr || strcmp(grpc_version(), GRPC_VERSION_STRING) != 0;
}
