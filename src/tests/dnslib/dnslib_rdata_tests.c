/*!
 * \file dnslib_rdata_tests.c
 * \author Lubos Slovak <lubos.slovak@nic.cz>
 *
 * Contains unit tests for RDATA (dnslib_rdata_t) and RDATA item
 * (dnslib_rdata_item_t) structures.
 *
 * Contains tests for:
 * - creating empty RDATA structure with or without reserved space.
 * - setting RDATA items one-by-one
 * - setting RDATA items all at once
 *
 * As for now, the tests use several (TEST_RDATAS) RDATA structures, each
 * with different number of RDATA items (given by test_rdatas). These are all
 * initialized to pointers derived from RDATA_ITEM_PTR (first is RDATA_ITEM_PTR,
 * second RDATA_ITEM_PTR + 1, etc.). The functions only test if the pointer
 * is set properly.
 *
 * \todo It may be better to test also some RDATAs with predefined contents,
 *       such as some numbers, some domain name, etc. For this purpose, we'd
 *       need RDATA descriptors (telling the types of each RDATA item within an
 *       RDATA).
 *
 * \todo It will be fine to test all possible output values of all functions,
 *       e.g. test whether dnslib_rdata_get_item() returns NULL when passed an
 *       illegal position, etc.
 */

#include <stdlib.h>

#include "tap_unit.h"

#include "common.h"
#include "rdata.h"
#include "descriptor.h"

static const struct test_domain test_domains_ok[];

static int dnslib_rdata_tests_count(int argc, char *argv[]);
static int dnslib_rdata_tests_run(int argc, char *argv[]);

/*! Exported unit API.
 */
unit_api dnslib_rdata_tests_api = {
   "DNS library - rdata",        //! Unit name
   &dnslib_rdata_tests_count,  //! Count scheduled tests
   &dnslib_rdata_tests_run     //! Run scheduled tests
};

/*----------------------------------------------------------------------------*/
/*
 *  Unit implementation.
 */

static uint8_t *RDATA_ITEM_PTR = (uint8_t *)0xDEADBEEF;

/*----------------------------------------------------------------------------*/
/*!
 * \brief Tests dnslib_rdata_new().
 *
 * Creates new RDATA structure with no items and tests if there really are no
 * items in it.
 *
 * \retval > 0 on success.
 * \retval 0 otherwise.
 */
static int test_rdata_create()
{
	dnslib_rdata_t *rdata = dnslib_rdata_new();
	if (rdata == NULL) {
		diag("RDATA structure not created!");
		return 0;
	}

	if (dnslib_rdata_get_item(rdata, 0) != NULL) {
		diag("Get item returned something else than NULL!");
		return 0;
	}

	dnslib_rdata_free(&rdata);
	return 1;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Tests dnslib_rdata_free().
 * \retval > 0 on success.
 * \retval 0 otherwise.
 */
static int test_rdata_delete() {
	// how to test this??
	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Sets RDATA items within the given RDATA structure one-by-one.
 *
 * Sets the items to hold value RDATA_ITEM_PTR increased by the index of the
 * item (i.e. + 0 for the first, + 1 for the second, etc.).
 */
//static void set_rdata( dnslib_rdata_t *rdata, int i )
//{
//	assert(rdata != NULL);

////	dnslib_rdata_item_t item;
////	item.raw_data = RDATA_ITEM_PTR;

////	for (int j = 0; j < test_rdatas[i].items; ++j) {
////		dnslib_rdata_set_item(rdata, j, item);
////		++item.raw_data;
////	}
//}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Sets RDATA items within the given RDATA structure all at once.
 *
 * Sets the items to hold value RDATA_ITEM_PTR increased by the index of the
 * item (i.e. + 0 for the first, + 1 for the second, etc.).
 *
 * \retval > 0 if successful.
 * \retval 0 if an error occured.
 */
//static int set_rdata_all( dnslib_rdata_t *rdata, int i )
//{
//	assert(rdata != NULL);

//	dnslib_rdata_item_t *items = (dnslib_rdata_item_t *)malloc(
//			test_rdatas[i].items * sizeof(dnslib_rdata_item_t));

//	if (items == NULL) {
//		diag("Allocation failed in set_rdata_all().");
//		return 0;
//	}

//	for (int j = 0; j < test_rdatas[i].items; ++j) {
//		items[j].raw_data = RDATA_ITEM_PTR + j;
//	}

//	dnslib_rdata_set_items(rdata, items, test_rdatas[i].items);

//	return 1;
//}

/*----------------------------------------------------------------------------*/

static void generate_rdata( uint8_t *data, int size )
{
	for (int i = 0; i < size; ++i) {
		data[i] = rand() % 256;
	}
}

/*----------------------------------------------------------------------------*/

static int fill_rdata( uint8_t *data, int max_size, uint16_t rrtype,
				dnslib_rdata_t *rdata )
{
	assert(rdata != NULL);
	assert(data != NULL);
	assert(max_size > 0);

	uint8_t *pos = data;
	int used = 0;

	//note("Filling RRType %u", rrtype);

	dnslib_rrtype_descriptor_t *desc = dnslib_rrtype_descriptor_by_type(rrtype);

	uint item_count = desc->length;
	dnslib_rdata_item_t *items = (dnslib_rdata_item_t *)malloc(
			item_count * sizeof(dnslib_rdata_item_t));

	for (int i = 0; i < item_count; ++i) {
		uint8_t size = 0;
		int domain = 0;
		int store_length = 0;
		dnslib_dname_t *dname = NULL;
		int binary = 0;

		switch (desc->wireformat[i]) {
		case DNSLIB_RDATA_WF_COMPRESSED_DNAME:
		case DNSLIB_RDATA_WF_UNCOMPRESSED_DNAME:
		case DNSLIB_RDATA_WF_LITERAL_DNAME:
			dname = dnslib_dname_new_from_wire(
							(uint8_t *)test_domains_ok[0].wire,
							test_domains_ok[0].size, NULL);
			assert(dname != NULL);
			//note("Created domain name: %s", dnslib_dname_name(dname));
			//note("Domain name ptr: %p", dname);
			domain = 1;
			break;
		case DNSLIB_RDATA_WF_BYTE:
			size = 1;
			break;
		case DNSLIB_RDATA_WF_SHORT:
			size = 2;
			break;
		case DNSLIB_RDATA_WF_LONG:
			size = 4;
			break;
		case DNSLIB_RDATA_WF_A:
			size = 4;
			break;
		case DNSLIB_RDATA_WF_AAAA:
			size = 16;
			break;
		case DNSLIB_RDATA_WF_TEXT:
		case DNSLIB_RDATA_WF_BINARYWITHLENGTH:
			store_length = 1;
		case DNSLIB_RDATA_WF_BINARY:
		case DNSLIB_RDATA_WF_APL:			// saved as binary
		case DNSLIB_RDATA_WF_IPSECGATEWAY:	// saved as binary
			// size stored in the first byte
			size = rand() % DNSLIB_MAX_RDATA_ITEM_SIZE + 1;
			binary = 1;
			break;
		default:
			assert(0);
		}

		assert(size > 0 || domain);
		used += size;
		assert(used < max_size);

		if (binary) {
			// rewrite the actual byte in the data array with length octet
			// (this is a bit ugly, but does the work ;-)
			*pos = size;
		}

		if (domain) {
			items[i].dname = dname;
			//note("Saved domain name ptr: %p", items[i].dname);
		} else {
			items[i].raw_data = pos;
			pos += size;
			if (binary) {
				++pos;
			}
		}
	}

	int res = dnslib_rdata_set_items(rdata, items, item_count);
	if (res != 0) {
		diag("dnslib_rdata_set_items() returned %d.", res);
		return 1;
	} else {
		return 0;
	}
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Checks if all RDATA items in the given RDATA structure are correct.
 *
 * \return Number of errors encountered. Error is either if some RDATA item
 *         is not set (i.e. NULL) or if it has other than the expected value.
 */
static int check_rdata( const uint8_t *data, int max_size, uint16_t rrtype,
						const dnslib_rdata_t *rdata )
{
	assert(rdata != NULL);
	assert(data != NULL);
	assert(max_size > 0);

	int errors = 0;

	const uint8_t *pos = data;
	int used = 0;

	dnslib_rrtype_descriptor_t *desc = dnslib_rrtype_descriptor_by_type(rrtype);
	uint item_count = desc->length;
	//note("check_rdata(), RRType: %u", rrtype);
	//note("  item count: %u", item_count);

	for (int i = 0; i < item_count; ++i) {
		uint size = 0;
		int domain = 0;
		int stored_length = 0;
		int binary = 0;

		//note("  item: %d", i);

		switch (desc->wireformat[i]) {
		case DNSLIB_RDATA_WF_COMPRESSED_DNAME:
		case DNSLIB_RDATA_WF_UNCOMPRESSED_DNAME:
		case DNSLIB_RDATA_WF_LITERAL_DNAME:
			//note("    domain name");
			domain = 1;
			break;
		case DNSLIB_RDATA_WF_BYTE:
			//note("    1byte int");
			size = 1;
			break;
		case DNSLIB_RDATA_WF_SHORT:
			//note("    2byte int");
			size = 2;
			break;
		case DNSLIB_RDATA_WF_LONG:
			//note("    4byte int");
			size = 4;
			break;
		case DNSLIB_RDATA_WF_A:
			//note("    A");
			size = 4;
			break;
		case DNSLIB_RDATA_WF_AAAA:
			//note("    AAAA");
			size = 16;
			break;
		case DNSLIB_RDATA_WF_BINARY:
		case DNSLIB_RDATA_WF_APL:			// saved as binary
		case DNSLIB_RDATA_WF_IPSECGATEWAY:	// saved as binary
			//note("    binary");
			// size should be stored in the data array
			stored_length = 1;
		case DNSLIB_RDATA_WF_TEXT:
		case DNSLIB_RDATA_WF_BINARYWITHLENGTH:
			//note("    text or binary with length (%u)", *pos);
			size = *pos + 1;
			binary = 1;
			break;
		default:
			assert(0);
		}

		assert(size > 0 || domain);
		used += size;
		assert(used < max_size);

		//note("    item size: %u", size);

		if (domain) {
			//note("Domain name ptr: %p", dnslib_rdata_get_item(rdata, i)->dname);
			if (strncmp((char *)dnslib_dname_name(
					dnslib_rdata_get_item(rdata, i)->dname),
					(char *)test_domains_ok[0].wire,
					test_domains_ok[0].size) != 0) {
				diag("Domain name stored in %d-th RDATA item is wrong: %s ("
					 "should be %s)", i,
					 dnslib_dname_name(dnslib_rdata_get_item(rdata, i)->dname),
					 test_domains_ok[0].wire);
				++errors;
				continue;
			}
		}

//		if (stored_length) {
//			// if the length octet is not stored in the RDATA item, skip
//			// it in the data array, so we can compare
//			++pos;
//		}

		if (binary
			&& size != dnslib_rdata_get_item(rdata, i)->raw_data[0] + 1) {
			diag("Size of stored binary data is wrong: %u (should be %u)",
				 dnslib_rdata_get_item(rdata, i)->raw_data[0] + 1, size);
			++errors;
		}

		if (strncmp((char *)(&dnslib_rdata_get_item(rdata, i)->raw_data[0]),
					(char *)pos, size) != 0) {
			diag("Data stored in %d-th RDATA item are wrong.", i);
			++errors;
		}

		pos += size;
	}

	return errors;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Tests dnslib_rdata_set_item().
 *
 * \retval > 0 on success.
 * \retval 0 otherwise.
 */
static int test_rdata_set_item()
{
	dnslib_rdata_t *rdata = dnslib_rdata_new();
	dnslib_rdata_item_t item;
	item.raw_data = RDATA_ITEM_PTR;

	int ret = dnslib_rdata_set_item(rdata, 0, item);
	if (ret == 0) {
		diag("dnslib_rdata_set_item() called on empty RDATA returned %d instead"
			 "of error (-1).", ret);
		dnslib_rdata_free(&rdata);
		return 0;
	}

	uint8_t data[DNSLIB_MAX_RDATA_WIRE_SIZE];
	generate_rdata(data, DNSLIB_MAX_RDATA_WIRE_SIZE);

	// set items through set_items() and then call set_item()
	uint16_t rrtype = rand() % DNSLIB_RRTYPE_LAST + 1;
	fill_rdata(data, DNSLIB_MAX_RDATA_WIRE_SIZE, rrtype, rdata);

	uint8_t pos = rand() % dnslib_rrtype_descriptor_by_type(rrtype)->length;

	ret = dnslib_rdata_set_item(rdata, pos, item);
	if (ret != 0) {
		diag("dnslib_rdata_set_item() called on filled RDATA returned %d"
			 " instead of 0.", ret);
		dnslib_rdata_free(&rdata);
		return 0;
	}

	if (dnslib_rdata_get_item(rdata, pos)->raw_data != RDATA_ITEM_PTR) {
		diag("RDATA item on position %d is wrong: %p (should be %p).",
			 pos, dnslib_rdata_get_item(rdata, pos)->raw_data, RDATA_ITEM_PTR);
		dnslib_rdata_free(&rdata);
		return 0;
	}

	dnslib_rdata_free(&rdata);
	return 1;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Tests dnslib_rdata_set_items().
 *
 * Iterates over the test_rdatas array and for each testing RDATA it creates
 * the RDATA structure, sets its items (\see set_rdata_all()) and checks if the
 * items are set properly (\see check_rdata()).
 *
 * \retval > 0 on success.
 * \retval 0 otherwise.
 */
static int test_rdata_set_items()
{
	dnslib_rdata_t *rdata;
	int errors = 0;

	// generate some random data
	uint8_t data[DNSLIB_MAX_RDATA_WIRE_SIZE];
	generate_rdata(data, DNSLIB_MAX_RDATA_WIRE_SIZE);

	for (int i = 0; i <= DNSLIB_RRTYPE_LAST; ++i) {
		rdata = dnslib_rdata_new();

		errors += fill_rdata(data, DNSLIB_MAX_RDATA_WIRE_SIZE, i, rdata);
		errors += check_rdata(data, DNSLIB_MAX_RDATA_WIRE_SIZE, i, rdata);

		dnslib_rdata_free(&rdata);
	}

	return (errors == 0);
}

/*----------------------------------------------------------------------------*/

static const int DNSLIB_RDATA_TEST_COUNT = 5;

/*! This helper routine should report number of
 *  scheduled tests for given parameters.
 */
static int dnslib_rdata_tests_count(int argc, char *argv[])
{
   return DNSLIB_RDATA_TEST_COUNT;
}

/*! Run all scheduled tests for given parameters.
 */
static int dnslib_rdata_tests_run(int argc, char *argv[])
{
	int res = 0;

	res = test_rdata_create(0);
	ok(res, "rdata: create empty");

	skip(!res, 3);

	todo();

	ok(test_rdata_delete(), "rdata: delete");

	endtodo;

	ok(res = test_rdata_set_items(), "rdata: set items all at once");

	skip(!res, 1);

	ok(test_rdata_set_item(), "rdata: set items one-by-one");

	endskip;	/* test_rdata_set_items() failed */

	endskip;	/* test_rdata_create() failed */

	return 0;
}
