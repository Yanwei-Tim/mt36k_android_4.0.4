#include "app_init.h"
#include <string.h>
#include <stdio.h>
#define MAX_NUM_HANDLES      ((unsigned short) 4096)
#define SYS_MEM_SIZE ((unsigned int) 12 * 1024 * 1024)

const char * g_main_argv[] = {"abc", "cde"};
int g_main_argc = 3;

#ifndef PUSH_PLAYER_LOCAL_TEST

typedef struct _THREAD_DESCR_T{
    unsigned int  z_stack_size;
    unsigned char  ui1_priority;
    unsigned short  ui2_num_msgs;
}   THREAD_DESCR_T;

typedef struct _GEN_CONFIG_T{
    unsigned short  ui2_version;
    void*  pv_config;
    unsigned int  z_config_size;
    THREAD_DESCR_T  t_mheg5_thread;
}   GEN_CONFIG_T;

extern "C" int c_rpc_init_client(void);
extern "C" int c_rpc_start_client(void);
extern "C" int os_init(const void *pv_addr, unsigned int z_size);
extern "C" int handle_init (unsigned short   ui2_num_handles,
                            void**   ppv_mem_addr,
                            unsigned int*  pz_mem_size);
extern "C" int x_rtos_init (GEN_CONFIG_T*  pt_config);

#endif

bool init_rpc(void){
    GEN_CONFIG_T  t_rtos_config;
    memset(&t_rtos_config, 0, sizeof(GEN_CONFIG_T));
    void*       pv_mem_addr = 0;
    unsigned int z_mem_size = 0xc00000;
    int ret = 0;

    ret = x_rtos_init(&t_rtos_config);
    if (ret != 0) {
        fprintf(stderr, "rtos init failed %d \n", ret);
        return false;
    }
    ret = handle_init(MAX_NUM_HANDLES, &pv_mem_addr, &z_mem_size);
    if (ret != 0) {
        fprintf(stderr, "handle init failed %d \n", ret);
        return false;
    }
    ret = os_init(pv_mem_addr, z_mem_size);
    if (ret != 0) {
        fprintf(stderr, "os init failed %d \n", ret);
        return false;
    }
    ret = c_rpc_init_client();
    if (ret != 0) {
        fprintf(stderr, "rpc init failed %d \n", ret);
        return false;
    }
    ret = c_rpc_start_client();
    if (ret < 0) {
        fprintf(stderr, "rpc start failed %d \n", ret);
        return false;
    }
    printf("Rpc init OK\n");
    return true;
}
