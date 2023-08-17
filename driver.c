/*
 *  Rng_Prg260 Driver
 *
 *  Copyright (C) 2023, HENSOLDT Cyber GmbH
 */

#include "OS_Error.h"
#include "OS_Dataport.h"
#include "lib_io/FifoDataport.h"
#include "lib_debug/Debug.h"

#include <camkes.h>

#include <string.h>

//---------------------------------------------------------------------------

typedef struct {
    FifoDataport*   uart_input_fifo; // FIFO in dataport shared with the UART driver
    uint8_t*        uart_output_fifo;
    OS_Dataport_t   entropy_port;
} uart_ctx_t;

uart_ctx_t uart_ctx = { 0 };

void print_bytes(void *ptr, int size) 
{
    unsigned char *p = ptr;
    int i;
    for (i=0; i<size; i++) {
        printf("%02hhX ", p[i]);
    }
    printf("\n");
}

//---------------------------------------------------------------------------

OS_Error_t fifo_remove_bytes(uart_ctx_t *ctx, size_t *amount) {
    FifoDataport* fifo = ctx->uart_input_fifo;
    size_t bytes_avail = FifoDataport_getSize(fifo);
    if (bytes_avail < *amount) {
        FifoDataport_remove(fifo, bytes_avail);
        *amount = bytes_avail;
        return OS_ERROR_OUT_OF_BOUNDS;
    }
    FifoDataport_remove(fifo, *amount);
    return OS_SUCCESS;
}

void fifo_clean(uart_ctx_t *ctx) {
    FifoDataport* fifo = ctx->uart_input_fifo;

    size_t bytes_available = FifoDataport_getSize(fifo);
    printf("Size to clean fifo: %lu\n", bytes_available);
    FifoDataport_remove(fifo, bytes_available);
}

void uart_rng_write(uart_ctx_t *ctx, char *bytes, size_t amount) {
    // Copy data into uart ouput buffer
    memcpy(ctx->uart_output_fifo, bytes, amount);

    // Notfiy the uart driver about the data
    uart_rpc_write(amount);
}


void uart_rng_read(uart_ctx_t *ctx, void *buffer, size_t *amount) {
    FifoDataport* fifo = ctx->uart_input_fifo;

    // Get buffer with the available data
    void * buf = NULL;
    *amount = FifoDataport_getContiguous(fifo, &buf);
    memcpy(buffer, buf, *amount);
    fifo_remove_bytes(ctx, amount);
}

void uart_rng_read_blocking(uart_ctx_t *ctx, void *buffer, size_t amount) {
    FifoDataport* fifo = ctx->uart_input_fifo;

    while (FifoDataport_getSize(fifo) < amount);

    void * buf = NULL;
    FifoDataport_getContiguous(fifo, &buf);
    memcpy(buffer, buf, amount);
    fifo_remove_bytes(ctx, &amount);
}


//---------------------------------------------------------------------------


// IF_OS_ENTROPY RPC Function
size_t entropy_rpc_read(const size_t len) {
    uart_ctx_t *ctx = &uart_ctx;
    size_t sz;
    FifoDataport* fifo = ctx->uart_input_fifo;

    fifo_clean(ctx);
    // Make sure we don't exceed the dataport len
    sz = OS_Dataport_getSize(ctx->entropy_port);
    sz = len > sz ? sz : len;

    printf("Capacity of fifo: %lu\n", FifoDataport_getCapacity(fifo));
    printf("Size of data available in fifo: %lu\n", FifoDataport_getSize(fifo));

    //Check that requested amount is not more than the capacity of the fifo
    size_t fifo_cap = FifoDataport_getCapacity(fifo);
    sz = fifo_cap < sz ? fifo_cap : sz;

    //Check that that requested amount is not larger than the 4096 limit of the PRG260
    sz = 4096 < sz ? 4096 : sz; 

    //The amount of bytes the PRG260 returns is a multiple of 16.
    int bytes_to_request = sz / 16;

    //set sz to a the amount of bytes returned
    sz = bytes_to_request * 16;

    // fill the buffer
    void * entropy_port = OS_Dataport_getBuf(ctx->entropy_port);
    memset(entropy_port, 0xff, sz);

    printf("SZ Bytes: %lu\nBytes to Request: %i\n", sz, bytes_to_request);

    char msg[2] =  {0x73, bytes_to_request};
    uart_rng_write(ctx, msg, 2);
    uart_rng_read_blocking(ctx, entropy_port, sz);

    return sz;
}

//---------------------------------------------------------------------------
void pre_init(void)
{
    Debug_LOG_DEBUG("pre_init");
}


//---------------------------------------------------------------------------
void post_init(void)
{
    uart_ctx_t *ctx = &uart_ctx;

    // init function to register callback
    OS_Dataport_t input_port  = OS_DATAPORT_ASSIGN(uart_input_port);
    OS_Dataport_t output_port = OS_DATAPORT_ASSIGN(uart_output_port);
    ctx->entropy_port = (OS_Dataport_t) OS_DATAPORT_ASSIGN(entropy_port);

    ctx->uart_input_fifo  = (FifoDataport *) OS_Dataport_getBuf(input_port);
    ctx->uart_output_fifo = (uint8_t *) OS_Dataport_getBuf(output_port);
}