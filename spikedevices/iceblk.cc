#include "iceblk.h"

#include <fdt/libfdt.h>
#include <map>
#include <memory>
#include <riscv/dts.h>
#include <riscv/mmu.h>
#include <sstream>
#include <stdexcept>

namespace {

constexpr reg_t blockdevice_base = 0x10015000;
constexpr uint32_t blockdevice_interrupt_id = 2;
constexpr reg_t blockdevice_size = 0x1000;

constexpr reg_t register_address = 0;
constexpr reg_t register_offset = 8;
constexpr reg_t register_length = 12;
constexpr reg_t register_write = 16;
constexpr reg_t register_request = 17;
constexpr reg_t register_available_requests = 18;
constexpr reg_t register_complete = 19;
constexpr reg_t register_completed_requests = 20;
constexpr reg_t register_sectors = 24;
constexpr reg_t register_max_request_length = 28;

std::string image_path(const std::vector<std::string>& arguments)
{
  for (const auto& argument : arguments) {
    constexpr const char prefix[] = "img=";
    if (argument.rfind(prefix, 0) == 0 && argument.size() > sizeof(prefix) - 1)
      return argument.substr(sizeof(prefix) - 1);
  }
  throw std::runtime_error("The Iceblk device requires img=<raw-image>");
}

int parse_blockdevice(const void* fdt, reg_t* address, uint32_t* interrupt_id,
                      const char* compatible)
{
  int node = fdt_node_offset_by_compatible(fdt, -1, compatible);
  if (node < 0)
    return node;

  int result = fdt_get_node_addr_size(fdt, node, address, nullptr, "reg");
  if (result < 0 || !address)
    return -ENODEV;

  int length = 0;
  const auto* interrupts = static_cast<const fdt32_t*>(fdt_getprop(fdt, node, "interrupts", &length));
  if (interrupt_id)
    *interrupt_id = interrupts ? fdt32_to_cpu(*interrupts) : blockdevice_interrupt_id;
  return 0;
}

}  // namespace

iceblk_t::iceblk_t(const simif_t* sim,
                   abstract_interrupt_controller_t* intctrl,
                   uint32_t interrupt_id,
                   const std::vector<std::string>& arguments)
  : image_(image_path(arguments)), sim_(sim), intctrl_(intctrl), interrupt_id_(interrupt_id)
{
  for (int index = 0; index < trackers; ++index)
    idle_tags_.push(index);
}

void iceblk_t::validate_request() const
{
  if (request_address_ % sizeof(uint64_t) != 0)
    throw std::runtime_error("Iceblk DMA address is not 64-bit aligned");
  if (request_length_ == 0 || request_length_ > max_request_length)
    throw std::runtime_error("Iceblk request length is invalid");
  if (request_offset_ > image_.sectors() || request_length_ > image_.sectors() - request_offset_)
    throw std::runtime_error("Iceblk request is outside the image");
}

void iceblk_t::handle_read_request()
{
  std::vector<uint64_t> data(request_length_ * sector_size / sizeof(uint64_t));
  image_.read(request_offset_, request_length_, data.data());
  for (size_t index = 0; index < data.size(); ++index)
    sim_->debug_mmu->store<uint64_t>(request_address_ + index * sizeof(uint64_t), data[index]);
}

void iceblk_t::handle_write_request()
{
  std::vector<uint64_t> data(request_length_ * sector_size / sizeof(uint64_t));
  for (size_t index = 0; index < data.size(); ++index)
    data[index] = sim_->debug_mmu->load<uint64_t>(request_address_ + index * sizeof(uint64_t));
  // request_offset_ 是 Linux blk_rq_pos() 提供的磁盘起始 sector，不能只使用请求内索引。
  image_.write(request_offset_, request_length_, data.data());
}

void iceblk_t::handle_request()
{
  validate_request();
  request_write_ ? handle_write_request() : handle_read_request();
  intctrl_->set_interrupt_level(interrupt_id_, 1);
}

void iceblk_t::post_request()
{
  if (idle_tags_.empty())
    throw std::runtime_error("Iceblk request posted without an available tag");
  pending_tags_.push(idle_tags_.front());
  idle_tags_.pop();
}

bool iceblk_t::load(reg_t address, size_t length, uint8_t* bytes)
{
  if (length > sizeof(reg_t))
    return false;

  switch (address) {
    case register_request:
      post_request();
      break;
    case register_available_requests:
      read_little_endian_reg(static_cast<int>(idle_tags_.size()), 0, length, bytes);
      break;
    case register_completed_requests:
      read_little_endian_reg(static_cast<int>(complete_tags_.size()), 0, length, bytes);
      break;
    case register_complete: {
      if (complete_tags_.empty())
        throw std::runtime_error("Iceblk completion read without a completed request");
      const unsigned int tag = complete_tags_.front();
      read_little_endian_reg(tag, 0, length, bytes);
      complete_tags_.pop();
      idle_tags_.push(tag);
      if (complete_tags_.empty())
        intctrl_->set_interrupt_level(interrupt_id_, 0);
      break;
    }
    case register_sectors:
      read_little_endian_reg(static_cast<uint32_t>(image_.sectors()), 0, length, bytes);
      break;
    case register_max_request_length:
      read_little_endian_reg(static_cast<uint32_t>(max_request_length), 0, length, bytes);
      break;
    default:
      return false;
  }
  return true;
}

bool iceblk_t::store(reg_t address, size_t length, const uint8_t* bytes)
{
  if (length > sizeof(reg_t))
    return false;

  switch (address) {
    case register_address:
      write_little_endian_reg(&request_address_, 0, length, bytes);
      break;
    case register_offset:
      write_little_endian_reg(&request_offset_, 0, length, bytes);
      break;
    case register_length:
      write_little_endian_reg(&request_length_, 0, length, bytes);
      break;
    case register_write:
      write_little_endian_reg(&request_write_, 0, length, bytes);
      break;
    default:
      return false;
  }
  return true;
}

void iceblk_t::tick(reg_t rtc_ticks UNUSED)
{
  if (++current_tick_ % blockdevice_latency == 0)
    current_tick_ = 0;
  if (current_tick_ != 0 || pending_tags_.empty())
    return;

  handle_request();
  complete_tags_.push(pending_tags_.front());
  pending_tags_.pop();
}

std::string iceblk_generate_dts(const sim_t* sim UNUSED,
                                const std::vector<std::string>& arguments UNUSED)
{
  std::stringstream stream;
  stream << std::hex
         << "    iceblk: blkdev-controller@" << blockdevice_base << " {\n"
         << "      compatible = \"ucb-bar,blkdev\";\n"
         << "      interrupt-parent = <&PLIC>;\n"
         << "      interrupts = <" << std::dec << blockdevice_interrupt_id << ">;\n"
         << "      reg = <0x" << std::hex << (blockdevice_base >> 32)
         << " 0x" << (blockdevice_base & static_cast<uint32_t>(-1))
         << " 0x" << (blockdevice_size >> 32)
         << " 0x" << (blockdevice_size & static_cast<uint32_t>(-1)) << ">;\n"
         << "    };\n";
  return stream.str();
}

iceblk_t* iceblk_parse_from_fdt(const void* fdt, const sim_t* sim, reg_t* base,
                                const std::vector<std::string>& arguments)
{
  uint32_t interrupt_id = 0;
  if (parse_blockdevice(fdt, base, &interrupt_id, "ucb-bar,blkdev") != 0 &&
      parse_blockdevice(fdt, base, &interrupt_id, "ucbbar,blkdev") != 0)
    return nullptr;
  return new iceblk_t(sim, sim->get_intctrl(), interrupt_id, arguments);
}

REGISTER_DEVICE(iceblk, iceblk_parse_from_fdt, iceblk_generate_dts)
