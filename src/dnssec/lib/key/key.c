/*  Copyright (C) 2014 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include <assert.h>
#include <gnutls/abstract.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "binary.h"
#include "error.h"
#include "hex_gnutls.h"
#include "key.h"
#include "key/algorithm.h"
#include "key/convert.h"
#include "key/dnskey.h"
#include "key/internal.h"
#include "keyid.h"
#include "keystore.h"
#include "keytag.h"
#include "pem.h"
#include "shared.h"
#include "wire.h"

/*!
 * Minimal size of DNSKEY RDATA.
 */
#define DNSKEY_RDATA_MIN_SIZE DNSKEY_RDATA_OFFSET_PUBKEY

/*!
 * RDATA template for newly allocated keys.
 */
static const dnssec_binary_t DNSKEY_RDATA_TEMPLATE = {
	.size = 4,
	.data = (uint8_t []) { 0x01, 0x00, 0x03, 0x00 }
};

/* -- key allocation ------------------------------------------------------- */

_public_
int dnssec_key_new(dnssec_key_t **key_ptr)
{
	if (!key_ptr) {
		return DNSSEC_EINVAL;
	}

	dnssec_key_t *key = calloc(1, sizeof(*key));
	if (!key) {
		return DNSSEC_ENOMEM;
	}

	int r = dnssec_binary_dup(&DNSKEY_RDATA_TEMPLATE, &key->rdata);
	if (r != DNSSEC_EOK) {
		free(key);
		return DNSSEC_ENOMEM;
	}

	*key_ptr = key;

	return DNSSEC_EOK;
}

/*!
 * Clear allocated fields inside the key structure, except RDATA.
 */
static void key_free_internals(dnssec_key_t *key)
{
	assert(key);

	free(key->dname);
	key->dname = NULL;

	gnutls_privkey_deinit(key->private_key);
	key->private_key = NULL;

	gnutls_pubkey_deinit(key->public_key);
	key->public_key = NULL;
}

_public_
void dnssec_key_clear(dnssec_key_t *key)
{
	if (!key) {
		return;
	}

	// reuse RDATA
	dnssec_binary_t rdata = key->rdata;

	// clear the structure
	key_free_internals(key);
	clear_struct(key);

	// restore template RDATA (downsize, no need to realloc)
	assert(rdata.size >= DNSKEY_RDATA_MIN_SIZE);
	rdata.size = DNSKEY_RDATA_MIN_SIZE;
	memmove(rdata.data, DNSKEY_RDATA_TEMPLATE.data, rdata.size);

	key->rdata = rdata;
}

_public_
void dnssec_key_free(dnssec_key_t *key)
{
	if (!key) {
		return;
	}

	key_free_internals(key);
	dnssec_binary_free(&key->rdata);

	free(key);
}

_public_
dnssec_key_t *dnssec_key_dup(const dnssec_key_t *key)
{
	if (!key) {
		return NULL;
	}

	dnssec_key_t *dup = NULL;

	if (dnssec_key_new(&dup) != DNSSEC_EOK ||
	    dnssec_key_set_dname(dup, key->dname) != DNSSEC_EOK ||
	    dnssec_key_set_rdata(dup, &key->rdata) != DNSSEC_EOK
	) {
		dnssec_key_free(dup);
		return NULL;
	}

	return dup;
}

/* -- key identifiers ------------------------------------------------------ */

/*!
 * Update key tag, should be called when anything in RDATA changes.
 */
static void update_keytag(dnssec_key_t *key)
{
	assert(key);
	dnssec_keytag(&key->rdata, &key->keytag);
}

/*!
 * Update key ID (X.509 CKA_ID), should be called when public key changes.
 */
static void update_key_id(dnssec_key_t *key)
{
	assert(key);
	assert(key->public_key);

	_cleanup_free_ char *new_id = gnutls_pubkey_hex_key_id(key->public_key);
	assert(new_id);
	memcpy(key->id, new_id, sizeof(key->id));
}

void key_update_identifiers(dnssec_key_t *key)
{
	assert(key);

	update_keytag(key);
	update_key_id(key);
}

static bool has_valid_id(const dnssec_key_t *key)
{
	assert(key);

	return (key->id[0] != '\0');
}

_public_
uint16_t dnssec_key_get_keytag(const dnssec_key_t *key)
{
	if (!key || !has_valid_id(key)) {
		return 0;
	}

	return key->keytag;
}

_public_
const char *dnssec_key_get_id(const dnssec_key_t *key)
{
	if (!key || !has_valid_id(key)) {
		return NULL;
	}

	return key->id;
}

/* -- freely modifiable attributes ----------------------------------------- */

_public_
const uint8_t *dnssec_key_get_dname(const dnssec_key_t *key)
{
	if (!key) {
		return NULL;
	}

	return key->dname;
}

_public_
int dnssec_key_set_dname(dnssec_key_t *key, const uint8_t *dname)
{
	if (!key) {
		return DNSSEC_EINVAL;
	}

	uint8_t *copy = NULL;
	if (dname) {
		copy = dname_copy(dname);
		if (!copy) {
			return DNSSEC_ENOMEM;
		}

		dname_normalize(copy);
	}

	free(key->dname);
	key->dname = copy;

	return DNSSEC_EOK;
}

_public_
uint16_t dnssec_key_get_flags(const dnssec_key_t *key)
{
	if (!key) {
		return 0;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_FLAGS);
	return wire_read_u16(&wire);
}

_public_
int dnssec_key_set_flags(dnssec_key_t *key, uint16_t flags)
{
	if (!key) {
		return DNSSEC_EINVAL;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_FLAGS);
	wire_write_u16(&wire, flags);

	update_keytag(key);

	return DNSSEC_EOK;
}

_public_
uint8_t dnssec_key_get_protocol(const dnssec_key_t *key)
{
	if (!key) {
		return DNSSEC_EINVAL;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_PROTOCOL);
	return wire_read_u8(&wire);
}

_public_
int dnssec_key_set_protocol(dnssec_key_t *key, uint8_t protocol)
{
	if (!key) {
		return DNSSEC_EINVAL;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_PROTOCOL);
	wire_write_u8(&wire, protocol);

	update_keytag(key);

	return DNSSEC_EOK;
}

/* -- restricted attributes ------------------------------------------------ */

/*!
 * Check if current public key algorithm matches with the new algorithm.
 */
static bool can_change_algorithm(dnssec_key_t *key, uint8_t algorithm)
{
	assert(key);

	if (!key->public_key) {
		return true;
	}

	gnutls_pk_algorithm_t update = algorithm_to_gnutls(algorithm);
	if (update == GNUTLS_PK_UNKNOWN) {
		return false;
	}

	int current = gnutls_pubkey_get_pk_algorithm(key->public_key, NULL);
	assert(current >= 0);

	return current == update;
}

_public_
uint8_t dnssec_key_get_algorithm(const dnssec_key_t *key)
{
	if (!key) {
		return 0;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_ALGORITHM);
	return wire_read_u8(&wire);
}

_public_
int dnssec_key_set_algorithm(dnssec_key_t *key, uint8_t algorithm)
{
	if (!key) {
		return DNSSEC_EINVAL;
	}

	if (!can_change_algorithm(key, algorithm)) {
		return DNSSEC_INVALID_KEY_ALGORITHM;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_ALGORITHM);
	wire_write_u8(&wire, algorithm);

	update_keytag(key);

	return DNSSEC_EOK;
}

_public_
int dnssec_key_get_pubkey(const dnssec_key_t *key, dnssec_binary_t *pubkey)
{
	if (!key || !pubkey) {
		return DNSSEC_EINVAL;
	}

	wire_ctx_t wire = wire_init_binary(&key->rdata);
	wire_seek(&wire, DNSKEY_RDATA_OFFSET_PUBKEY);
	wire_available_binary(&wire, pubkey);

	return DNSSEC_EOK;
}

_public_
int dnssec_key_set_pubkey(dnssec_key_t *key, const dnssec_binary_t *pubkey)
{
	if (!key || !pubkey || !pubkey->data) {
		return DNSSEC_EINVAL;
	}

	if (key->public_key) {
		return DNSSEC_KEY_ALREADY_PRESENT;
	}

	if (dnssec_key_get_algorithm(key) == 0) {
		return DNSSEC_INVALID_KEY_ALGORITHM;
	}

	int result = dnskey_rdata_set_pubkey(&key->rdata, pubkey);
	if (result != DNSSEC_EOK) {
		return result;
	}

	result = dnskey_rdata_to_crypto_key(&key->rdata, &key->public_key);
	if (result != DNSSEC_EOK) {
		key->rdata.size = DNSKEY_RDATA_OFFSET_PUBKEY; // downsize
		return result;
	}

	key_update_identifiers(key);

	return DNSSEC_EOK;
}

_public_
unsigned dnssec_key_get_size(const dnssec_key_t *key)
{
	if (!key || !key->public_key) {
		return 0;
	}

	unsigned bits = 0;
	gnutls_pubkey_get_pk_algorithm(key->public_key, &bits);

	return bits;
}

_public_
int dnssec_key_get_rdata(const dnssec_key_t *key, dnssec_binary_t *rdata)
{
	if (!key || !rdata) {
		return DNSSEC_EINVAL;
	}

	*rdata = key->rdata;

	return DNSSEC_EOK;
}

_public_
int dnssec_key_set_rdata(dnssec_key_t *key, const dnssec_binary_t *rdata)
{
	if (!key || !rdata || !rdata->data) {
		return DNSSEC_EINVAL;
	}

	if (rdata->size < DNSKEY_RDATA_MIN_SIZE) {
		return DNSSEC_MALFORMED_DATA;
	}

	if (key->public_key) {
		return DNSSEC_KEY_ALREADY_PRESENT;
	}

	gnutls_pubkey_t new_pubkey = NULL;
	int result = dnskey_rdata_to_crypto_key(rdata, &new_pubkey);
	if (result != DNSSEC_EOK) {
		return result;
	}

	result = dnssec_binary_resize(&key->rdata, rdata->size);
	if (result != DNSSEC_EOK) {
		gnutls_pubkey_deinit(new_pubkey);
		return result;
	}

	// commit result
	memmove(key->rdata.data, rdata->data, rdata->size);
	key->public_key = new_pubkey;
	key_update_identifiers(key);
	return DNSSEC_EOK;
}

/* -- key presence checking ------------------------------------------------ */

_public_
bool dnssec_key_can_sign(const dnssec_key_t *key)
{
	return key && key->private_key;
}

_public_
bool dnssec_key_can_verify(const dnssec_key_t *key)
{
	return key && key->public_key;
}
