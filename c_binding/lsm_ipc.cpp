/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2011-2023 Red Hat, Inc.
 *
 * Author: Tony Asleson <tasleson@redhat.com>
 */

#include "lsm_ipc.hpp"

#include "libstoragemgmt/libstoragemgmt_plug_interface.h"

#include <algorithm>
#include <errno.h>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <list>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lsm_value_jsmn.hpp"

static std::string zero_pad_num(unsigned int num) {
    std::ostringstream ss;
    ss << std::setw(Transport::HDR_LEN) << std::setfill('0') << num;
    return ss.str();
}

Transport::Transport() : s(-1), recv_timeout_seconds(0) {}

Transport::Transport(int socket_desc)
    : s(socket_desc), recv_timeout_seconds(0) {}

int Transport::msg_send(const std::string &msg, int &error_code) {
    int rc = -1;
    error_code = 0;

    if (msg.size() > 0) {
        ssize_t written = 0;
        // fprintf(stderr, ">>> %s\n", msg.c_str());
        std::string data = zero_pad_num(msg.size()) + msg;
        ssize_t msg_size = data.size();

        while (written < msg_size) {
            ssize_t wrote =
                send(s, data.c_str() + written, (msg_size - written),
                     MSG_NOSIGNAL); // Prevent SIGPIPE on write
            if (wrote != -1) {
                ssize_t t = written;
                if (__builtin_add_overflow(t, wrote, &written)) {
                    error_code = EOVERFLOW;
                    break;
                }
            } else {
                error_code = errno;
                break;
            }
        }

        if ((written == msg_size) && error_code == 0) {
            rc = 0;
        }
    }
    return rc;
}

// SO_RCVTIMEO (set via Transport::recv_timeout) bounds a single recv() call,
// not the time spent reading a whole message. A peer that trickles data in,
// always inside the timeout but slower than we would like, would otherwise
// keep this loop making "forward progress" forever. The caller passes a
// deadline covering the entire message so the configured timeout means what
// it says; NULL leaves the read unbounded.
static std::string string_read(int fd, ssize_t count, int &error_code,
                               const struct timespec *deadline) {
    char buff[4096];
    ssize_t amount_read = 0;
    std::string rc = "";

    error_code = 0;

    while (amount_read < count) {
        ssize_t rd = recv(
            fd, buff, std::min((ssize_t)(sizeof(buff)), (count - amount_read)),
            MSG_WAITALL);
        if (rd > 0) {
            ssize_t t = amount_read;
            if (__builtin_add_overflow(t, rd, &amount_read)) {
                error_code = EOVERFLOW;
                break;
            }
            rc += std::string(buff, rd);

            // Only give up while the read is still short; a message whose
            // last chunk lands on the deadline is complete and usable.
            if (deadline != NULL && amount_read < count) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec > deadline->tv_sec ||
                    (now.tv_sec == deadline->tv_sec &&
                     now.tv_nsec > deadline->tv_nsec)) {
                    error_code =
                        EAGAIN; // Mapped to TimeoutException by callers.
                    break;
                }
            }
        } else if (rd == 0) {
            throw EOFException("");
        } else {
            error_code = errno;
            break;
        }
    }

    return rc;
}

std::string Transport::msg_recv(int &error_code) {
    std::string msg;
    error_code = 0;
    unsigned long int payload_len = 0;

    // One deadline spans the header and the payload; giving each read its own
    // would hand a slow peer the configured timeout twice over.
    struct timespec deadline;
    const struct timespec *dl = NULL;
    if (recv_timeout_seconds > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += recv_timeout_seconds;
        dl = &deadline;
    }

    std::string len = string_read(s, HDR_LEN, error_code, dl); // Read length
    if (len.size() && error_code == 0) {
        payload_len = strtoul(len.c_str(), NULL, 10);
        if (payload_len < 0x80000000) { /* Should be big enough */
            ssize_t len = payload_len;
            msg = string_read(s, len, error_code, dl);
        } else {
            error_code = EOVERFLOW;
        }
        // fprintf(stderr, "<<< %s\n", msg.c_str());
    }
    return msg;
}

int Transport::socket_get(const std::string &path, int &error_code) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    int rc = -1;
    error_code = 0;

    if (sfd != -1) {
        struct sockaddr_un addr;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        // Connect
        rc = connect(sfd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc != 0) {
            error_code = errno;
            rc = -1; // Redundant, connect should set to -1 on error
            ::close(sfd);
        } else {
            rc = sfd; // We are good to go.
        }
    }
    return rc;
}

int Transport::recv_timeout(int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;

    if (-1 == setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
        return errno;
    }
    recv_timeout_seconds = seconds;
    return 0;
}

Transport::~Transport() { close(); }

void Transport::close() {
    if (s >= 0) {
        ::close(s);
        // Regardless, clear out socket
        s = -1;
    }
}

EOFException::EOFException(std::string m) : std::runtime_error(m) {}

TimeoutException::TimeoutException(std::string m) : std::runtime_error(m) {}

ValueException::ValueException(std::string m) : std::runtime_error(m) {}

LsmException::LsmException(int code, std::string &msg)
    : std::runtime_error(msg), error_code(code) {}

LsmException::LsmException(int code, std::string &msg,
                           const std::string &debug_addl)
    : std::runtime_error(msg), error_code(code), debug(debug_addl) {}

LsmException::~LsmException() throw() {}

LsmException::LsmException(int code, std::string &msg,
                           const std::string &debug_addl,
                           const std::string &debug_data_addl)
    : std::runtime_error(msg), error_code(code), debug(debug_addl),
      debug_data(debug_data_addl) {}

Ipc::Ipc() {}

Ipc::Ipc(int fd) : t(fd) {}

Ipc::Ipc(std::string socket_path) {
    int e = 0;
    int fd = Transport::socket_get(socket_path, e);
    if (fd >= 0) {
        t = Transport(fd);
    }
}

Ipc::~Ipc() { t.close(); }

void Ipc::requestSend(const std::string request, const Value &params,
                      int32_t id) {
    int rc = 0;
    int ec = 0;
    std::map<std::string, Value> v;

    v["method"] = Value(request);
    v["id"] = Value(id);
    v["params"] = params;

    Value req(v);
    rc = t.msg_send(Payload::serialize(req), ec);

    if (rc != 0) {
        std::string em =
            std::string("Error sending message: errno ") + ::to_string(ec);
        throw LsmException((int)LSM_ERR_TRANSPORT_COMMUNICATION, em);
    }
}

void Ipc::errorSend(int error_code, std::string msg, std::string debug,
                    uint32_t id) {
    int ec = 0;
    int rc = 0;
    std::map<std::string, Value> v;
    std::map<std::string, Value> error_data;

    error_data["code"] = Value(error_code);
    error_data["message"] = Value(msg);
    error_data["data"] = Value(debug);

    v["error"] = Value(error_data);
    v["id"] = Value(id);

    Value e(v);
    rc = t.msg_send(Payload::serialize(e), ec);

    if (rc != 0) {
        std::string em = std::string("Error sending error message: errno ") +
                         ::to_string(ec);
        throw LsmException((int)LSM_ERR_TRANSPORT_COMMUNICATION, em);
    }
}

int Ipc::recv_timeout(int seconds) { return t.recv_timeout(seconds); }

Value Ipc::readRequest(void) {
    int ec;
    std::string resp = t.msg_recv(ec);
    if (ec != 0) {
        if (ec == EAGAIN || ec == EWOULDBLOCK) {
            throw TimeoutException("Timed out reading message");
        }
        std::string em =
            std::string("Error reading message: errno ") + ::to_string(ec);
        throw LsmException((int)LSM_ERR_TRANSPORT_COMMUNICATION, em);
    }
    return Payload::deserialize(resp);
}

void Ipc::responseSend(const Value &response, uint32_t id) {
    int rc;
    int ec;
    std::map<std::string, Value> v;

    v["id"] = id;
    v["result"] = response;

    Value resp(v);
    rc = t.msg_send(Payload::serialize(resp), ec);

    if (rc != 0) {
        std::string em =
            std::string("Error sending response: errno ") + ::to_string(ec);
        throw LsmException((int)LSM_ERR_TRANSPORT_COMMUNICATION, em);
    }
}

Value Ipc::responseRead() {
    Value r = readRequest();
    if (r.hasKey(std::string("result"))) {
        return r.getValue("result");
    } else {
        std::map<std::string, Value> rp = r.asObject();
        std::map<std::string, Value> error = rp["error"].asObject();

        std::string msg = error["message"].asString();
        std::string data = error["data"].asString();
        throw LsmException((int)(error["code"].asInt32_t()), msg, data);
    }
}

Value Ipc::rpc(const std::string &request, const Value &params, int32_t id) {
    requestSend(request, params, id);
    return responseRead();
}
