.thumb_func
svc_entry:
  ldr r0, =outgoing_sp
  ldr r0, [r0]
  msr psp, r0
  ldr lr, =0xfffffffd
  b pendsv

.global resume_vpop
.thumb_func
resume_vpop:
  vpop {d8-d11}
  mov r0, sp
  b completed
