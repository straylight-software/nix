// h2_parallel_test.cpp - Test parallel HTTP/2 fetching
// Blast all requests at once on a single connection

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <tls.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/tls.h"

int main(int argc, char* argv[]) {
  // Test URLs - first few files from dhall-kubernetes
  std::vector<std::string> urls = {
      "/dhall-lang/dhall-kubernetes/master/1.30/package.dhall",
      "/dhall-lang/dhall-kubernetes/master/1.30/schemas.dhall",
      "/dhall-lang/dhall-kubernetes/master/1.30/types.dhall",
      "/dhall-lang/dhall-kubernetes/master/1.30/typesUnion.dhall",
      "/dhall-lang/dhall-kubernetes/master/1.30/defaults.dhall",
  };

  std::string host = "raw.githubusercontent.com";
  std::string port = "443";

  if (argc > 1) {
    // Fetch more URLs for stress test
    int n = std::atoi(argv[1]);
    if (n > 0) {
      urls.clear();
      // Add unique types files from dhall-kubernetes
      std::vector<std::string> type_files = {
          "io.k8s.api.admissionregistration.v1.MutatingWebhook",
          "io.k8s.api.admissionregistration.v1.MutatingWebhookConfiguration",
          "io.k8s.api.admissionregistration.v1.RuleWithOperations",
          "io.k8s.api.admissionregistration.v1.ValidatingWebhook",
          "io.k8s.api.admissionregistration.v1.ValidatingWebhookConfiguration",
          "io.k8s.api.admissionregistration.v1.WebhookClientConfig",
          "io.k8s.api.apps.v1.DaemonSet",
          "io.k8s.api.apps.v1.DaemonSetCondition",
          "io.k8s.api.apps.v1.DaemonSetSpec",
          "io.k8s.api.apps.v1.DaemonSetStatus",
          "io.k8s.api.apps.v1.DaemonSetUpdateStrategy",
          "io.k8s.api.apps.v1.Deployment",
          "io.k8s.api.apps.v1.DeploymentCondition",
          "io.k8s.api.apps.v1.DeploymentSpec",
          "io.k8s.api.apps.v1.DeploymentStatus",
          "io.k8s.api.apps.v1.DeploymentStrategy",
          "io.k8s.api.apps.v1.ReplicaSet",
          "io.k8s.api.apps.v1.ReplicaSetCondition",
          "io.k8s.api.apps.v1.ReplicaSetSpec",
          "io.k8s.api.apps.v1.ReplicaSetStatus",
          "io.k8s.api.apps.v1.RollingUpdateDaemonSet",
          "io.k8s.api.apps.v1.RollingUpdateDeployment",
          "io.k8s.api.apps.v1.RollingUpdateStatefulSetStrategy",
          "io.k8s.api.apps.v1.StatefulSet",
          "io.k8s.api.apps.v1.StatefulSetCondition",
          "io.k8s.api.apps.v1.StatefulSetSpec",
          "io.k8s.api.apps.v1.StatefulSetStatus",
          "io.k8s.api.apps.v1.StatefulSetUpdateStrategy",
          "io.k8s.api.autoscaling.v1.HorizontalPodAutoscaler",
          "io.k8s.api.autoscaling.v1.HorizontalPodAutoscalerSpec",
          "io.k8s.api.core.v1.Pod",
          "io.k8s.api.core.v1.PodSpec",
          "io.k8s.api.core.v1.PodStatus",
          "io.k8s.api.core.v1.Container",
          "io.k8s.api.core.v1.ContainerPort",
          "io.k8s.api.core.v1.EnvVar",
          "io.k8s.api.core.v1.Volume",
          "io.k8s.api.core.v1.VolumeMount",
          "io.k8s.api.core.v1.Service",
          "io.k8s.api.core.v1.ServiceSpec",
          "io.k8s.api.core.v1.ServicePort",
          "io.k8s.api.core.v1.ConfigMap",
          "io.k8s.api.core.v1.Secret",
          "io.k8s.api.core.v1.Namespace",
          "io.k8s.api.core.v1.Node",
          "io.k8s.api.core.v1.NodeSpec",
          "io.k8s.api.core.v1.NodeStatus",
          "io.k8s.api.core.v1.PersistentVolume",
          "io.k8s.api.core.v1.PersistentVolumeClaim",
          "io.k8s.api.batch.v1.Job",
      };
      for (int i = 0; i < n && i < (int)type_files.size(); ++i) {
        urls.push_back("/dhall-lang/dhall-kubernetes/master/1.30/types/" + type_files[i] +
                       ".dhall");
      }
      // If n > type_files.size(), repeat with /defaults/
      for (int i = type_files.size();
           i < n && (i - (int)type_files.size()) < (int)type_files.size(); ++i) {
        urls.push_back("/dhall-lang/dhall-kubernetes/master/1.30/defaults/" +
                       type_files[i - type_files.size()] + ".dhall");
      }
    }
  }

  std::printf("Testing parallel HTTP/2 fetch of %zu URLs from %s\n", urls.size(), host.c_str());
  auto start = std::chrono::high_resolution_clock::now();

  // Create ring
  auto ring = evring::make_io_uring_ring(4096);

  // DNS + TCP connect
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
    std::fprintf(stderr, "DNS resolution failed\n");
    return 1;
  }

  ring->enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_CLOEXEC));
  auto events = ring->submit_and_wait(1);
  evring::handle socket = events[0].resource_handle;

  ring->enqueue(evring::operation::make_connect(socket, result->ai_addr,
                                                static_cast<std::uint32_t>(result->ai_addrlen)));
  events = ring->submit_and_wait(1);
  freeaddrinfo(result);

  if (!events[0].ok()) {
    std::fprintf(stderr, "TCP connect failed\n");
    return 1;
  }

  auto connect_time = std::chrono::high_resolution_clock::now();
  std::printf("TCP connected in %.1f ms\n",
              std::chrono::duration_cast<std::chrono::microseconds>(connect_time - start).count() /
                  1000.0);

  // TLS handshake with ALPN h2
  auto tls_config = evring::tls_client_config::create_default();
  tls_config.set_alpn("h2");
  evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, host};
  auto tls_state = evring::run(tls_hs, *ring);

  if (!tls_state.ok()) {
    std::fprintf(stderr, "TLS handshake failed: %s\n", tls_state.error_message.c_str());
    return 1;
  }

  auto tls = tls_state.take_context();
  const char* alpn = tls.alpn_selected();
  if (!alpn || std::strcmp(alpn, "h2") != 0) {
    std::fprintf(stderr, "Server doesn't support HTTP/2\n");
    return 1;
  }

  auto tls_time = std::chrono::high_resolution_clock::now();
  std::printf(
      "TLS handshake in %.1f ms (ALPN: %s)\n",
      std::chrono::duration_cast<std::chrono::microseconds>(tls_time - connect_time).count() /
          1000.0,
      alpn);

  // HTTP/2 connection setup
  evring::http2_session session;
  session.init_client();
  evring::http2_connection_machine conn_machine{session, tls, socket};
  auto conn_state = evring::run(conn_machine, *ring);

  if (!conn_state.ok()) {
    std::fprintf(stderr, "HTTP/2 connection failed: %s\n", conn_state.error_message.c_str());
    return 1;
  }

  auto h2_time = std::chrono::high_resolution_clock::now();
  std::printf("HTTP/2 connection in %.1f ms\n",
              std::chrono::duration_cast<std::chrono::microseconds>(h2_time - tls_time).count() /
                  1000.0);

  // BLAST ALL REQUESTS AT ONCE
  std::printf("Submitting %zu requests...\n", urls.size());

  std::map<std::int32_t, std::string> stream_to_path;
  for (const auto& path : urls) {
    evring::http2_request req;
    req.method = "GET";
    req.scheme = "https";
    req.authority = host;
    req.path = path;
    req.headers.push_back({"user-agent", "h2_parallel_test/1.0"});
    req.headers.push_back({"accept", "*/*"});

    std::int32_t stream_id = session.submit_request(req);
    if (stream_id > 0) {
      stream_to_path[stream_id] = path;
    } else {
      std::fprintf(stderr, "Failed to submit request for %s\n", path.c_str());
    }
  }

  auto submit_time = std::chrono::high_resolution_clock::now();
  std::printf("Submitted %zu streams in %.1f ms\n", stream_to_path.size(),
              std::chrono::duration_cast<std::chrono::microseconds>(submit_time - h2_time).count() /
                  1000.0);

  // Send all pending data
  int socket_fd = ring->get_file_descriptor(socket);
  auto pending = session.get_pending_data();
  std::printf("Sending %zu bytes of request frames...\n", pending.size());

  while (!pending.empty()) {
    ssize_t written = tls_write(tls.raw(), pending.data(), pending.size());
    if (written > 0) {
      pending.erase(pending.begin(), pending.begin() + written);
    } else if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT) {
      struct pollfd pfd{};
      pfd.fd = socket_fd;
      pfd.events = (written == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
      poll(&pfd, 1, 1000);
    } else {
      std::fprintf(stderr, "TLS write error: %s\n", tls_error(tls.raw()));
      break;
    }
  }

  auto send_time = std::chrono::high_resolution_clock::now();
  std::printf(
      "Sent all requests in %.1f ms\n",
      std::chrono::duration_cast<std::chrono::microseconds>(send_time - submit_time).count() /
          1000.0);

  // Read all responses
  std::size_t streams_pending = stream_to_path.size();
  std::size_t bytes_received = 0;
  std::byte buffer[65536];

  while (streams_pending > 0) {
    ssize_t nread = tls_read(tls.raw(), buffer, sizeof(buffer));

    if (nread > 0) {
      session.receive_data(std::span<const std::byte>(buffer, nread));

      // Check for completed streams
      for (auto it = stream_to_path.begin(); it != stream_to_path.end();) {
        if (session.is_stream_closed(it->first)) {
          auto* resp = session.get_stream_response(it->first);
          if (resp && resp->status_code == 200) {
            bytes_received += resp->body.size();
          }
          it = stream_to_path.erase(it);
          streams_pending--;
        } else {
          ++it;
        }
      }

      // Send any pending data (WINDOW_UPDATE etc)
      pending = session.get_pending_data();
      while (!pending.empty()) {
        ssize_t written = tls_write(tls.raw(), pending.data(), pending.size());
        if (written > 0) {
          pending.erase(pending.begin(), pending.begin() + written);
        } else if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT) {
          struct pollfd pfd{};
          pfd.fd = socket_fd;
          pfd.events = (written == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
          poll(&pfd, 1, 100);
        } else {
          break;
        }
      }
    } else if (nread == TLS_WANT_POLLIN || nread == TLS_WANT_POLLOUT) {
      struct pollfd pfd{};
      pfd.fd = socket_fd;
      pfd.events = (nread == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
      poll(&pfd, 1, 1000);
    } else if (nread == 0) {
      std::fprintf(stderr, "Connection closed with %zu streams pending\n", streams_pending);
      break;
    } else {
      std::fprintf(stderr, "TLS read error: %s\n", tls_error(tls.raw()));
      break;
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start).count();
  auto fetch_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - send_time).count();

  std::printf("\n=== Results ===\n");
  std::printf("Total time: %ld ms\n", total_ms);
  std::printf("Fetch time: %ld ms\n", fetch_ms);
  std::printf("Bytes received: %zu (%.2f KB)\n", bytes_received, bytes_received / 1024.0);
  std::printf("Throughput: %.2f MB/s\n",
              fetch_ms > 0 ? (bytes_received / 1048576.0) / (fetch_ms / 1000.0) : 0);

  // Cleanup
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  return 0;
}
