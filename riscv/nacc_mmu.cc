// See LICENSE for license details.

#include "mmu.h"

void mmu_t::nacc_check_root(const mem_access_info_t& access_info)
{
  const auto mode = access_info.effective_priv;
  const auto addr = access_info.transformed_vaddr;
  const auto type = access_info.type;
  const auto virt = access_info.effective_virt;
  // RTL 在 translation/PMP 之前让 sticky fatal 优先拒绝所有 S/U-effective 请求。
  // M-effective 访问是 reset 前诊断和恢复所需的可信兜底。
  if (proc->state.nacc_bitmap_fatal && mode != PRV_M)
    throw_access_exception(virt, addr, type);

  const bool effective_a = nacc_effective_a(access_info);
  if (effective_a && mode == PRV_U && ((addr >> 38) & 1))
    throw_access_exception(virt, addr, type);

  // Effective AS may bootstrap in Bare; AU still requires a protected root.
  // Paging in either A-side mode always validates ROOT_L0, including MPRV data.
  if (effective_a && mode <= PRV_S) {
    const reg_t satp = proc->state.satp->readvirt(virt);
    if (get_field(satp, SATP64_MODE) == SATP_MODE_OFF) {
      if (mode != PRV_S)
        throw_access_exception(virt, addr, type);
    } else {
      const reg_t root_paddr = get_field(satp, SATP64_PPN) << PGSHIFT;
      if (nacc_root_tag(root_paddr, access_info) != 1)
        throw_access_exception(virt, addr, type);
    }
  }

}

bool mmu_t::nacc_bitmap_configured() const
{
  return proc && proc->state.bitmap_target_end &&
         proc->state.bitmap_target_end->read() > proc->state.bitmap_target_start->read();
}

bool mmu_t::nacc_effective_a(const mem_access_info_t& access_info) const
{
  if (access_info.type != FETCH && proc->state.prv == PRV_M &&
      get_field(proc->state.mstatus->read(), MSTATUS_MPRV) &&
      access_info.effective_priv != PRV_M)
    return get_field(proc->state.asstatus->read(), NACC_ASSTATUS_MPA);
  return proc->state.nacc_a;
}

uint8_t mmu_t::nacc_bitmap_tag(reg_t paddr, const mem_access_info_t& access_info)
{
  if (proc->state.nacc_bitmap_fatal)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);

  const reg_t start = proc->state.bitmap_target_start->read();
  const reg_t end = proc->state.bitmap_target_end->read();
  const reg_t storage = proc->state.bitmap_storage_base->read();
  // 没有有序 target range 时不存在 metadata request；A-world root gate 只应得到
  // 普通 access fault，不能把未配置状态升级为 reset-only fatal。
  if ((start & (PGSIZE - 1)) || (end & (PGSIZE - 1)) || end <= start)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);
  if (paddr < start || paddr >= end)
    return 0;

  const reg_t page_index = (paddr - start) >> PGSHIFT;
  const reg_t page_count = (end - start) >> PGSHIFT;
  const reg_t bitmap_bytes = (page_count + 3) >> 2;
  const reg_t bitmap_addr = storage + (page_index >> 2);
  const reg_t bitmap_end = storage + bitmap_bytes;
  const unsigned paddr_bits = proc->paddr_bits();
  const auto fits_paddr = [paddr_bits](reg_t value) {
    return paddr_bits >= sizeof(reg_t) * 8 || (value >> paddr_bits) == 0;
  };
  const reg_t agent_start = proc->state.sagent->read();
  const reg_t agent_end = proc->state.eagent->read();
  const bool lookup_in_agent = agent_end > agent_start && paddr >= agent_start && paddr < agent_end;
  if (page_count > ~reg_t(0) - 3 || bitmap_end < storage || bitmap_addr < storage ||
      bitmap_addr >= bitmap_end || !fits_paddr(start) ||
      (paddr_bits < sizeof(reg_t) * 8 && end > (reg_t(1) << paddr_bits)) ||
      !fits_paddr(storage) || !fits_paddr(bitmap_addr) ||
      (paddr_bits < sizeof(reg_t) * 8 && bitmap_end > (reg_t(1) << paddr_bits)) ||
      lookup_in_agent) {
    nacc_poison(access_info);
  }
  if (!pmp_ok(bitmap_addr, 1, LOAD, PRV_M, false)) {
    nacc_poison(access_info);
  }
  nacc_trace("bitmap", "metadata", access_info.vaddr, paddr, access_info.type);
  uint8_t byte = 0;
  void* host_addr = sim->addr_to_mem(bitmap_addr);
  if (host_addr)
    memcpy(&byte, host_addr, 1);
  else if (!mmio_load(bitmap_addr, 1, &byte)) {
    nacc_poison(access_info);
  }
  return (byte >> ((page_index & 3) * 2)) & 3;
}

uint8_t mmu_t::nacc_check_access(const mem_access_info_t& access_info, reg_t paddr, reg_t len, const uint8_t* cached_tag)
{
  const reg_t mode = access_info.effective_priv;
  const bool effective_a = nacc_effective_a(access_info);
  const reg_t access_end = paddr + len;
  if (access_end < paddr)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);

  const reg_t agent_start = proc->state.sagent->read();
  const reg_t agent_end = proc->state.eagent->read();
  if (agent_end > agent_start && paddr < agent_end && access_end > agent_start &&
      !(mode == PRV_M || (effective_a && mode == PRV_S)))
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);

  // Agent region 是独立物理区间，不以 bitmap target 是否配置为开关。
  if (!nacc_bitmap_configured())
    return 0;

  const reg_t target_start = proc->state.bitmap_target_start->read();
  const reg_t target_end = proc->state.bitmap_target_end->read();
  const reg_t page_count = (target_end - target_start) >> PGSHIFT;
  const reg_t bitmap_bytes = (page_count + 3) >> 2;
  const reg_t storage_start = proc->state.bitmap_storage_base->read();
  const reg_t storage_end = storage_start + bitmap_bytes;
  if (storage_end < storage_start)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);
  if (paddr < storage_end && access_end > storage_start &&
      (access_info.type == FETCH || !(mode == PRV_M || (effective_a && mode == PRV_S))))
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);

  // M-effective data access 是可信兜底，不读取 bitmap metadata；PMP 与 backing fetch
  // 禁令已经分别在 translate() 和上面的区间检查中执行。
  if (mode == PRV_M)
    return 0;

  if (proc->state.nacc_bitmap_fatal)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);

  const uint8_t tag = cached_tag ? *cached_tag : nacc_bitmap_tag(paddr, access_info);
  bool allowed = false;
  switch (tag) {
    case 0: // NORMAL
      allowed = true;
      break;
    case 1: { // ROOT_L0
      if (mode == PRV_M)
        allowed = true;
      else if (access_info.type == FETCH)
        allowed = false;
      else if (effective_a && mode == PRV_S)
        allowed = true;
      else {
        const reg_t satp = proc->state.satp->readvirt(access_info.effective_virt);
        const reg_t root_ppn = get_field(satp, SATP64_PPN);
        const bool translation_enabled = get_field(satp, SATP64_MODE) != SATP_MODE_OFF;
        const bool current_root = translation_enabled && (paddr >> PGSHIFT) == root_ppn;
        if (access_info.type == LOAD)
          allowed = current_root;
        else
          allowed = !effective_a && mode == PRV_S && current_root &&
                    (paddr & (PGSIZE - 1)) >= PGSIZE / 2 + access_info.nacc_span_offset &&
                    ((paddr & (PGSIZE - 1)) - access_info.nacc_span_offset +
                     (access_info.nacc_span_len ? access_info.nacc_span_len : len)) <= PGSIZE;
      }
      break;
    }
    case 2: // PRIVATE_DATA
      allowed = mode == PRV_M || effective_a;
      break;
    case 3: // PRIVATE_COPY_PENDING
      allowed = mode == PRV_M ||
                (access_info.type != FETCH && effective_a && mode == PRV_S);
      break;
    default:
      abort();
  }
  if (!allowed)
    throw_access_exception(access_info.effective_virt, access_info.transformed_vaddr, access_info.type);
  return tag;
}

[[noreturn]] void mmu_t::nacc_poison(const mem_access_info_t& info)
{
  proc->state.nacc_bitmap_fatal = true;
  // Invalidate immediately, even if a vector instruction suppresses the trap.
  flush_tlb("fatal");
  throw_access_exception(info.effective_virt, info.transformed_vaddr, info.type);
  abort();
}

uint8_t mmu_t::nacc_root_tag(reg_t paddr, const mem_access_info_t& info)
{
  if (!nacc_root_valid || nacc_root_paddr != paddr) {
    const auto tag = nacc_bitmap_tag(paddr, info);
    nacc_root_paddr = paddr;
    nacc_root_raw_tag = tag;
    nacc_root_valid = true;
    nacc_trace("root-refill", "root", info.vaddr, paddr, info.type, tag);
  } else {
    nacc_trace("root-hit", "root", info.vaddr, paddr, info.type, nacc_root_raw_tag);
  }
  return nacc_root_raw_tag;
}

bool mmu_t::nacc_guarded_page(reg_t paddr) const
{
  if (!proc) return false;
  const auto page = paddr & ~reg_t(PGSIZE - 1);
  if (page >= proc->state.sagent->read() && page < proc->state.eagent->read())
    return true;
  if (!nacc_bitmap_configured()) return false;
  const auto start = proc->state.bitmap_storage_base->read();
  const auto bytes = (((proc->state.bitmap_target_end->read() -
                        proc->state.bitmap_target_start->read()) >> PGSHIFT) + 3) >> 2;
  // Include the unused tail of the last backing page: byte ranges are checked live.
  return start + bytes < start || (page >= start && page < start + bytes);
}

void mmu_t::nacc_check_hit_slow(const tlb_entry_t& entry, reg_t addr, reg_t len,
                               access_type type, const mem_access_info_t* original)
{
  auto info = original ? *original : generate_access_info(addr, type, {});
  nacc_check_root(info);
  nacc_check_access(info, entry.target_addr + addr % PGSIZE, len, &entry.nacc_raw_tag);
}

#ifdef NACC_MMU_TRACE
void mmu_t::nacc_trace(const char* event, const char* reason, reg_t va, reg_t pa,
                       access_type type, uint8_t tag)
{
  // The debug MMU is not a hart translation cache.
  if (!proc) return;
  // Host-only diagnostics; no guest instructions or CSR protocol are introduced.
  fprintf(stderr, "NACC_MMU %s %s va=%lx pa=%lx type=%u tag=%u prv=%u a=%u epoch=%lu\n",
          event, reason, va, pa, unsigned(type), unsigned(tag),
          proc ? unsigned(proc->state.prv) : 3, proc ? unsigned(proc->state.nacc_a) : 0,
          nacc_trace_epoch);
  if (!strcmp(event, "flush")) ++nacc_trace_epoch;
}
#endif
