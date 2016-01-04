// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */
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

#ifndef CEPH_DPDK_DEV_H
#define CEPH_DPDK_DEV_H

#include <memory>
#include <functional>

#include "common/Tub.h"
#include "msg/async/Event.h"


struct port_stats {
  port_stats() : rx{}, tx{} {}

  struct {
    struct {
      uint64_t mcast;        // number of received multicast packets
      uint64_t pause_xon;    // number of received PAUSE XON frames
      uint64_t pause_xoff;   // number of received PAUSE XOFF frames
    } good;

    struct {
      uint64_t dropped;      // missed packets (e.g. full FIFO)
      uint64_t crc;          // packets with CRC error
      uint64_t len;          // packets with a bad length
      uint64_t total;        // total number of erroneous received packets
    } bad;
  } rx;

  struct {
    struct {
      uint64_t pause_xon;   // number of sent PAUSE XON frames
      uint64_t pause_xoff;  // number of sent PAUSE XOFF frames
    } good;

    struct {
      uint64_t total;   // total number of failed transmitted packets
    } bad;
  } tx;
};

enum {
  l_dpdk_dev_first,
  l_dpdk_dev_rx_mcast,
  l_dpdk_dev_rx_total_errors,
  l_dpdk_dev_tx_total_errors,
  l_dpdk_dev_rx_badcrc_errors,
  l_dpdk_dev_rx_dropped_errors,
  l_dpdk_dev_rx_badlength_errors,
  l_dpdk_dev_rx_pause_xon,
  l_dpdk_dev_tx_pause_xon,
  l_dpdk_dev_rx_pause_xoff,
  l_dpdk_dev_tx_pause_xoff,
  l_dpdk_dev_last
};


class DPDKDevice {
 public:
  CephContext *cct;
  std::unique_ptr<DPDKQueuePair*[]> _queues;
  size_t _rss_table_bits = 0;
  uint8_t _port_idx;
  uint16_t _num_queues;
  unsigned  cores;
  hw_features _hw_features;
  uint8_t _queues_ready = 0;
  unsigned _home_cpu;
  bool _use_lro;
  bool _enable_fc;
  std::vector<uint8_t> _redir_table;
  rss_key_type _rss_key;
  port_stats _stats;
  bool _is_i40e_device = false;

 public:
  rte_eth_dev_info _dev_info = {};

 private:
  /**
   * Port initialization consists of 3 main stages:
   * 1) General port initialization which ends with a call to
   *    rte_eth_dev_configure() where we request the needed number of Rx and
   *    Tx queues.
   * 2) Individual queues initialization. This is done in the constructor of
   *    DPDKQueuePair class. In particular the memory pools for queues are allocated
   *    in this stage.
   * 3) The final stage of the initialization which starts with the call of
   *    rte_eth_dev_start() after which the port becomes fully functional. We
   *    will also wait for a link to get up in this stage.
   */


  /**
   * First stage of the port initialization.
   *
   * @return 0 in case of success and an appropriate error code in case of an
   *         error.
   */
  int init_port_start();

  /**
   * The final stage of a port initialization.
   * @note Must be called *after* all queues from stage (2) have been
   *       initialized.
   */
  int init_port_fini();

  /**
   * Check the link status of out port in up to 9s, and print them finally.
   */
  int check_port_link_status();

  /**
   * Configures the HW Flow Control
   */
  void set_hw_flow_control();

 public:
  DPDKDevice(CephContext *c, uint8_t port_idx, uint16_t num_queues,
             unsigned cores, bool use_lro, bool enable_fc):
      cct(c), _port_idx(port_idx), _num_queues(num_queues), cores(cores),
      _home_cpu(0), _use_lro(use_lro),
      _enable_fc(enable_fc) {
    _queues = std::make_unique<DPDKQueuePair*[]>(cores);
    /* now initialise the port we will use */
    int ret = init_port_start();
    if (ret != 0) {
      rte_exit(EXIT_FAILURE, "Cannot initialise port %u\n", _port_idx);
    }
    string name(std::string("port") + std::to_string(qid));
    PerfCountersBuilder plb(cct, name, l_dpdk_dev_first, l_dpdk_dev_last);

    plb.add_u64_counter(l_dpdk_dev_rx_mcast, "dpdk_device_receive_multicast_packets", "DPDK received multicast packets");
    plb.add_u64_counter(l_dpdk_dev_rx_total_errors, "dpdk_device_receive_total_errors", "DPDK received total_errors");
    plb.add_u64_counter(l_dpdk_dev_tx_total_errors, "dpdk_device_send_total_errors", "DPDK sendd total_errors");
    plb.add_u64_counter(l_dpdk_dev_rx_badcrc_errors, "dpdk_device_receive_badcrc_errors", "DPDK received bad crc errors");
    plb.add_u64_counter(l_dpdk_dev_rx_dropped_errors, "dpdk_device_receive_dropped_errors", "DPDK received dropped errors");
    plb.add_u64_counter(l_dpdk_dev_rx_badlength_errors, "dpdk_device_receive_badlength_errors", "DPDK received bad length errors");
    plb.add_u64_counter(l_dpdk_dev_rx_pause_xon, "dpdk_device_receive_pause_xon", "DPDK received received PAUSE XON frames");
    plb.add_u64_counter(l_dpdk_dev_tx_pause_xon, "dpdk_device_send_pause_xon", "DPDK received sendd PAUSE XON frames");
    plb.add_u64_counter(l_dpdk_dev_rx_pause_xoff, "dpdk_device_receive_pause_xoff", "DPDK received received PAUSE XOFF frames");
    plb.add_u64_counter(l_dpdk_dev_tx_pause_xoff, "dpdk_device_send_pause_xoff", "DPDK received sendd PAUSE XOFF frames");

    perf_logger = plb.create_perf_counters();
    cct->get_perfcounters_collection()->add(perf_logger);
  }

  ~DPDKDevice() {}

  qp& queue_for_cpu(unsigned cpu) { return *_queues[cpu]; }
  void l2receive(int qid, Packet p) {
    _queues[qid]->_rx_stream.produce(std::move(p));
  }
  subscription<Packet> receive(unsigned cpuid,
      std::function<void (Packet)> next_packet) {
    auto sub = _queues[cpuid]->_rx_stream.listen(std::move(next_packet));
    _queues[cpuid]->rx_start();
    return std::move(sub);
  }
  ethernet_address hw_address() {
    struct ether_addr mac;
    rte_eth_macaddr_get(_port_idx, &mac);

    return mac.addr_bytes;
  }
  hw_features hw_features() {
    return _hw_features;
  }
  const rss_key_type& rss_key() const { return _rss_key; }
  uint16_t hw_queues_count() { return _num_queues; }
  std::unique_ptr<qp> init_local_queue(EventCenter *center, string hugepages, uint16_t qid) {
    std::unique_ptr<qp> qp;
    if (!hugepages.empty())
      qp = std::make_unique<DPDKQueuePair<true>>(center, this, qid);
    else
      qp = std::make_unique<DPDKQueuePair<false>>(center, this, qid);

    return std::move(qp);
  }
  unsigned hash2qid(uint32_t hash) {
    assert(_redir_table.size());
    return _redir_table[hash & (_redir_table.size() - 1)];
  }
  void set_local_queue(unsigned i, std::unique_ptr<qp> qp) {
    assert(!_queues[i]);
    _queues[i] = std::move(qp);
  }
  template <typename Func>
  unsigned forward_dst(unsigned src_cpuid, Func&& hashfn) {
    auto& qp = queue_for_cpu(src_cpuid);
    if (!qp._sw_reta) {
      return src_cpuid;
    }
    auto hash = hashfn() >> _rss_table_bits;
    auto& reta = *qp._sw_reta;
    return reta[hash % reta.size()];
  }

  hw_features& hw_features_ref() { return _hw_features; }

  const rte_eth_rxconf* def_rx_conf() const {
    return &_dev_info.default_rxconf;
  }

  const rte_eth_txconf* def_tx_conf() const {
    return &_dev_info.default_txconf;
  }

  /**
   *  Set the RSS table in the device and store it in the internal vector.
   */
  void set_rss_table();

  uint8_t port_idx() { return _port_idx; }
  bool is_i40e_device() const {
    return _is_i40e_device;
  }
};

std::unique_ptr<DPDKDevice> create_dpdk_net_device(
        uint8_t port_idx = 0, uint8_t num_queues = 1,
        bool use_lro = true, bool enable_fc = true);

struct qp_stats_good {
  /**
   * Update the packets bunch related statistics.
   *
   * Update the last packets bunch size and the total packets counter.
   *
   * @param count Number of packets in the last packets bunch.
   */
  void update_pkts_bunch(uint64_t count) {
    last_bunch = count;
    packets   += count;
  }

  /**
   * Increment the appropriate counters when a few fragments have been
   * processed in a copy-way.
   *
   * @param nr_frags Number of copied fragments
   * @param bytes    Number of copied bytes
   */
  void update_copy_stats(uint64_t nr_frags, uint64_t bytes) {
    copy_frags += nr_frags;
    copy_bytes += bytes;
  }

  /**
   * Increment total fragments and bytes statistics
   *
   * @param nfrags Number of processed fragments
   * @param nbytes Number of bytes in the processed fragments
   */
  void update_frags_stats(uint64_t nfrags, uint64_t nbytes) {
    nr_frags += nfrags;
    bytes    += nbytes;
  }

  uint64_t bytes;      // total number of bytes
  uint64_t nr_frags;   // total number of fragments
  uint64_t copy_frags; // fragments that were copied on L2 level
  uint64_t copy_bytes; // bytes that were copied on L2 level
  uint64_t packets;    // total number of packets
  uint64_t last_bunch; // number of packets in the last sent/received bunch
};

struct qp_stats {
  qp_stats() : rx{}, tx{} {}

  struct {
    struct qp_stats_good good;

    struct {
      void inc_csum_err() {
        ++csum;
        ++total;
      }

      void inc_no_mem() {
        ++no_mem;
        ++total;
      }

      uint64_t no_mem;       // Packets dropped due to allocation failure
      uint64_t total;        // total number of erroneous packets
      uint64_t csum;         // packets with bad checksum
    } bad;
  } rx;

  struct {
    struct qp_stats_good good;
    uint64_t linearized;       // number of packets that were linearized
  } tx;
};

enum {
  l_dpdk_qp_first,
  l_dpdk_qp_rx_packets,
  l_dpdk_qp_tx_packets,
  l_dpdk_qp_rx_total_errors,
  l_dpdk_qp_rx_bad_checksum_errors,
  l_dpdk_qp_rx_no_memory_errors,
  l_dpdk_qp_rx_bytes,
  l_dpdk_qp_tx_bytes,
  l_dpdk_qp_rx_last_bunch,
  l_dpdk_qp_tx_last_bunch,
  l_dpdk_qp_rx_fragments,
  l_dpdk_qp_tx_fragments,
  l_dpdk_qp_rx_copy_ops,
  l_dpdk_qp_tx_copy_ops,
  l_dpdk_qp_rx_copy_bytes,
  l_dpdk_qp_tx_copy_bytes,
  l_dpdk_qp_rx_linearize_ops,
  l_dpdk_qp_tx_linearize_ops,
  l_dpdk_qp_tx_queue_length,
  l_dpdk_qp_last
};

template <bool HugetlbfsMemBackend>
class DPDKQueuePair {
 public:
  void configure_proxies(const std::map<unsigned, float>& cpu_weights);
  // build REdirection TAble for cpu_weights map: target cpu -> weight
  void build_sw_reta(const std::map<unsigned, float>& cpu_weights);
  void proxy_send(packet p) {
      _proxy_packetq.push_back(std::move(p));
  }
  void register_packet_provider(packet_provider_type func) {
      _pkt_providers.push_back(std::move(func));
  }
  bool poll_tx();
  friend class DPDKDevice;

  class tx_buf_factory;

  class tx_buf {
    friend class DPDKQueuePair;
   public:
    static tx_buf* me(rte_mbuf* mbuf) {
      return reinterpret_cast<tx_buf*>(mbuf);
    }

    private:
    /**
     * Checks if the original packet of a given cluster should be linearized
     * due to HW limitations.
     *
     * @param head head of a cluster to check
     *
     * @return TRUE if a packet should be linearized.
     */
    static bool i40e_should_linearize(rte_mbuf *head) {
      bool is_tso = head->ol_flags & PKT_TX_TCP_SEG;

      // For a non-TSO case: number of fragments should not exceed 8
      if (!is_tso){
        return head->nb_segs > i40e_max_xmit_segment_frags;
      }

      //
      // For a TSO case each MSS window should not include more than 8
      // fragments including headers.
      //

      // Calculate the number of frags containing headers.
      //
      // Note: we support neither VLAN nor tunneling thus headers size
      // accounting is super simple.
      //
      size_t headers_size = head->l2_len + head->l3_len + head->l4_len;
      unsigned hdr_frags = 0;
      size_t cur_payload_len = 0;
      rte_mbuf *cur_seg = head;

      while (cur_seg && cur_payload_len < headers_size) {
        cur_payload_len += cur_seg->data_len;
        cur_seg = cur_seg->next;
        hdr_frags++;
      }

      //
      // Header fragments will be used for each TSO segment, thus the
      // maximum number of data segments will be 8 minus the number of
      // header fragments.
      //
      // It's unclear from the spec how the first TSO segment is treated
      // if the last fragment with headers contains some data bytes:
      // whether this fragment will be accounted as a single fragment or
      // as two separate fragments. We prefer to play it safe and assume
      // that this fragment will be accounted as two separate fragments.
      //
      size_t max_win_size = i40e_max_xmit_segment_frags - hdr_frags;

      if (head->nb_segs <= max_win_size) {
        return false;
      }

      // Get the data (without headers) part of the first data fragment
      size_t prev_frag_data = cur_payload_len - headers_size;
      auto mss = head->tso_segsz;

      while (cur_seg) {
        unsigned frags_in_seg = 0;
        size_t cur_seg_size = 0;

        if (prev_frag_data) {
          cur_seg_size = prev_frag_data;
          frags_in_seg++;
          prev_frag_data = 0;
        }

        while (cur_seg_size < mss && cur_seg) {
          cur_seg_size += cur_seg->data_len;
          cur_seg = cur_seg->next;
          frags_in_seg++;

          if (frags_in_seg > max_win_size) {
              return true;
          }
        }

        if (cur_seg_size > mss) {
          prev_frag_data = cur_seg_size - mss;
        }
      }

      return false;
    }

      /**
       * Sets the offload info in the head buffer of an rte_mbufs cluster.
       *
       * @param p an original packet the cluster is built for
       * @param qp QP handle
       * @param head a head of an rte_mbufs cluster
       */
      static void set_cluster_offload_info(const packet& p, const DPDKQueuePair& qp, rte_mbuf* head) {
        // Handle TCP checksum offload
        auto oi = p.offload_info();
        if (oi.needs_ip_csum) {
          head->ol_flags |= PKT_TX_IP_CKSUM;
          // TODO: Take a VLAN header into an account here
          head->l2_len = sizeof(struct ether_hdr);
          head->l3_len = oi.ip_hdr_len;
        }
        if (qp.port().hw_features().tx_csum_l4_offload) {
          if (oi.protocol == ip_protocol_num::tcp) {
              head->ol_flags |= PKT_TX_TCP_CKSUM;
              // TODO: Take a VLAN header into an account here
              head->l2_len = sizeof(struct ether_hdr);
              head->l3_len = oi.ip_hdr_len;

              if (oi.tso_seg_size) {
                assert(oi.needs_ip_csum);
                head->ol_flags |= PKT_TX_TCP_SEG;
                head->l4_len = oi.tcp_hdr_len;
                head->tso_segsz = oi.tso_seg_size;
              }
          } else if (oi.protocol == ip_protocol_num::udp) {
            head->ol_flags |= PKT_TX_UDP_CKSUM;
            // TODO: Take a VLAN header into an account here
            head->l2_len = sizeof(struct ether_hdr);
            head->l3_len = oi.ip_hdr_len;
          }
        }
      }

      /**
       * Creates a tx_buf cluster representing a given packet in a "zero-copy"
       * way.
       *
       * @param p packet to translate
       * @param qp DPDKQueuePair handle
       *
       * @return the HEAD tx_buf of the cluster or nullptr in case of a
       *         failure
       */
      static tx_buf* from_packet_zc(packet&& p, DPDKQueuePair& qp) {
        // Too fragmented - linearize
        if (p.nr_frags() > max_frags) {
          p.linearize();
          ++qp._stats.tx.linearized;
        }

       build_mbuf_cluster:
        rte_mbuf *head = nullptr, *last_seg = nullptr;
        unsigned nsegs = 0;

        //
        // Create a HEAD of the fragmented packet: check if frag0 has to be
        // copied and if yes - send it in a copy way
        //
        if (!check_frag0(p)) {
          if (!copy_one_frag(qp, p.frag(0), head, last_seg, nsegs)) {
              return nullptr;
          }
        } else if (!translate_one_frag(qp, p.frag(0), head, last_seg, nsegs)) {
          return nullptr;
        }

        unsigned total_nsegs = nsegs;

        for (unsigned i = 1; i < p.nr_frags(); i++) {
          rte_mbuf *h = nullptr, *new_last_seg = nullptr;
          if (!translate_one_frag(qp, p.frag(i), h, new_last_seg, nsegs)) {
            me(head)->recycle();
            return nullptr;
          }

          total_nsegs += nsegs;

          // Attach a new buffers' chain to the packet chain
          last_seg->next = h;
          last_seg = new_last_seg;
        }

        // Update the HEAD buffer with the packet info
        head->pkt_len = p.len();
        head->nb_segs = total_nsegs;

        set_cluster_offload_info(p, qp, head);

        //
        // If a packet hasn't been linearized already and the resulting
        // cluster requires the linearisation due to HW limitation:
        //
        //    - Recycle the cluster.
        //    - Linearize the packet.
        //    - Build the cluster once again
        //
        if (head->nb_segs > max_frags ||
              (p.nr_frags() > 1 && qp.port().is_i40e_device() && i40e_should_linearize(head))) {
          me(head)->recycle();
          p.linearize();
          ++qp._stats.tx.linearized;

          goto build_mbuf_cluster;
        }

        me(last_seg)->set_packet(std::move(p));

        return me(head);
      }

      /**
       * Copy the contents of the "packet" into the given cluster of
       * rte_mbuf's.
       *
       * @note Size of the cluster has to be big enough to accommodate all the
       *       contents of the given packet.
       *
       * @param p packet to copy
       * @param head head of the rte_mbuf's cluster
       */
      static void copy_packet_to_cluster(const packet& p, rte_mbuf* head) {
          rte_mbuf* cur_seg = head;
          size_t cur_seg_offset = 0;
          unsigned cur_frag_idx = 0;
          size_t cur_frag_offset = 0;

          while (true) {
              size_t to_copy = std::min(p.frag(cur_frag_idx).size - cur_frag_offset,
                      inline_mbuf_data_size - cur_seg_offset);

              memcpy(rte_pktmbuf_mtod_offset(cur_seg, void*, cur_seg_offset),
                      p.frag(cur_frag_idx).base + cur_frag_offset, to_copy);

              cur_frag_offset += to_copy;
              cur_seg_offset += to_copy;

              if (cur_frag_offset >= p.frag(cur_frag_idx).size) {
                  ++cur_frag_idx;
                  if (cur_frag_idx >= p.nr_frags()) {
                      //
                      // We are done - set the data size of the last segment
                      // of the cluster.
                      //
                      cur_seg->data_len = cur_seg_offset;
                      break;
                  }

                  cur_frag_offset = 0;
              }

              if (cur_seg_offset >= inline_mbuf_data_size) {
                  cur_seg->data_len = inline_mbuf_data_size;
                  cur_seg = cur_seg->next;
                  cur_seg_offset = 0;

                  // FIXME: assert in a fast-path - remove!!!
                  assert(cur_seg);
              }
          }
      }

      /**
       * Creates a tx_buf cluster representing a given packet in a "copy" way.
       *
       * @param p packet to translate
       * @param qp DPDKQueuePair handle
       *
       * @return the HEAD tx_buf of the cluster or nullptr in case of a
       *         failure
       */
      static tx_buf* from_packet_copy(packet&& p, DPDKQueuePair& qp) {
          // sanity
          if (!p.len()) {
              return nullptr;
          }

          /*
           * Here we are going to use the fact that the inline data size is a
           * power of two.
           *
           * We will first try to allocate the cluster and only if we are
           * successful - we will go and copy the data.
           */
          auto aligned_len = align_up((size_t)p.len(), inline_mbuf_data_size);
          unsigned nsegs = aligned_len / inline_mbuf_data_size;
          rte_mbuf *head = nullptr, *last_seg = nullptr;

          tx_buf* buf = qp.get_tx_buf();
          if (!buf) {
              return nullptr;
          }

          head = buf->rte_mbuf_p();
          last_seg = head;
          for (unsigned i = 1; i < nsegs; i++) {
            buf = qp.get_tx_buf();
            if (!buf) {
              me(head)->recycle();
              return nullptr;
            }

            last_seg->next = buf->rte_mbuf_p();
            last_seg = last_seg->next;
          }

          //
          // If we've got here means that we have succeeded already!
          // We only need to copy the data and set the head buffer with the
          // relevant info.
          //
          head->pkt_len = p.len();
          head->nb_segs = nsegs;

          copy_packet_to_cluster(p, head);
          set_cluster_offload_info(p, qp, head);

          return me(head);
      }

      /**
       * Zero-copy handling of a single fragment.
       *
       * @param do_one_buf Functor responsible for a single rte_mbuf
       *                   handling
       * @param qp DPDKQueuePair handle (in)
       * @param frag Fragment to copy (in)
       * @param head Head of the cluster (out)
       * @param last_seg Last segment of the cluster (out)
       * @param nsegs Number of segments in the cluster (out)
       *
       * @return TRUE in case of success
       */
      template <class DoOneBufFunc>
      static bool do_one_frag(DoOneBufFunc do_one_buf, DPDKQueuePair& qp,
              fragment& frag, rte_mbuf*& head,
              rte_mbuf*& last_seg, unsigned& nsegs) {
          size_t len, left_to_set = frag.size;
          char* base = frag.base;

          rte_mbuf* m;

          // TODO: assert() in a fast path! Remove me ASAP!
          assert(frag.size);

          // Create a HEAD of mbufs' cluster and set the first bytes into it
          len = do_one_buf(qp, head, base, left_to_set);
          if (!len) {
              return false;
          }

          left_to_set -= len;
          base += len;
          nsegs = 1;

          //
          // Set the rest of the data into the new mbufs and chain them to
          // the cluster.
          //
          rte_mbuf* prev_seg = head;
          while (left_to_set) {
              len = do_one_buf(qp, m, base, left_to_set);
              if (!len) {
                  me(head)->recycle();
                  return false;
              }

              left_to_set -= len;
              base += len;
              nsegs++;

              prev_seg->next = m;
              prev_seg = m;
          }

          // Return the last mbuf in the cluster
          last_seg = prev_seg;

          return true;
      }

      /**
       * Zero-copy handling of a single fragment.
       *
       * @param qp DPDKQueuePair handle (in)
       * @param frag Fragment to copy (in)
       * @param head Head of the cluster (out)
       * @param last_seg Last segment of the cluster (out)
       * @param nsegs Number of segments in the cluster (out)
       *
       * @return TRUE in case of success
       */
      static bool translate_one_frag(DPDKQueuePair& qp, fragment& frag,
              rte_mbuf*& head, rte_mbuf*& last_seg,
              unsigned& nsegs) {
        return do_one_frag(set_one_data_buf, qp, frag, head,
                last_seg, nsegs);
      }

      /**
       * Copies one fragment into the cluster of rte_mbuf's.
       *
       * @param qp DPDKQueuePair handle (in)
       * @param frag Fragment to copy (in)
       * @param head Head of the cluster (out)
       * @param last_seg Last segment of the cluster (out)
       * @param nsegs Number of segments in the cluster (out)
       *
       * We return the "last_seg" to avoid traversing the cluster in order to get
       * it.
       *
       * @return TRUE in case of success
       */
      static bool copy_one_frag(DPDKQueuePair& qp, fragment& frag,
              rte_mbuf*& head, rte_mbuf*& last_seg,
              unsigned& nsegs) {
        return do_one_frag(copy_one_data_buf, qp, frag, head,
                last_seg, nsegs);
      }

      /**
       * Allocates a single rte_mbuf and sets it to point to a given data
       * buffer.
       *
       * @param qp DPDKQueuePair handle (in)
       * @param m New allocated rte_mbuf (out)
       * @param va virtual address of a data buffer (in)
       * @param buf_len length of the data to copy (in)
       *
       * @return The actual number of bytes that has been set in the mbuf
       */
      static size_t set_one_data_buf(
              DPDKQueuePair& qp, rte_mbuf*& m, char* va, size_t buf_len) {
          static constexpr size_t max_frag_len = 15 * 1024; // 15K

          using namespace memory;
          translation tr = translate(va, buf_len);

          //
          // Currently we break a buffer on a 15K boundary because 82599
          // devices have a 15.5K limitation on a maximum single fragment
          // size.
          //
          phys_addr_t pa = tr.addr;

          if (!tr.size) {
              return copy_one_data_buf(qp, m, va, buf_len);
          }

          tx_buf* buf = qp.get_tx_buf();
          if (!buf) {
              return 0;
          }

          size_t len = std::min(tr.size, max_frag_len);

          buf->set_zc_info(va, pa, len);
          m = buf->rte_mbuf_p();

          return len;
      }

      /**
       *  Allocates a single rte_mbuf and copies a given data into it.
       *
       * @param qp DPDKQueuePair handle (in)
       * @param m New allocated rte_mbuf (out)
       * @param data Data to copy from (in)
       * @param buf_len length of the data to copy (in)
       *
       * @return The actual number of bytes that has been copied
       */
      static size_t copy_one_data_buf(
              DPDKQueuePair& qp, rte_mbuf*& m, char* data, size_t buf_len)
      {
          tx_buf* buf = qp.get_tx_buf();
          if (!buf) {
              return 0;
          }

          size_t len = std::min(buf_len, inline_mbuf_data_size);

          m = buf->rte_mbuf_p();

          // mbuf_put()
          m->data_len = len;
          m->pkt_len  = len;

          qp._stats.tx.good.update_copy_stats(1, len);

          memcpy(rte_pktmbuf_mtod(m, void*), data, len);

          return len;
      }

      /**
       * Checks if the first fragment of the given packet satisfies the
       * zero-copy flow requirement: its first 128 bytes should not cross the
       * 4K page boundary. This is required in order to avoid splitting packet
       * headers.
       *
       * @param p packet to check
       *
       * @return TRUE if packet is ok and FALSE otherwise.
       */
      static bool check_frag0(packet& p)
      {
          using namespace memory;

          //
          // First frag is special - it has headers that should not be split.
          // If the addressing is such that the first fragment has to be
          // split, then send this packet in a (non-zero) copy flow. We'll
          // check if the first 128 bytes of the first fragment reside in the
          // physically contiguous area. If that's the case - we are good to
          // go.
          //
          size_t frag0_size = p.frag(0).size;
          void* base = p.frag(0).base;
          translation tr = translate(base, frag0_size);

          if (tr.size < frag0_size && tr.size < 128) {
              return false;
          }

          return true;
      }

      public:
      tx_buf(tx_buf_factory& fc) : _fc(fc) {

          _buf_physaddr = _mbuf.buf_physaddr;
          _data_off     = _mbuf.data_off;
      }

      rte_mbuf* rte_mbuf_p() { return &_mbuf; }

      void set_zc_info(void* va, phys_addr_t pa, size_t len) {
          // mbuf_put()
          _mbuf.data_len           = len;
          _mbuf.pkt_len            = len;

          // Set the mbuf to point to our data
          _mbuf.buf_addr           = va;
          _mbuf.buf_physaddr       = pa;
          _mbuf.data_off           = 0;
          _is_zc                   = true;
      }

      void reset_zc() {

          //
          // If this mbuf was the last in a cluster and contains an
          // original packet object then call the destructor of the
          // original packet object.
          //
          if (_p) {
              //
              // Reset the std::optional. This in particular is going
              // to call the "packet"'s destructor and reset the
              // "optional" state to "nonengaged".
              //
              _p.destroy();

          } else if (!_is_zc) {
              return;
          }

          // Restore the rte_mbuf fields we trashed in set_zc_info()
          _mbuf.buf_physaddr = _buf_physaddr;
          _mbuf.buf_addr     = rte_mbuf_to_baddr(&_mbuf);
          _mbuf.data_off     = _data_off;

          _is_zc             = false;
      }

      void recycle() {
          struct rte_mbuf *m = &_mbuf, *m_next;

          while (m != nullptr) {
              m_next = m->next;
              rte_pktmbuf_reset(m);
              _fc.put(me(m));
              m = m_next;
          }
      }

      void set_packet(Packet&& p) {
          _p.constrct(p);
      }

      private:
      struct rte_mbuf _mbuf;
      MARKER private_start;
      Tub<Packet> _p;
      phys_addr_t _buf_physaddr;
      uint16_t _data_off;
      // TRUE if underlying mbuf has been used in the zero-copy flow
      bool _is_zc = false;
      // buffers' factory the buffer came from
      tx_buf_factory& _fc;
      MARKER private_end;
    };

    class tx_buf_factory {
        //
        // Number of buffers to free in each GC iteration:
        // We want the buffers to be allocated from the mempool as many as
        // possible.
        //
        // On the other hand if there is no Tx for some time we want the
        // completions to be eventually handled. Thus we choose the smallest
        // possible packets count number here.
        //
        static constexpr int gc_count = 1;
        public:
        tx_buf_factory(uint8_t qid) {
            using namespace memory;

            sstring name = sstring(pktmbuf_pool_name) + to_sstring(qid) + "_tx";
            printf("Creating Tx mbuf pool '%s' [%u mbufs] ...\n",
                    name.c_str(), mbufs_per_queue_tx);

            if (HugetlbfsMemBackend) {
                std::vector<phys_addr_t> mappings;

                _xmem.reset(DPDKQueuePair::alloc_mempool_xmem(mbufs_per_queue_tx,
                            inline_mbuf_size,
                            mappings));
                if (!_xmem.get()) {
                    printf("Can't allocate a memory for Tx buffers\n");
                    exit(1);
                }

                //
                // We are going to push the buffers from the mempool into
                // the circular_buffer and then poll them from there anyway, so
                // we prefer to make a mempool non-atomic in this case.
                //
                _pool =
                    rte_mempool_xmem_create(name.c_str(),
                            mbufs_per_queue_tx, inline_mbuf_size,
                            mbuf_cache_size,
                            sizeof(struct rte_pktmbuf_pool_private),
                            rte_pktmbuf_pool_init, nullptr,
                            rte_pktmbuf_init, nullptr,
                            rte_socket_id(), 0,
                            _xmem.get(), mappings.data(),
                            mappings.size(), page_bits);

            } else {
                _pool =
                    rte_mempool_create(name.c_str(),
                            mbufs_per_queue_tx, inline_mbuf_size,
                            mbuf_cache_size,
                            sizeof(struct rte_pktmbuf_pool_private),
                            rte_pktmbuf_pool_init, nullptr,
                            rte_pktmbuf_init, nullptr,
                            rte_socket_id(), 0);
            }

            if (!_pool) {
                printf("Failed to create mempool for Tx\n");
                exit(1);
            }

            //
            // Fill the factory with the buffers from the mempool allocated
            // above.
            //
            init_factory();
        }

        /**
         * @note Should not be called if there are no free tx_buf's
         *
         * @return a free tx_buf object
         */
        tx_buf* get() {
            // Take completed from the HW first
            tx_buf *pkt = get_one_completed();
            if (pkt) {
                if (HugetlbfsMemBackend) {
                    pkt->reset_zc();
                }

                return pkt;
            }

            //
            // If there are no completed at the moment - take from the
            // factory's cache.
            //
            if (_ring.empty()) {
                return nullptr;
            }

            pkt = _ring.back();
            _ring.pop_back();

            return pkt;
        }

        void put(tx_buf* buf) {
            if (HugetlbfsMemBackend) {
                buf->reset_zc();
            }
            _ring.push_back(buf);
        }

        bool gc() {
            for (int cnt = 0; cnt < gc_count; ++cnt) {
                auto tx_buf_p = get_one_completed();
                if (!tx_buf_p) {
                    return false;
                }

                put(tx_buf_p);
            }

            return true;
        }
        private:
        /**
         * Fill the mbufs circular buffer: after this the _pool will become
         * empty. We will use it to catch the completed buffers:
         *
         * - Underlying PMD drivers will "free" the mbufs once they are
         *   completed.
         * - We will poll the _pktmbuf_pool_tx till it's empty and release
         *   all the buffers from the freed mbufs.
         */
        void init_factory() {
            while (rte_mbuf* mbuf = rte_pktmbuf_alloc(_pool)) {
                _ring.push_back(new(tx_buf::me(mbuf)) tx_buf{*this});
            }
        }

        /**
         * PMD puts the completed buffers back into the mempool they have
         * originally come from.
         *
         * @note rte_pktmbuf_alloc() resets the mbuf so there is no need to call
         *       rte_pktmbuf_reset() here again.
         *
         * @return a single tx_buf that has been completed by HW.
         */
        tx_buf* get_one_completed() {
            return tx_buf::me(rte_pktmbuf_alloc(_pool));
        }

        private:
        std::vector<tx_buf*> _ring;
        rte_mempool* _pool = nullptr;
        std::unique_ptr<void, free_deleter> _xmem;
    };

 public:
    explicit DPDKQueuePair(DPDKDevice* dev, uint8_t qid);

    void rx_start();
    ~DPDKQueuePair() {}

    uint32_t send(circular_buffer<packet>& pb) {
       if (HugetlbfsMemBackend) {
            // Zero-copy send
            return _send(pb, [&] (packet&& p) {
                    return tx_buf::from_packet_zc(std::move(p), *this);
                    });
        } else {
            // "Copy"-send
            return _send(pb, [&](packet&& p) {
                    return tx_buf::from_packet_copy(std::move(p), *this);
                    });
        }
    }

    DPDKDevice& port() const { return *_dev; }
    tx_buf* get_tx_buf() { return _tx_buf_factory.get(); }
 private:
  template <class Func>
    uint32_t _send(circular_buffer<packet>& pb, Func packet_to_tx_buf_p) {
      if (_tx_burst.size() == 0) {
        for (auto&& p : pb) {
          // TODO: assert() in a fast path! Remove me ASAP!
          assert(p.len());

          tx_buf* buf = packet_to_tx_buf_p(std::move(p));
          if (!buf) {
            break;
          }

          _tx_burst.push_back(buf->rte_mbuf_p());
        }
      }

      uint16_t sent = rte_eth_tx_burst(_dev->port_idx(), _qid,
              _tx_burst.data() + _tx_burst_idx,
              _tx_burst.size() - _tx_burst_idx);

      uint64_t nr_frags = 0, bytes = 0;

      for (int i = 0; i < sent; i++) {
        rte_mbuf* m = _tx_burst[_tx_burst_idx + i];
        bytes    += m->pkt_len;
        nr_frags += m->nb_segs;
        pb.pop_front();
      }

      _stats.tx.good.update_frags_stats(nr_frags, bytes);

      _tx_burst_idx += sent;

      if (_tx_burst_idx == _tx_burst.size()) {
        _tx_burst_idx = 0;
        _tx_burst.clear();
      }

      return sent;
    }

  /**
   * Allocate a new data buffer and set the mbuf to point to it.
   *
   * Do some DPDK hacks to work on PMD: it assumes that the buf_addr
   * points to the private data of RTE_PKTMBUF_HEADROOM before the actual
   * data buffer.
   *
   * @param m mbuf to update
   */
  static bool refill_rx_mbuf(rte_mbuf* m, size_t size = mbuf_data_size) {
      char* data;

      if (posix_memalign((void**)&data, size, size)) {
          return false;
      }

      using namespace memory;
      translation tr = translate(data, size);

      // TODO: assert() in a fast path! Remove me ASAP!
      assert(tr.size == size);

      //
      // Set the mbuf to point to our data.
      //
      // Do some DPDK hacks to work on PMD: it assumes that the buf_addr
      // points to the private data of RTE_PKTMBUF_HEADROOM before the
      // actual data buffer.
      //
      m->buf_addr      = data - RTE_PKTMBUF_HEADROOM;
      m->buf_physaddr  = tr.addr - RTE_PKTMBUF_HEADROOM;
      return true;
  }

  static bool init_noninline_rx_mbuf(rte_mbuf* m,
          size_t size = mbuf_data_size) {
      if (!refill_rx_mbuf(m, size)) {
          return false;
      }
      // The below fields stay constant during the execution.
      m->buf_len       = size + RTE_PKTMBUF_HEADROOM;
      m->data_off      = RTE_PKTMBUF_HEADROOM;
      return true;
  }

  bool init_rx_mbuf_pool();
  bool rx_gc();
  bool refill_one_cluster(rte_mbuf* head);

  /**
   * Allocates a memory chunk to accommodate the given number of buffers of
   * the given size and fills a vector with underlying physical pages.
   *
   * The chunk is going to be used as an external memory buffer of the DPDK
   * memory pool (created using rte_mempool_xmem_create()).
   *
   * The chunk size if calculated using rte_mempool_xmem_size() function.
   *
   * @param num_bufs Number of buffers (in)
   * @param buf_sz   Size of each buffer (in)
   * @param mappings vector of physical pages (out)
   *
   * @note this function assumes that "mappings" is properly set and adds the
   *       mappings to the back of the vector.
   *
   * @return a virtual address of the allocated memory chunk or nullptr in
   *         case of a failure.
   */
  static void* alloc_mempool_xmem(uint16_t num_bufs, uint16_t buf_sz,
          std::vector<phys_addr_t>& mappings);

  /**
   * Polls for a burst of incoming packets. This function will not block and
   * will immediately return after processing all available packets.
   *
   */
  bool poll_rx_once();

  /**
   * Translates an rte_mbuf's into packet and feeds them to _rx_stream.
   *
   * @param bufs An array of received rte_mbuf's
   * @param count Number of buffers in the bufs[]
   */
  void process_packets(struct rte_mbuf **bufs, uint16_t count);

  /**
   * Translate rte_mbuf into the "packet".
   * @param m mbuf to translate
   *
   * @return a "optional" object representing the newly received data if in an
   *         "engaged" state or an error if in a "disengaged" state.
   */
  Tub<Packet> from_mbuf(rte_mbuf* m);

  /**
   * Transform an LRO rte_mbuf cluster into the "packet" object.
   * @param m HEAD of the mbufs' cluster to transform
   *
   * @return a "optional" object representing the newly received LRO packet if
   *         in an "engaged" state or an error if in a "disengaged" state.
   */
  Tub<Packet> from_mbuf_lro(rte_mbuf* m);

 private:
  using packet_provider_type = std::function<Tub<Packet> ()>;
  std::vector<packet_provider_type> _pkt_providers;
  Tub<std::array<uint8_t, 128>> _sw_reta;
  circular_buffer<packet> _proxy_packetq;
  stream<Packet> _rx_stream;
  class DPDKTXPoller : public EventCenter::Poller {
    DPDKQueuePair *qp;

   public:
    explicit DPDKTXPoller(DPDKQueuePair *qp)
      : EventCenter::Poller(qp->center, "DPDK::DPDKTXPoller"), qp(pp) {}

    virtual int poll() {
      return qp->poll_tx();
    }
  } _tx_poller;
  circular_buffer<packet> _tx_packetq;

  qp_stats _stats;
  PerfCounters *perf_logger;
  DPDKDevice* _dev;
  EventCenter *center;
  uint8_t _qid;
  rte_mempool *_pktmbuf_pool_rx;
  std::vector<rte_mbuf*> _rx_free_pkts;
  std::vector<rte_mbuf*> _rx_free_bufs;
  std::vector<fragment> _frags;
  std::vector<char*> _bufs;
  size_t _num_rx_free_segs = 0;
  class DPDKRXGCPoller : public EventCenter::Poller {
    DPDKQueuePair *qp;

   public:
    explicit DPDKRXGCPoller(DPDKQueuePair *qp)
      : EventCenter::Poller(qp->center, "DPDK::DPDKRXGCPoller"), qp(pp) {}

    virtual int poll() {
      return qp->rx_gc();
    }
  } _rx_gc_poller;
  std::unique_ptr<void, free_deleter> _rx_xmem;
  tx_buf_factory _tx_buf_factory;
  class DPDKRXPoller : public EventCenter::Poller {
    DPDKQueuePair *qp;

   public:
    explicit DPDKRXPoller(DPDKQueuePair *qp)
      : EventCenter::Poller(qp->center, "DPDK::DPDKRXPoller"), qp(pp) {}

    virtual int poll() {
      return qp->poll_rx_once();
    }
  };
  Tub<DPDKRXPoller> _rx_poller;
  class DPDKTXGCPoller : public EventCenter::Poller {
    DPDKQueuePair *qp;

   public:
    explicit DPDKTXGCPoller(DPDKQueuePair *qp)
      : EventCenter::Poller(qp->center, "DPDK::DPDKTXGCPoller"), qp(pp) {}

    virtual int poll() {
      return qp->_tx_buf_factory.gc();
    }
  } _tx_gc_poller;
  std::vector<rte_mbuf*> _tx_burst;
  uint16_t _tx_burst_idx = 0;
  static constexpr phys_addr_t page_mask = ~(memory::page_size - 1);
};


/**
 * @return Number of bytes needed for mempool objects of each QP.
 */
uint32_t qp_mempool_obj_size(bool hugetlbfs_membackend);

#endif // CEPH_DPDK_DEV_H
