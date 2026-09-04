#include "iceblk_image.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::runtime_error io_error(const std::string& operation, const std::string& path)
{
  return std::runtime_error(operation + " " + path + ": " + std::strerror(errno));
}

void transfer_exact(int fd, void* buffer, size_t length, off_t offset,
                    bool write, const std::string& path)
{
  auto* cursor = static_cast<unsigned char*>(buffer);
  size_t remaining = length;
  while (remaining != 0) {
    ssize_t result = write
      ? ::pwrite(fd, cursor, remaining, offset)
      : ::pread(fd, cursor, remaining, offset);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      throw io_error(write ? "Cannot write Iceblk image" : "Cannot read Iceblk image", path);
    }
    if (result == 0)
      throw std::runtime_error("Short I/O on Iceblk image " + path);
    cursor += result;
    remaining -= static_cast<size_t>(result);
    offset += result;
  }
}

}  // namespace

iceblk_image_t::iceblk_image_t(const std::string& path) : path_(path)
{
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0)
    throw io_error("Cannot open Iceblk image", path_);

  struct stat status = {};
  if (::fstat(fd_, &status) < 0) {
    const auto error = io_error("Cannot stat Iceblk image", path_);
    ::close(fd_);
    fd_ = -1;
    throw error;
  }
  if (status.st_size <= 0 || status.st_size % sector_size != 0) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("Iceblk image size must be a positive multiple of 512 bytes: " + path_);
  }
  size_ = static_cast<uint64_t>(status.st_size);
  if (sectors() > std::numeric_limits<uint32_t>::max()) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("Iceblk image exceeds the 32-bit sector-count protocol: " + path_);
  }
}

iceblk_image_t::~iceblk_image_t()
{
  if (fd_ < 0)
    return;
  // 正常退出前提交 guest 写入，使随后挂载 Image 提取 outputs 时能看到完整结果。
  ::fdatasync(fd_);
  ::close(fd_);
}

uint64_t iceblk_image_t::byte_offset(uint64_t sector, uint64_t count) const
{
  if (count == 0)
    throw std::runtime_error("Iceblk request length must be non-zero");
  if (sector > sectors() || count > sectors() - sector)
    throw std::runtime_error("Iceblk request is outside the image");
  return sector * sector_size;
}

void iceblk_image_t::read(uint64_t sector, uint64_t count, void* destination) const
{
  const uint64_t offset = byte_offset(sector, count);
  const uint64_t length = count * sector_size;
  transfer_exact(fd_, destination, static_cast<size_t>(length), static_cast<off_t>(offset), false, path_);
}

void iceblk_image_t::write(uint64_t sector, uint64_t count, const void* source)
{
  const uint64_t offset = byte_offset(sector, count);
  const uint64_t length = count * sector_size;
  transfer_exact(fd_, const_cast<void*>(source), static_cast<size_t>(length), static_cast<off_t>(offset), true, path_);
}
