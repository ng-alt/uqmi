#include "qmi-message.h"

static void cmd_wms_list_messages_cb(struct qmi_dev *qmi, struct qmi_request *req, struct qmi_msg *msg)
{
	struct qmi_wms_list_messages_response res;
	int i, len = 0;

	qmi_parse_wms_list_messages_response(msg, &res);
	blobmsg_alloc_string_buffer(&status, "messages", 1);
	for (i = 0; i < res.data.message_list_n; i++) {
		len += sprintf(blobmsg_realloc_string_buffer(&status, len + 12) + len,
		               " %d" + (len ? 0 : 1),
					   res.data.message_list[i].memory_index);
	}
	blobmsg_add_string_buffer(&status);
}

static enum qmi_cmd_result
cmd_wms_list_messages_prepare(struct qmi_dev *qmi, struct qmi_request *req, struct qmi_msg *msg, char *arg)
{
	static struct qmi_wms_list_messages_request mreq = {
		QMI_INIT(storage_type, QMI_WMS_STORAGE_TYPE_UIM),
		QMI_INIT(message_tag, QMI_WMS_MESSAGE_TAG_TYPE_MT_NOT_READ),
	};

	qmi_set_wms_list_messages_request(msg, &mreq);

	return QMI_CMD_REQUEST;
}

static int
pdu_decode_7bit_str(char *dest, const unsigned char *data, int data_len, int bit_offset)
{
	char *orig_dest = dest;
	int i;

	for (i = 0; i < data_len; i++) {
		int pos = (i + bit_offset) % 7;

		if (pos == 0) {
			*(dest++) = data[i] & 0x7f;
		} else {
			if (i)
				*(dest++) = (data[i - 1] >> (7 + 1 - pos)) |
				            ((data[i] << pos) & 0x7f);

			if (pos == 6)
				*(dest++) = (data[i] >> 1) & 0x7f;
		}
	}
	*dest = 0;
	return dest - orig_dest;
}

static void decode_udh(const unsigned char *data)
{
	const unsigned char *start = data;
	const unsigned char *end;
	unsigned int type, len;

	len = *(data++);
	end = data + len;
	while (data < end) {
		const unsigned char *val;

		type = data[0];
		len = data[1];
		val = &data[2];
		data += 2 + len;
		if (data > end)
			break;

		switch (type) {
		case 0:
			blobmsg_add_u32(&status, "concat_ref", (uint32_t) val[0]);
			blobmsg_add_u32(&status, "concat_part", (uint32_t) val[2] + 1);
			blobmsg_add_u32(&status, "concat_parts", (uint32_t) val[1]);
			break;
		default:
			break;
		}
	}
}

static void decode_7bit_field(char *name, const unsigned char *data, int data_len, bool udh)
{
	bool multipart = false;
	const unsigned char *udh_start;
	char *dest;
	int pos_offset = 0;

	if (udh) {
		int len = data[0] + 1;

		udh_start = data;
		data += len;
		data_len -= len;
		pos_offset = len % 7;
	}

	dest = blobmsg_alloc_string_buffer(&status, name, data_len * 8 / 7 + 2);
	pdu_decode_7bit_str(dest, data, data_len, pos_offset);
	blobmsg_add_string_buffer(&status);

	if (udh)
		decode_udh(udh_start);
}

static char *pdu_add_semioctet(char *str, char val)
{
	*str = '0' + (val & 0xf);
	if (*str <= '9')
		str++;

	*str = '0' + ((val >> 4) & 0xf);
	if (*str <= '9')
		str++;

	return str;
}

static void
pdu_decode_address(char *str, unsigned char *data, int len)
{
	unsigned char toa;

	toa = *(data++);
	switch (toa & 0x70) {
	case 0x50:
		pdu_decode_7bit_str(str, data, len, 0);
		return;
	case 0x10:
		*(str++) = '+';
		/* fall through */
	default:
		while (len--) {
			str = pdu_add_semioctet(str, *data);
			data++;
		}
	}

	*str = 0;
}

static void wms_decode_address(char *str, char *name, unsigned char *data, int len)
{
	str = blobmsg_alloc_string_buffer(&status, name, len * 2 + 2);
	pdu_decode_address(str, data, len);
	blobmsg_add_string_buffer(&status);
}

static void cmd_wms_get_message_cb(struct qmi_dev *qmi, struct qmi_request *req, struct qmi_msg *msg)
{
	struct qmi_wms_raw_read_response res;
	unsigned char *data, *end;
	char *str;
	int i, cur_len;
	bool sent;
	unsigned char first;

	qmi_parse_wms_raw_read_response(msg, &res);
	data = (unsigned char *) res.data.raw_message_data.raw_data;
	end = data + res.data.raw_message_data.raw_data_n;

	cur_len = *(data++);
	if (data + cur_len >= end)
		return;

	if (cur_len) {
		wms_decode_address(str, "smsc", data, cur_len - 1);
		data += cur_len;
	}

	if (data + 3 >= end)
		return;

	first = *(data++);
	sent = (first & 0x3) == 1;
	if (sent)
		data++;

	cur_len = *(data++);
	if (data + cur_len >= end)
		return;

	if (cur_len) {
		cur_len = (cur_len + 1) / 2;
		wms_decode_address(str, sent ? "receiver" : "sender", data, cur_len);
		data += cur_len + 1;
	}

	if (data + 3 >= end)
		return;

	/* Protocol ID */
	if (*(data++) != 0)
		return;

	/* Data Encoding */
	if (*(data++) != 0)
		return;

	if (sent) {
		/* Message validity */
		data++;
	} else {
		if (data + 6 >= end)
			return;

		str = blobmsg_alloc_string_buffer(&status, "timestamp", 32);

		/* year */
		*(str++) = '2';
		*(str++) = '0';
		str = pdu_add_semioctet(str, data[0]);
		/* month */
		*(str++) = '-';
		str = pdu_add_semioctet(str, data[1]);
		/* day */
		*(str++) = '-';
		str = pdu_add_semioctet(str, data[2]);

		/* hour */
		*(str++) = ' ';
		str = pdu_add_semioctet(str, data[3]);
		/* minute */
		*(str++) = ':';
		str = pdu_add_semioctet(str, data[4]);
		/* second */
		*(str++) = ':';
		str = pdu_add_semioctet(str, data[5]);
		*str = 0;

		blobmsg_add_string_buffer(&status);

		data += 7;
	}

	cur_len = *(data++);
	decode_7bit_field("text", data, end - data, !!(first & 0x40));
}

static enum qmi_cmd_result
cmd_wms_get_message_prepare(struct qmi_dev *qmi, struct qmi_request *req, struct qmi_msg *msg, char *arg)
{
	static struct qmi_wms_raw_read_request mreq = {
		QMI_INIT_SEQUENCE(message_memory_storage_id,
			.storage_type = QMI_WMS_STORAGE_TYPE_UIM,
		),
		QMI_INIT(message_mode, QMI_WMS_MESSAGE_MODE_GSM_WCDMA),
	};
	char *err;
	int id;

	id = strtoul(arg, &err, 10);
	if (err && *err) {
		blobmsg_add_string(&status, "error", "Invalid message ID");
		return QMI_CMD_EXIT;
	}

	mreq.data.message_memory_storage_id.memory_index = id;
	qmi_set_wms_raw_read_request(msg, &mreq);

	return QMI_CMD_REQUEST;
}


static void cmd_wms_get_raw_message_cb(struct qmi_dev *qmi, struct qmi_request *req, struct qmi_msg *msg)
{
	struct qmi_wms_raw_read_response res;
	unsigned char *data;
	int i, len = 0;
	char *str;

	qmi_parse_wms_raw_read_response(msg, &res);
	data = (unsigned char *) res.data.raw_message_data.raw_data;
	str = blobmsg_alloc_string_buffer(&status, "data", res.data.raw_message_data.raw_data_n * 3);
	for (i = 0; i < res.data.raw_message_data.raw_data_n; i++) {
		str += sprintf(str, " %02x" + (i ? 0 : 1), data[i]);
	}
	blobmsg_add_string_buffer(&status);
}

#define cmd_wms_get_raw_message_prepare cmd_wms_get_message_prepare