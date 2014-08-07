/*  Copyright (C) 2013 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include "knot/nameserver/update.h"
#include "knot/nameserver/internet.h"
#include "knot/nameserver/process_query.h"
#include "knot/updates/apply.h"
#include "knot/dnssec/zone-sign.h"
#include "common/debug.h"
#include "knot/dnssec/zone-events.h"
#include "knot/updates/ddns.h"
#include "knot/updates/zone-update.h"
#include "libknot/descriptor.h"
#include "libknot/tsig-op.h"
#include "knot/zone/zone.h"
#include "knot/zone/events.h"
#include "knot/server/tcp-handler.h"
#include "knot/server/udp-handler.h"
#include "knot/nameserver/requestor.h"
#include "knot/nameserver/capture.h"
#include "libknot/dnssec/random.h"

/* UPDATE-specific logging (internal, expects 'qdata' variable set). */
#define UPDATE_LOG(severity, msg...) \
	QUERY_LOG(severity, &qdata, "UPDATE", msg)

static bool apex_rr_changed(const zone_contents_t *old_contents,
                            const zone_contents_t *new_contents,
                            uint16_t type)
{
	knot_rrset_t old_rr = node_rrset(old_contents->apex, type);
	knot_rrset_t new_rr = node_rrset(new_contents->apex, type);

	return !knot_rrset_equal(&old_rr, &new_rr, KNOT_RRSET_COMPARE_WHOLE);
}

static int sign_update(zone_t *zone, const zone_contents_t *old_contents,
                       zone_contents_t *new_contents, changeset_t *ddns_ch,
                       changeset_t *sec_ch)
{
	assert(zone != NULL);
	assert(old_contents != NULL);
	assert(new_contents != NULL);
	assert(ddns_ch != NULL);

	/*
	 * Check if the UPDATE changed DNSKEYs or NSEC3PARAM.
	 * If so, we have to sign the whole zone.
	 */
	int ret = KNOT_EOK;
	uint32_t refresh_at = 0;
	if (apex_rr_changed(old_contents, new_contents, KNOT_RRTYPE_DNSKEY) ||
	    apex_rr_changed(old_contents, new_contents, KNOT_RRTYPE_NSEC3PARAM)) {
		ret = knot_dnssec_zone_sign(new_contents, zone->conf,
		                            sec_ch, KNOT_SOA_SERIAL_KEEP,
		                            &refresh_at);
	} else {
		// Sign the created changeset
		ret = knot_dnssec_sign_changeset(new_contents, zone->conf,
		                                 ddns_ch, sec_ch,
		                                 &refresh_at);
	}
	if (ret != KNOT_EOK) {
		return ret;
	}

	// Apply DNSSEC changeset
	ret = apply_changeset_directly(new_contents, sec_ch);
	if (ret != KNOT_EOK) {
		return ret;
	}

	// Merge changesets
	ret = changeset_merge(ddns_ch, sec_ch);
	if (ret != KNOT_EOK) {
		update_cleanup(sec_ch);
		return ret;
	}

	// Plan next zone resign.
	const time_t resign_time = zone_events_get_time(zone, ZONE_EVENT_DNSSEC);
	if (time(NULL) + refresh_at < resign_time) {
		zone_events_schedule(zone, ZONE_EVENT_DNSSEC, refresh_at);
	}

	return KNOT_EOK;
}

static int process_single_update(struct request_data *request,
                                 const zone_t *zone, zone_update_t *update)
{
	// Needed for logging
	struct process_query_param param = { 0 };
	param.remote = &request->remote;
	struct query_data qdata = { 0 };
	qdata.param = &param;
	qdata.query = request->query;
	qdata.zone  = zone;

	uint16_t rcode = KNOT_RCODE_NOERROR;
	int ret = ddns_process_prereqs(request->query, update, &rcode);
	if (ret != KNOT_EOK) {
		UPDATE_LOG(LOG_WARNING, "prerequisites not met - %s\n",
		           knot_strerror(ret));
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	ret = ddns_process_update(zone, request->query, update, &rcode);
	if (ret != KNOT_EOK) {
		UPDATE_LOG(LOG_WARNING, "failed to apply - %s\n",
		           knot_strerror(ret));
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	return KNOT_EOK;
}

#undef UPDATE_LOG

static void set_rcodes(list_t *requests, const uint16_t rcode)
{
	struct request_data *req;
	WALK_LIST(req, *requests) {
		if (knot_wire_get_rcode(req->resp->wire) ==
		    KNOT_RCODE_NOERROR) {
			knot_wire_set_rcode(req->resp->wire, rcode);
		}
	}
}

static int process_normal(zone_t *zone, list_t *requests)
{
	assert(requests);

	// Create DDNS change.
	changeset_t ddns_ch;
	int ret = changeset_init(&ddns_ch, zone->name);
	if (ret != KNOT_EOK) {
		set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		return ret;
	}
	
	// Init zone update structure.
	zone_update_t zone_update;
	zone_update_init(&zone_update, zone->contents, &ddns_ch);

	// Walk all the requests and process.
	struct request_data *req;
	WALK_LIST(req, *requests) {
		ret = process_single_update(req, zone, &zone_update);
		if (ret != KNOT_EOK) {
			changeset_clear(&ddns_ch);
			set_rcodes(requests, KNOT_RCODE_SERVFAIL);
			return ret;
		}
	}

	zone_contents_t *new_contents = NULL;
	const bool change_made = !changeset_empty(&ddns_ch);
	if (change_made) {
		ret = apply_changeset(zone, &ddns_ch, &new_contents);
		if (ret != KNOT_EOK) {
			if (ret == KNOT_ETTL) {
				set_rcodes(requests, KNOT_RCODE_REFUSED);
			} else {
				set_rcodes(requests, KNOT_RCODE_SERVFAIL);
			}
			changeset_clear(&ddns_ch);
			return ret;
		}
	} else {
		changeset_clear(&ddns_ch);
		return KNOT_EOK;
	}
	assert(new_contents);

	changeset_t sec_ch;
	if (zone->conf->dnssec_enable) {
		ret = changeset_init(&sec_ch, zone->name);
		if (ret != KNOT_EOK) {
			set_rcodes(requests, KNOT_RCODE_SERVFAIL);
			return ret;
		}
		ret = sign_update(zone, zone->contents, new_contents, &ddns_ch,
		                  &sec_ch);
		if (ret != KNOT_EOK) {
			update_rollback(&ddns_ch);
			update_free_zone(&new_contents);
			changeset_clear(&ddns_ch);
			set_rcodes(requests, KNOT_RCODE_SERVFAIL);
			return ret;
		}
	}

	// Write changes to journal if all went well. (DNSSEC merged)
	ret = zone_change_store(zone, &ddns_ch);
	if (ret != KNOT_EOK) {
		update_rollback(&ddns_ch);
		update_free_zone(&new_contents);
		changeset_clear(&ddns_ch);
		if (zone->conf->dnssec_enable) {
			changeset_clear(&sec_ch);
		}
		set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		return ret;
	}

	// Switch zone contents.
	zone_contents_t *old_contents = zone_switch_contents(zone, new_contents);
	synchronize_rcu();

	// Clear DNSSEC changes
	if (zone->conf->dnssec_enable) {
		update_cleanup(&sec_ch);
		changeset_clear(&sec_ch);
	}

	// Clear obsolete zone contents
	update_free_zone(&old_contents);

	update_cleanup(&ddns_ch);
	changeset_clear(&ddns_ch);

	/* Sync zonefile immediately if configured. */
	if (zone->conf->dbsync_timeout == 0) {
		zone_events_schedule(zone, ZONE_EVENT_FLUSH, ZONE_EVENT_NOW);
	}

	return ret;
}


static int process_requests(zone_t *zone, list_t *requests)
{
	if (zone == NULL || requests == NULL) {
		return KNOT_EINVAL;
	}

	/* Keep original state. */
	struct timeval t_start, t_end;
	gettimeofday(&t_start, NULL);
	const uint32_t old_serial = zone_contents_serial(zone->contents);

	/* Process authenticated packet. */
	int ret = process_normal(zone, requests);
	if (ret != KNOT_EOK) {
		log_zone_error(zone->name, "DDNS: processing failed - %s\n",
		               knot_strerror(ret));
		return ret;
	}

	/* Evaluate response. */
	const uint32_t new_serial = zone_contents_serial(zone->contents);
	if (new_serial == old_serial) {
		log_zone_info(zone->name, "DDNS: no change to zone made\n");
		return KNOT_EOK;
	}

	gettimeofday(&t_end, NULL);
	log_zone_info(zone->name, "DDNS: Serial %u -> %u\n", old_serial, new_serial);
	log_zone_info(zone->name, "DDNS: Update finished in %.02fs.\n",
	              time_diff(&t_start, &t_end) / 1000.0);
	
	zone_events_schedule(zone, ZONE_EVENT_NOTIFY, ZONE_EVENT_NOW);

	return KNOT_EOK;
}

static int forward_request(zone_t *zone, struct request_data *request)
{
	/* Create requestor instance. */
	struct requestor re;
	requestor_init(&re, NS_PROC_CAPTURE, NULL);

	/* Fetch primary master. */
	const conf_iface_t *master = zone_master(zone);

	/* Copy request and assign new ID. */
	knot_pkt_t *query = knot_pkt_new(NULL, request->query->max_size, NULL);
	int ret = knot_pkt_copy(query, request->query);
	if (ret != KNOT_EOK) {
		knot_wire_set_rcode(request->resp->wire, KNOT_RCODE_SERVFAIL);
		return ret;
	}
	knot_wire_set_id(query->wire, knot_random_uint16_t());
	knot_tsig_append(query->wire, &query->size, query->max_size, query->tsig_rr);

	/* Create a request. */
	struct request *req = requestor_make(&re, master, query);
	if (req == NULL) {
		knot_pkt_free(&query);
		return KNOT_ENOMEM;
	}

	/* Enqueue and execute request. */
	struct process_capture_param param;
	param.sink = request->resp;
	ret = requestor_enqueue(&re, req, &param);
	if (ret == KNOT_EOK) {
		struct timeval tv = { conf()->max_conn_reply, 0 };
		ret = requestor_exec(&re, &tv);
	}

	requestor_clear(&re);

	/* Restore message ID and TSIG. */
	knot_wire_set_id(request->resp->wire, knot_wire_get_id(request->query->wire));
	knot_tsig_append(request->resp->wire, &request->resp->size,
	                 request->resp->max_size, request->resp->tsig_rr);

	/* Set RCODE if forwarding failed. */
	if (ret != KNOT_EOK) {
		knot_wire_set_rcode(request->resp->wire, KNOT_RCODE_SERVFAIL);
		log_zone_error(zone->name, "DDNS: "
		               "Failed to forward UPDATEs to master: %s\n",
		               knot_strerror(ret));
	} else {
		log_zone_info(zone->name, "DDNS: UPDATEs forwarded\n");
	}

	return ret;
}

static void forward_requests(zone_t *zone, list_t *requests)
{
	struct request_data *req;
	WALK_LIST(req, *requests) {
		forward_request(zone, req);
	}
}

static int init_update_respones(list_t *updates)
{
	struct request_data *r = NULL;
	WALK_LIST(r, *updates) {
		r->resp = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, NULL);
		if (r->resp == NULL) {
			return KNOT_ENOMEM;
		}

		assert(r->query);
		knot_pkt_init_response(r->resp, r->query);
	}

	return KNOT_EOK;
}

static void send_update_responses(list_t *updates)
{
	struct request_data *r, *nxt;
	WALK_LIST_DELSAFE(r, nxt, *updates) {
		if (r->resp) {
			if (net_is_connected(r->fd)) {
				tcp_send_msg(r->fd, r->resp->wire, r->resp->size);
			} else {
				udp_send_msg(r->fd, r->resp->wire, r->resp->size,
				             (struct sockaddr *)&r->remote);
			}
		}

		close(r->fd);
		knot_pkt_free(&r->query);
		knot_pkt_free(&r->resp);
		free(r);
	}
}

int update_query_process(knot_pkt_t *pkt, struct query_data *qdata)
{
	/* RFC1996 require SOA question. */
	NS_NEED_QTYPE(qdata, KNOT_RRTYPE_SOA, KNOT_RCODE_FORMERR);

	/* Check valid zone. */
	NS_NEED_ZONE(qdata, KNOT_RCODE_NOTAUTH);

	/* Need valid transaction security. */
	zone_t *zone = (zone_t *)qdata->zone;
	NS_NEED_AUTH(&zone->conf->acl.update_in, qdata);
	/* Check expiration. */
	NS_NEED_ZONE_CONTENTS(qdata, KNOT_RCODE_SERVFAIL);

	/* Store update into DDNS queue. */
	int ret = zone_update_enqueue(zone, qdata->query, qdata->param);
	if (ret != KNOT_EOK) {
		return NS_PROC_FAIL;
	}

	/* No immediate response. */
	pkt->size = 0;
	return NS_PROC_DONE;
}

int updates_execute(zone_t *zone)
{
	/* Get list of pending updates. */
	list_t updates;
	size_t update_count;
	zone_update_dequeue(zone, &updates, &update_count);
	if (EMPTY_LIST(updates)) {
		return KNOT_EOK;
	}

	/* Init updates respones. */
	int ret = init_update_respones(&updates);
	if (ret != KNOT_EOK) {
		/* Send what responses we can. */
		set_rcodes(&updates, KNOT_RCODE_SERVFAIL);
		send_update_responses(&updates);
		return ret;
	}

	/* Process update list - forward if zone has master, or execute. */
	if (zone_master(zone)) {
		log_zone_info(zone->name,
		              "DDNS: forwarding %zu dynamic updates\n", update_count);
		forward_requests(zone, &updates);
	} else {
		log_zone_info(zone->name,
		              "DDNS: processing %zu dynamic updates\n", update_count);
		ret = process_requests(zone, &updates);
	}
	UNUSED(ret); /* Don't care about the Knot code, RCODEs are set. */

	/* Send responses. */
	send_update_responses(&updates);

	return KNOT_EOK;
}

