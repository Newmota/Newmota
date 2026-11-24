/* Host shim for the hostap SAE test: BIT() macros. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BIT(nr) (1UL << (nr))
#define BIT64(nr) (1ULL << (nr))
