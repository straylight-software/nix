#include "nix/util/muxable-pipe.h"

#include <poll.h>

#include "nix/util/logging.h"
#include "nix/util/util.h"

namespace nix {

void muxable_pipe_poll_state_t::poll(std::optional<unsigned int> timeout) {
  if (::poll(poll_status.data(), poll_status.size(), timeout ? *timeout : -1) == -1) {
    if (errno == EINTR)
      return;
    throw sys_error_t("waiting for input");
  }
}

void muxable_pipe_poll_state_t::iterate(
    std::set<muxable_pipe_poll_state_t::comm_channel_t>& channels,
    std::function<void(descriptor_t fd, std::string_view data)> handle_read,
    std::function<void(descriptor_t fd)> handle_eof) {
  std::set<descriptor_t> fds2(channels);
  std::vector<unsigned char> buffer(4096);
  for (auto& k : fds2) {
    const auto fd_poll_status_id = get(fd_to_poll_status, k);
    assert(fd_poll_status_id);
    assert(*fd_poll_status_id < poll_status.size());
    if (poll_status.at(*fd_poll_status_id).revents) {
      ssize_t rd = ::read(from_descriptor_read_only(k), buffer.data(), buffer.size());
      // FIXME: is there a cleaner way to handle pt close
      // than EIO? Is this even standard?
      if (rd == 0 || (rd == -1 && errno == EIO)) {
        handle_eof(k);
        channels.erase(k);
      } else if (rd == -1) {
        if (errno != EINTR)
          throw sys_error_t("read failed");
      } else {
        std::string_view data((char*)buffer.data(), rd);
        handle_read(k, data);
      }
    }
  }
}

} // namespace nix
