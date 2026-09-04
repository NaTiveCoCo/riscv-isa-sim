#include "iceblk_image.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace {

void write_exact(int fd, const void* data, size_t size, off_t offset)
{
  assert(::pwrite(fd, data, size, offset) == static_cast<ssize_t>(size));
}

void read_exact(int fd, void* data, size_t size, off_t offset)
{
  assert(::pread(fd, data, size, offset) == static_cast<ssize_t>(size));
}

}  // namespace

int main()
{
  char path[] = "/tmp/nacc-spike-iceblk-test.XXXXXX";
  const int fd = ::mkstemp(path);
  assert(fd >= 0);
  assert(::ftruncate(fd, 4 * iceblk_image_t::sector_size) == 0);

  std::array<unsigned char, iceblk_image_t::sector_size> sector_zero{};
  std::array<unsigned char, iceblk_image_t::sector_size> sector_two{};
  std::array<unsigned char, iceblk_image_t::sector_size> replacement{};
  sector_zero.fill(0x11);
  sector_two.fill(0x22);
  replacement.fill(0xa5);
  write_exact(fd, sector_zero.data(), sector_zero.size(), 0);
  write_exact(fd, sector_two.data(), sector_two.size(), 2 * iceblk_image_t::sector_size);

  {
    iceblk_image_t image(path);
    assert(image.sectors() == 4);

    std::array<unsigned char, iceblk_image_t::sector_size> observed{};
    image.read(2, 1, observed.data());
    assert(observed == sector_two);
    image.write(3, 1, replacement.data());

    bool rejected = false;
    try {
      image.read(4, 1, observed.data());
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    assert(rejected);
  }

  std::array<unsigned char, iceblk_image_t::sector_size> observed{};
  read_exact(fd, observed.data(), observed.size(), 0);
  assert(observed == sector_zero);
  read_exact(fd, observed.data(), observed.size(), 3 * iceblk_image_t::sector_size);
  assert(observed == replacement);
  ::close(fd);
  assert(::unlink(path) == 0);

  char misaligned_path[] = "/tmp/nacc-spike-iceblk-misaligned.XXXXXX";
  const int misaligned_fd = ::mkstemp(misaligned_path);
  assert(misaligned_fd >= 0);
  assert(::ftruncate(misaligned_fd, iceblk_image_t::sector_size + 1) == 0);
  ::close(misaligned_fd);
  bool rejected = false;
  try {
    iceblk_image_t image(misaligned_path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
  assert(::unlink(misaligned_path) == 0);
  return 0;
}
