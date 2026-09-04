#ifndef NACC_SPIKEDEVICES_ICEBLK_H
#define NACC_SPIKEDEVICES_ICEBLK_H

#include "iceblk_image.h"

#include <queue>
#include <riscv/abstract_device.h>
#include <riscv/sim.h>
#include <string>
#include <vector>

class iceblk_t : public abstract_device_t {
 public:
  iceblk_t(const simif_t* sim,
           abstract_interrupt_controller_t* intctrl,
           uint32_t interrupt_id,
           const std::vector<std::string>& arguments);
  bool load(reg_t addr, size_t len, uint8_t* bytes) override;
  bool store(reg_t addr, size_t len, const uint8_t* bytes) override;
  void tick(reg_t rtc_ticks) override;
  reg_t size() override { return device_size; }

 private:
  void post_request();
  void handle_request();
  void handle_read_request();
  void handle_write_request();
  void validate_request() const;

  static constexpr uint64_t blockdevice_latency = 500;
  static constexpr reg_t device_size = 0x1000;
  static constexpr reg_t sector_size = 512;
  static constexpr reg_t max_request_length = 16;
  static constexpr int trackers = 1;

  uint64_t current_tick_ = 0;
  iceblk_image_t image_;
  const simif_t* sim_;
  abstract_interrupt_controller_t* intctrl_;
  uint32_t interrupt_id_;
  std::queue<unsigned int> idle_tags_;
  std::queue<unsigned int> pending_tags_;
  std::queue<unsigned int> complete_tags_;
  reg_t request_address_ = 0;
  reg_t request_offset_ = 0;
  reg_t request_length_ = 0;
  reg_t request_write_ = 0;
};

#endif
