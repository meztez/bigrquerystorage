#include "bqs_client.h"

#include <fstream>
#include <iterator>

#if __has_include(<grpcpp/version_info.h>)
#include <grpcpp/version_info.h>
#if GRPC_CPP_VERSION_MAJOR >= 1 & GRPC_CPP_VERSION_MINOR >= 66
#define ABSL_LOGGING 1
#endif
#endif

#ifdef ABSL_LOGGING
#include <absl/base/log_severity.h>
#include <absl/base/no_destructor.h>
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#else
#include <grpc/support/log.h>
#endif

namespace {

#ifndef ABSL_LOGGING
void bqs_default_log(gpr_log_func_args* args) {
  Rcpp::Rcerr << args->message << std::endl;
}
#else
class RLogSink : public absl::LogSink {
public:
  void Send(const absl::LogEntry& entry) override {
    Rcpp::Rcerr << entry.text_message_with_prefix_and_newline() << std::endl;
  }
};
#endif

std::string readfile(const std::string& filename) {
  if (filename.empty()) return std::string();
  std::ifstream ifs(filename);
  return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

std::shared_ptr<grpc::ChannelCredentials> bqs_ssl(const std::string& root_certificate) {
  grpc::SslCredentialsOptions ssl_options;
  if (!root_certificate.empty()) {
    ssl_options.pem_root_certs = root_certificate;
  }
  return grpc::SslCredentials(ssl_options);
}

std::shared_ptr<grpc::ChannelCredentials> bqs_composite_credentials(
    std::shared_ptr<grpc::ChannelCredentials> channel_cred,
    std::shared_ptr<grpc::CallCredentials> call_cred) {
  if (channel_cred && call_cred) {
    return grpc::CompositeChannelCredentials(channel_cred, call_cred);
  }
  return channel_cred;
}

} // namespace

// [[Rcpp::export(rng=false)]]
void bqs_set_log_verbosity(int severity) {
#ifdef ABSL_LOGGING
  absl::SetMinLogLevel(static_cast<absl::LogSeverityAtLeast>(severity));
#else
  gpr_set_log_verbosity(static_cast<gpr_log_severity>(severity));
#endif
}

// [[Rcpp::export(rng=false)]]
void bqs_init_logger() {
  static std::once_flag init_once;
  std::call_once(init_once, []() {
#ifdef ABSL_LOGGING
    absl::InitializeLog();
    static absl::NoDestructor<RLogSink> r_log_sink;
    absl::AddLogSink(r_log_sink.get());
#else
    gpr_set_log_function(bqs_default_log);
#endif
  });
  bqs_set_log_verbosity(2);
}

// [[Rcpp::export(rng=false)]]
std::string grpc_version() {
  return std::string(grpc_version_string()) + " " + grpc_g_stands_for();
}

BigQueryReadClient::BigQueryReadClient(std::shared_ptr<grpc::ChannelCredentials> cred,
                                       const std::string& target,
                                       const std::string& service_configuration)
    : cred_(std::move(cred)),
      target_(target),
      service_configuration_(service_configuration) {
  EnsureChannels(24, true);
  EnsureWriteChannels(12, true);
}

template <typename ServiceT>
void BigQueryReadClient::EnsureChannelPool(
    size_t n,
    bool warm_connect,
    bool is_write,
    std::vector<std::shared_ptr<grpc::Channel>>* channels,
    std::vector<std::unique_ptr<typename ServiceT::Stub>>* stubs) {
  while (stubs->size() < n) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(134217728);
    args.SetServiceConfigJSON(service_configuration_);
    args.SetInt("grpc.use_local_subchannel_pool", 1);
    args.SetInt("grpc.http2.bdp_probe", 1);
    if (is_write) {
      args.SetMaxSendMessageSize(134217728);
      args.SetInt("grpc.http2.write_buffer_size", 4 * 1024 * 1024);
      args.SetInt("bigrquerystorage.write_channel_id", static_cast<int>(stubs->size()));
    } else {
      args.SetInt("grpc.http2.lookahead_bytes", 32 * 1024 * 1024);
      args.SetInt("bigrquerystorage.channel_id", static_cast<int>(stubs->size()));
    }
    auto channel = grpc::CreateCustomChannel(target_, cred_, args);
    if (warm_connect) channel->GetState(true);
    channels->push_back(channel);
    stubs->push_back(ServiceT::NewStub(channel));
  }
  if (warm_connect) {
    for (size_t i = 0; i < n && i < channels->size(); i++) {
      (*channels)[i]->GetState(true);
    }
  }
}

void BigQueryReadClient::EnsureChannels(size_t n, bool warm_connect) {
  EnsureChannelPool<BigQueryRead>(n, warm_connect, false, &read_channels_, &stubs_);
}

void BigQueryReadClient::EnsureWriteChannels(size_t n, bool warm_connect) {
  EnsureChannelPool<BigQueryWrite>(n, warm_connect, true, &write_channels_, &write_stubs_);
}

void BigQueryReadClient::InitContext(grpc::ClientContext* ctx,
                                     const std::string& request_params,
                                     int deadline_ms) const {
  if (deadline_ms > 0) {
    ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));
  }
  ctx->AddMetadata("x-goog-request-params", request_params);
  ctx->AddMetadata("x-goog-api-client", client_info_);
}

// [[Rcpp::export(rng=false)]]
SEXP bqs_client(std::string client_info,
                std::string service_configuration,
                std::string refresh_token = "",
                std::string access_token = "",
                std::string root_certificate = "",
                std::string target = "bigquerystorage.googleapis.com:443") {
  std::shared_ptr<grpc::ChannelCredentials> cred;
  if (!refresh_token.empty()) {
    cred = bqs_composite_credentials(
      bqs_ssl(readfile(root_certificate)),
      grpc::GoogleRefreshTokenCredentials(refresh_token));
  }
  if (!cred && !access_token.empty()) {
    cred = bqs_composite_credentials(
      bqs_ssl(readfile(root_certificate)),
      grpc::AccessTokenCredentials(access_token));
  }
  if (!cred) {
    cred = grpc::GoogleDefaultCredentials();
  }
  if (!cred) {
    Rcpp::stop("Could not create credentials.");
  }

  auto* client = new BigQueryReadClient(cred, target, readfile(service_configuration));
  client->SetClientInfo(client_info);
  return Rcpp::XPtr<BigQueryReadClient>(client, true);
}
