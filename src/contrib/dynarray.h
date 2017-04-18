/*  Copyright (C) 2017 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*!
 * \brief Simple write-once allocation-optimal dynamic array.
 *
 * Include it into your .c file
 *
 * prefix - identifier prefix, e.g. ptr -> struct ptr_dynarray, ptr_dynarray_add(), ...
 * ntype - data type to be stored. Let it be a number, pointer or small struct
 * initial_capacity - how many data items will be allocated on stac and copied with assignment
 *
 * prefix_dynarray_add() - add a data item
 * prefix_dynarray_fix() - call EVERYTIME the array is copied from some already invalid stack
 * prefix_dynarray_free() - call EVERYTIME you dismiss all copies of the array
 *
 */

#include <stdlib.h>
#include <assert.h>

#pragma once

#define DYNARRAY_VISIBILITY_STATIC static
#define DYNARRAY_VISIBILITY_PUBLIC
#define DYNARRAY_VISIBILITY_LIBRARY __public__

#define dynarray_declare(prefix, ntype, visibility, initial_capacity) \
	typedef struct prefix ## _dynarray { \
		ssize_t capacity; \
		ssize_t size; \
		ntype init[initial_capacity]; \
		ntype *arr; \
	} prefix ## _dynarray_t; \
	\
	visibility void prefix ## _dynarray_fix(prefix ## _dynarray_t *dynarray); \
	visibility void prefix ## _dynarray_add(prefix ## _dynarray_t *dynarray, \
	                                        ntype const *to_add); \
	visibility void prefix ## _dynarray_free(prefix ## _dynarray_t *dynarray);

#define dynarray_foreach(prefix, ntype, ptr, array) \
	for (ntype *ptr = (prefix ## _dynarray_fix(&(array)), (array).arr); \
	     ptr < (array).arr + (array).size; ptr++)

#define dynarray_define(prefix, ntype, visibility, initial_capacity) \
	\
	static void prefix ## _dynarray_free__(struct prefix ## _dynarray *dynarray) \
	{ \
		if (dynarray->capacity > initial_capacity) { \
			free(dynarray->arr); \
		} \
	} \
	\
	__attribute__((unused)) \
	visibility void prefix ## _dynarray_fix(struct prefix ## _dynarray *dynarray) \
	{ \
		assert(dynarray->size <= dynarray->capacity); \
		if (dynarray->capacity <= initial_capacity) { \
			dynarray->capacity = initial_capacity; \
			dynarray->arr = dynarray->init; \
		} \
	} \
	\
	__attribute__((unused)) \
	visibility void prefix ## _dynarray_add(struct prefix ## _dynarray *dynarray, \
	                                        ntype const *to_add) \
	{ \
		prefix ## _dynarray_fix(dynarray); \
		if (dynarray->size >= dynarray->capacity) { \
			ssize_t new_capacity = dynarray->capacity * 2 + 1; \
			ntype *new_arr = calloc(new_capacity, sizeof(ntype)); \
			if (new_arr == NULL) { \
				prefix ## _dynarray_free__(dynarray); \
				dynarray->capacity = dynarray->size = -1; \
				return; \
			} \
			if (dynarray->capacity > 0) { \
				memcpy(new_arr, dynarray->arr, \
				       dynarray->capacity * sizeof(ntype)); \
			} \
			prefix ## _dynarray_free__(dynarray); \
			dynarray->arr = new_arr; \
			dynarray->capacity = new_capacity; \
		} \
		dynarray->arr[dynarray->size++] = *to_add; \
	} \
	\
	__attribute__((unused)) \
	visibility void prefix ## _dynarray_free(struct prefix ## _dynarray *dynarray) \
	{ \
		prefix ## _dynarray_free__(dynarray); \
		memset(dynarray, 0, sizeof(*dynarray)); \
	}