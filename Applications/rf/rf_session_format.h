#ifndef RF_SESSION_FORMAT_H
#define RF_SESSION_FORMAT_H

/* On-disk format for files under /rf/sessions/ (e.g. /rf/sessions/foo.rflog). */

#define RF_SESSION_MAGIC "RFLOGv1\n"

enum rf_session_record_type {
	RF_REC_CONFIG = 1,
	RF_REC_SWEEP,
	RF_REC_PACKET,
	RF_REC_ANNOTATION,
	RF_REC_EVENT,
};

#endif
