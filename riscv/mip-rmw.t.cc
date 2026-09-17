#include "processor.h"
#include "simif.h"
#include "csrs.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>

// 无设备的 hart；直接驱动外部 pending source，并执行真实 CSR 指令实现。
struct csr_test_sim_t : simif_t {
  cfg_t cfg;
  std::map<size_t, processor_t*> harts;
  char* addr_to_mem(reg_t) override { return nullptr; }
  bool mmio_load(reg_t, size_t, uint8_t*) override { return false; }
  bool mmio_store(reg_t, size_t, const uint8_t*) override { return false; }
  void proc_reset(unsigned) override {}
  const cfg_t& get_cfg() const override { return cfg; }
  const std::map<size_t, processor_t*>& get_harts() const override { return harts; }
  const char* get_symbol(uint64_t) override { return nullptr; }
};

#define CSR_FUNCTIONS(xlen) \
  extern reg_t fast_rv##xlen##i_csrrs(processor_t*, insn_t, reg_t); \
  extern reg_t fast_rv##xlen##i_csrrc(processor_t*, insn_t, reg_t); \
  extern reg_t fast_rv##xlen##i_csrrsi(processor_t*, insn_t, reg_t); \
  extern reg_t fast_rv##xlen##i_csrrci(processor_t*, insn_t, reg_t);
CSR_FUNCTIONS(32)
CSR_FUNCTIONS(64)

static void check(bool condition, const char* message)
{
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

static void test(unsigned xlen)
{
  csr_test_sim_t sim;
  processor_t p(xlen == 64 ? "RV64IMAFDC" : "RV32IMAFDC", "MSU",
                &sim.cfg, &sim, 0, false, nullptr, std::cerr);
  auto* s = p.get_state();
  const insn_func_t funcs32[] = {fast_rv32i_csrrs, fast_rv32i_csrrc,
                                fast_rv32i_csrrsi, fast_rv32i_csrrci};
  const insn_func_t funcs64[] = {fast_rv64i_csrrs, fast_rv64i_csrrc,
                                fast_rv64i_csrrsi, fast_rv64i_csrrci};
  const auto* funcs = xlen == 64 ? funcs64 : funcs32;
  const unsigned funct3[] = {2, 3, 6, 7};
  for (unsigned op = 0; op < 4; ++op) {
    for (bool sw : {false, true}) {
      for (bool hw : {false, true}) {
        // immediate 无法选中 SEIP，但必须保持其软件位，同时正确返回硬件位。
        for (reg_t mask : {reg_t(0), reg_t(MIP_SSIP), reg_t(MIP_SEIP)}) {
          if (op >= 2 && mask == MIP_SEIP)
            continue;
          p.put_csr(CSR_MIP, (sw ? MIP_SEIP : 0) | MIP_SSIP);
          s->mip->backdoor_write_with_mask(MIP_SEIP, hw ? MIP_SEIP : 0);
          const reg_t before = s->mip->read();
          s->XPR.write(1, mask);
          const unsigned source = op >= 2 ? mask : (mask ? 1 : 0);
          const insn_t insn((CSR_MIP << 20) | (source << 15) |
                            (funct3[op] << 12) | (2 << 7) | 0x73);
          s->serialized = true;
          funcs[op](&p, insn, 0x80000000);
          check(s->XPR[2] == before, "CSR rd must include hardware SEIP");
          s->mip->backdoor_write_with_mask(MIP_SEIP, 0);
          const reg_t software_before = (sw ? MIP_SEIP : 0) | MIP_SSIP;
          const reg_t expected = op % 2 ? software_before & ~mask : software_before | mask;
          check(s->mip->read() == expected, "CSR RMW must not latch hardware SEIP");
        }
      }
    }
  }
}

int main()
{
  test(32);
  test(64);
  std::puts("PASS: RV32/RV64 MIP hardware/software SEIP RMW");
}
