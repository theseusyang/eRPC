#include "common.h"
#include "nexus.h"
#include "ops.h"
#include "session.h"
#include "util/mt_queue.h"

namespace erpc {

void Nexus::bg_thread_func(BgThreadCtx ctx) {
  ctx.tls_registry->init();  // Initialize thread-local variables

  // The BgWorkItem request list can be indexed using the background thread's
  // index in the Nexus, or its eRPC TID.
  assert(ctx.bg_thread_index == ctx.tls_registry->get_etid());
  LOG_INFO("eRPC Nexus: Background thread %zu running. Tiny TID = %zu.\n",
           ctx.bg_thread_index, ctx.tls_registry->get_etid());

  while (*ctx.kill_switch == false) {
    if (ctx.bg_req_queue->size == 0) {
      // Try again later
      usleep(1);
      continue;
    }

    while (ctx.bg_req_queue->size > 0) {
      BgWorkItem wi = ctx.bg_req_queue->unlocked_pop();
      SSlot *s = wi.sslot;

      LOG_TRACE(
          "eRPC Background: Background thread %zu running %s for Rpc %u."
          "Request number = %zu.\n",
          ctx.bg_thread_index, wi.is_req() ? "request handler" : "continuation",
          wi.rpc_id, s->cur_req_num);

      if (wi.is_req()) {
        uint8_t req_type = s->server_info.req_msgbuf.get_req_type();
        const ReqFunc &req_func = ctx.req_func_arr->at(req_type);
        req_func.req_func(static_cast<ReqHandle *>(s), wi.context);
      } else {
        wi.sslot->client_info.cont_func(static_cast<RespHandle *>(s),
                                        wi.context, s->client_info.tag);
      }
    }
  }

  LOG_INFO("eRPC Nexus: Background thread %zu exiting.\n", ctx.bg_thread_index);
  return;
}

}  // End erpc
