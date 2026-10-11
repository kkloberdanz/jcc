/* included by tests/cpp.c, twice: the guard and #pragma once must both work */
#ifndef CPPTEST_H
#define CPPTEST_H
#pragma once
#define FROM_HEADER 7
#define HEADER_LINE __LINE__
static int header_fn(void) { return FROM_HEADER + __INCLUDE_LEVEL__; }
#endif
