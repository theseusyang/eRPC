#pragma once

#include <set>
#include "cc/timing_wheel.h"
#include "common.h"
#include "msg_buffer.h"
#include "nexus.h"
#include "ops.h"
#include "pkthdr.h"
#include "session.h"
#include "transport.h"
#include "transport_impl/infiniband/ib_transport.h"
#include "transport_impl/raw/raw_transport.h"
#include "util/buffer.h"
#include "util/fixed_queue.h"
#include "util/huge_alloc.h"
#include "util/logger.h"
#include "util/mt_queue.h"
#include "util/rand.h"
#include "util/timer.h"
#include "util/udp_client.h"

namespace erpc {

/**
 * @brief Rpc object created by foreground threads, and possibly shared with
 * background threads.
 *
 * Non-const functions that are not thread-safe should be marked in the
 * documentation.
 *
 * @tparam TTr The unreliable transport
 */
template <class TTr>
class Rpc {
  friend class RpcTest;

 public:
  /// Max request or response *data* size, i.e., excluding packet headers
  static constexpr size_t kMaxMsgSize =
      HugeAlloc::kMaxClassSize -
      ((HugeAlloc::kMaxClassSize / TTr::kMaxDataPerPkt) * sizeof(pkthdr_t));
  static_assert((1ull << kMsgSizeBits) >= kMaxMsgSize, "");

  static_assert((1ull << kPktNumBits) * TTr::kMaxDataPerPkt > 2 * kMaxMsgSize,
                "");

  /// Initial capacity of the hugepage allocator
  static constexpr size_t kInitialHugeAllocSize = (8 * MB(1));

  /// Timeout for a session management request in milliseconds
  static constexpr size_t kSMTimeoutMs = kTesting ? 10 : 100;

  //
  // Constructor/destructor (rpc.cc)
  //

  /**
   * @brief Construct the Rpc object from a foreground thread
   * @param phy_port Zero-based index among active fabric ports
   * @throw runtime_error if construction fails
   */
  Rpc(Nexus *nexus, void *context, uint8_t rpc_id, sm_handler_t sm_handler,
      uint8_t phy_port = 0);

  /// Destroy the Rpc from a foreground thread
  ~Rpc();

  //
  // MsgBuffer management
  //

  /**
   * @brief Create a hugepage-backed MsgBuffer for the eRPC user.
   *
   * The returned MsgBuffer's \p buf is surrounded by packet headers that the
   * user must not modify. This function does not fill in these message headers,
   * though it sets the magic field in the zeroth header.
   *
   * @param max_data_size Maximum non-header bytes in the returned MsgBuffer
   *
   * @return \p The allocated MsgBuffer. The MsgBuffer is invalid (i.e., its
   * \p buf is null) if we ran out of memory.
   *
   * @throw runtime_error if \p size is too large for the allocator, or if
   * hugepage reservation failure is catastrophic. An exception is *not* thrown
   * if allocation fails simply because we ran out of memory.
   */
  inline MsgBuffer alloc_msg_buffer(size_t max_data_size) {
    assert(max_data_size > 0);  // Doesn't work for max_data_size = 0

    // This function avoids division for small data sizes
    size_t max_num_pkts = data_size_to_num_pkts(max_data_size);

    lock_cond(&huge_alloc_lock);
    Buffer buffer =
        huge_alloc->alloc(max_data_size + (max_num_pkts * sizeof(pkthdr_t)));
    unlock_cond(&huge_alloc_lock);

    if (unlikely(buffer.buf == nullptr)) {
      MsgBuffer msg_buffer;
      msg_buffer.buf = nullptr;
      return msg_buffer;
    }

    MsgBuffer msg_buffer(buffer, max_data_size, max_num_pkts);
    return msg_buffer;
  }

  /// Identical to alloc_msg_buffer, but dies on failure
  inline MsgBuffer alloc_msg_buffer_or_die(size_t max_data_size) {
    MsgBuffer m = alloc_msg_buffer(max_data_size);
    rt_assert(m.buf != nullptr);
    return m;
  }

  /// Resize a MsgBuffer to a smaller size than its max allocation, including
  /// zero size. This does not modify the MsgBuffer's packet headers.
  static inline void resize_msg_buffer(MsgBuffer *msg_buffer,
                                       size_t new_data_size) {
    assert(msg_buffer->is_valid());  // Can be fake
    assert(new_data_size <= msg_buffer->max_data_size);

    // Avoid division for single-packet data sizes
    size_t new_num_pkts = data_size_to_num_pkts(new_data_size);
    msg_buffer->resize(new_data_size, new_num_pkts);
  }

  /// Free a MsgBuffer created by \p alloc_msg_buffer()
  inline void free_msg_buffer(MsgBuffer msg_buffer) {
    lock_cond(&huge_alloc_lock);
    huge_alloc->free_buf(msg_buffer.buffer);
    unlock_cond(&huge_alloc_lock);
  }

  /// Return the total amount of memory allocated to the user
  inline size_t get_stat_user_alloc_tot() {
    lock_cond(&huge_alloc_lock);
    size_t ret = huge_alloc->get_stat_user_alloc_tot();
    unlock_cond(&huge_alloc_lock);
    return ret;
  }

 private:
  // Methods to bury server-side request and response MsgBuffers. Client-side
  // request and response MsgBuffers are owned by user apps, so eRPC doesn't
  // free their backing memory.

  /**
   * @brief Bury a server sslot's response MsgBuffer (i.e., sslot->tx_msgbuf).
   * This is done in the foreground thread after receiving a packet for the
   * next request.
   *
   * This does not fully validate the MsgBuffer, since we don't want to
   * conditionally bury only initialized MsgBuffers.
   *
   * The server's response MsgBuffers are always backed by dynamic memory, since
   * even prealloc response MsgBuffers are non-fake: the \p prealloc_used field
   * is used to decide if we need to free memory.
   */
  inline void bury_resp_msgbuf_server_st(SSlot *sslot) {
    assert(in_dispatch());

    // Free the response MsgBuffer iff it is not preallocated
    if (unlikely(!sslot->prealloc_used)) {
      MsgBuffer *tx_msgbuf = sslot->tx_msgbuf;
      free_msg_buffer(*tx_msgbuf);
      // Need not nullify tx_msgbuf->buffer.buf: we'll just nullify tx_msgbuf
    }

    sslot->tx_msgbuf = nullptr;
  }

  /**
   * @brief Bury a server sslot's request MsgBuffer. This is done in
   * enqueue_response(), so only in the foreground thread.
   *
   * This does not fully validate the MsgBuffer, since we don't want to
   * conditinally bury only initialized MsgBuffers.
   */
  inline void bury_req_msgbuf_server_st(SSlot *sslot) {
    MsgBuffer &req_msgbuf = sslot->server_info.req_msgbuf;
    if (unlikely(req_msgbuf.is_dynamic())) {
      free_msg_buffer(req_msgbuf);
      req_msgbuf.buffer.buf = nullptr;  // Mark invalid for future
    }

    req_msgbuf.buf = nullptr;
  }

  //
  // Session management API (rpc_sm_api.cc)
  //

 public:
  /**
   * @brief Create a session and initiate session connection. This function
   * can only be called from the creator thread.
   *
   * @return The local session number (>= 0) of the session if creation succeeds
   * and the connect request is sent, negative errno otherwise.
   *
   * A callback of type \p kConnected or \p kConnectFailed will be invoked if
   * this call is successful.
   *
   * @param remote_uri The remote Nexus's URI, formatted as hostname:udp_port
   */
  int create_session(std::string remote_uri, uint8_t rem_rpc_id) {
    return create_session_st(remote_uri, rem_rpc_id);
  }

  /**
   * @brief Disconnect and destroy a client session. The session should not
   * be used by the application after this function is called. This functio
   * can only be called from the creator thread.
   *
   * @param session_num A session number returned from a successful
   * create_session()
   *
   * @return 0 if (a) the session disconnect packet was sent, and the disconnect
   * callback will be invoked later. Negative errno if the session cannot be
   * disconnected.
   */
  int destroy_session(int session_num) {
    return destroy_session_st(session_num);
  }

  /// Return the number of active server or client sessions. This function
  /// can be called only from the creator thread.
  size_t num_active_sessions() { return num_active_sessions_st(); }

  /// Return true iff this session is connected. The session must not have
  /// been disconnected.
  bool is_connected(int session_num) const {
    return session_vec[static_cast<size_t>(session_num)]->is_connected();
  }

  /// Return the Timely instance for a connected session. Expert use only.
  Timely *get_timely(int session_num) {
    Session *session = session_vec[static_cast<size_t>(session_num)];
    return &session->client_info.cc.timely;
  }

  /// Return the physical link bandwidth (bytes per second)
  size_t get_bandwidth() const { return transport->get_bandwidth(); }

  /// Return the number of retransmissions for a connected session
  size_t get_num_re_tx(int session_num) const {
    Session *session = session_vec[static_cast<size_t>(session_num)];
    return session->client_info.num_re_tx;
  }

  /// Reset the number of retransmissions for a connected session
  void reset_num_re_tx(int session_num) {
    Session *session = session_vec[static_cast<size_t>(session_num)];
    session->client_info.num_re_tx = 0;
  }

  /// Return the Timing Wheel for this Rpc. Expert use only.
  TimingWheel *get_wheel() { return wheel; }

 private:
  int create_session_st(std::string remote_uri, uint8_t rem_rpc_id);
  int destroy_session_st(int session_num);
  size_t num_active_sessions_st();

  //
  // Session management helper functions
  //
 private:
  // rpc_sm_helpers.cc

  /// Process all session management packets in the hook's RX list
  void handle_sm_rx_st();

  /// Free a session's resources and mark it as null in the session vector.
  /// Only the MsgBuffers allocated by the Rpc layer are freed. The user is
  /// responsible for freeing user-allocated MsgBuffers.
  void bury_session_st(Session *);

  /// Send an SM packet. The packet's destination (i.e., client or server) is
  /// determined using the packet's type.
  void sm_pkt_udp_tx_st(const SmPkt &);

  /// Send a session management request for a client session. This includes
  /// saving retransmission information for the request. The SM request type is
  /// computed using the session state
  void send_sm_req_st(Session *);

  //
  // Session management packet handlers (rpc_connect_handlers.cc,
  // rpc_disconnect_handlers.cc, rpc_reset_handlers.cc)
  //
  void handle_connect_req_st(const SmPkt &);
  void handle_connect_resp_st(const SmPkt &);

  void handle_disconnect_req_st(const SmPkt &);
  void handle_disconnect_resp_st(const SmPkt &);

  /// Try to reset a client session. If this is not currently possible, the
  /// session state must be set to reset-in-progress.
  bool handle_reset_client_st(Session *session);

  /// Try to reset a server session. If this is not currently possible, the
  /// session state must be set to reset-in-progress.
  bool handle_reset_server_st(Session *session);

  //
  // Handle available ring entries
  //

  /// Return true iff there are sufficient ring entries available for a session
  bool have_ring_entries() const {
    return ring_entries_available >= kSessionCredits;
  }

  /// Allocate ring entries for one session
  void alloc_ring_entries() {
    assert(have_ring_entries());
    ring_entries_available -= kSessionCredits;
  }

  /// Free ring entries allocated for one session
  void free_ring_entries() {
    ring_entries_available += kSessionCredits;
    assert(ring_entries_available <= Transport::kNumRxRingEntries);
  }

  //
  // Datapath helpers
  //

  /// Convert a response packet number to its index in the response MsgBuffer
  static inline size_t resp_ntoi(size_t pkt_num, size_t num_req_pkts) {
    return pkt_num - (num_req_pkts - 1);
  }

  /// Return true iff a packet received by a client is in order. This must be
  /// only a few instructions.
  inline size_t in_order_client(const SSlot *sslot, const pkthdr_t *pkthdr) {
    // Counters for pkthdr's request number are valid only if req numbers match
    if (unlikely(pkthdr->req_num != sslot->cur_req_num)) return false;

    const auto &ci = sslot->client_info;
    if (unlikely(pkthdr->pkt_num != ci.num_rx)) return false;

    // Ignore spurious packets received as a consequence of rollback:
    // 1. We've only sent pkts up to (ci.num_tx - 1). Ignore later packets.
    // 2. Ignore if the corresponding client packet for pkthdr is still in wheel
    if (unlikely(pkthdr->pkt_num >= ci.num_tx)) return false;

    if (kCcPacing && unlikely(ci.in_wheel[pkthdr->pkt_num % kSessionCredits])) {
      pkt_loss_stats.still_in_wheel_during_retx++;
      return false;
    }

    return true;
  }

  /// Return the total number of packets sent on the wire by one RPC endpoint.
  /// The client must have received the first response packet to call this.
  static inline size_t wire_pkts(MsgBuffer *req_msgbuf,
                                 MsgBuffer *resp_msgbuf) {
    return req_msgbuf->num_pkts + resp_msgbuf->num_pkts - 1;
  }

  /// Return true iff this sslot needs to send more request packets
  static inline bool req_pkts_pending(SSlot *sslot) {
    return sslot->client_info.num_tx < sslot->tx_msgbuf->num_pkts;
  }

  /// Return true iff it's currently OK to bypass the wheel for this request
  inline bool can_bypass_wheel(SSlot *sslot) const {
    if (!kCcPacing) return true;
    if (kTesting) return faults.hard_wheel_bypass;
    if (kCcOptWheelBypass) {
      return sslot->client_info.wheel_count == 0 &&
             sslot->session->is_uncongested();
    }
    return false;
  }

  /// Complete transmission for all packets in the Rpc's TX batch and the
  /// transport's DMA queue
  void drain_tx_batch_and_dma_queue() {
    if (tx_batch_i > 0) do_tx_burst_st();
    transport->tx_flush();
  }

  /// Add an RPC slot to the list of active RPCs
  inline void add_to_active_rpc_list(SSlot &sslot) {
    SSlot *prev_tail = active_rpcs_tail_sentinel.client_info.prev;

    prev_tail->client_info.next = &sslot;
    sslot.client_info.prev = prev_tail;

    sslot.client_info.next = &active_rpcs_tail_sentinel;
    active_rpcs_tail_sentinel.client_info.prev = &sslot;
  }

  /// Delete an active RPC slot from the list of active RPCs
  inline void delete_from_active_rpc_list(SSlot &sslot) {
    sslot.client_info.prev->client_info.next = sslot.client_info.next;
    sslot.client_info.next->client_info.prev = sslot.client_info.prev;
  }

  // rpc_kick.cc

  /// Enqueue client packets for a sslot that has at least one credit and
  /// request packets to send. Packets may be added to the timing wheel or the
  /// TX burst; credits are used in both cases.
  void kick_req_st(SSlot *);

  /// Enqueue client packets for a sslot that has at least one credit and
  /// RFR packets to send. Packets may be added to the timing wheel or the
  /// TX burst; credits are used in both cases.
  void kick_rfr_st(SSlot *);

  // rpc_req.cc
 public:
  /**
   * @brief Try to enqueue a request for transmission.
   *
   * If a session slot is available for this session, the request will be
   * enqueued. If this call succeeds, eRPC owns \p msg_buffer until the request
   * completes (i.e., the continuation is invoked).
   *
   * @param session_num The session number to send the request on
   * @param req_type The type of the request
   * @param req_msgbuf The user-created MsgBuffer containing the request payload
   * but not packet headers
   *
   * @param cont_func The continuation function for this request
   * @tag The tag for this request
   * @cont_etid The eRPC thread ID of the background thread to run the
   * continuation on. The default value of \p kInvalidBgETid means that the
   * continuation runs in the foreground. This argument is meant only for
   * internal use by eRPC (i.e., user calls must ignore it).
   *
   * @return Currently this function always returns 0
   */
  void enqueue_request(int session_num, uint8_t req_type, MsgBuffer *req_msgbuf,
                       MsgBuffer *resp_msgbuf, erpc_cont_func_t cont_func,
                       size_t tag, size_t cont_etid = kInvalidBgETid);

 private:
  /// Process a single-packet request message. Using (const pkthdr_t *) instead
  /// of (pkthdr_t *) is messy because of fake MsgBuffer constructor.
  void process_small_req_st(SSlot *, pkthdr_t *);

  /// Process a packet for a multi-packet request
  void process_large_req_one_st(SSlot *, const pkthdr_t *);

  // rpc_resp.cc
 public:
  /// Enqueue a response for transmission at the server
  void enqueue_response(ReqHandle *req_handle);

  /// From a continuation, release ownership of a response handle. The response
  /// MsgBuffer is owned by the app and shouldn't be freed.
  inline void release_response(RespHandle *resp_handle) {
    // When called from a background thread, enqueue to the foreground thread
    if (unlikely(!in_dispatch())) {
      bg_queues.release_response.unlocked_push(resp_handle);
      return;
    }

    // If we're here, we're in the dispatch thread
    SSlot *sslot = static_cast<SSlot *>(resp_handle);
    assert(sslot->tx_msgbuf == nullptr);  // Response was received previously

    Session *session = sslot->session;
    assert(session != nullptr && session->is_client());
    session->client_info.sslot_free_vec.push_back(sslot->index);

    if (!session->client_info.enq_req_backlog.empty()) {
      // We just got a new sslot, and we should have no more if there's backlog
      assert(session->client_info.sslot_free_vec.size() == 1);
      enq_req_args_t &args = session->client_info.enq_req_backlog.front();
      enqueue_request(args.session_num, args.req_type, args.req_msgbuf,
                      args.resp_msgbuf, args.cont_func, args.tag,
                      args.cont_etid);
      session->client_info.enq_req_backlog.pop();
    }
  }

 private:
  /**
   * @brief Process a single-packet response
   * @param rx_tsc The timestamp at which this packet was received
   */
  void process_resp_one_st(SSlot *, const pkthdr_t *, size_t rx_tsc);

  //
  // Event loop
  //

 public:
  /// Run the event loop for \p timeout_ms milliseconds
  inline void run_event_loop(size_t timeout_ms) {
    run_event_loop_timeout_st(timeout_ms);
  }

  /// Run the event loop once
  inline void run_event_loop_once() { run_event_loop_do_one_st(); }

 private:
  /// Implementation of the run_event_loop(timeout) API function
  void run_event_loop_timeout_st(size_t timeout_ms);

  /// Actually run one iteration of the event loop
  void run_event_loop_do_one_st();

 private:
  /// Return true iff a packet should be dropped
  inline bool roll_pkt_drop() {
    static constexpr uint32_t billion = 1000000000;
    return ((fast_rand.next_u32() % billion) < faults.pkt_drop_thresh_billion);
  }

  /**
   * @brief Enqueue a data packet from sslot's tx_msgbuf for tx_burst
   * @param pkt_idx The index of the packet in tx_msgbuf, not packet number
   */
  inline void enqueue_pkt_tx_burst_st(SSlot *sslot, size_t pkt_idx,
                                      size_t *tx_ts) {
    assert(in_dispatch());
    const MsgBuffer *tx_msgbuf = sslot->tx_msgbuf;
    assert(tx_msgbuf->is_req() || tx_msgbuf->is_resp());

    Transport::tx_burst_item_t &item = tx_burst_arr[tx_batch_i];
    item.routing_info = sslot->session->remote_routing_info;
    item.msg_buffer = const_cast<MsgBuffer *>(tx_msgbuf);
    item.pkt_idx = pkt_idx;
    if (kCcRTT) item.tx_ts = tx_ts;

    if (kTesting) {
      item.drop = roll_pkt_drop();
      testing.pkthdr_tx_queue.push(*tx_msgbuf->get_pkthdr_n(pkt_idx));
    }

    LOG_TRACE("Rpc %u, lsn %u (%s): TX %s. Slot %s.%s\n", rpc_id,
              sslot->session->local_session_num,
              sslot->session->get_remote_hostname().c_str(),
              tx_msgbuf->get_pkthdr_str(pkt_idx).c_str(),
              sslot->progress_str().c_str(), item.drop ? " Drop." : "");

    tx_batch_i++;
    if (tx_batch_i == TTr::kPostlist) do_tx_burst_st();
  }

  /// Enqueue a control packet for tx_burst. ctrl_msgbuf can be reused after
  /// (2 * unsig_batch) calls to this function.
  inline void enqueue_hdr_tx_burst_st(SSlot *sslot, MsgBuffer *ctrl_msgbuf,
                                      size_t *tx_ts) {
    assert(in_dispatch());
    assert(ctrl_msgbuf->is_expl_cr() || ctrl_msgbuf->is_rfr());

    Transport::tx_burst_item_t &item = tx_burst_arr[tx_batch_i];
    item.routing_info = sslot->session->remote_routing_info;
    item.msg_buffer = ctrl_msgbuf;
    item.pkt_idx = 0;
    if (kCcRTT) item.tx_ts = tx_ts;

    if (kTesting) {
      item.drop = roll_pkt_drop();
      testing.pkthdr_tx_queue.push(*ctrl_msgbuf->get_pkthdr_0());
    }

    LOG_TRACE("Rpc %u, lsn %u (%s): TX %s. Slot %s.%s.\n", rpc_id,
              sslot->session->local_session_num,
              sslot->session->get_remote_hostname().c_str(),
              ctrl_msgbuf->get_pkthdr_str(0).c_str(),
              sslot->progress_str().c_str(), item.drop ? " Drop." : "");

    tx_batch_i++;
    if (tx_batch_i == TTr::kPostlist) do_tx_burst_st();
  }

  /// Enqueue a request packet to the timing wheel
  inline void enqueue_wheel_req_st(SSlot *sslot, size_t pkt_num) {
    const size_t pkt_idx = pkt_num;
    size_t pktsz = sslot->tx_msgbuf->get_pkt_size<TTr::kMaxDataPerPkt>(pkt_idx);
    size_t ref_tsc = dpath_rdtsc();
    size_t desired_tx_tsc = sslot->session->cc_getupdate_tx_tsc(ref_tsc, pktsz);

    LOG_CC("Rpc %u: lsn/req/pkt %u/%zu/%zu, REQ wheeled for %.3f us.\n", rpc_id,
           sslot->session->local_session_num, sslot->cur_req_num, pkt_num,
           to_usec(desired_tx_tsc - creation_tsc, freq_ghz));

    wheel->insert(wheel_ent_t(sslot, pkt_num), ref_tsc, desired_tx_tsc);
    sslot->client_info.in_wheel[pkt_num % kSessionCredits] = true;
    sslot->client_info.wheel_count++;
  }

  /// Enqueue an RFR packet to the timing wheel
  inline void enqueue_wheel_rfr_st(SSlot *sslot, size_t pkt_num) {
    const size_t pkt_idx = resp_ntoi(pkt_num, sslot->tx_msgbuf->num_pkts);
    const MsgBuffer *resp_msgbuf = sslot->client_info.resp_msgbuf;
    size_t pktsz = resp_msgbuf->get_pkt_size<TTr::kMaxDataPerPkt>(pkt_idx);
    size_t ref_tsc = dpath_rdtsc();
    size_t desired_tx_tsc = sslot->session->cc_getupdate_tx_tsc(ref_tsc, pktsz);

    LOG_CC("Rpc %u: lsn/req/pkt %u/%zu/%zu, RFR wheeled for %.3f us.\n", rpc_id,
           sslot->session->local_session_num, sslot->cur_req_num, pkt_num,
           to_usec(desired_tx_tsc - creation_tsc, freq_ghz));

    wheel->insert(wheel_ent_t(sslot, pkt_num), ref_tsc, desired_tx_tsc);
    sslot->client_info.in_wheel[pkt_num % kSessionCredits] = true;
    sslot->client_info.wheel_count++;
  }

  /// Transmit packets in the TX batch
  inline void do_tx_burst_st() {
    assert(in_dispatch());
    assert(tx_batch_i > 0);

    // Measure TX burst size
    dpath_stat_inc(dpath_stats.tx_burst_calls, 1);
    dpath_stat_inc(dpath_stats.pkts_tx, tx_batch_i);

    if (kCcRTT) {
      size_t batch_tsc = 0;
      if (kCcOptBatchTsc) batch_tsc = dpath_rdtsc();

      for (size_t i = 0; i < tx_batch_i; i++) {
        if (tx_burst_arr[i].tx_ts != nullptr) {
          *tx_burst_arr[i].tx_ts = kCcOptBatchTsc ? batch_tsc : dpath_rdtsc();
        }
      }
    }

    transport->tx_burst(tx_burst_arr, tx_batch_i);
    tx_batch_i = 0;
  }

  //
  // rpc_rx.cc
  //

 private:
  /// Return a credit to this session
  inline void bump_credits(Session *session) {
    assert(session->is_client());
    assert(session->client_info.credits < kSessionCredits);
    session->client_info.credits++;
  }

  /// Copy the data from a packet to a MsgBuffer at a packet index
  inline void copy_data_to_msgbuf(MsgBuffer *msgbuf, size_t pkt_idx,
                                  const pkthdr_t *pkthdr) {
    size_t offset = pkt_idx * TTr::kMaxDataPerPkt;
    size_t to_copy = std::min(TTr::kMaxDataPerPkt, pkthdr->msg_size - offset);
    memcpy(&msgbuf->buf[offset], pkthdr + 1, to_copy);  // From end of pkthdr
  }

  /**
   * @brief Process received packets and post RECVs. The ring buffers received
   * from `rx_burst` must not be used after new RECVs are posted.
   *
   * Although none of the polled RX ring buffers can be overwritten by the
   * NIC until we send at least one response/CR packet back, we do not control
   * the order or time at which these packets are sent, due to constraints like
   * session credits and packet pacing.
   */
  void process_comps_st();

  /**
   * @brief Submit a work item to a background thread
   *
   * @param sslot Session sslot with a complete request or response MsgBuffer
   * @param wi_type Work item type (request or response)
   * @param bg_etid eRPC thread ID of the background thread to submit to
   */
  void submit_background_st(SSlot *sslot, Nexus::BgWorkItemType wi_type,
                            size_t bg_etid = kMaxBgThreads);

  //
  // Queue handlers (rpc_queues.cc)
  //
 private:
  /// Try to transmit request packets from sslots that are stalled for credits.
  void process_credit_stall_queue_st();

  /// Process the wheel. We have already paid credits for sslots in the wheel.
  void process_wheel_st();

  /// Process the requests enqueued by background threads
  void process_bg_queues_enqueue_request_st();

  /// Process the responses enqueued by background threads
  void process_bg_queues_enqueue_response_st();

  /// Process the responses freed by background threads
  void process_bg_queues_release_response_st();

  //
  // Fault injection
  //
 public:
  /**
   * @brief Inject a fault that always fails all routing info resolution
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_fail_resolve_rinfo_st();

  /**
   * @brief Set the TX packet drop probability for this Rpc
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_set_pkt_drop_prob_st(double pkt_drop_prob);

 private:
  /**
   * @brief Check if the caller can inject faults
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_check_ok() const;

  //
  // Packet loss handling (rpc_pkt_loss.cc)
  //

  /// Scan sessions and requests for session management and datapath packet loss
  void pkt_loss_scan_st();

  /// Retransmit packets for an sslot for which we suspect a packet loss
  void pkt_loss_retransmit_st(SSlot *sslot);

  //
  // Misc public functions
  //

 public:
  /// Set this Rpc's context
  inline void set_context(void *_context) {
    rt_assert(context == nullptr, "Cannot reset non-null Rpc context");
    context = _context;
  }

  /// Retrieve this Rpc's hugepage allocator. For expert use only.
  inline HugeAlloc *get_huge_alloc() const {
    rt_assert(nexus->num_bg_threads == 0,
              "Cannot extract allocator because background threads exist.");
    return huge_alloc;
  }

  /**
   * @brief Return the number of packets required for \p data_size data bytes.
   *
   * This should avoid division if \p data_size fits in one packet.
   * For \p data_size = 0, the return value need not be 0, i.e., it can be 1.
   */
  static size_t data_size_to_num_pkts(size_t data_size) {
    if (data_size <= TTr::kMaxDataPerPkt) return 1;
    return (data_size + TTr::kMaxDataPerPkt - 1) / TTr::kMaxDataPerPkt;
  }

  /// Return the maximum *data* size in one packet for the (private) transport
  static inline constexpr size_t get_max_data_per_pkt() {
    return TTr::kMaxDataPerPkt;
  }

  /// Return the (private) transport's RX ring size
  static inline constexpr size_t get_num_rx_ring_entries() {
    return TTr::kNumRxRingEntries;
  }

  /// Return the hostname of the remote endpoint for a connected session
  std::string get_remote_hostname(int session_num) const {
    return session_vec[static_cast<size_t>(session_num)]->get_remote_hostname();
  }

  /// Return the maximum number of sessions supported
  static inline constexpr size_t get_max_num_sessions() {
    return Transport::kNumRxRingEntries / kSessionCredits;
  }

  /// Return the maximum message *data* size that can be sent
  static inline size_t get_max_msg_size() { return kMaxMsgSize; }

  /// Return the ID of this Rpc object
  inline uint8_t get_rpc_id() const { return rpc_id; }

  /// Return true iff the caller is running in a background thread
  inline bool in_background() const { return !in_dispatch(); }

  /// Return the eRPC thread ID of the caller
  inline size_t get_etid() const { return tls_registry->get_etid(); }

  /// Return RDTSC frequency in GHz
  inline double get_freq_ghz() const { return freq_ghz; }

  /// Return the number of seconds elapsed since this Rpc was created
  double sec_since_creation() {
    return to_sec(rdtsc() - creation_tsc, nexus->freq_ghz);
  }

  /// Return the average number of packets received in a call to rx_burst
  double get_avg_rx_batch() {
    if (!kDatapathStats || dpath_stats.rx_burst_calls == 0) return -1.0;
    return dpath_stats.pkts_rx * 1.0 / dpath_stats.rx_burst_calls;
  }

  /// Return the average number of packets sent in a call to tx_burst
  double get_avg_tx_batch() {
    if (!kDatapathStats || dpath_stats.tx_burst_calls == 0) return -1.0;
    return dpath_stats.pkts_tx * 1.0 / dpath_stats.tx_burst_calls;
  }

  /// Reset all datapath stats to zero
  void reset_dpath_stats() { memset(&dpath_stats, 0, sizeof(dpath_stats)); }

  //
  // Misc private functions
  //

 private:
  /// Return true iff we're currently running in this Rpc's creator thread
  inline bool in_dispatch() const { return get_etid() == creator_etid; }

  /// Return true iff a user-provided session number is in the session vector
  inline bool is_usr_session_num_in_range_st(int session_num) const {
    assert(in_dispatch());
    return session_num >= 0 &&
           static_cast<size_t>(session_num) < session_vec.size();
  }

  /// Lock the mutex if the Rpc is accessible from multiple threads
  inline void lock_cond(std::mutex *mutex) {
    if (unlikely(multi_threaded)) mutex->lock();
  }

  /// Unlock the mutex if the Rpc is accessible from multiple threads
  inline void unlock_cond(std::mutex *mutex) {
    if (unlikely(multi_threaded)) mutex->unlock();
  }

  /**
   * @brief Perform a Timely rate update on receiving the explict CR or response
   * packet for this triggering packet number
   *
   * @param sslot The request sslot for which a packet is received
   * @param pkt_num The received packet's packet number
   * @param Time at which the explicit CR or response packet was received
   */
  inline void update_timely_rate(SSlot *sslot, size_t pkt_num, size_t rx_tsc) {
    size_t rtt_tsc =
        rx_tsc - sslot->client_info.tx_ts[pkt_num % kSessionCredits];
    // This might use Timely bypass
    sslot->session->client_info.cc.timely.update_rate(rx_tsc, rtt_tsc);
  }

  // rpc_cr.cc

  /**
   * @brief Enqueue an explicit credit return
   *
   * @param sslot The session slot to send the explicit CR for
   * @param req_pkthdr The packet header of the request packet that triggered
   * this explicit CR. The packet number of req_pkthdr is copied to the CR.
   */
  void enqueue_cr_st(SSlot *sslot, const pkthdr_t *req_pkthdr);

  /**
   * @brief Process an explicit credit return packet
   * @param rx_tsc Timestamp at which the packet was received
   */
  void process_expl_cr_st(SSlot *, const pkthdr_t *, size_t rx_tsc);

  // rpc_rfr.cc

  /**
   * @brief Enqueue a request-for-response. This doesn't modify credits or
   * sslot's num_tx.
   *
   * @param sslot The session slot to send the RFR for
   * @param req_pkthdr The packet header of the response packet that triggered
   * this RFR. Since one response packet can trigger multiple RFRs, the RFR's
   * packet number should be computed from num_tx, not from resp_pkthdr.
   */
  void enqueue_rfr_st(SSlot *sslot, const pkthdr_t *resp_pkthdr);

  /// Process a request-for-response
  void process_rfr_st(SSlot *, const pkthdr_t *);

 public:
  // Hooks for apps to modify eRPC behavior

  /// Retry session connection if the remote RPC ID was invalid. This usually
  /// happens when the server RPC thread has not started.
  bool retry_connect_on_invalid_rpc_id = false;

 private:
  // Constructor args
  Nexus *nexus;
  void *context;  ///< The application context
  const uint8_t rpc_id;
  const sm_handler_t sm_handler;
  const uint8_t phy_port;  ///< Zero-based physical port specified by app
  const size_t numa_node;
  const size_t creation_tsc;  ///< Timestamp of creation of this Rpc endpoint

  // Derived
  const bool multi_threaded;    ///< True iff there are background threads
  const double freq_ghz;        ///< RDTSC frequency, derived from Nexus
  const size_t rpc_rto_cycles;  ///< RPC RTO in cycles
  const size_t rpc_pkt_loss_scan_cycles;  ///< Packet loss scan frequency

  /// A copy of the request/response handlers from the Nexus. We could use
  /// a pointer instead, but an array is faster.
  const std::array<ReqFunc, kReqTypeArraySize> req_func_arr;

  // Rpc metadata
  size_t creator_etid;        ///< eRPC thread ID of the creator thread
  TlsRegistry *tls_registry;  ///< Pointer to the Nexus's thread-local registry

  // Sessions

  /// The append-only list of session pointers, indexed by session number.
  /// Disconnected sessions are denoted by null pointers. This grows as sessions
  /// are repeatedly connected and disconnected, but 8 bytes per session is OK.
  std::vector<Session *> session_vec;

  // Transport
  TTr *transport = nullptr;  ///< The unreliable transport

  /// Current number of ring buffers available to use for sessions
  size_t ring_entries_available = TTr::kNumRxRingEntries;

  Transport::tx_burst_item_t tx_burst_arr[TTr::kPostlist];  ///< Tx batch info
  size_t tx_batch_i = 0;  ///< The batch index for TX burst array

  /// On calling rx_burst(), Transport fills-in packet buffer pointers into the
  /// RX ring. Some transports such as InfiniBand and Raw reuse RX ring packet
  /// buffers in a circular order, so the ring's pointers remain unchanged
  /// after initialization. Other transports (e.g., DPDK) update rx_ring on
  /// every successful rx_burst.
  uint8_t *rx_ring[TTr::kNumRxRingEntries];
  size_t rx_ring_head = 0;  ///< Current unused RX ring buffer

  std::vector<SSlot *> stallq;  ///< Req sslots stalled for credits

  size_t ev_loop_tsc;  ///< TSC taken at each iteration of the ev loop

  // Packet loss
  size_t pkt_loss_scan_tsc;  ///< Timestamp of the previous scan for lost pkts

  /// The doubly-linked list of active RPCs. An RPC slot is added to this list
  /// when the request is enqueued. The slot is deleted from this list when its
  /// continuation is invoked or queued to a background thread.
  /// Having permanent root and tail sentinels allows adding and deleting slots
  /// from the list without conditional statements.
  SSlot active_rpcs_root_sentinel, active_rpcs_tail_sentinel;

  // Allocator
  HugeAlloc *huge_alloc = nullptr;  ///< This thread's hugepage allocator
  std::mutex huge_alloc_lock;       ///< A lock to guard the huge allocator

  MsgBuffer ctrl_msgbufs[2 * TTr::kUnsigBatch];  ///< Buffers for RFR/CR
  size_t ctrl_msgbuf_head = 0;
  FastRand fast_rand;  ///< A fast random generator

  // Cold members live below, in order of coolness

  /// The timing-wheel rate limiter. Packets in the wheel have consumed credits,
  /// but not bumped the num_tx counter.
  TimingWheel *wheel;

  /// Queues for datapath API requests from background threads
  struct {
    MtQueue<enq_req_args_t> enqueue_request;
    MtQueue<ReqHandle *> enqueue_response;
    MtQueue<RespHandle *> release_response;
  } bg_queues;

  // Misc
  SlowRand slow_rand;  ///< A slow random generator for "real" randomness
  UDPClient<SmPkt> udp_client;  ///< UDP endpoint used to send SM packets
  Nexus::Hook nexus_hook;       ///< A hook shared with the Nexus

  /// To avoid allocating a new session on receiving a duplicate session
  /// connect request, the server remembers all (XXX) the unique connect
  /// requests it has received. To accomplish this, the client generates a
  /// globally-unique (XXX) token for the first copy of its connect request.
  /// The server saves maps this token to the index of the allocated session.
  std::map<conn_req_uniq_token_t, uint16_t> conn_req_token_map;

  /// Sessions for which a session management request is outstanding
  std::set<uint16_t> sm_pending_reqs;

  /// All the faults that can be injected into eRPC for testing
  struct {
    bool fail_resolve_rinfo = false;  ///< Fail routing info resolution
    bool hard_wheel_bypass = false;   ///< Wheel bypass regardless of congestion
    double pkt_drop_prob = 0.0;       ///< Probability of dropping an RPC packet

    /// Derived: Drop packet iff urand[0, ..., one billion] is smaller than this
    uint32_t pkt_drop_thresh_billion = 0;
  } faults;

  // Additional members for testing
  struct {
    FixedQueue<pkthdr_t, kSessionCredits> pkthdr_tx_queue;
  } testing;

  /// File for dispatch thread trace output. This is used indirectly by
  /// LOG_TRACE and other macros.
  FILE *trace_file;

  /// Datapath stats that can be disabled
  struct {
    size_t ev_loop_calls = 0;
    size_t pkts_tx = 0;
    size_t tx_burst_calls = 0;
    size_t pkts_rx = 0;
    size_t rx_burst_calls = 0;
  } dpath_stats;

 public:
  struct {
    size_t num_re_tx = 0;  /// Total retransmissions across all sessions

    /// Number of times we could not retranmsit a request, or we had to drop
    /// a received packet, because a request reference was still in the wheel.
    size_t still_in_wheel_during_retx = 0;
  } pkt_loss_stats;
};

// This goes at the end of every Rpc implementation file to force compilation
#define FORCE_COMPILE_TRANSPORTS template class Rpc<CTransport>;
}  // namespace erpc
