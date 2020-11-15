#include <R_ext/Rdynload.h>
#include <grpc/grpc.h>

void R_init_bigrquerystorage(DllInfo *info){
  grpc_init();
}

void R_unload_bigrquerystorage(DllInfo *info){
  grpc_shutdown();
}
