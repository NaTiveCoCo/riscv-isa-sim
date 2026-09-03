require_extension('S');
require(STATE.nacc_a && STATE.prv == PRV_S);

reg_t next_pc = STATE.asepc->read();
set_pc_and_serialize(next_pc);

reg_t as = STATE.asstatus->read();
reg_t prev_prv = get_field(as, NACC_ASSTATUS_ASPP);
STATE.nacc_a = get_field(as, NACC_ASSTATUS_ASPA);
as = set_field(as, NACC_ASSTATUS_ASIE, get_field(as, NACC_ASSTATUS_ASPIE));
as = set_field(as, NACC_ASSTATUS_ASPIE, 1);
as = set_field(as, NACC_ASSTATUS_ASPP, 0);
as = set_field(as, NACC_ASSTATUS_ASPA, 0);
STATE.asstatus->write(as);
p->set_privilege(prev_prv, false);
