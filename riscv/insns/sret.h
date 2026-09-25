require_extension('S');
require(!STATE.nacc_a);
reg_t prev_hstatus = STATE.hstatus->read();
if (STATE.v) {
  if (STATE.prv == PRV_U || get_field(prev_hstatus, HSTATUS_VTSR))
    require_novirt();
} else {
  require_privilege(get_field(STATE.mstatus->read(), MSTATUS_TSR) ? PRV_M : PRV_S);
}
const bool enter_as = get_field(STATE.asstatus->read(), NACC_ASSTATUS_SPA);
if (enter_as) {
  require(get_field(STATE.sstatus->read(), MSTATUS_SPP) == PRV_S);
  require(get_field(STATE.satp->read(), SATP64_MODE) == SATP_MODE_SV39);
  require((STATE.astvec->read() & ~reg_t(3)) != 0);
  // An untrusted S context must not carry stale translations into AS.
  MMU.flush_tlb("as-entry");
}
reg_t next_pc = enter_as ? STATE.astvec->read() & ~reg_t(3) : STATE.sepc->read();
set_pc_and_serialize(next_pc);
reg_t s = STATE.sstatus->read();
reg_t prev_prv = get_field(s, MSTATUS_SPP);
s = set_field(s, MSTATUS_SIE, enter_as ? 0 : get_field(s, MSTATUS_SPIE));
s = set_field(s, MSTATUS_SPIE, 1);
s = set_field(s, MSTATUS_SPP, PRV_U);
bool prev_virt = STATE.v;
if (!STATE.v) {
  if (p->extension_enabled('H')) {
    prev_virt = get_field(prev_hstatus, HSTATUS_SPV);
    reg_t new_hstatus = set_field(prev_hstatus, HSTATUS_SPV, 0);
    STATE.hstatus->write(new_hstatus);
  }

  STATE.mstatus->write(set_field(STATE.mstatus->read(), MSTATUS_MPRV, 0));
}
if (ZICFILP_xLPE(prev_virt, prev_prv)) {
  STATE.elp = static_cast<elp_t>(get_field(s, SSTATUS_SPELP));
}

if (STATE.prv == PRV_M) {
  STATE.mstatus->write(STATE.mstatus->read() & ~MSTATUS_MDT);
  if (prev_prv == PRV_U || prev_virt)
    STATE.mstatus->write(STATE.mstatus->read() & ~MSTATUS_SDT);
  if (prev_virt && prev_prv == PRV_U)
    STATE.vsstatus->write(STATE.vsstatus->read() & ~SSTATUS_SDT);
}

s = set_field(s, SSTATUS_SPELP, elp_t::NO_LP_EXPECTED);

if (STATE.prv == PRV_S) {
  s = set_field(s, SSTATUS_SDT, 0);
  if (!STATE.v && prev_virt && prev_prv == PRV_U)
    STATE.vsstatus->write(STATE.vsstatus->read() & ~SSTATUS_SDT);
}

STATE.sstatus->write(s);
reg_t as = STATE.asstatus->read();
STATE.nacc_a = get_field(as, NACC_ASSTATUS_SPA);
as = set_field(as, NACC_ASSTATUS_SPA, 0);
if (enter_as) {
  // Record the actual S origin; leave the saved AU EPC/cause/tval alone.
  as &= ~(NACC_ASSTATUS_ASPA | NACC_ASSTATUS_ASPP |
          NACC_ASSTATUS_ASPIE | NACC_ASSTATUS_ASIE);
  as |= NACC_ASSTATUS_ASPP;
}
STATE.asstatus->write(as);
p->set_privilege(prev_prv, prev_virt);
