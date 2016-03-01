// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_MSG_DPDKSTACK_H
#define CEPH_MSG_DPDKSTACK_H

#include <functional>

#include "common/ceph_context.h"
#include "common/Tub.h"

#include "msg/async/Stack.h"
#include "DPDK.h"
#include "net.h"
#include "const.h"
#include "IP.h"
#include "Packet.h"

class interface;

template <typename Protocol>
class NativeConnectedSocketImpl;

// DPDKServerSocketImpl
template <typename Protocol>
class DPDKServerSocketImpl : public ServerSocketImpl {
  typename Protocol::listener _listener;
 public:
  DPDKServerSocketImpl(Protocol& proto, uint16_t port, const SocketOptions &opt);
  int listen() {
    return _listener.listen();
  }
  virtual int accept(ConnectedSocket *s, const SocketOptions &opts, entity_addr_t *out) override;
  virtual void abort_accept() override;
  virtual int fd() const override {
    return _listener.fd();
  }
};


// NativeConnectedSocketImpl
template <typename Protocol>
class NativeConnectedSocketImpl : public ConnectedSocketImpl {
  typename Protocol::connection _conn;
  Tub<Packet> _buf;

 public:
  explicit NativeConnectedSocketImpl(typename Protocol::connection conn)
          : _conn(std::move(conn)) {}
  NativeConnectedSocketImpl(NativeConnectedSocketImpl &&rhs)
      : _conn(std::move(rhs._conn)), _buf(std::move(rhs.buf))  {}
  virtual int is_connected() override {
    return _conn.is_connected();
  }

  virtual ssize_t read(char *buf, size_t len) override {
    bufferlist data;
    ssize_t r = zero_copy_read(len, data);
    if (r < 0)
      return r;
    assert(data.length() <= len);
    data.copy(0, data.length(), buf);
    return data.length();
  }

  virtual ssize_t zero_copy_read(size_t len, bufferlist &data) override {
    auto err = _conn.get_errno();
    if (err <= 0)
      return err;

    size_t left = len;
    while (left > 0) {
      if (!_buf) {
        _buf = std::move(_conn.read());
        if (!_buf)
          return left == len ? -EAGAIN : len - left;
      }

      size_t off = 0;
      for (auto&& f : _buf->fragments()) {
        if (f.size <= left) {
          Packet p = _buf->share(off, f.size);
          auto del = std::bind(
                  [](Packet &p) {}, std::move(p));
          data.push_back(buffer::claim_buffer(
                      f.size, f.base,
                      make_deleter(std::move(del))));
          off +=  f.size;
          left -= f.size;
        } else {
          Packet p = _buf->share(off, f.size - left);
          auto del = std::bind(
                  [](Packet &p) {}, std::move(p));
          data.push_back(buffer::claim_buffer(
                      left, f.base,
                      make_deleter(std::move(del))));
          off += left;
          left = 0;
          break;
        }
      }
      if (left) {
        _buf.destroy();
      } else {
        _buf->trim_front(off);
      }
    }
    return len - left;
  }
  virtual ssize_t send(bufferlist &bl, bool more) override {
    auto err = _conn.get_errno();
    if (err < 0)
      return (ssize_t)err;

    size_t available = _conn.peek_sent_available();
    if (available == 0) {
      _conn.register_write_waiter();
      return -EAGAIN;
    }

    std::vector<fragment> frags;
    std::list<bufferptr>::const_iterator pb = bl.buffers().begin();
    uint64_t left_pbrs = bl.buffers().size();
    uint64_t len = 0;
    uint64_t seglen = 0;
    while (len < available && left_pbrs--) {
      seglen = pb->length();
      if (len + seglen > available) {
        // don't continue if we enough at least 1 fragment since no available
        // space for next ptr.
        if (len > 0)
          break;
        seglen = MIN(seglen, available);
      }
      len += seglen;
      frags.push_back(fragment{(char*)pb->c_str(), seglen});
      ++pb;
    }

    Packet p;
    if (len != bl.length()) {
      _conn.register_write_waiter();
      bufferlist swapped;
      bl.splice(0, len, &swapped);
      auto del = std::bind(
              [](bufferlist &bl) { bl.clear(); }, std::move(swapped));
      return _conn.send(Packet(std::move(frags), make_deleter(std::move(del))));
    } else {
      auto del = std::bind(
              [](bufferlist &bl) { bl.clear(); }, std::move(bl));

      return _conn.send(Packet(std::move(frags), make_deleter(std::move(del))));
    }
  }
  virtual void shutdown() override {
    _conn.close_write();
  }
  // FIXME need to impl close
  virtual void close() override {
    _conn.close_write();
  }
  virtual int fd() const override {
    return _conn.fd();
  }
};

template <typename Protocol>
DPDKServerSocketImpl<Protocol>::DPDKServerSocketImpl(
        Protocol& proto, uint16_t port, const SocketOptions &opt)
        : _listener(proto.listen(port)) {}

template <typename Protocol>
int DPDKServerSocketImpl<Protocol>::accept(ConnectedSocket *s, const SocketOptions &options, entity_addr_t *out){
  if (_listener.get_errno() < 0)
    return _listener.get_errno();
  auto c = _listener.accept();
  if (!c)
    return -EAGAIN;

  if (out)
    *out = c->remote_addr();
  std::unique_ptr<NativeConnectedSocketImpl<Protocol>> csi(
          new NativeConnectedSocketImpl<Protocol>(std::move(*c)));
  *s = ConnectedSocket(std::move(csi));
  return 0;
}

template <typename Protocol>
void DPDKServerSocketImpl<Protocol>::abort_accept() {
  _listener.abort_accept();
}

class DPDKStack : public NetworkStack {
  interface _netif;
  ipv4 _inet;
  unsigned cores;

  void set_ipv4_packet_filter(ip_packet_filter* filter) {
    _inet.set_packet_filter(filter);
  }
  using tcp4 = tcp<ipv4_traits>;

 public:
  EventCenter *center;

  explicit DPDKStack(CephContext *cct, EventCenter *c,
                     std::shared_ptr<DPDKDevice> dev, unsigned cores);
  virtual int listen(entity_addr_t &addr, const SocketOptions &opts, ServerSocket *) override;
  virtual int connect(const entity_addr_t &addr, const SocketOptions &opts, ConnectedSocket *socket) override;
  static std::unique_ptr<NetworkStack> create(CephContext *cct, EventCenter *center);
  void arp_learn(ethernet_address l2, ipv4_address l3) {
    _inet.learn(l2, l3);
  }
  virtual bool support_zero_copy_read() const override { return true; }
  friend class DPDKServerSocketImpl<tcp4>;
};

#endif
