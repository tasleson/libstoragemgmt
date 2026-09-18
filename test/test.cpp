/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2011-2023 Red Hat, Inc.
 */

#include "../c_binding/lsm_ipc.hpp"
#include "libstoragemgmt/libstoragemgmt_error.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

/* check.h's "fail" macro clashes with std::basic_ios::fail(), so it must be
 * included only after all standard library headers are pulled in. */
#include <check.h>

/*
 * Writes a raw, zero-padded header declaring a payload of 'declared_len'
 * bytes, without ever writing a matching payload.  Used to exercise the
 * oversized message guard in Transport::msg_recv() / Ipc::readRequest().
 */
static void send_oversized_header(int fd, unsigned long declared_len) {
    std::ostringstream ss;
    ss << std::setw(Transport::HDR_LEN) << std::setfill('0') << declared_len;
    std::string hdr = ss.str();
    ssize_t written = write(fd, hdr.c_str(), hdr.size());
    ck_assert_int_eq(written, (ssize_t)hdr.size());
}

START_TEST(test_oversized_message_rejected) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* Declare a payload at the overflow guard boundary (matches the
     * 0x80000000 check in Transport::msg_recv). */
    send_oversized_header(fds[1], 0x80000000UL);
    close(fds[1]);

    Ipc ipc(fds[0]);
    bool caught = false;

    try {
        ipc.readRequest();
    } catch (const LsmException &e) {
        caught = true;
        ck_assert_int_eq(e.error_code, (int)LSM_ERR_TRANSPORT_COMMUNICATION);
    }

    ck_assert_msg(caught,
                  "Expected LsmException(LSM_ERR_TRANSPORT_COMMUNICATION) "
                  "for an oversized message");
}
END_TEST

START_TEST(test_undersized_message_accepted) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* One byte under the overflow guard boundary should still attempt to
     * read the (non-existent) payload rather than being rejected outright,
     * so closing the peer results in an EOF, not a size related error. */
    send_oversized_header(fds[1], 0x80000000UL - 1);
    close(fds[1]);

    Ipc ipc(fds[0]);
    bool caught_eof = false;

    try {
        ipc.readRequest();
    } catch (const EOFException &) {
        caught_eof = true;
    } catch (const LsmException &e) {
        ck_assert_msg(false,
                      "Unexpected LsmException(%d) for a message just under "
                      "the size limit",
                      e.error_code);
    }

    ck_assert_msg(caught_eof, "Expected EOFException reading a truncated, "
                              "but not oversized, message");
}
END_TEST

START_TEST(test_recv_timeout_fires) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Ipc ipc(fds[0]);
    ck_assert_int_eq(ipc.recv_timeout(1), 0);

    /* Peer stays connected but never writes; readRequest() must time out
     * rather than block forever (the DoS this guards against). */
    bool caught_timeout = false;
    time_t start = time(NULL);
    try {
        ipc.readRequest();
    } catch (const TimeoutException &) {
        caught_timeout = true;
    }
    time_t elapsed = time(NULL) - start;

    close(fds[1]);

    ck_assert_msg(caught_timeout,
                  "Expected TimeoutException when the peer sends nothing");
    ck_assert_msg(elapsed < 5,
                  "recv_timeout did not fire promptly (elapsed %ld s)",
                  (long)elapsed);
}
END_TEST

START_TEST(test_recv_timeout_partial_header) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* Write fewer bytes than a full header, then stall.  This exercises the
     * MSG_WAITALL partial-read-then-EAGAIN path: it must map to a timeout,
     * not a hang or a misframed message. */
    const char partial[] = "00";
    ssize_t written = write(fds[1], partial, sizeof(partial) - 1);
    ck_assert_int_eq(written, (ssize_t)(sizeof(partial) - 1));
    ck_assert_int_lt((int)(sizeof(partial) - 1), Transport::HDR_LEN);

    Ipc ipc(fds[0]);
    ck_assert_int_eq(ipc.recv_timeout(1), 0);

    bool caught_timeout = false;
    try {
        ipc.readRequest();
    } catch (const TimeoutException &) {
        caught_timeout = true;
    }

    close(fds[1]);

    ck_assert_msg(caught_timeout,
                  "Expected TimeoutException on a stalled partial header");
}
END_TEST

START_TEST(test_recv_timeout_cleared) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* A complete, valid request is buffered by the socketpair, so a
     * single-threaded write-then-read is sufficient. */
    Ipc peer(fds[1]);
    peer.requestSend("systems", Value());

    Ipc ipc(fds[0]);
    /* Arm then clear the timeout; a cleared timeout must not spuriously fire
     * on a message that is actually available. */
    ck_assert_int_eq(ipc.recv_timeout(1), 0);
    ck_assert_int_eq(ipc.recv_timeout(0), 0);

    bool caught = false;
    std::string method;
    try {
        Value req = ipc.readRequest();
        ck_assert_msg(req.isValidRequest(), "Expected a valid request");
        method = req["method"].asString();
    } catch (...) {
        caught = true;
    }

    ck_assert_msg(!caught, "readRequest threw after the timeout was cleared");
    ck_assert_str_eq(method.c_str(), "systems");
}
END_TEST

START_TEST(test_recv_timeout_drip) {
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Ipc ipc(fds[0]);
    ck_assert_int_eq(ipc.recv_timeout(1), 0);

    /* Peer declares a 50 byte payload, then keeps sending a trickle of
     * payload bytes one at a time, each arriving well inside the configured
     * timeout, but never enough to complete the declared payload. The
     * timeout must bound the whole read, not just each individual recv()
     * call, otherwise this drip defeats it and readRequest() never returns. */
    std::thread dripper([&]() {
        const char hdr[] = "0000000050";
        if (write(fds[1], hdr, sizeof(hdr) - 1) != (ssize_t)sizeof(hdr) - 1)
            return;
        for (int i = 0; i < 8; i++) {
            if (write(fds[1], "x", 1) != 1)
                break;
            usleep(150000); /* 150ms, well under the 1s recv_timeout */
        }
    });

    bool caught_timeout = false;
    time_t start = time(NULL);
    try {
        ipc.readRequest();
    } catch (const TimeoutException &) {
        caught_timeout = true;
    }
    time_t elapsed = time(NULL) - start;

    dripper.join();
    close(fds[1]);

    ck_assert_msg(caught_timeout,
                  "Expected TimeoutException despite steady forward progress");
    ck_assert_msg(elapsed < 3,
                  "recv_timeout did not bound the overall read (elapsed %ld s)",
                  (long)elapsed);
}
END_TEST

static Suite *ipc_suite(void) {
    Suite *s = suite_create("ipc");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_oversized_message_rejected);
    tcase_add_test(tc, test_undersized_message_accepted);
    tcase_add_test(tc, test_recv_timeout_fires);
    tcase_add_test(tc, test_recv_timeout_partial_header);
    tcase_add_test(tc, test_recv_timeout_cleared);
    tcase_add_test(tc, test_recv_timeout_drip);
    suite_add_tcase(s, tc);
    return s;
}

int main(void) {
    int number_failed;
    Suite *s = ipc_suite();
    SRunner *sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
