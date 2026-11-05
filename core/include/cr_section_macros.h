/*
 * cr_section_macros.h
 *
 * Copyright 2010-2012 Code Red Technologies Ltd
 * Copyright 2013-2014, 2017-2018, 2020-2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Set of macros to allow code/data to be placed into alternate memory banks.
 */

#ifndef CR_SECTION_MACROS_H_INCLUDED
#define CR_SECTION_MACROS_H_INCLUDED

// A macro for placing text (code), data, or bss into a named RAM section
// These will be automatically placed into the named section by the linker.
//
// RAM banks are numbered starting from 1. The actual configuration is 
// dependent on the selected MCU type.
//
// Example:
//        __SECTION(data,RAM1) char buffer[1024] ;
//
// This will place the 1024 byte buffer into the RAM1
//
#if defined(PLATFORM_APPLE)
// Mach-O rejects ELF-style section names; use default sections on the host.
#define __SECTION_EXT(type, bank, name)
#define __SECTION(type, bank)
#define __SECTION_SIMPLE(type)
#else
#define __SECTION_EXT(type, bank, name) __attribute__ ((section("." #type ".$" #bank "." #name)))
#define __SECTION(type, bank) __attribute__ ((section("." #type ".$" #bank)))
#define __SECTION_SIMPLE(type) __attribute__ ((section("." #type)))
#endif

#define __DATA_EXT(bank, name) __SECTION_EXT(data, bank, name)
#define __DATA(bank) __SECTION(data, bank)

#define __BSS_EXT(bank, name) __SECTION_EXT(bss, bank, name)
#define __BSS(bank) __SECTION(bss, bank)

// Macros for placing text (code), data, or bss into a section that is automatically
// placed after the vectors in the target image.
#if defined(PLATFORM_APPLE)
#define __AFTER_VECTORS_EXT(name)
#define __AFTER_VECTORS
#else
#define __AFTER_VECTORS_EXT(name) __attribute__ ((section(".after_vectors.$" #name)))
#define __AFTER_VECTORS __attribute__ ((section(".after_vectors")))
#endif

// Macros for causing functions to be relocated to RAM
#if defined(PLATFORM_APPLE)
#define __RAM_FUNC_EXT(name)
#define __RAM_FUNC
#else
#define __RAM_FUNC_EXT(name) __attribute__ ((section(".ramfunc.$" #name)))
#define __RAM_FUNC __attribute__ ((section(".ramfunc")))
#endif

// Macros to be used in preference to __RAM_FUNC to better match __DATA behaviour
#define __RAMFUNC_EXT(bank, name) __SECTION_EXT(ramfunc, bank, name)
#define __RAMFUNC(bank) __SECTION(ramfunc, bank)

// Macros for placing data or bss into a section that has the NOLOAD option set in the linker script
#define __NOINIT_DEF __SECTION_SIMPLE(noinit)
#define __NOINIT_EXT(bank, name) __SECTION_EXT(noinit, bank, name)
#define __NOINIT(bank) __SECTION(noinit, bank)

// Macros for placing text (code), or rodata into a different (flash) bank
#define __RODATA_EXT(bank,name) __SECTION_EXT(rodata, bank, name)
#define __RODATA(bank) __SECTION(rodata, bank)

#define __TEXT_EXT(bank,name) __SECTION_EXT(text, bank, name)
#define __TEXT(bank) __SECTION(text, bank)

#endif /* CR_SECTION_MACROS_H_INCLUDED */
