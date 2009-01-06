/* $Id$
 *
 * UBX driver
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>

#include "gpsd_config.h"
#include "gpsd.h"
#if defined(UBX_ENABLE) && defined(BINARY_ENABLE)
#include "ubx.h"

#include "bits.h"

/*
 * A ubx packet looks like this:
 * leader: 0xb5 0x62
 * message class: 1 byte
 * message type: 1 byte
 * length of payload: 2 bytes
 * payload: variable length
 * checksum: 2 bytes
 *
 * see also the FV25 and UBX documents on reference.html
 */

static bool have_port_configuration = false;
static unsigned char original_port_settings[20];
static unsigned char sbas_in_use;

	bool 		ubx_write(int fd, unsigned int msg_class, unsigned int msg_id, unsigned char *msg, unsigned short data_len);
	gps_mask_t 	ubx_parse(struct gps_device_t *session, unsigned char *buf, size_t len);
	void 		ubx_catch_model(struct gps_device_t *session, unsigned char *buf, size_t len);
static	gps_mask_t 	ubx_msg_nav_sol(struct gps_device_t *session, unsigned char *buf, size_t data_len);
static	gps_mask_t 	ubx_msg_nav_dop(struct gps_device_t *session, unsigned char *buf, size_t data_len);
static	gps_mask_t 	ubx_msg_nav_timegps(struct gps_device_t *session, unsigned char *buf, size_t data_len);
static	gps_mask_t 	ubx_msg_nav_svinfo(struct gps_device_t *session, unsigned char *buf, size_t data_len);
static	void		ubx_msg_sbas(unsigned char *buf);
static	void       	ubx_msg_inf(unsigned char *buf, size_t data_len);

/**
 * Navigation solution message
 */
static gps_mask_t
ubx_msg_nav_sol(struct gps_device_t *session, unsigned char *buf, size_t data_len)
{
    unsigned short gw;
    unsigned int tow, flags;
    double epx, epy, epz, evx, evy, evz;
    unsigned char navmode;
    gps_mask_t mask;
    double t;

    if (data_len != 52)
	return 0;

    flags = (unsigned int)getub(buf, 11);
    mask =  ONLINE_SET;
    if ((flags & (UBX_SOL_VALID_WEEK |UBX_SOL_VALID_TIME)) != 0){
	tow = getleul(buf, 0);
	gw = (unsigned short)getlesw(buf, 8);
	session->driver.ubx.gps_week = gw;

	t = gpstime_to_unix((int)session->driver.ubx.gps_week, tow/1000.0) - session->context->leap_seconds;
	session->gpsdata.sentence_time = t;
	session->gpsdata.fix.time = t;
	mask |= TIME_SET;
#ifdef NTPSHM_ENABLE
	/* TODO overhead */
	if (session->context->enable_ntpshm)
	    (void)ntpshm_put(session, session->gpsdata.sentence_time);
#endif
    }

    epx = (double)(getlesl(buf, 12)/100.0);
    epy = (double)(getlesl(buf, 16)/100.0);
    epz = (double)(getlesl(buf, 20)/100.0);
    evx = (double)(getlesl(buf, 28)/100.0);
    evy = (double)(getlesl(buf, 32)/100.0);
    evz = (double)(getlesl(buf, 36)/100.0);
    ecef_to_wgs84fix(&session->gpsdata, epx, epy, epz, evx, evy, evz);
    mask |= LATLON_SET | ALTITUDE_SET | SPEED_SET | TRACK_SET | CLIMB_SET;
    session->gpsdata.fix.eph = (double)(getlesl(buf, 24)/100.0);
    session->gpsdata.fix.eps = (double)(getlesl(buf, 40)/100.0);
    session->gpsdata.pdop = (double)(getleuw(buf, 44)/100.0);
    session->gpsdata.satellites_used = (int)getub(buf, 47);
    mask |= PDOP_SET ;

    navmode = getub(buf, 10);
    switch (navmode){
    case UBX_MODE_TMONLY:
    case UBX_MODE_3D:
	session->gpsdata.fix.mode = MODE_3D;
	break;
    case UBX_MODE_2D:
    case UBX_MODE_DR:	    /* consider this too as 2D */
    case UBX_MODE_GPSDR:    /* XXX DR-aided GPS may be valid 3D */
	session->gpsdata.fix.mode = MODE_2D;
	break;
    default:
	session->gpsdata.fix.mode = MODE_NO_FIX;
    }

    if ((flags & UBX_SOL_FLAG_DGPS) != 0)
	session->gpsdata.status = STATUS_DGPS_FIX;
    else if (session->gpsdata.fix.mode != MODE_NO_FIX)
	session->gpsdata.status = STATUS_FIX;

    mask |= MODE_SET | STATUS_SET | CYCLE_START_SET | USED_SET ;

    return mask;
}

/**
 * Dilution of precision message
 */
static gps_mask_t
ubx_msg_nav_dop(struct gps_device_t *session, unsigned char *buf, size_t data_len)
{
    if (data_len != 18)
	return 0;

    session->gpsdata.gdop = (double)(getleuw(buf, 4)/100.0);
    session->gpsdata.pdop = (double)(getleuw(buf, 6)/100.0);
    session->gpsdata.tdop = (double)(getleuw(buf, 8)/100.0);
    session->gpsdata.vdop = (double)(getleuw(buf, 10)/100.0);
    session->gpsdata.hdop = (double)(getleuw(buf, 12)/100.0);

    return DOP_SET;
}

/**
 * GPS Leap Seconds
 */
static gps_mask_t
ubx_msg_nav_timegps(struct gps_device_t *session, unsigned char *buf, size_t data_len)
{
    unsigned int gw, tow, flags;
    double t;

    if (data_len != 16)
	return 0;

    tow = getleul(buf, 0);
    gw = (uint)getlesw(buf, 8);
    if (gw > session->driver.ubx.gps_week)
	session->driver.ubx.gps_week = gw;

    flags = (unsigned int)getub(buf, 11);
    if ((flags & 0x7) != 0)
	session->context->leap_seconds = (int)getub(buf, 10);

    t = gpstime_to_unix((int)session->driver.ubx.gps_week, tow/1000.0) - session->context->leap_seconds;
    session->gpsdata.sentence_time = session->gpsdata.fix.time = t;

    return TIME_SET | ONLINE_SET;
}

/**
 * GPS Satellite Info
 */
static gps_mask_t
ubx_msg_nav_svinfo(struct gps_device_t *session, unsigned char *buf, size_t data_len)
{
    unsigned int i, j, tow, nchan, nsv, st;

    if (data_len < 152 ) {
	gpsd_report(LOG_PROG, "runt svinfo (datalen=%zd)\n", data_len);
	return 0;
    }
    tow = getleul(buf, 0);
//    session->gpsdata.sentence_time = gpstime_to_unix(gps_week, tow) 
//				- session->context->leap_seconds;
    /*@ +charint @*/
    nchan = getub(buf, 4);
    if (nchan > MAXCHANNELS){
	gpsd_report(LOG_WARN, "Invalid NAV SVINFO message, >%d reported",MAXCHANNELS);
	return 0;
    }
    /*@ -charint @*/
    gpsd_zero_satellites(&session->gpsdata);
    nsv = 0;
    for (i = j = st = 0; i < nchan; i++) {
	unsigned int off = 8 + 12 * i;
	if((int)getub(buf, off+4) == 0) continue; /* LEA-5H seems to have a bug reporting sats it does not see or hear*/
	session->gpsdata.PRN[j]		= (int)getub(buf, off+1);
	session->gpsdata.ss[j]		= (int)getub(buf, off+4);
	session->gpsdata.elevation[j]	= (int)getsb(buf, off+5);
	session->gpsdata.azimuth[j]	= (int)getlesw(buf, off+6);
	if(session->gpsdata.PRN[j])
		st++;
	/*@ -predboolothers */
	if (getub(buf, off+2) & 0x01)
	    session->gpsdata.used[nsv++] = session->gpsdata.PRN[j];
	if (session->gpsdata.PRN[j] == (int)sbas_in_use)
	    session->gpsdata.used[nsv++] = session->gpsdata.PRN[j];
	/*@ +predboolothers */
	j++;
    }
    session->gpsdata.satellites = (int)st;
    session->gpsdata.satellites_used = (int)nsv;
    return SATELLITE_SET | USED_SET;
}

/*
 * SBAS Info
 */
static void
ubx_msg_sbas(unsigned char *buf)
{
#ifdef UBX_SBAS_DEBUG
    unsigned int i, nsv;

    gpsd_report(LOG_WARN, "SBAS: %d %d %d %d %d\n",
		(int)getub(buf, 4), (int)getub(buf, 5), (int)getub(buf, 6), (int)getub(buf, 7), (int)getub(buf, 8));

    nsv = (int)getub(buf, 8);
    for (i = 0; i < nsv; i++) {
	int off = 12 + 12 * i;
	gpsd_report(LOG_WARN, "SBAS info on SV: %d\n", (int)getub(buf, off));
    }
#endif
/* really 'in_use' depends on the sats info, EGNOS is still in test */
/* In WAAS areas one might also check for the type of corrections indicated */
    sbas_in_use = getub(buf, 4);
}

static void
ubx_msg_inf(unsigned char *buf, size_t data_len)
{
    unsigned short msgid;
    static char txtbuf[MAX_PACKET_LENGTH];

    msgid = (unsigned short)((buf[2] << 8) | buf[3]);
    if (data_len > MAX_PACKET_LENGTH-1)
	data_len = MAX_PACKET_LENGTH-1;

    (void)strlcpy(txtbuf, (char *)buf+6, MAX_PACKET_LENGTH); txtbuf[data_len] = '\0';
    switch (msgid) {
	case UBX_INF_DEBUG:
	    gpsd_report(LOG_PROG, "UBX_INF_DEBUG: %s\n", txtbuf);
	    break;
	case UBX_INF_TEST:
	    gpsd_report(LOG_PROG, "UBX_INF_TEST: %s\n", txtbuf);
	    break;
	case UBX_INF_NOTICE:
	    gpsd_report(LOG_INF, "UBX_INF_NOTICE: %s\n", txtbuf);
	    break;
	case UBX_INF_WARNING:
	    gpsd_report(LOG_WARN, "UBX_INF_WARNING: %s\n", txtbuf);
	    break;
	case UBX_INF_ERROR:
	    gpsd_report(LOG_WARN, "UBX_INF_ERROR: %s\n", txtbuf);
	    break;
	default:
	    break;
    }
    return ;
}

/*@ +charint @*/
gps_mask_t ubx_parse(struct gps_device_t *session, unsigned char *buf, size_t len)
{
    size_t data_len;
    unsigned short msgid;
    gps_mask_t mask = 0;
    int i;

    if (len < 6)    /* the packet at least contains a head of six bytes */
	return 0;

    /* extract message id and length */
    msgid = (buf[2] << 8) | buf[3];
    data_len = (size_t)getlesw(buf, 4);
    switch (msgid)
    {
	case UBX_NAV_POSECEF:
	    gpsd_report(LOG_IO, "UBX_NAV_POSECEF\n");
	    break;
	case UBX_NAV_POSLLH:
	    gpsd_report(LOG_IO, "UBX_NAV_POSLLH\n");
	    break;
	case UBX_NAV_STATUS:
	    gpsd_report(LOG_IO, "UBX_NAV_STATUS\n");
	    break;
	case UBX_NAV_DOP:
	    gpsd_report(LOG_PROG, "UBX_NAV_DOP\n");
	    mask = ubx_msg_nav_dop(session, &buf[6], data_len);
	    break;
	case UBX_NAV_SOL:
	    gpsd_report(LOG_PROG, "UBX_NAV_SOL\n");
	    mask = ubx_msg_nav_sol(session, &buf[6], data_len);
	    break;
	case UBX_NAV_POSUTM:
	    gpsd_report(LOG_IO, "UBX_NAV_POSUTM\n");
	    break;
	case UBX_NAV_VELECEF:
	    gpsd_report(LOG_IO, "UBX_NAV_VELECEF\n");
	    break;
	case UBX_NAV_VELNED:
	    gpsd_report(LOG_IO, "UBX_NAV_VELNED\n");
	    break;
	case UBX_NAV_TIMEGPS:
	    gpsd_report(LOG_PROG, "UBX_NAV_TIMEGPS\n");
	    mask = ubx_msg_nav_timegps(session, &buf[6], data_len);
	    break;
	case UBX_NAV_TIMEUTC:
	    gpsd_report(LOG_IO, "UBX_NAV_TIMEUTC\n");
	    break;
	case UBX_NAV_CLOCK:
	    gpsd_report(LOG_IO, "UBX_NAV_CLOCK\n");
	    break;
	case UBX_NAV_SVINFO:
	    gpsd_report(LOG_PROG, "UBX_NAV_SVINFO\n");
	    mask = ubx_msg_nav_svinfo(session, &buf[6], data_len);
	    break;
	case UBX_NAV_DGPS:
	    gpsd_report(LOG_IO, "UBX_NAV_DGPS\n");
	    break;
	case UBX_NAV_SBAS:
	    gpsd_report(LOG_IO, "UBX_NAV_SBAS\n");
	    ubx_msg_sbas(&buf[6]);
	    break;
	case UBX_NAV_EKFSTATUS:
	    gpsd_report(LOG_IO, "UBX_NAV_EKFSTATUS\n");
	    break;

	case UBX_RXM_RAW:
	    gpsd_report(LOG_IO, "UBX_RXM_RAW\n");
	    break;
	case UBX_RXM_SFRB:
	    gpsd_report(LOG_IO, "UBX_RXM_SFRB\n");
	    break;
	case UBX_RXM_SVSI:
	    gpsd_report(LOG_PROG, "UBX_RXM_SVSI\n");
	    break;
	case UBX_RXM_ALM:
	    gpsd_report(LOG_IO, "UBX_RXM_ALM\n");
	    break;
	case UBX_RXM_EPH:
	    gpsd_report(LOG_IO, "UBX_RXM_EPH\n");
	    break;
	case UBX_RXM_POSREQ:
	    gpsd_report(LOG_IO, "UBX_RXM_POSREQ\n");
	    break;

	case UBX_MON_SCHED:
	    gpsd_report(LOG_IO, "UBX_MON_SCHED\n");
	    break;
	case UBX_MON_IO:
	    gpsd_report(LOG_IO, "UBX_MON_IO\n");
	    break;
	case UBX_MON_IPC:
	    gpsd_report(LOG_IO, "UBX_MON_IPC\n");
	    break;
	case UBX_MON_VER:
	    gpsd_report(LOG_IO, "UBX_MON_VER\n");
	    break;
	case UBX_MON_EXCEPT:
	    gpsd_report(LOG_IO, "UBX_MON_EXCEPT\n");
	    break;
	case UBX_MON_MSGPP:
	    gpsd_report(LOG_IO, "UBX_MON_MSGPP\n");
	    break;
	case UBX_MON_RXBUF:
	    gpsd_report(LOG_IO, "UBX_MON_RXBUF\n");
	    break;
	case UBX_MON_TXBUF:
	    gpsd_report(LOG_IO, "UBX_MON_TXBUF\n");
	    break;
	case UBX_MON_HW:
	    gpsd_report(LOG_IO, "UBX_MON_HW\n");
	    break;
	case UBX_MON_USB:
	    gpsd_report(LOG_IO, "UBX_MON_USB\n");
	    break;

	case UBX_INF_DEBUG:
	    /* FALLTHROUGH */
	case UBX_INF_TEST:
	    /* FALLTHROUGH */
	case UBX_INF_NOTICE:
	    /* FALLTHROUGH */
	case UBX_INF_WARNING:
	    /* FALLTHROUGH */
	case UBX_INF_ERROR:
	    ubx_msg_inf(buf, data_len);
	    break;

	case UBX_TIM_TP:
	    gpsd_report(LOG_IO, "UBX_TIM_TP\n");
	    break;
	case UBX_TIM_TM:
	    gpsd_report(LOG_IO, "UBX_TIM_TM\n");
	    break;
	case UBX_TIM_TM2:
	    gpsd_report(LOG_IO, "UBX_TIM_TM2\n");
	    break;
	case UBX_TIM_SVIN:
	    gpsd_report(LOG_IO, "UBX_TIM_SVIN\n");
	    break;

	case UBX_CFG_PRT:
	    gpsd_report(LOG_IO, "UBX_CFG_PRT\n");
	    for(i=6;i<26;i++)
		original_port_settings[i-6] = buf[i];				/* copy the original port settings */
	    buf[14+6] &= ~0x02;							/* turn off NMEA output on this port */
	    (void)ubx_write(session->gpsdata.gps_fd, 0x06, 0x00, &buf[6], 20);	/* send back with all other settings intact */
	    have_port_configuration = true;
	    break;

	case UBX_ACK_NAK:
	    gpsd_report(LOG_IO, "UBX_ACK_NAK, class: %02x, id: %02x\n", buf[6], buf[7]);
	    break;
	case UBX_ACK_ACK:
	    gpsd_report(LOG_IO, "UBX_ACK_ACK, class: %02x, id: %02x\n", buf[6], buf[7]);
	    break;

    default:
	gpsd_report(LOG_WARN,
	    "UBX: unknown packet id 0x%04hx (length %zd) %s\n",
	    msgid, len, gpsd_hexdump_wrapper(buf, len, LOG_WARN));
    }

    if (mask)
	(void)snprintf(session->gpsdata.tag, sizeof(session->gpsdata.tag),
	   "0x%04hx", msgid);

    return mask | ONLINE_SET;
}
/*@ -charint @*/

static gps_mask_t parse_input(struct gps_device_t *session)
{
    gps_mask_t st;

    if (session->packet.type == UBX_PACKET){
	st = ubx_parse(session, session->packet.outbuffer, session->packet.outbuflen);
	session->gpsdata.driver_mode = 1;
	return st;
#ifdef NMEA_ENABLE
    } else if (session->packet.type == NMEA_PACKET) {
	st = nmea_parse((char *)session->packet.outbuffer, session);
	session->gpsdata.driver_mode = 0;
	return st;
#endif /* NMEA_ENABLE */
    } else
	return 0;
}

void ubx_catch_model(struct gps_device_t *session, unsigned char *buf, size_t len)
{
    /*@ +charint */
    unsigned char *ip = &buf[19];
    unsigned char *op = (unsigned char *)session->subtype;
    size_t end = ((len - 19) < 63)?(len - 19):63;
    size_t i;

    for(i=0;i<end;i++) {
	if((*ip == 0x00) || (*ip == '*')) {
	    *op = 0x00;
	    break;
	}
	*(op++) = *(ip++);
    }
    /*@ -charint */
}

bool ubx_write(int fd, unsigned int msg_class, unsigned int msg_id, unsigned char *msg, unsigned short data_len) {
   unsigned char CK_A, CK_B;
   unsigned char head_tail[8];
   ssize_t i, count;
   bool      ok;

   /*@ -type @*/
   head_tail[0] = 0xb5;
   head_tail[1] = 0x62;

   /* calculate CRC */
   CK_A = CK_B = 0;
   head_tail[2] = msg_class;
   head_tail[3] = msg_id;
   head_tail[4] = data_len & 0xff;
   head_tail[5] = (data_len >> 8) & 0xff;

   for (i = 2; i < 6; i++) {
	CK_A += head_tail[i];
	CK_B += CK_A;
   }

   /*@ -nullderef @*/
   for (i = 0; i < data_len; i++) {
	CK_A += msg[i];
	CK_B += CK_A;
   }

   head_tail[6] = CK_A;
   head_tail[7] = CK_B;
   /*@ +type @*/

   gpsd_report(LOG_IO,
       "=> GPS: UBX class: %02x, id: %02x, len: %d, data:%s, crc: %02x%02x\n",
       msg_class, msg_id, data_len,
	       gpsd_hexdump_wrapper(msg, (size_t)data_len, LOG_IO),
       CK_A, CK_B);

   assert(msg != NULL || data_len == 0);
   count = write(fd, head_tail, 6);
   (void)tcdrain(fd);
   if(data_len)
       /*@ -nullpass @*/
       count += write(fd, msg, (size_t)data_len);
       /*@ +nullpass @*/
   (void)tcdrain(fd);
   count += write(fd, &head_tail[6], 2);

   ok = (count == ((ssize_t)data_len + 8));
   (void)tcdrain(fd);
   /*@ +nullderef @*/
   return(0);
}

#ifdef ALLOW_RECONFIGURE
static void ubx_configure(struct gps_device_t *session, unsigned int seq)
{
    unsigned char msg[32];

    gpsd_report(LOG_IO, "UBX configure: %d\n",seq);

    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x00, NULL, 0);	/* get this port's settings */

    /*@ -type @*/
    msg[0] = 0x03; /* SBAS mode enabled, accept testbed mode */
    msg[1] = 0x07; /* SBAS usage: range, differential corrections and integrity */
    msg[2] = 0x03; /* use the maximun search range: 3 channels */
    msg[3] = 0x00; /* PRN numbers to search for all set to 0 => auto scan */
    msg[4] = 0x00;
    msg[5] = 0x00;
    msg[6] = 0x00;
    msg[7] = 0x00;
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x16, msg, 8);

    msg[0] = 0x01; /* class */
    msg[1] = 0x04; /* msg id  = UBX_NAV_DOP */
    msg[2] = 0x01; /* rate */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x01, msg, 3);
    msg[0] = 0x01; /* class */
    msg[1] = 0x06; /* msg id  = NAV-SOL */
    msg[2] = 0x01; /* rate */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x01, msg, 3);
    msg[0] = 0x01; /* class */
    msg[1] = 0x20; /* msg id  = UBX_NAV_TIMEGPS */
    msg[2] = 0x01; /* rate */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x01, msg, 3);
    msg[0] = 0x01; /* class */
    msg[1] = 0x30; /* msg id  = NAV-SVINFO */
    msg[2] = 0x0a; /* rate */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x01, msg, 3);
    msg[0] = 0x01; /* class */
    msg[1] = 0x32; /* msg id  = NAV-SBAS */
    msg[2] = 0x0a; /* rate */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x01, msg, 3);
    /*@ +type @*/

}

static void ubx_revert(struct gps_device_t *session)
{
    /*@ -type @*/
    unsigned char msg[4] = {
	0x00, 0x00,	/* hotstart */
	0x01,		/* controlled software reset */
	0x00};		/* reserved */
    /*@ +type @*/

    gpsd_report(LOG_IO, "UBX revert\n");

    /* Reverting all in one fast and reliable reset */
    (void)ubx_write(session->gpsdata.gps_fd, 0x06, 0x04, msg, 4); /* CFG-RST */
}
#endif /* ALLOW_RECONFIGURE */

static void ubx_nmea_mode(struct gps_device_t *session, int mode)
{
    int i;
    unsigned char buf[20];

    if(!have_port_configuration)
	return;

    /*@ +charint -usedef @*/
    for(i=0;i<22;i++)
	buf[i] = original_port_settings[i];	/* copy the original port settings */
    if(buf[0] == 0x01)				/* set baudrate on serial port only */
	putlelong(buf, 8, session->gpsdata.baudrate);

    if (mode == 0) {
	buf[14] &= ~0x01;			/* turn off UBX output on this port */
	buf[14] |=  0x02;			/* turn on NMEA output on this port */
    } else {
	buf[14] &= ~0x02;			/* turn off NMEA output on this port */
	buf[14] |=  0x01;			/* turn on UBX output on this port */
    }
    /*@ -charint +usedef @*/
    (void)ubx_write(session->gpsdata.gps_fd, 0x06u, 0x00, &buf[6], 20);	/* send back with all other settings intact */
}

static bool ubx_speed(struct gps_device_t *session, speed_t speed)
{
    int i;
    unsigned char buf[20];

    /*@ +charint -usedef -compdef */
    if((!have_port_configuration) || (buf[0] != 0x01))	/* set baudrate on serial port only */
	return false;

    for(i=0;i<22;i++)
	buf[i] = original_port_settings[i];	/* copy the original port settings */
    putlelong(buf, 8, speed);
    (void)ubx_write(session->gpsdata.gps_fd, 0x06, 0x00, &buf[6], 20);	/* send back with all other settings intact */
    /*@ -charint +usedef +compdef */
    return true;
}

/* This is everything we export */
struct gps_type_t ubx_binary = {
    .type_name        = "uBlox UBX",    /* Full name of type */
    .trigger          = NULL,           /* Response string that identifies device (not active) */
    .channels         = 50,             /* Number of satellite channels supported by the device */
    .control_send     = NULL,		/* no control sender yet */
    .probe_detect     = NULL,           /* Startup-time device detector */
    .probe_wakeup     = NULL,           /* Wakeup to be done before each baud hunt */
    .probe_subtype    = NULL,           /* Initialize the device and get subtype */
#ifdef ALLOW_RECONFIGURE
    .configurator     = ubx_configure,  /* Enable what reports we need */
#endif /* ALLOW_RECONFIGURE */
    .get_packet       = generic_get,    /* Packet getter (using default routine) */
    .parse_packet     = parse_input,    /* Parse message packets */
    .rtcm_writer      = NULL,           /* RTCM handler (using default routine) */
    .speed_switcher   = ubx_speed,      /* Speed (baudrate) switch */
    .mode_switcher    = ubx_nmea_mode,  /* Switch to NMEA mode */
    .rate_switcher    = NULL,           /* Message delivery rate switcher */
    .cycle_chars      = -1,             /* Number of chars per report cycle */
#ifdef ALLOW_RECONFIGURE
    .revert           = ubx_revert,     /* Undo the actions of .configurator */
#endif /* ALLOW_RECONFIGURE */
    .wrapup           = NULL,           /* Puts device back to original settings */
    .cycle            = 1               /* Number of updates per second */
};
#endif /* defined(UBX_ENABLE) && defined(BINARY_ENABLE) */
