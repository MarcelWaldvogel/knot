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

#include <config.h>
#include <assert.h>
#include <tap/basic.h>

#include "knot/updates/changesets.h"

int main(int argc, char *argv[])
{
	plan(22);

	// Test with NULL changeset
	ok(changeset_size(NULL) == 0, "changeset: NULL size");
	ok(changeset_empty(NULL), "changeset: NULL empty");

	// Test creation.
	knot_dname_t *d = knot_dname_from_str("test.");
	assert(d);
	changeset_t *ch = changeset_new(NULL, d);
	knot_dname_free(&d, NULL);
	ok(ch != NULL, "changeset: new");
	ok(changeset_empty(ch), "changeset: empty");
	ok(changeset_size(ch) == 0, "changeset: empty size");

	// Test additions.
	d = knot_dname_from_str("non.terminals.test.");
	assert(d);
	knot_rrset_t *apex_txt_rr = knot_rrset_new(d, KNOT_RRTYPE_TXT, KNOT_CLASS_IN, NULL);
	assert(apex_txt_rr);
	uint8_t data[8] = "\7teststr";
	knot_rrset_add_rdata(apex_txt_rr, data, sizeof(data), 3600, NULL);

	int ret = changeset_add_rrset(ch, apex_txt_rr);
	ok(ret == KNOT_EOK, "changeset: add RRSet");
	ok(changeset_size(ch) == 1, "changeset: size add");
	ret = changeset_rem_rrset(ch, apex_txt_rr);
	ok(ret == KNOT_EOK, "changeset: rem RRSet");
	ok(changeset_size(ch) == 2, "changeset: size remove");

	ok(!changeset_empty(ch), "changeset: not empty");

	// Add another RR to node.
	knot_rrset_t *apex_spf_rr = knot_rrset_new(d, KNOT_RRTYPE_SPF, KNOT_CLASS_IN, NULL);
	assert(apex_spf_rr);
	knot_rrset_add_rdata(apex_spf_rr, data, sizeof(data), 3600, NULL);
	ret = changeset_add_rrset(ch, apex_spf_rr);
	ok(ret == KNOT_EOK, "changeset: add multiple");

	// Add another node.
	knot_dname_free(&d, NULL);
	d = knot_dname_from_str("here.come.more.non.terminals.test");
	assert(d);
	knot_rrset_t *other_rr = knot_rrset_new(d, KNOT_RRTYPE_TXT, KNOT_CLASS_IN, NULL);
	assert(other_rr);
	knot_rrset_add_rdata(other_rr, data, sizeof(data), 3600, NULL);
	ret = changeset_add_rrset(ch, other_rr);
	ok(ret == KNOT_EOK, "changeset: remove multiple");

	// Test add traversal.
	changeset_iter_t *it = changeset_iter_add(ch, true);
	ok(it != NULL, "changeset: create iter add");
	// Order: non.terminals.test. TXT, SPF, here.come.more.non.terminals.test. TXT.
	knot_rrset_t iter = changeset_iter_next(it);
	bool trav_ok = knot_rrset_equal(&iter, apex_txt_rr, KNOT_RRSET_COMPARE_WHOLE);
	iter = changeset_iter_next(it);
	trav_ok = trav_ok && knot_rrset_equal(&iter, apex_spf_rr, KNOT_RRSET_COMPARE_WHOLE);
	iter = changeset_iter_next(it);
	trav_ok = trav_ok && knot_rrset_equal(&iter, other_rr, KNOT_RRSET_COMPARE_WHOLE);

	ok(trav_ok, "changeset: add traversal");

	iter = changeset_iter_next(it);
	changeset_iter_free(it, NULL);
	ok(knot_rrset_empty(&iter), "changeset: traversal: skip non-terminals");

	// Test remove traversal.
	it = changeset_iter_rem(ch, false);
	ok(it != NULL, "changeset: create iter rem");
	iter = changeset_iter_next(it);
	ok(knot_rrset_equal(&iter, apex_txt_rr, KNOT_RRSET_COMPARE_WHOLE),
	   "changeset: rem traversal");
	changeset_iter_free(it, NULL);

	// Test all traversal - just count.
	it = changeset_iter_all(ch, false);
	ok(it != NULL, "changest: create iter all");
	size_t size = 0;
	iter = changeset_iter_next(it);
	while (!knot_rrset_empty(&iter)) {
		++size;
		iter = changeset_iter_next(it);
	}
	changeset_iter_free(it, NULL);
	ok(size == 4, "changeset: iter all");

	// Create new changeset.
	knot_dname_free(&d, NULL);
	d = knot_dname_from_str("test.");
	assert(d);
	changeset_t *ch2 = changeset_new(NULL, d);
	knot_dname_free(&d, NULL);
	assert(ch2);
	// Add something to add section.
	knot_dname_free(&apex_txt_rr->owner, NULL);
	apex_txt_rr->owner = knot_dname_from_str("something.test.");
	assert(apex_txt_rr->owner);
	ret = changeset_add_rrset(ch2, apex_txt_rr);
	assert(ret == KNOT_EOK);

	// Add something to remove section.
	knot_dname_free(&apex_txt_rr->owner, NULL);
	apex_txt_rr->owner =
		knot_dname_from_str("and.now.for.something.completely.different.test.");
	assert(apex_txt_rr->owner);
	ret = changeset_rem_rrset(ch2, apex_txt_rr);
	assert(ret == KNOT_EOK);

	// Test merge.
	ret = changeset_merge(ch, ch2);
	ok(ret == KNOT_EOK && changeset_size(ch) == 6, "changeset: merge");

	// Test cleanup.
	changeset_clear(ch, NULL);
	ok(changeset_empty(ch), "changeset: clear");
	free(ch);

	list_t chgs;
	init_list(&chgs);
	add_head(&chgs, &ch2->n);
	changesets_free(&chgs, NULL);
	ok(changeset_empty(ch2), "changeset: clear list");
	free(ch2);

	knot_rrset_free(&apex_txt_rr, NULL);
	knot_rrset_free(&apex_spf_rr, NULL);
	knot_rrset_free(&other_rr, NULL);

	return 0;
}

