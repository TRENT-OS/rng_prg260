/*
 * Rng_Prg260 Keystore Defines
 *
 * Copyright (C) 2023-2024, HENSOLDT Cyber GmbH
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * For commercial licensing, contact: info.cyber@hensoldt.net
 */

#pragma once

#include "OS_Error.h"

typedef char Prg260_Key_t[192];

typedef uint64_t Prg260_Pin_t;

typedef struct
{
    OS_Error_t (*init)(
        const Prg260_Key_t* key1,
        const Prg260_Key_t* key2,
        const Prg260_Pin_t user_pin,
        const Prg260_Pin_t master_pin);
    OS_Error_t (*change_user_pin)(
        const Prg260_Pin_t master_pin,
        const Prg260_Pin_t new_pin);
    OS_Error_t (*get_key)(
        const Prg260_Pin_t user_pin,
        Prg260_Key_t* key);
    OS_Error_t (*verify_key)(
        const Prg260_Pin_t user_pin,
        const Prg260_Key_t* key);
    OS_Error_t (*state)(void);
    OS_Error_t (*reset)(const Prg260_Pin_t master_pin);
} if_OS_PRG260_Keystore_t;
