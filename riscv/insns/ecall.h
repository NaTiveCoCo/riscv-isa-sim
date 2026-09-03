switch (STATE.prv)
{
  case PRV_U: throw trap_user_ecall();
  case PRV_S:
    if (STATE.nacc_a)
      throw trap_t(CAUSE_AS_ECALL);
    else if (STATE.v)
      throw trap_virtual_supervisor_ecall();
    else
      throw trap_supervisor_ecall();
  case PRV_M: throw trap_machine_ecall();
  default: abort();
}
