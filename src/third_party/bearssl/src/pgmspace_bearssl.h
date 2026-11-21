/* PGMSPACE.H - Accessor utilities/types for accessing PROGMEM data */

#ifndef _PGMSPACEWRAPPER_H_
#define _PGMSPACEWRAPPER_H_

#include <stdint.h>

/*
 * Every Newmota target maps constant data directly, so the flash-resident
 * markers expand to nothing and the accessors are plain reads. The imported
 * i15 integer code, the hashes, and the curve tables call the macros below
 * directly, so this shim stays part of the import.
 */
#ifndef PROGMEM
#define PROGMEM
#endif

#define pgm_read_byte(addr)             (*(const uint8_t*)(addr))
#define pgm_read_word(addr)             (*(const uint16_t*)(addr))

#define memcpy_P memcpy
#define memcmp_P memcmp

#endif
