/*
 * ghostrd - IOP module to read DECKARD memory region
 *
 * This module runs on the IOP and reads the DECKARD emulator memory
 * at address 0xA29000 (Ghost data), returning it to the EE
 * via SIF RPC.
 *
 * On Deckard PS2 consoles, any IOP module (including PS1 games) can directly
 * access the 0xA00000-0xBFFFFF address range (upper 2MB of PPC-IOP RAM).
 * The EE cannot reach this memory via the bus window (only covers
 * lower 2MB), so we need this IOP-side module to copy the data.
 */

#include <types.h>
#include <irx.h>
#include <thbase.h>
#include <sifcmd.h>
#include <loadcore.h>

IRX_ID("ghostrd", 1, 0);

#define GHOST_RPC_ID  0x80000A29
#define MAX_CHUNK     16384

static SifRpcDataQueue_t rpc_queue __attribute__((aligned(64)));
static SifRpcServerData_t rpc_server __attribute__((aligned(64)));
static unsigned char rpc_recv[64] __attribute__((aligned(64)));
static unsigned char rpc_data[MAX_CHUNK] __attribute__((aligned(64)));

typedef struct {
    u32 offset;
    u32 size;
} GhostReadReq;

static void* ghost_rpc_handler(int fno, void* buffer, int length)
{
    GhostReadReq* req = (GhostReadReq*)buffer;
    volatile unsigned char* src = (volatile unsigned char*)(0xA29000 + req->offset);
    u32 size = req->size;
    u32 i;

    if (size > MAX_CHUNK)
        size = MAX_CHUNK;

    for (i = 0; i < size; i++)
        rpc_data[i] = src[i];

    return rpc_data;
}

static void rpc_thread(void* arg)
{
    int tid;

    sceSifInitRpc(0);

    tid = GetThreadId();
    sceSifSetRpcQueue(&rpc_queue, tid);
    sceSifRegisterRpc(&rpc_server, GHOST_RPC_ID, ghost_rpc_handler,
        rpc_recv, NULL, NULL, &rpc_queue);
    sceSifRpcLoop(&rpc_queue);
}

int _start(int argc, char* argv[])
{
    iop_thread_t thread;
    int tid;

    thread.attr = TH_C;
    thread.thread = rpc_thread;
    thread.priority = 40;
    thread.stacksize = 4096;
    thread.option = 0;

    tid = CreateThread(&thread);
    if (tid > 0)
        StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}

/* IOP kernel import stubs - resolved by module loader at load time */

thbase_IMPORTS_start
I_CreateThread
I_StartThread
I_GetThreadId
thbase_IMPORTS_end

sifcmd_IMPORTS_start
I_sceSifInitRpc
I_sceSifSetRpcQueue
I_sceSifRegisterRpc
I_sceSifRpcLoop
sifcmd_IMPORTS_end
