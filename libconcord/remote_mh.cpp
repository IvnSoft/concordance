/*
 * vi: formatoptions+=tc textwidth=80 tabstop=8 shiftwidth=8 noexpandtab:
 *
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * (C) Copyright Kevin Timmerman 2007
 * (C) Copyright Phil Dibowitz 2007
 * (C) Copyright Scott Talbert 2012
 */

#include "remote.h"

#include <string.h>
#include <iostream>
#include <stdlib.h>

#include "libconcord.h"
#include "lc_internal.h"
#include "hid.h"
#include "protocol.h"
#include "remote_info.h"
#include "web.h"

/* Timeout to wait for a response, in ms. */
#define MH_TIMEOUT 5000
#define MH_MAX_PACKET_SIZE 64
/* In data mode, two bytes are used for the header. */
#define MH_MAX_DATA_SIZE 62

void debug_print_packet(uint8_t* p)
{
	debug("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x " \
		"%02x %02x %02x %02x",
		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
		p[10], p[11], p[12], p[13], p[14], p[15]);
}

int CRemoteMH::Reset(uint8_t kind)
{
	int err;
	uint8_t rsp[MH_MAX_PACKET_SIZE];

	/* write reset msg */
	const uint8_t msg_reset[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0xFF, 0x02, 0x01, 0x01 };
	if ((err = HID_WriteReport(msg_reset))) {
		debug("Failed to write to remote");
		return 1;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return 1;
	}
	debug("msg_reset");
	debug_print_packet(rsp);

	return 0;
}

string find_value(string str, string key)
{
	string value = "";
	int pos = str.find(key);
	if (pos != string::npos) {
		int value_start = str.find(" ", pos) + 1;
		int value_end = str.find(0x0A, pos);
		int len = value_end - value_start;
		if ((value_start != string::npos) &&
		    (value_end != string::npos))
			value = str.substr(value_start, len);
	}
	return value;
}

struct mh_config_attributes {
	uint8_t type[3];
	uint8_t seed[2];
	uint8_t length[4];
	uint8_t expectedvalue[2];
};

// Parse the XML file for the checksum-related attributes that are needed to
// complete an update configuration operation.
int get_mh_config_attributes(uint8_t *xml, uint32_t xml_size,
	mh_config_attributes *attr)
{
	int err;
	string checksum;
	uint8_t *ptr;

	err = GetTag("CHECKSUM", xml, xml_size, ptr, &checksum, true);
	if (err)
		return err;

	string type;
	err = GetAttribute("TYPE", checksum, &type);
	if (err)
		return err;
	const char *type_cstr = type.c_str();
	if (strlen(type_cstr) == 3) {
		attr->type[0] = type_cstr[0];
		attr->type[1] = type_cstr[1];
		attr->type[2] = type_cstr[2];
	}

	string seed;
	err = GetAttribute("SEED", checksum, &seed);
	if (err)
		return err;
	uint16_t seed_int = strtol(seed.c_str(), NULL, 16);
	attr->seed[0] = (seed_int & 0xFF00) >> 8;
	attr->seed[1] = (seed_int & 0x00FF);

	string length;
	err = GetAttribute("LENGTH", checksum, &length);
	if (err)
		return err;
	uint32_t length_int = strtol(length.c_str(), NULL, 16);
	attr->length[0] = (length_int & 0xFF000000) >> 24;
	attr->length[1] = (length_int & 0x00FF0000) >> 16;
	attr->length[2] = (length_int & 0x0000FF00) >> 8;
	attr->length[3] = (length_int & 0x000000FF);

	string expectedvalue;
	err = GetAttribute("EXPECTEDVALUE", checksum, &expectedvalue);
	if (err)
		return err;
	uint16_t expectedvalue_int = strtol(expectedvalue.c_str(), NULL, 16);
	attr->expectedvalue[0] = (expectedvalue_int & 0xFF00) >> 8;
	attr->expectedvalue[1] = (expectedvalue_int & 0x00FF);

	return 0;
}

// Returns the current sequence number and increments it appropriately.
uint8_t get_seq(uint8_t &seq)
{
	uint8_t tmp = seq;
	seq++;
	if (seq > 0x3F) { // seq rolls over after 0x3F
		seq = 0x00;
	}
	return tmp;
}

/*
 * Send the GET_VERSION command to the remote, and read the response.
 *
 * Then populate our struct with all the relevant info.
 */
int CRemoteMH::GetIdentity(TRemoteInfo &ri, THIDINFO &hid,
	lc_callback cb, void *cb_arg, uint32_t cb_stage)
{
	int err = 0;
	uint32_t cb_count = 0;

	const uint8_t msg_one[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x00, 0x00, 0x01, 0x01 };
	const uint8_t msg_two[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0xFF, 0x01, 0x01, 0x01, 0x66 };
	const uint8_t msg_three[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x00, 0x02 };
	const uint8_t msg_four[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x01, 0x03, 0x02, 0x80, '/', 's', 'y', 's', '/',
		  's', 'y', 's', 'i', 'n', 'f', 'o', 0x00, 0x80, 'R', 0x00 };
	const uint8_t msg_five[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x04, 0x04, 0x02, 0x01, 0x06, 0x01, 0x06 };
	const uint8_t msg_six[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x07, 0x05, 0x01, 0x01, 0x06 };
	uint8_t rsp[MH_MAX_PACKET_SIZE];

	/* 
	 * Send msg_one five times.  Yes, this is weird, but this is what the
	 * official software does and seems to be required to establish comms.
	 */
	for (int i = 0; i < 5; i++) {
		if ((err = HID_WriteReport(msg_one))) {
			debug("Failed to write to remote");
			return LC_ERROR_WRITE;
		}
	}

	if ((err = HID_WriteReport(msg_two))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}

	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_two");
	debug_print_packet(rsp);

	if ((err = HID_WriteReport(msg_three))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}

	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_three");
	debug_print_packet(rsp);

	if ((err = HID_WriteReport(msg_four))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}

	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_four");
	debug_print_packet(rsp);

	if ((err = HID_WriteReport(msg_five))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}

	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_five");
	debug_print_packet(rsp);

	string identity = "";
	while(!(err = HID_ReadReport(rsp))) {
		// Ignore 1st two bits on 2nd byte for length.
		int len = rsp[1] & 0x3F; 
		// Skip 1st two bytes, read up to packet length.  "len"
		// represents the payload length (not including the two size
		// bytes), so we read a full "len" bytes from 2 to len+2.
		for (int i = 2; i < len + 2; i++) {
			identity += rsp[i];
		}
	}
	debug("%s", identity.c_str());

	ri.fw_ver_major = strtol(find_value(identity, "fw_ver").c_str(),
				 NULL, 10);
	ri.fw_ver_minor = strtol(find_value(identity, "fw_ver").c_str()+2,
				 NULL, 10);
	ri.hw_ver_major = strtol(find_value(identity, "hw_ver").c_str(),
				 NULL, 16);
	ri.hw_ver_minor = 0;
	ri.hw_ver_micro = 0; /* usbnet remotes have a non-zero micro version */
	ri.flash_id = 0x12; // TODO: FIXME
	ri.flash_mfg = 0xFF; // TODO: FIXME
	ri.architecture = strtol(find_value(identity, "arch").c_str(),
				 NULL, 16);
	ri.fw_type = strtol(find_value(identity, "fw_type").c_str(), NULL, 16);
	ri.skin = strtol(find_value(identity, "skin").c_str(), NULL, 16);
	ri.protocol = 9; // TODO: FIXME

	setup_ri_pointers(ri);

	if (cb) {
		cb(cb_stage, cb_count++, 1, 2,
			LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);
	}

	ri.config_bytes_used = 0;
	ri.max_config_size = 1;
	ri.valid_config = 1;

	if (cb) {
		cb(cb_stage, cb_count++, 2, 2,
			LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);
	}

	string guid_str = find_value(identity, "guid");
	if (guid_str.length() >= 98) {
		uint8_t guid[48];
		char guid_char[3];
		guid_char[2] = '\0';
		char *guid_cstr = (char*)guid_str.c_str() + 2;
		for (int i = 0; i < 48; i++) {
			guid_char[0] = guid_cstr[0];
			guid_char[1] = guid_cstr[1];
			guid[i] = strtol(guid_char, NULL, 16);
			guid_cstr = guid_cstr + 2;
		}
		make_serial(guid, ri);
	}

	/* reset the sequence number to 0 */
	if ((err = HID_WriteReport(msg_six))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}

	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_six");
	debug_print_packet(rsp);

	return 0;
}

int CRemoteMH::ReadFlash(uint32_t addr, const uint32_t len, uint8_t *rd,
	unsigned int protocol, bool verify, lc_callback cb,
	void *cb_arg, uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::InvalidateFlash(lc_callback cb, void *cb_arg, uint32_t lc_stage)
{
	return LC_ERROR_UNSUPP;
}


int CRemoteMH::EraseFlash(uint32_t addr, uint32_t len,  const TRemoteInfo &ri,
	lc_callback cb, void *arg, uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::PrepFirmware(const TRemoteInfo &ri, lc_callback cb, void *cb_arg,
        uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::FinishFirmware(const TRemoteInfo &ri, lc_callback cb, void *cb_arg,
        uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::PrepConfig(const TRemoteInfo &ri, lc_callback cb, void *cb_arg,
	uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::FinishConfig(const TRemoteInfo &ri, lc_callback cb, void *cb_arg,
        uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::WriteRam(uint32_t addr, const uint32_t len, uint8_t *wr)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::ReadRam(uint32_t addr, const uint32_t len, uint8_t *rd)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::WriteFlash(uint32_t addr, const uint32_t len, const uint8_t *wr,
	unsigned int protocol, lc_callback cb, void *arg, uint32_t cb_stage)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::ReadMiscByte(uint8_t addr, uint32_t len,
		uint8_t kind, uint8_t *rd)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::ReadMiscWord(uint16_t addr, uint32_t len,
		uint8_t kind, uint16_t *rd)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::WriteMiscByte(uint8_t addr, uint32_t len,
		uint8_t kind, uint8_t *wr)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::WriteMiscWord(uint16_t addr, uint32_t len,
		uint8_t kind, uint16_t *wr)
{
	return LC_ERROR_UNSUPP;
}


int CRemoteMH::GetTime(const TRemoteInfo &ri, THarmonyTime &ht)
{
	return LC_ERROR_UNSUPP;
}

int CRemoteMH::SetTime(const TRemoteInfo &ri, const THarmonyTime &ht,
	lc_callback cb, void *cb_arg, uint32_t cb_stage)
{
	/* 
	 * MH remotes do not support SetTime() operations, but we return
	 * success because some higher level operations (for example, update
	 * configuration) call SetTime() and thus the whole operation would be
	 * declared a failure, which we do not want.
	 */
	return 0;
}

int CRemoteMH::LearnIR(uint32_t *freq, uint32_t **ir_signal,
		uint32_t *ir_signal_length, lc_callback cb, void *cb_arg,
		uint32_t cb_stage)
{
	int err = 0;
	uint32_t cb_count = 0;

	const uint8_t msg_one[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x01, 0x00, 0x02, 0x80, '/', 'i', 'r', '/', 'i', 'r',
		  '_', 'c', 'a', 'p', 0x00, 0x80, 'R', 0x00 };
	const uint8_t msg_two[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x04, 0x01, 0x02, 0x01, 0x0C, 0x01, 0x00 };
	uint8_t rsp[MH_MAX_PACKET_SIZE];

	if (cb) {
		cb(cb_stage, 0, 0, 1, LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);
	}

	if ((err = HID_WriteReport(msg_one))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_one");
	debug_print_packet(rsp);

	if ((err = HID_WriteReport(msg_two))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_two");
	debug_print_packet(rsp);

	err = LearnIRInnerLoop(freq, ir_signal, ir_signal_length, 0x90);

	/* send stop message */
	const uint8_t msg_stop[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x06, 0x02, 0x02, 0x01, 0x0C, 0x01, 0x06 };
	if (HID_WriteReport(msg_stop) != 0) {
		debug("Failed to write to remote");
		err = LC_ERROR_WRITE;
	}
	if (HID_ReadReport(rsp) != 0) {
		debug("Failed to read from remote");
		err = LC_ERROR_READ;
	}
	debug("msg_stop");
	debug_print_packet(rsp);

	/* send reset sequence message */
	const uint8_t msg_reset_seq[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x07, 0x03, 0x01, 0x01, 0x0C };
	if (HID_WriteReport(msg_reset_seq) != 0) {
		debug("Failed to write to remote");
		err = LC_ERROR_WRITE;
	}
	if (HID_ReadReport(rsp) != 0) {
		debug("Failed to read from remote");
		err = LC_ERROR_READ;
	}
	debug("msg_reset_seq");
	debug_print_packet(rsp);

	if (cb && !err) {
		cb(cb_stage, 1, 1, 1, LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);
	}

	return err;
}

int CRemoteMH::UpdateConfig(const uint32_t len, const uint8_t *wr,
	lc_callback cb, void *cb_arg, uint32_t cb_stage, uint32_t xml_size,
	uint8_t *xml)
{
	int err = 0;
	uint32_t cb_count = 0;

	const uint8_t msg_one[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0xFF, 0x00, 0x01, 0x01, 0x66 };
	const uint8_t msg_two[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x01, 0x01, 0x03, 0x80, '/', 'c', 'f', 'g', '/',
		  'u', 's', 'e', 'r', 'c', 'f', 'g', 0x00, 0x80, 'W', 0x00,
		  0x04, (len & 0xFF000000) >> 24, (len & 0x00FF0000) >> 16,
		  (len & 0x0000FF00) >> 8, len & 0x000000FF };
	const uint8_t msg_three[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x03, 0x03, 0x02, 0x01, 0x05, 0x01, 0x33 };
	uint8_t rsp[MH_MAX_PACKET_SIZE];

	cb(LC_CB_STAGE_INITIALIZE_UPDATE, cb_count++, 0, 3,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	if ((err = HID_WriteReport(msg_one))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_one");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_INITIALIZE_UPDATE, cb_count++, 1, 3,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	if ((err = HID_WriteReport(msg_two))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_two");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_INITIALIZE_UPDATE, cb_count++, 2, 3,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	if ((err = HID_WriteReport(msg_three))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	// No response is expected to this packet.
	debug("msg_three");

	cb(LC_CB_STAGE_INITIALIZE_UPDATE, cb_count++, 3, 3,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);
	cb_count = 0;

	// Start writing data:
	// First byte is the sequence number, which starts at 0x04 and rolls
	// over to 0x00 after 0x3F.
	// Second byte is the data length, up to 0x3E (62 bytes).
	uint8_t *wr_ptr = const_cast<uint8_t*>(wr);
	uint8_t seq = 0x04;
	uint32_t tlen = len;
	uint8_t pkt_len;
	uint8_t tmp_pkt[MH_MAX_PACKET_SIZE];
	int pkt_count = 0;
	int pkts_to_send = len / MH_MAX_DATA_SIZE;
	if ((len % MH_MAX_DATA_SIZE) != 0)
		pkts_to_send++;
	pkts_to_send++; // include file completion packet, 0x7E, in count.
	uint8_t ack_rsp[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x03, 0x00, 0x02, 0x01, 0x05, 0x01, 0x33 };
	while (tlen) {
		pkt_len = MH_MAX_DATA_SIZE;
		if (tlen < pkt_len) {
			pkt_len = tlen;
		}
		tlen -= pkt_len;

		tmp_pkt[0] = get_seq(seq);
		tmp_pkt[1] = pkt_len;
		memcpy(&tmp_pkt[2], wr_ptr, pkt_len);

		debug("DATA %d, sending %d bytes, %d bytes left", cb_count,
			pkt_len, tlen);

		if (err = HID_WriteReport(tmp_pkt)) {
			return err;
		}
		wr_ptr += pkt_len;
		pkt_count++;
		pkts_to_send--;

		/* Every 50 data packets, the remote seems to send us an "ack"
		   of some sort.  Read it and send a response back. */
		if (pkt_count == 50) {
			if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
				debug("Failed to read from remote");
				return LC_ERROR_READ;
			}
			debug_print_packet(rsp);
			/* 3rd byte is the sequence number */
			ack_rsp[2] = get_seq(seq);
			/* 2nd parameter is the number of packets remaining, 
			   plus one */
			if (pkts_to_send < 50)
				ack_rsp[7] = pkts_to_send + 1;
			if ((err = HID_WriteReport(ack_rsp))) {
				debug("Failed to write to remote");
				return LC_ERROR_WRITE;
			}
			pkt_count = 0;
		}

		if (cb) {
			cb(LC_CB_STAGE_WRITE_CONFIG, cb_count++,
				(int)(wr_ptr - wr), len,
				LC_CB_COUNTER_TYPE_BYTES, cb_arg, NULL);
		}
	}

	cb_count = 0;
	cb(LC_CB_STAGE_FINALIZE_UPDATE, cb_count++, 0, 4,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	/* write end of data stream message */
	const uint8_t end_msg[MH_MAX_PACKET_SIZE] = { 0x7E };
	if ((err = HID_WriteReport(end_msg))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_WRITE;
	}
	debug("end_msg");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_FINALIZE_UPDATE, cb_count++, 1, 4,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	/* write finish config message */
	mh_config_attributes mca;
	if (err = get_mh_config_attributes(xml, xml_size, &mca)) {
		debug("Failed to parse config attributes");
		return LC_ERROR;
	}
	const uint8_t finish_msg[MH_MAX_PACKET_SIZE] = {
		0xFF, 0x06, get_seq(seq), 0x07, 0x01, 0x05, 0x01, 0x01,
		0x80, mca.type[0], mca.type[1], mca.type[2], 0x00,
		0x02, mca.seed[0], mca.seed[1],
		0x04, 0x00, 0x00, 0x00, 0x00, 0x04,
		mca.length[0], mca.length[1], mca.length[2], mca.length[3],
		0x02, mca.expectedvalue[0], mca.expectedvalue[1] };
	if ((err = HID_WriteReport(finish_msg))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("finish_msg");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_FINALIZE_UPDATE, cb_count++, 2, 4,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	/* write msg 5 */
	const uint8_t msg_5[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x05, get_seq(seq), 0x02, 0x01, 0x05, 0x01, 0x00 };
	if ((err = HID_WriteReport(msg_5))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_5");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_FINALIZE_UPDATE, cb_count++, 3, 4,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	/* write msg 7 */
	const uint8_t msg_7[MH_MAX_PACKET_SIZE] =
		{ 0xFF, 0x07, get_seq(seq), 0x01, 0x01, 0x05 };
	if ((err = HID_WriteReport(msg_7))) {
		debug("Failed to write to remote");
		return LC_ERROR_WRITE;
	}
	if ((err = HID_ReadReport(rsp, MH_TIMEOUT))) {
		debug("Failed to read from remote");
		return LC_ERROR_READ;
	}
	debug("msg_7");
	debug_print_packet(rsp);

	cb(LC_CB_STAGE_FINALIZE_UPDATE, cb_count++, 4, 4,
		LC_CB_COUNTER_TYPE_STEPS, cb_arg, NULL);

	return 0;
}
