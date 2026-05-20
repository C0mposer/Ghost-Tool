/*
 * ghostwr - IOP module to write DECKARD memory region
 *
 * Receives data from EE via SIF RPC and writes it byte-by-byte into the
 * DECKARD emulator memory starting at 0xA28FE0:
 *   - 0xA28FE0..0xA28FFF : boot handshake (8 x 32-bit words, Deckard storage layout)
 *   - 0xA29000..          : Ghost A payload
 *
 * Mirror of ghostrd.c, but in reverse: the EE sends chunks to the IOP
 * and the IOP writes them to the address the EE cannot reach directly.
 *
 * No swapping is done on the IOP: each 32-bit word must already match what the
 * practice ROM expects in RAM (same per-word byte-reverse as GhostHdrStoreI32 /
 * deckard_ghost_boot_store_int on PS1). The EE packs the boot handshake that
 * way; ghost payload bytes are copied from USB/network as saved by ghostrd.
 */

#include <types.h>
#include <irx.h>
#include <thbase.h>
#include <sifcmd.h>
#include <loadcore.h>

IRX_ID("ghostwr", 1, 0);

#define GHOST_RPC_WR_ID  0x80000A2B
#define WR_CHUNK_SIZE    4096

typedef struct {
    u32 offset;
    u32 size;
    unsigned char data[WR_CHUNK_SIZE];
} GhostWriteReq;

static SifRpcDataQueue_t  rpc_queue  __attribute__((aligned(64)));
static SifRpcServerData_t rpc_server __attribute__((aligned(64)));

/* Must hold one full GhostWriteReq (4104 bytes), rounded to 64-byte alignment */
static unsigned char rpc_recv[4160] __attribute__((aligned(64)));

static unsigned char ack[4] __attribute__((aligned(64)));

static void *ghost_rpc_handler(int fno, void *buffer, int length)
{
    GhostWriteReq *req = (GhostWriteReq *)buffer;
    volatile unsigned char *dst = (volatile unsigned char *)(0xA28FE0 + req->offset);
    u32 size = req->size;
    u32 i;

    if (size > WR_CHUNK_SIZE)
        size = WR_CHUNK_SIZE;

    for (i = 0; i < size; i++)
        dst[i] = req->data[i];

    ack[0] = 1;
    return ack;
}

static void rpc_thread(void *arg)
{
    int tid = GetThreadId();
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&rpc_queue, tid);
    sceSifRegisterRpc(&rpc_server, GHOST_RPC_WR_ID, ghost_rpc_handler,
                      rpc_recv, NULL, NULL, &rpc_queue);
    sceSifRpcLoop(&rpc_queue);
}

int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    int tid;

    thread.attr      = TH_C;
    thread.thread    = rpc_thread;
    thread.priority  = 40;
    thread.stacksize = 4096;
    thread.option    = 0;

    tid = CreateThread(&thread);
    if (tid > 0)
        StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}

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
