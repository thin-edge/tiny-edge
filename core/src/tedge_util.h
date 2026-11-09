/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Small macros the core uses everywhere, in plain C.
 *
 * Each RTOS ships its own version of these. The core uses this set so that
 * it reads the same on every port and no RTOS header is needed for them.
 */

#ifndef TEDGE_UTIL_H_
#define TEDGE_UTIL_H_

/** Number of elements of the array @p a (not of a pointer). */
#define TEDGE_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/** The smaller and the larger of two values. Each argument is evaluated
 *  twice, so neither may have side effects. */
#define TEDGE_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TEDGE_MAX(a, b) (((a) > (b)) ? (a) : (b))

/** @p x as a string literal, after macro expansion. */
#define TEDGE_STRINGIFY(x) TEDGE_STRINGIFY_(x)
#define TEDGE_STRINGIFY_(x) #x

/** Marks a parameter or variable that is deliberately not used. */
#define TEDGE_UNUSED(x) ((void)(x))

#endif /* TEDGE_UTIL_H_ */
