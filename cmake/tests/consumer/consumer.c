#include "grpc.h"

int main(void) {
  return grpc_get_allocator_name() == nullptr;
}
