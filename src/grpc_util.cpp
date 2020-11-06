#include <Rcpp.h>
#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/impl/codegen/byte_buffer_reader.h>
#include <grpc/impl/codegen/log.h>
#include <grpc/slice.h>

using namespace Rcpp;

void rgpr_default_log(gpr_log_func_args* args) {
  Rcout << args->message << std::endl;
}

// [[Rcpp::export]]
void rpc_init(bool verbose = false) {

  // set logger
  gpr_set_log_function(rgpr_default_log);

  if (verbose) {
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
  } else {
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_ERROR);
  }
}


static void *tag(intptr_t i) { return (void *)i; }

RawVector sliceToRaw2(grpc_slice slice){

  int n = GRPC_SLICE_LENGTH(slice);

  char* data = grpc_slice_to_c_string(slice);

  gpr_log(GPR_DEBUG, "RGRPC: Slice2Raw: length: %i", n);

  RawVector out(n);
  for(int i = 0; i < n; i++)
    out[i] = (unsigned char) data[i];

  return out;
}

// [[Rcpp::export]]
RawVector rpc(CharacterVector server, CharacterVector endpoint, RawVector requestArg, CharacterVector metadata, int timeout = 5000) {

  // creds
  grpc_channel_credentials *creds = grpc_google_default_credentials_create(nullptr);

  if (creds != nullptr) {

    gpr_log(GPR_DEBUG, "%s", "Credentials created");

    //setup
    grpc_call *c;
    const char* target = server[0];
    const char* method = endpoint[0];

    grpc_completion_queue *cq = grpc_completion_queue_create_for_next(NULL);

    const grpc_slice server_slice = grpc_slice_from_copied_string(target);
    const grpc_slice method_slice = grpc_slice_from_copied_string(method);

    SEXP raw_ = requestArg;
    int len = requestArg.length();
    grpc_slice request_payload_slice = grpc_slice_from_copied_buffer((char*) RAW(raw_), len);


    const grpc_slice *sp = &server_slice;

    gpr_log(GPR_DEBUG, "Targeting %s with method %s", target, method);
    grpc_channel *channel = grpc_secure_channel_create(creds, target, NULL, NULL);

    gpr_timespec deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(timeout, GPR_TIMESPAN));

    gpr_log(GPR_DEBUG, "%s", "Create Call");
    c = grpc_channel_create_call(
      channel, NULL, GRPC_PROPAGATE_DEFAULTS, cq,
      method_slice,
      sp,
      deadline,
      NULL);


    gpr_log(GPR_DEBUG, "%s", "Making ops");
    // perform request
    grpc_op ops[6];
    grpc_op *op;

    int metadata_length = metadata.length() / 2;
    grpc_metadata *meta_c = new grpc_metadata[metadata_length];

    for(int i = 0; i < metadata_length; i++) {
      meta_c[i] = {grpc_slice_from_static_string(metadata[i * 2]),
          grpc_slice_from_static_string(metadata[i * 2 + 1]),
          0,
          {{nullptr, nullptr, nullptr, nullptr}}};
    }

    grpc_metadata_array initial_metadata_recv;
    grpc_metadata_array trailing_metadata_recv;
    grpc_metadata_array request_metadata_recv;
    grpc_byte_buffer *response_payload_recv;
    grpc_call_details call_details;
    grpc_status_code status;
    grpc_call_error error;
    grpc_event event;
    grpc_slice details;

    grpc_metadata_array_init(&initial_metadata_recv);
    grpc_metadata_array_init(&trailing_metadata_recv);
    grpc_metadata_array_init(&request_metadata_recv);
    grpc_call_details_init(&call_details);

    grpc_byte_buffer *request_payload =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);

    memset(ops, 0, sizeof(ops));
    op = ops;
    op->op = GRPC_OP_SEND_INITIAL_METADATA;
    op->data.send_initial_metadata.count = metadata_length;
    if (metadata_length > 0) {
      op->data.send_initial_metadata.metadata = meta_c;
    }
    else {
      op->data.send_initial_metadata.maybe_compression_level.is_set = false;
    }
    op->flags = 0;
    op->reserved = NULL;
    op++;
    op->op = GRPC_OP_SEND_MESSAGE;
    op->data.send_message.send_message = request_payload;
    op->flags = 0;
    op->reserved = NULL;
    op++;
    op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    op->flags = 0;
    op->reserved = NULL;
    op++;
    op->op = GRPC_OP_RECV_INITIAL_METADATA;
    op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
    op->flags = 0;
    op->reserved = NULL;
    op++;
    op->op = GRPC_OP_RECV_MESSAGE;
    op->data.recv_message.recv_message = &response_payload_recv;
    op->flags = 0;
    op->reserved = NULL;
    op++;
    op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
    op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
    op->data.recv_status_on_client.status = &status;
    op->data.recv_status_on_client.status_details = &details;
    op->flags = 0;
    op->reserved = NULL;
    op++;

    gpr_log(GPR_DEBUG, "%s", "Starting batch...");
    error = grpc_call_start_batch(c, ops, (size_t)(op - ops), tag(1), NULL);
    if (error != GRPC_CALL_OK) { // 0
      stop("gRPC c++ call start failed");
    }
    event = grpc_completion_queue_next(cq, deadline, NULL); //actually does the work
    if (event.type == GRPC_QUEUE_TIMEOUT) { // 1
      stop("gRPC c++ call timeout");
    }
    if (event.success == 0) {
      stop("gRPC c++ call error");
    }
    if (!response_payload_recv) {
      stop("No response from the gRPC server");
    }

    // Call Header, followed by optional Initial-Metadata, followed by zero or more Payload Messages
    gpr_log(GPR_DEBUG, "%s", "Read response Slice");
    grpc_byte_buffer_reader bbr;

    try {
      grpc_byte_buffer_reader_init(&bbr, response_payload_recv);
    }
    catch (std::exception& e) {
      gpr_log(GPR_ERROR, "RGRPC: Segfault %s", e.what());
      stop("Segfault in C++");
    }

    grpc_slice response_payload_slice = grpc_byte_buffer_reader_readall(&bbr);
    RawVector response_payload_raw = sliceToRaw2(response_payload_slice);

    //Cleanup

    // teardown
    grpc_completion_queue_shutdown(cq);
    grpc_completion_queue_destroy(cq);
    cq = NULL;
    grpc_channel_destroy(channel);
    channel = NULL;
    delete[] meta_c;


    grpc_slice_unref(details);
    grpc_metadata_array_destroy(&initial_metadata_recv);
    grpc_metadata_array_destroy(&trailing_metadata_recv);
    grpc_metadata_array_destroy(&request_metadata_recv);
    grpc_call_details_destroy(&call_details);
    grpc_call_unref(c);

    grpc_byte_buffer_destroy(request_payload);
    grpc_byte_buffer_destroy(response_payload_recv);

    grpc_channel_credentials_release(creds);
    creds = NULL;

    return response_payload_raw;

  } else {
    gpr_log(GPR_ERROR, "%s", "RGRPC: Credentials could not be instanciated using Google ADC Strategy");
    RawVector out(1);
    return out;
  }



}


//' Check grpc version
//' @return Version string and what g stands for
//' @export
// [[Rcpp::export]]
CharacterVector grpc_version() {
  return CharacterVector::create(grpc_version_string(), grpc_g_stands_for());
}

