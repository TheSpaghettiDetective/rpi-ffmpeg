#ifndef rpi_shader_H
#define rpi_shader_H

extern unsigned int rpi_shader[];

#define mc_setup_uv (rpi_shader + 0)
#define mc_filter_uv (rpi_shader + 130)
#define mc_filter_uv_b0 (rpi_shader + 294)
#define mc_filter_uv_b (rpi_shader + 472)
#define mc_exit_c (rpi_shader + 620)
#define mc_exit (rpi_shader + 650)
#define mc_setup (rpi_shader + 666)
#define mc_filter (rpi_shader + 940)
#define mc_filter_b (rpi_shader + 1060)
#define mc_interrupt_exit12c (rpi_shader + 1180)
#define mc_interrupt_exit12 (rpi_shader + 1230)
#define mc_exit1 (rpi_shader + 1266)
#define mc_end (rpi_shader + 1282)

#endif
