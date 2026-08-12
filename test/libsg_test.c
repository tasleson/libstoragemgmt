/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * Unit tests for the SCSI response parsing in c_binding/libsg.c.
 *
 * These exercise the code paths that interpret whatever a device chose to
 * send back, without needing a device to send it. Everything here is a pure
 * function of a buffer, so no SG_IO, no daemon and no privileges.
 *
 * libsg.c is included rather than linked because most of what is worth
 * testing here is static to that translation unit.
 */

#include "libsg.c"

#include <check.h>
#include <stdlib.h>

/* Stands in for the uninitialized stack a caller's buffer used to hold past
 * the region the device actually filled.
 */
#define POISON 0xa5

/* SPC-5 rev 07 Table 27 - Sense data response codes */
#define SENSE_FIXED     _T10_SPC_SENSE_REPORT_TYPE_CUR_INFO_FIXED
#define SENSE_FIXED_DEF _T10_SPC_SENSE_REPORT_TYPE_DEF_ERR_FIXED
#define SENSE_DP        _T10_SPC_SENSE_REPORT_TYPE_CUR_INFO_DP
#define SENSE_DP_DEF    _T10_SPC_SENSE_REPORT_TYPE_DEF_ERR_DP

/* SAM-5 rev 21 Table 41 - Status codes */
#define STATUS_BUSY 0x08

static void vpd_hdr_set(uint8_t *buf, uint8_t page_code, uint16_t page_len) {
    buf[0] = 0;
    buf[1] = page_code;
    buf[2] = (page_len >> 8) & UINT8_MAX;
    buf[3] = page_len & UINT8_MAX;
}

/*
 * VPD page 0x83, Device Identification.
 */

/* One well formed 8 byte designation descriptor: CODE SET 1, type NAA,
 * DESIGNATOR LENGTH 4, then four bytes of identifier.
 */
static const uint8_t DESIGNATOR_PATTERN[8] = {0x01, 0x03, 0x00, 0x04,
                                              0xde, 0xad, 0xbe, 0xef};

/*
 * Fill the whole buffer with back to back valid descriptors, aligned so the
 * first one starts right after the 4 byte page header.
 *
 * This is what makes the bounds tests meaningful: stack residue that looks
 * like garbage trips the parser's internal consistency checks by luck, and a
 * test built on it passes even when the bound is wrong. Residue that looks
 * like real descriptors does not, so a parser that walks past the transferred
 * length happily returns success and a pile of invented designators.
 */
static void vpd83_fill_plausible(uint8_t *buf, size_t len) {
    size_t i = 0;

    for (; i < len; ++i)
        buf[i] = DESIGNATOR_PATTERN[(i + sizeof(struct _sg_t10_vpd83_header)) %
                                    sizeof(DESIGNATOR_PATTERN)];
}

START_TEST(test_vpd83_length_fields) {
    static const struct {
        const char *name;
        uint16_t page_len;
        uint16_t data_len;
        int expect_rc;
        uint16_t expect_count;
    } cases[] = {
        /* PAGE LENGTH + sizeof(header) overflows a uint16_t. This used to
         * wrap to a small value and quietly report zero designators. */
        {"page_len wraps on +sizeof(header)", 0xfffc, 252, LSM_ERR_DEVICE_BUG,
         0},
        {"page_len wraps to exactly zero", 0xfffb + 1, 252, LSM_ERR_DEVICE_BUG,
         0},
        /* The device claims far more than it was ever asked for. Walking that
         * far builds designators out of the caller's stack. */
        {"page_len beyond transferred length", 0x1000, 252, LSM_ERR_DEVICE_BUG,
         0},
        {"page_len one byte too long", 249, 252, LSM_ERR_DEVICE_BUG, 0},
        {"page_len exactly fills the transfer", 248, 252, LSM_ERR_OK, 31},
        {"page_len well inside the transfer", 8, 252, LSM_ERR_OK, 1},
        {"empty page", 0, 252, LSM_ERR_OK, 0},
    };
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    char err_msg[_LSM_ERR_MSG_LEN];
    struct _sg_t10_vpd83_dp **dps = NULL;
    uint16_t dp_count = 0;
    size_t i = 0;
    int rc = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        vpd83_fill_plausible(buf, sizeof(buf));
        vpd_hdr_set(buf, _SG_T10_SPC_VPD_DI, cases[i].page_len);
        dps = NULL;
        dp_count = 0;

        rc = _sg_parse_vpd_83(err_msg, buf, cases[i].data_len, &dps, &dp_count);

        /* The exact code matters: a device sending contradictory lengths is
         * LSM_ERR_DEVICE_BUG, not the library's fault. */
        ck_assert_msg(rc == cases[i].expect_rc,
                      "%s: expected rc %d, got %d (%s)", cases[i].name,
                      cases[i].expect_rc, rc, err_msg);
        ck_assert_msg(dp_count == cases[i].expect_count,
                      "%s: expected %" PRIu16 " designators, got %" PRIu16,
                      cases[i].name, cases[i].expect_count, dp_count);
        if (dps != NULL)
            _sg_t10_vpd83_dp_array_free(dps, dp_count);
    }
}
END_TEST

START_TEST(test_vpd83_rejects_short_buffer) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    char err_msg[_LSM_ERR_MSG_LEN];
    struct _sg_t10_vpd83_dp **dps = NULL;
    uint16_t dp_count = 0;

    memset(buf, POISON, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_DI, 0);

    /* Too short to even hold a page header, so we cannot say the device has
     * the page at all. */
    ck_assert_int_eq(_sg_parse_vpd_83(err_msg, buf, 2, &dps, &dp_count),
                     LSM_ERR_NO_SUPPORT);
    ck_assert_ptr_null(dps);
}
END_TEST

START_TEST(test_vpd83_rejects_wrong_page_code) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    char err_msg[_LSM_ERR_MSG_LEN];
    struct _sg_t10_vpd83_dp **dps = NULL;
    uint16_t dp_count = 0;

    /* Some devices answer any VPD query with standard INQUIRY data. */
    memset(buf, 0, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_SUP_VPD_PGS, 8);

    ck_assert_int_eq(_sg_parse_vpd_83(err_msg, buf, 252, &dps, &dp_count),
                     LSM_ERR_NO_SUPPORT);
    ck_assert_ptr_null(dps);
}
END_TEST

START_TEST(test_vpd83_parses_designator) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    char err_msg[_LSM_ERR_MSG_LEN];
    struct _sg_t10_vpd83_dp **dps = NULL;
    uint16_t dp_count = 0;

    memset(buf, POISON, sizeof(buf));
    memset(buf, 0, 16);
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_DI, 8);
    buf[4] = 0x01; /* CODE SET 1 (binary), PROTOCOL IDENTIFIER 0 */
    buf[5] = _SG_T10_SPC_VPD_DI_DESIGNATOR_TYPE_NAA;
    buf[6] = 0x00; /* reserved */
    buf[7] = 0x04; /* DESIGNATOR LENGTH */
    buf[8] = 0xde;
    buf[9] = 0xad;
    buf[10] = 0xbe;
    buf[11] = 0xef;

    ck_assert_int_eq(_sg_parse_vpd_83(err_msg, buf, 252, &dps, &dp_count),
                     LSM_ERR_OK);
    ck_assert_uint_eq(dp_count, 1);
    ck_assert_uint_eq(dps[0]->header.designator_len, 4);
    ck_assert_uint_eq(dps[0]->header.designator_type,
                      _SG_T10_SPC_VPD_DI_DESIGNATOR_TYPE_NAA);
    ck_assert_uint_eq(dps[0]->designator[0], 0xde);
    ck_assert_uint_eq(dps[0]->designator[3], 0xef);

    _sg_t10_vpd83_dp_array_free(dps, dp_count);
}
END_TEST

/*
 * VPD page 0x80, Unit Serial Number.
 */

START_TEST(test_vpd80_length_fields) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    uint8_t serial[256];
    char err_msg[_LSM_ERR_MSG_LEN];

    memset(buf, POISON, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_UNIT_SN, 0x1000);
    ck_assert_int_eq(
        _sg_parse_vpd_80(err_msg, buf, 252, serial, sizeof(serial)),
        LSM_ERR_DEVICE_BUG);

    memset(buf, POISON, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_UNIT_SN, 0);
    ck_assert_int_eq(_sg_parse_vpd_80(err_msg, buf, 3, serial, sizeof(serial)),
                     LSM_ERR_NO_SUPPORT);
}
END_TEST

START_TEST(test_vpd80_parses_serial) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    uint8_t serial[256];
    char err_msg[_LSM_ERR_MSG_LEN];

    memset(buf, POISON, sizeof(buf));
    memset(buf, 0, 16);
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_UNIT_SN, 4);
    memcpy(buf + 4, "ABCD", 4);

    ck_assert_int_eq(
        _sg_parse_vpd_80(err_msg, buf, 252, serial, sizeof(serial)),
        LSM_ERR_OK);
    ck_assert_str_eq((char *)serial, "ABCD");
}
END_TEST

START_TEST(test_vpd80_truncates_to_output_buffer) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];
    uint8_t serial[8];
    char err_msg[_LSM_ERR_MSG_LEN];

    memset(buf, 0, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_UNIT_SN, 32);
    memset(buf + 4, 'X', 32);

    ck_assert_int_eq(
        _sg_parse_vpd_80(err_msg, buf, 252, serial, sizeof(serial)),
        LSM_ERR_OK);
    ck_assert_uint_eq(strlen((char *)serial), sizeof(serial) - 1);
}
END_TEST

/*
 * VPD page 0x00, Supported VPD Pages.
 */

START_TEST(test_vpd00_respects_transferred_length) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];

    /* The device claims a 64KiB list. The 0x89 planted past the 252 bytes we
     * asked for must stay invisible, otherwise a stray byte of stack residue
     * misidentifies the disk as ATA.
     */
    memset(buf, POISON, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_SUP_VPD_PGS, 0xffff);
    memset(buf + 4, 0x00, 248);
    buf[300] = _SG_T10_SPC_VPD_ATA_INFO;

    ck_assert(_sg_is_vpd_page_supported(buf, 252, _SG_T10_SPC_VPD_ATA_INFO) ==
              false);
    /* Same buffer, but this time the page really was transferred in full. */
    ck_assert(_sg_is_vpd_page_supported(buf, 512, _SG_T10_SPC_VPD_ATA_INFO) ==
              true);
}
END_TEST

START_TEST(test_vpd00_page_lookup) {
    uint8_t buf[_SG_T10_SPC_VPD_MAX_LEN];

    memset(buf, POISON, sizeof(buf));
    vpd_hdr_set(buf, _SG_T10_SPC_VPD_SUP_VPD_PGS, 2);
    buf[4] = _SG_T10_SPC_VPD_UNIT_SN;
    buf[5] = _SG_T10_SPC_VPD_ATA_INFO;

    ck_assert(_sg_is_vpd_page_supported(buf, 252, _SG_T10_SPC_VPD_ATA_INFO) ==
              true);
    ck_assert(_sg_is_vpd_page_supported(buf, 252, _SG_T10_SPC_VPD_UNIT_SN) ==
              true);
    ck_assert(_sg_is_vpd_page_supported(buf, 252,
                                        _SG_T10_SBC_VPD_BLK_DEV_CHA) == false);
    /* Nothing but a header came back. */
    ck_assert(_sg_is_vpd_page_supported(buf, 4, _SG_T10_SPC_VPD_ATA_INFO) ==
              false);
}
END_TEST

/*
 * ADDITIONAL SENSE CODE extraction, which depends on the sense data format.
 */

START_TEST(test_sense_asc_by_response_code) {
    static const struct {
        const char *name;
        uint8_t response_code;
        size_t asc_offset;
        bool known;
    } cases[] = {
        {"current fixed", SENSE_FIXED, 12, true},
        {"deferred fixed", SENSE_FIXED_DEF, 12, true},
        {"current descriptor", SENSE_DP, 2, true},
        {"deferred descriptor", SENSE_DP_DEF, 2, true},
        {"all zero sense", 0x00, 0, false},
        {"vendor specific", 0x7f, 0, false},
    };
    uint8_t sense[_T10_SPC_SENSE_DATA_MAX_LENGTH];
    uint8_t asc = 0;
    size_t i = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(sense, POISON, sizeof(sense));
        sense[0] = cases[i].response_code;
        if (cases[i].known)
            sense[cases[i].asc_offset] = _T10_SPC_ASC_IMPENDING_FAILURE;
        asc = 0;

        if (cases[i].known) {
            ck_assert_msg(_sense_data_asc_get(sense, &asc) == 0,
                          "%s: expected the format to be recognised",
                          cases[i].name);
            ck_assert_msg(asc == _T10_SPC_ASC_IMPENDING_FAILURE,
                          "%s: read ASC 0x%02x from the wrong offset",
                          cases[i].name, asc);
        } else {
            ck_assert_msg(_sense_data_asc_get(sense, &asc) != 0,
                          "%s: expected the format to be rejected",
                          cases[i].name);
        }
    }
}
END_TEST

START_TEST(test_sense_asc_descriptor_ignores_fixed_offset) {
    uint8_t sense[_T10_SPC_SENSE_DATA_MAX_LENGTH];
    uint8_t asc = 0xff;

    /* A healthy drive answering in descriptor format, where byte 12 is
     * descriptor payload that happens to hold the impending failure code.
     * Reading it as fixed format condemns a working disk.
     */
    memset(sense, 0, sizeof(sense));
    sense[0] = SENSE_DP;
    sense[2] = 0x00;
    sense[12] = _T10_SPC_ASC_IMPENDING_FAILURE;

    ck_assert_int_eq(_sense_data_asc_get(sense, &asc), 0);
    ck_assert_uint_eq(asc, 0x00);
    ck_assert_int_eq(_sg_info_excep_interpret_asc(asc),
                     LSM_DISK_HEALTH_STATUS_GOOD);
}
END_TEST

START_TEST(test_info_excep_interpret_asc) {
    ck_assert_int_eq(
        _sg_info_excep_interpret_asc(_T10_SPC_ASC_IMPENDING_FAILURE),
        LSM_DISK_HEALTH_STATUS_FAIL);
    ck_assert_int_eq(_sg_info_excep_interpret_asc(_T10_SPC_ASC_WARNING),
                     LSM_DISK_HEALTH_STATUS_WARN);
    ck_assert_int_eq(_sg_info_excep_interpret_asc(0x00),
                     LSM_DISK_HEALTH_STATUS_GOOD);
}
END_TEST

/*
 * Informational Exceptions General log parameter.
 */

START_TEST(test_info_excep_log_validation) {
    static const struct {
        const char *name;
        uint16_t param_code;
        uint8_t param_len;
        uint16_t data_len;
        bool expect_ok;
    } cases[] = {
        {"well formed", 0x0000, 4, 8, true},
        {"minimum usable parameter length", 0x0000, 2, 6, true},
        /* Short page: reading ASC at offset 4 used to hit uninitialized
         * stack, and residue of 0x5d condemns a healthy disk. */
        {"page shorter than the parameter", 0x0000, 4, 1, false},
        {"page one byte short", 0x0000, 4, 5, false},
        /* A drive leading with some other log parameter. */
        {"leading parameter is not the general one", 0x0001, 4, 8, false},
        {"parameter too short for ASC and ASCQ", 0x0000, 1, 8, false},
    };
    uint8_t log_page[_T10_SPC_LOG_SENSE_MAX_LEN];
    char err_msg[_LSM_ERR_MSG_LEN];
    uint8_t asc = 0;
    size_t i = 0;
    int rc = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(log_page, POISON, sizeof(log_page));
        log_page[0] = (cases[i].param_code >> 8) & UINT8_MAX;
        log_page[1] = cases[i].param_code & UINT8_MAX;
        log_page[2] = 0x00; /* parameter control byte */
        log_page[3] = cases[i].param_len;
        log_page[4] = _T10_SPC_ASC_IMPENDING_FAILURE;
        log_page[5] = 0x00;
        asc = 0;

        rc =
            _info_excep_log_asc_get(err_msg, log_page, cases[i].data_len, &asc);

        ck_assert_msg((rc == LSM_ERR_OK) == cases[i].expect_ok,
                      "%s: expected %s, got rc %d (%s)", cases[i].name,
                      cases[i].expect_ok ? "success" : "failure", rc, err_msg);
        if (rc == LSM_ERR_OK)
            ck_assert_msg(asc == _T10_SPC_ASC_IMPENDING_FAILURE,
                          "%s: wrong ASC 0x%02x", cases[i].name, asc);
        else
            ck_assert_msg(asc == 0, "%s: ASC written despite failure",
                          cases[i].name);
    }
}
END_TEST

/*
 * SG_IO completion classification.
 */

START_TEST(test_sg_io_v3_status_get) {
    static const struct {
        const char *name;
        uint8_t status;
        uint16_t host_status;
        uint16_t driver_status;
        uint8_t sb_len_wr;
        bool expect;
    } cases[] = {
        {"clean success", _T10_SAM_STATUS_GOOD, 0, 0, 0, true},
        {"check condition with sense", _T10_SAM_STATUS_CHECK_CONDITION, 0,
         _LINUX_DRIVER_SENSE, 18, true},
        /* DRIVER_SENSE only marks sense data as present, it is not itself a
         * failure. */
        {"driver sense flag alone", _T10_SAM_STATUS_GOOD, 0,
         _LINUX_DRIVER_SENSE, 0, true},
        /* The command never reached the device. Before this was checked, the
         * caller got told it succeeded over a zeroed buffer. */
        {"adapter timeout", _T10_SAM_STATUS_GOOD, 0x03, 0, 0, false},
        {"driver gave up", _T10_SAM_STATUS_GOOD, 0, 0x06, 0, false},
        {"transport failure outranks sense", _T10_SAM_STATUS_CHECK_CONDITION,
         0x03, _LINUX_DRIVER_SENSE, 18, false},
        /* A bad status with nothing to explain it. */
        {"busy without sense", STATUS_BUSY, 0, 0, 0, false},
        {"check condition without sense", _T10_SAM_STATUS_CHECK_CONDITION, 0, 0,
         0, false},
    };
    struct sg_io_hdr io_hdr;
    struct _sg_io_status io_status;
    size_t i = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&io_hdr, 0, sizeof(io_hdr));
        memset(&io_status, 0, sizeof(io_status));
        io_hdr.status = cases[i].status;
        io_hdr.host_status = cases[i].host_status;
        io_hdr.driver_status = cases[i].driver_status;
        io_hdr.sb_len_wr = cases[i].sb_len_wr;

        _sg_io_v3_status_get(&io_hdr, &io_status);

        ck_assert_msg(io_status.completed == cases[i].expect,
                      "%s: expected completed=%s", cases[i].name,
                      cases[i].expect ? "true" : "false");
        /* The raw bytes are the diagnosis for an incomplete command, so they
         * have to survive the trip out. */
        ck_assert_msg(io_status.host_status == cases[i].host_status &&
                          io_status.driver_status == cases[i].driver_status &&
                          io_status.scsi_status == cases[i].status,
                      "%s: status bytes not carried out", cases[i].name);
    }
}
END_TEST

START_TEST(test_sg_io_v4_status_get) {
    static const struct {
        const char *name;
        uint32_t device_status;
        uint32_t transport_status;
        uint32_t driver_status;
        uint32_t response_len;
        bool expect;
    } cases[] = {
        {"clean success", _T10_SAM_STATUS_GOOD, 0, 0, 0, true},
        {"check condition with sense", _T10_SAM_STATUS_CHECK_CONDITION, 0,
         _LINUX_DRIVER_SENSE, 18, true},
        {"driver sense flag alone", _T10_SAM_STATUS_GOOD, 0,
         _LINUX_DRIVER_SENSE, 0, true},
        {"transport gave up", _T10_SAM_STATUS_GOOD, 0x03, 0, 0, false},
        {"busy without sense", STATUS_BUSY, 0, 0, 0, false},
    };
    struct sg_io_v4 io_hdr;
    struct _sg_io_status io_status;
    size_t i = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&io_hdr, 0, sizeof(io_hdr));
        memset(&io_status, 0, sizeof(io_status));
        io_hdr.device_status = cases[i].device_status;
        io_hdr.transport_status = cases[i].transport_status;
        io_hdr.driver_status = cases[i].driver_status;
        io_hdr.response_len = cases[i].response_len;

        _sg_io_v4_status_get(&io_hdr, &io_status);

        ck_assert_msg(io_status.completed == cases[i].expect,
                      "%s: expected completed=%s", cases[i].name,
                      cases[i].expect ? "true" : "false");
        /* v4 keeps them under different names; they land in the same slots. */
        ck_assert_msg(io_status.host_status == cases[i].transport_status &&
                          io_status.scsi_status == cases[i].device_status,
                      "%s: status bytes not carried out", cases[i].name);
    }
}
END_TEST

/*
 * Benign sense data must not be mistaken for a failed command.
 */

START_TEST(test_resolve_sense_by_key) {
    static const struct {
        const char *name;
        uint8_t sense_key;
        int expect;
    } cases[] = {
        /* The command did what was asked, the data is good. */
        {"no sense", _T10_SPC_SENSE_KEY_NO_SENSE, 0},
        {"recovered error", _T10_SPC_SENSE_KEY_RECOVERED_ERROR, 0},
        {"completed", _T10_SPC_SENSE_KEY_COMPLETED, 0},
        /* These really did fail. */
        {"illegal request", _T10_SPC_SENSE_KEY_ILLEGAL_REQUEST, -1},
        {"medium error", 0x03, -1},
        {"hardware error", 0x04, -1},
    };
    uint8_t sense[_T10_SPC_SENSE_DATA_MAX_LENGTH];
    char sense_err_msg[_LSM_ERR_MSG_LEN / 2];
    uint8_t sense_key = 0;
    size_t i = 0;
    int rc = 0;

    for (; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(sense, 0, sizeof(sense));
        memset(sense_err_msg, 0, sizeof(sense_err_msg));
        sense[0] = SENSE_FIXED;
        sense[2] = cases[i].sense_key;
        sense[7] = 10; /* ADDITIONAL SENSE LENGTH */
        sense_key = 0xff;

        rc = _sg_io_resolve_sense(-1, sense, sense_err_msg, &sense_key);

        ck_assert_msg(rc == cases[i].expect, "%s: expected %d, got %d",
                      cases[i].name, cases[i].expect, rc);
        ck_assert_msg(sense_key == cases[i].sense_key,
                      "%s: sense key not reported back", cases[i].name);
    }
}
END_TEST

START_TEST(test_resolve_sense_passes_other_codes_through) {
    uint8_t sense[_T10_SPC_SENSE_DATA_MAX_LENGTH];
    char sense_err_msg[_LSM_ERR_MSG_LEN / 2];
    uint8_t sense_key = 0;

    memset(sense, 0, sizeof(sense));
    memset(sense_err_msg, 0, sizeof(sense_err_msg));

    /* Nothing but the -1 "got sense data" value gets reinterpreted. */
    ck_assert_int_eq(_sg_io_resolve_sense(0, sense, sense_err_msg, &sense_key),
                     0);
    ck_assert_int_eq(_sg_io_resolve_sense(_SG_IO_INCOMPLETE_ERRNO, sense,
                                          sense_err_msg, &sense_key),
                     _SG_IO_INCOMPLETE_ERRNO);
    ck_assert_int_eq(
        _sg_io_resolve_sense(ENOTTY, sense, sense_err_msg, &sense_key), ENOTTY);
}
END_TEST

START_TEST(test_sg_io_err_str) {
    char out[_SG_IO_ERR_STR_LEN];
    struct _sg_io_status io_status;

    memset(&io_status, 0, sizeof(io_status));
    io_status.completed = true;

    /* -1 means "got sense data", not an errno. Handing it to strerror() is how
     * "Unknown error -1" used to end up in user facing messages.
     */
    _sg_io_err_str(-1, &io_status, out);
    ck_assert_ptr_null(strstr(out, "Unknown error"));
    ck_assert_ptr_null(strstr(out, "-1"));

    /* A real errno still gets translated. */
    _sg_io_err_str(ENOTTY, &io_status, out);
    ck_assert_ptr_nonnull(strstr(out, "error 25"));

    /* An incomplete command reports the status bytes, which are the only
     * thing that distinguishes a timeout from a target that went away.
     */
    io_status.completed = false;
    io_status.host_status = 0x03;
    io_status.driver_status = 0x08;
    io_status.scsi_status = 0x00;
    _sg_io_err_str(_SG_IO_INCOMPLETE_ERRNO, &io_status, out);
    ck_assert_ptr_nonnull(strstr(out, "did not complete"));
    ck_assert_ptr_nonnull(strstr(out, "host status 0x03"));
    ck_assert_ptr_nonnull(strstr(out, "driver status 0x08"));
}
END_TEST

static Suite *libsg_suite(void) {
    Suite *s = suite_create("libsg");
    TCase *vpd = tcase_create("vpd");
    TCase *health = tcase_create("health");
    TCase *sg_io = tcase_create("sg_io");

    tcase_add_test(vpd, test_vpd83_length_fields);
    tcase_add_test(vpd, test_vpd83_rejects_short_buffer);
    tcase_add_test(vpd, test_vpd83_rejects_wrong_page_code);
    tcase_add_test(vpd, test_vpd83_parses_designator);
    tcase_add_test(vpd, test_vpd80_length_fields);
    tcase_add_test(vpd, test_vpd80_parses_serial);
    tcase_add_test(vpd, test_vpd80_truncates_to_output_buffer);
    tcase_add_test(vpd, test_vpd00_respects_transferred_length);
    tcase_add_test(vpd, test_vpd00_page_lookup);
    suite_add_tcase(s, vpd);

    tcase_add_test(health, test_sense_asc_by_response_code);
    tcase_add_test(health, test_sense_asc_descriptor_ignores_fixed_offset);
    tcase_add_test(health, test_info_excep_interpret_asc);
    tcase_add_test(health, test_info_excep_log_validation);
    suite_add_tcase(s, health);

    tcase_add_test(sg_io, test_sg_io_v3_status_get);
    tcase_add_test(sg_io, test_sg_io_v4_status_get);
    tcase_add_test(sg_io, test_resolve_sense_by_key);
    tcase_add_test(sg_io, test_resolve_sense_passes_other_codes_through);
    tcase_add_test(sg_io, test_sg_io_err_str);
    suite_add_tcase(s, sg_io);

    return s;
}

int main(void) {
    int number_failed = 0;
    SRunner *sr = srunner_create(libsg_suite());

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
