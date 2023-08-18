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

#include "../../components/Rng_Prg260/interface/if_OS_PRG260_Keystore.h"

#define key_sz sizeof(Prg260_Key_t)
#define pin_sz sizeof(Prg260_Pin_t)

//---------------------------------------------------------------------------

typedef struct {
    FifoDataport*   uart_input_fifo; // FIFO in dataport shared with the UART driver
    uint8_t*        uart_output_fifo;
    OS_Dataport_t   entropy_port;
} uart_ctx_t;

uart_ctx_t uart_ctx = { 0 };


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


char checksum(char * buffer, size_t len) {
    char chksm = 0;
    for (int i = 0; i < len; i++) {
        chksm = chksm ^ buffer[i];
    }
    return chksm;
}


//---------------------------------------------------------------------------


// IF_OS_ENTROPY RPC Function
size_t entropy_rpc_read(const size_t len) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    size_t sz;
    FifoDataport* fifo = ctx->uart_input_fifo;

    fifo_clean(ctx);
    // Make sure we don't exceed the dataport len
    sz = OS_Dataport_getSize(ctx->entropy_port);
    sz = len > sz ? sz : len;

    //The amount of bytes the PRG260 returns is a multiple of 16.
    int bytes_to_request = sz % 16 ? (sz / 16) + 1 : sz / 16;

    //set sz to a the amount of bytes returned
    sz = bytes_to_request * 16;

    //Check that that requested amount is not larger than the 4096 limit of the PRG260
    sz = 4096 < sz ? 4096 : sz; 

    //Check that requested amount is not more than the capacity of the fifo
    size_t fifo_cap = FifoDataport_getCapacity(fifo);
    sz = fifo_cap < sz ? fifo_cap : sz;

    // fill the buffer
    void * entropy_port = OS_Dataport_getBuf(ctx->entropy_port);
    memset(entropy_port, 0xff, sz);


    char msg[2] =  {0x73, bytes_to_request};
    uart_rng_write(ctx, msg, 2);
    uart_rng_read_blocking(ctx, entropy_port, sz);

    return len;
}


//---------------------------------------------------------------------------
// if_OS_PRG260_Keystore rpc functions. These function are simply a 
// code implementation of the functions described in the manual of the PRG260
// section 17. If question arise of the function, please confer to the manual.


OS_Error_t prg260_keystore_rpc_init(
            Prg260_Key_t *key1,
            Prg260_Key_t *key2,
            Prg260_Pin_t user_pin,
            Prg260_Pin_t master_pin) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);
    
    const size_t msg_sz = 1 + key_sz *2 + pin_sz*2 + 1; 
    char msg[1 + key_sz *2 + pin_sz*2 + 1] = { 0 };
    char reply[2] = { 0 };
    
    msg[0] = 0x99;
    memcpy(msg + 1,                     key1,        key_sz);
    memcpy(msg + 1 + key_sz,            key2,        key_sz);
    memcpy(msg + 1 + key_sz*2,          &user_pin,   pin_sz);
    memcpy(msg + 1 + key_sz*2 + pin_sz, &master_pin, pin_sz);

    msg[msg_sz - 1] = checksum(msg + 1, key_sz*2 + pin_sz*2);

    uart_rng_write(ctx, msg, msg_sz);
    uart_rng_read_blocking(ctx, (void *) reply, 2);

    if (reply[0] != 0x99) {
        Debug_LOG_ERROR("Invalid Response code");
        return OS_ERROR_GENERIC;
    }

    switch(reply[1]) {
        case 0x55:
            Debug_LOG_DEBUG("Init success");
            return OS_SUCCESS;
        case 0x66:
            Debug_LOG_ERROR("Init Failed: EEPROM is already written and blocked.");
            return OS_ERROR_BUFFER_FULL;
        case 0xaa:
            Debug_LOG_ERROR("Checksum or timeout.");
            return OS_ERROR_TIMEOUT;
    }
    
    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
}


OS_Error_t prg260_keystore_rpc_change_user_pin(
            Prg260_Pin_t master_pin,
            Prg260_Pin_t new_pin) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    char msg[1 + pin_sz*2 + 1] = { 0 };
    char reply[2] = { 0 };

    msg[0] = 0x28;
    memcpy(msg + 1,          &master_pin, pin_sz);
    memcpy(msg + 1 + key_sz, &new_pin,    key_sz);
    msg[1 + pin_sz*2] = checksum(msg + 1, pin_sz*2);

    uart_rng_write(ctx, msg, 1 + 1 + pin_sz*2);
    uart_rng_read_blocking(ctx, (void *) reply, 2);

    if (reply[0] != 0x28) {
        Debug_LOG_ERROR("Invalid Response code");
        return OS_ERROR_GENERIC;
    }

    switch (reply[1]) {
        case 0x55:
            Debug_LOG_DEBUG("Pin changed successfully.");
            return OS_SUCCESS;
        case 0xaa:
            Debug_LOG_ERROR("Timeout Failure");
            return OS_ERROR_TIMEOUT;
    }

    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
}


OS_Error_t prg260_keystore_rpc_get_key(
            Prg260_Pin_t user_pin,
            Prg260_Key_t *key) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    char msg[1 + pin_sz + 1] = { 0 };
    char reply[key_sz + 1] = { 0 };

    msg[0] = 0x29;
    memcpy(msg + 1, &user_pin, pin_sz);
    msg[1 + pin_sz] = checksum((char *) &user_pin, pin_sz);

    uart_rng_write(ctx, msg, 1 + 1 + pin_sz);
    uart_rng_read_blocking(ctx, (void *) reply, 1 + 1);

    if (reply[0] != 0x29) {
        Debug_LOG_ERROR("Invalid Response code");
        return OS_ERROR_GENERIC;
    }

    switch (reply[1]) {
        case 0x55:
            uart_rng_read_blocking(ctx, (void *) reply, key_sz + 1);
            memcpy(key, reply, key_sz);
            if (reply[key_sz] != checksum(reply, key_sz)) {
                Debug_LOG_ERROR("Invalid transmission, checksums are showing an error.");
                return OS_ERROR_INVALID_PARAMETER;
            }
            Debug_LOG_DEBUG("Key retrieved.");
            return OS_SUCCESS;
        case 0xaa:
            Debug_LOG_ERROR("Timeout Failure");
            return OS_ERROR_TIMEOUT;
    }
    
    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
}


OS_Error_t prg260_keystore_rpc_verify_key(
            Prg260_Pin_t user_pin,
            Prg260_Key_t *key) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    char msg[1 + pin_sz + key_sz + 1] = { 0 };
    char reply[2] = { 0 };

    msg[0] = 0x2a;
    memcpy(msg + 1,          &user_pin, pin_sz);
    memcpy(msg + 1 + pin_sz, key,       key_sz);
    msg[1 + pin_sz + key_sz] = checksum(msg + 1, pin_sz + key_sz);

    uart_rng_write(ctx, msg, 1 + 1 + pin_sz + key_sz);
    uart_rng_read_blocking(ctx, (void *) reply, 2);

    if (reply[0] != 0x2a) {
        Debug_LOG_ERROR("Wrong Reply message reply");
        return OS_ERROR_GENERIC;
    }

    switch (reply[1]) {
        case 0x55:
            Debug_LOG_DEBUG("Key is verified.");
            return OS_SUCCESS;
        case 0xaa:
            Debug_LOG_ERROR("Timeout Failure");
            return OS_ERROR_TIMEOUT;
    }

    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
}


OS_Error_t prg260_keystore_rpc_state(void) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    char msg = 0x9a;
    char reply[2] = { 0 };

    uart_rng_write(ctx, &msg, 1);
    uart_rng_read_blocking(ctx, (void *) reply, 2);
    
    if (reply[0] != 0x9a) {
        Debug_LOG_ERROR("Invalid Response code");
        return OS_ERROR_GENERIC;
    }

    switch(reply[1]) {
        case 0x55:
            Debug_LOG_DEBUG("EEPROM is empty and ready to be written.");
            return OS_SUCCESS;
        case 0x66:
            Debug_LOG_DEBUG("The EEPROM is already written.");
            return OS_ERROR_BUFFER_FULL;
    }

    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
}


OS_Error_t prg260_keystore_rpc_reset(Prg260_Pin_t master_pin) {
    uart_ctx_t *ctx = &uart_ctx;
    fifo_clean(ctx);

    char msg[1 + pin_sz + 1] = { 0 };
    char reply[2] = { 0 };

    msg[0] = 0x39;
    memcpy(msg + 1, &master_pin, pin_sz);
    msg[1 + pin_sz] = checksum((char *) &master_pin, pin_sz);

    uart_rng_write(ctx, msg, 1 + pin_sz + 1);
    uart_rng_read_blocking(ctx, (void *) reply, 2);

    if (reply[0] != 0x39) {
        Debug_LOG_ERROR("Invalid Response code");
        return OS_ERROR_GENERIC;
    }

    switch(reply[1]) {
        case 0x55:
            Debug_LOG_DEBUG("Reset of EEPROM was successfull.");
            return OS_SUCCESS;
        case 0xaa:
            Debug_LOG_ERROR("Master Pin incorrect: EEPROM was not reset.");
            return OS_ERROR_ACCESS_DENIED;
    }

    Debug_LOG_ERROR("Generic Error");
    return OS_ERROR_GENERIC;
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