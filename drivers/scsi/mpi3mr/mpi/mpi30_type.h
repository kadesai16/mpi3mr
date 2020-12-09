/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright 2016-2020 Broadcom Inc. All rights reserved.
 *
 *           Name: mpi30_type.h
 *    Description: MPI basic type definitions
 *  Creation Date: 10/07/2016
 *        Version: 03.00.00
 */
#ifndef MPI30_TYPE_H
#define MPI30_TYPE_H     1

/*****************************************************************************
 * Define MPI3_POINTER if it has not already been defined. By default        *
 * MPI3_POINTER is defined to be a near pointer. MPI3_POINTER can be defined *
 * as a far pointer by defining MPI3_POINTER as "far *" before this header   *
 * file is included.                                                         *
 ****************************************************************************/
#ifndef MPI3_POINTER
#define MPI3_POINTER    *
#endif  /* MPI3_POINTER */

/* The basic types may have already been included by mpi_type.h or mpi2_type.h*/
#if !defined(MPI_TYPE_H) && !defined(MPI2_TYPE_H)
#if 1
/*****************************************************************************
*
*               Basic Types
*
*****************************************************************************/

typedef u8 U8;
typedef __le16 U16;
typedef __le32 U32;
typedef __le64 U64 __aligned(4);

/*****************************************************************************
*
*               Pointer Types
*
*****************************************************************************/

typedef U8 * PU8;
typedef U16 * PU16;
typedef U32 * PU32;
typedef U64 * PU64;
#else
/*****************************************************************************
 *              Basic Types                                                  *
 ****************************************************************************/
typedef int8_t      S8;
typedef uint8_t     U8;
typedef int16_t     S16;
typedef uint16_t    U16;
typedef int32_t     S32;
typedef uint32_t    U32;
typedef int64_t     S64;
typedef uint64_t    U64;

/*****************************************************************************
 *              Structure Types                                              *
 ****************************************************************************/
typedef struct _S64struct {
    U32         Low;
    S32         High;
} S64struct;

typedef struct _U64struct {
    U32         Low;
    U32         High;
} U64struct;

/*****************************************************************************
 *              Pointer Types                                                *
 ****************************************************************************/
typedef S8 * PS8;
typedef U8 * PU8;
typedef S16 * PS16;
typedef U16 * PU16;
typedef S32         *PS32;
typedef U32         *PU32;
typedef S64 * PS64;
typedef U64 * PU64;
typedef S64struct * PS64struct;
typedef U64struct * PU64struct;
#endif
#endif  /* MPI_TYPE_H && MPI2_TYPE_H */

#endif  /* MPI30_TYPE_H */
