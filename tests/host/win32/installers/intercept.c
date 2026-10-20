/* Structural loader evidence; no original redistributable is executed. */
#include "linux.h"
int host_main(int argc,char **argv){
  if(argc!=3)return 2;
  struct w32_loader loader;
  w32_ldr_init(&loader,&host.services,&host.peb);w32_ldr_bind(&loader);
  int before=host.invocations;
  int result=w32_installer_intercept(&loader,argv[1]);
  if(result!=(argv[2][0]=='1') || host.invocations!=(ULONG)before || loader.module_count){host_print("result=");host_number(result);host_print(" modules=");host_number(loader.module_count);host_print(" invokes=");host_number(host.invocations);host_print("\n");return 3;}
  w32_ldr_shutdown(&loader);
  if(host.allocations || host.mappings)return 4;
  host_print("PASS DirectSetup identity preflight no callbacks no mappings\n");return 0;
}
