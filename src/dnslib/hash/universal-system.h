/*!
 * \file universal-system.h
 *
 * \author Lubos Slovak <lubos.slovak@nic.cz>
 *
 * This file provides interface to a 2-universal system of hash functions that
 * hash from 32-bit unsigned integer to a 32-bit unsigned integer within a given
 * range. The range is always a power of two and is given by the exponent (see
 * function us_hash().
 *
 * Before using the system, it must be initialized by calling us_initialize().
 * The system stores 2 sets (generations), each of US_FNC_COUNT functions.
 * For generating a new set of coeficients (i.e. hash functions) use the
 * us_next() function.
 *
 * For hashing use the us_hash() function.
 *
 * \todo What if all numbers are tried and still need rehash?
 *       (that means 2mld rehashes - we can probably live with that ;)
 * \todo Consider counting generations from 0, will be easier!
 * \todo Check out some better random number generator.
 *
 * \addtogroup hashing
 * @{
 */

#ifndef _KNOT_UNIVERSAL_SYSTEM_H_
#define _KNOT_UNIVERSAL_SYSTEM_H_

#include <stdint.h>

typedef unsigned int uint;

enum { US_FNC_COUNT = 4 };

enum { GEN_COUNT = 2 };

/*----------------------------------------------------------------------------*/

struct us_system {
	uint coefs[US_FNC_COUNT * GEN_COUNT];
};

typedef struct us_system us_system_t;

/*----------------------------------------------------------------------------*/
/*!
 * \brief Initializes the universal system by generating coeficients for all
 *        hash functions and all generations.
 */
void us_initialize(us_system_t *system);

/*----------------------------------------------------------------------------*/
/*!
 * \brief Generates new hash functions' coeficients for the given \a generation.
 *
 * \return 0
 */
int us_next(us_system_t *system, uint generation);

/*----------------------------------------------------------------------------*/

/*!
 * \brief Hashes the \a value using the given \a exponent and function.
 *
 * The actual formula of the hash is:
 * h = ((coef * value) mod 2^32) / 2^(32 - table_exp)
 * where \a coef is the proper coeficient.
 *
 * \param value Value to be hashed.
 * \param table_exp Determines the upper bound for the result - the hash will
 *                  be between 0 and 2^(32 - table_exp).
 * \param fnc Which function from the set should be used.
 * \param generation Which set (generation) of functions should be used.
 *
 * \todo Make inline?
 *
 * \return Hash value (32bit unsigned).
 */
uint32_t us_hash(const us_system_t *system, uint32_t value, uint table_exp,
                 uint fnc, uint generation);

/*----------------------------------------------------------------------------*/

#endif /* _KNOT_UNIVERSAL_SYSTEM_H_ */

/*! @} */
