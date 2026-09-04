#ifndef NACC_SPIKEDEVICES_ICEBLK_IMAGE_H
#define NACC_SPIKEDEVICES_ICEBLK_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <string>

class iceblk_image_t {
 public:
  static constexpr uint64_t sector_size = 512;

  explicit iceblk_image_t(const std::string& path);
  ~iceblk_image_t();

  iceblk_image_t(const iceblk_image_t&) = delete;
  iceblk_image_t& operator=(const iceblk_image_t&) = delete;

  uint64_t sectors() const { return size_ / sector_size; }
  void read(uint64_t sector, uint64_t count, void* destination) const;
  void write(uint64_t sector, uint64_t count, const void* source);

 private:
  uint64_t byte_offset(uint64_t sector, uint64_t count) const;

  int fd_ = -1;
  uint64_t size_ = 0;
  std::string path_;
};

#endif
