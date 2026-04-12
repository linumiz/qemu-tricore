tricore_ss = ss.source_set()
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testboard.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_TESTBOARD', if_true: files('tricore_testdevice.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_SCU', if_true: files('tricore_scu.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_VIRT', if_true: files('tricore_virt.c'))
tricore_ss.add(when: 'CONFIG_TRICORE_SFR', if_true: files('tricore_sfr.c'))
tricore_ss.add(when: 'CONFIG_TRIBOARD', if_true: files('triboard.c'))
tricore_ss.add(when: 'CONFIG_TC4X_CLOCK', if_true: files('tc4x_clock.c'))
specific_ss.add(when: 'CONFIG_TC4DX_SOC', if_true: files('tc4dx_soc.c'))

hw_arch += {'tricore': tricore_ss}
