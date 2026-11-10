# Boot-arguments

Made by `tools/boot-args.py --markdown`; CI fails if it is out of date with the source (`tools/boot-args.py --check`).

Every argument that the kernel, the loader or user space of this port reads, and which file reads it. **539 arguments**: 473 read by the kernel, 17 by the loader, 61 by user space (one is read in more than one place).

This is a catalogue of what exists and who reads it, not a manual. What an argument means is in the head of the file that reads it. The stable switches have a symbol in the configuration (`tools/hakios-config.py help SYMBOL`, `docs/configs.md`); the rest are the knobs of bring-up (a driver's steps, pauses, traces, test modes) and can change with the driver. Any of them can be given in a configuration with `CONFIG_BOOTARGS="name=value ..."`, which checks that each name is in this list.

Layers: **K** the kernel, **L** the loader, **U** user space (through `sysctl kern.bootargs`).

## GPU and display (105)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `adreno` | K | `hakios.adreno.c` | `CONFIG_ADRENO` |  |
| `adreno_blind` | K | `hakios.adreno.c` | `CONFIG_ADRENO_BLIND` |  |
| `adreno_dirty` | K | `hakios.adreno.c` | `CONFIG_ADRENO_DIRTY` |  |
| `adreno_fine` | K | `hakios.adreno.c` | `CONFIG_ADRENO_FINE` |  |
| `adreno_keep` | K | `hakios.adreno.c` | `CONFIG_ADRENO_KEEP` |  |
| `adreno_only` | K | `hakios.adreno.c` | `CONFIG_ADRENO_ONLY` |  |
| `adreno_pause` | K | `hakios.adreno.c` | `CONFIG_ADRENO_PAUSE` |  |
| `adreno_return` | K | `hakios.adreno.c` | `CONFIG_ADRENO_RETURN` |  |
| `adreno_skip` | K | `hakios.adreno.c` | `CONFIG_ADRENO_SKIP` |  |
| `adreno_speed` | K | `hakios.adreno.c` | `CONFIG_ADRENO_SPEED` |  |
| `adreno_stream1` | K | `hakios.adreno.c` | `CONFIG_ADRENO_STREAM1` |  |
| `adreno_watch` | K | `hakios.adreno.c` | `CONFIG_ADRENO_WATCH` |  |
| `backlight` | K | `hakios.backlight.cpp` | `CONFIG_BACKLIGHT` |  |
| `display_probe` | K | `hakios.adreno.c` | `CONFIG_DISPLAY_PROBE` |  |
| `fb_beat` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_BEAT` |  |
| `fb_ca_hz` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_CA_HZ` |  |
| `fb_diff` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_DIFF` |  |
| `fb_dpp` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_DPP` |  |
| `fb_filter` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_FILTER` |  |
| `fb_flip` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_FLIP` |  |
| `fb_hz` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_HZ` |  |
| `fb_measure` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_MEASURE` |  |
| `fb_pri` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_PRI` |  |
| `fb_queue` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_QUEUE` |  |
| `fb_rt` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_RT` |  |
| `fb_rt_us` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_RT_US` |  |
| `fb_swapwait` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_SWAPWAIT` |  |
| `fb_tcoal` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_TCOAL` |  |
| `fb_threads` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_THREADS` |  |
| `fb_zc` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_ZC` |  |
| `fb_zc_hold` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FB_ZC_HOLD` |  |
| `fbheight` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FBHEIGHT` |  |
| `fbscale` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FBSCALE` |  |
| `fbtrace` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FBTRACE` |  |
| `fbwidth` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_FBWIDTH` |  |
| `fps_log` | K | `MiniIOMobileFramebuffer.cpp`, `hakios.ufs.c` | `CONFIG_FPS_LOG` |  |
| `glblit` | U | `eagl.c` | `CONFIG_GLBLIT` |  |
| `glcensus` | U | `eagl.c` | `CONFIG_GLCENSUS` |  |
| `glcopy` | U | `eagl.c` | `CONFIG_GLCOPY` |  |
| `glenv` | U | `eagl.c` | `CONFIG_GLENV` |  |
| `glerrors` | U | `eagl.c` | `CONFIG_GLERRORS` |  |
| `glfetch` | U | `eagl.c` | `CONFIG_GLFETCH` |  |
| `glframes` | U | `eagl.c` | `CONFIG_GLFRAMES` |  |
| `glgroup` | U | `eagl.c` | `CONFIG_GLGROUP` |  |
| `glnumbers` | U | `eagl.c` | `CONFIG_GLNUMBERS` |  |
| `glpace` | U | `eagl.c` | `CONFIG_GLPACE` |  |
| `glplain` | U | `eagl.c` | `CONFIG_GLPLAIN` |  |
| `glprecision` | U | `eagl.c` | `CONFIG_GLPRECISION` |  |
| `glprof` | U | `eagl.c` | `CONFIG_GLPROF` |  |
| `glshaders` | U | `eagl.c` | `CONFIG_GLSHADERS` |  |
| `glshow` | U | `eagl.c` | `CONFIG_GLSHOW` |  |
| `glshow_part` | U | `eagl.c` | `CONFIG_GLSHOW_PART` |  |
| `glshow_zoom` | U | `eagl.c` | `CONFIG_GLSHOW_ZOOM` |  |
| `glskip` | U | `eagl.c` | `CONFIG_GLSKIP` |  |
| `glslow` | U | `eagl.c` | `CONFIG_GLSLOW` |  |
| `glstill` | U | `eagl.c` | `CONFIG_GLSTILL` |  |
| `glsurfaces` | U | `eagl.c` | `CONFIG_GLSURFACES` |  |
| `glsurfaces_mb` | U | `eagl.c` | `CONFIG_GLSURFACES_MB` |  |
| `glsync` | U | `eagl.c` | `CONFIG_GLSYNC` |  |
| `gltrace` | U | `eagl.c` | `CONFIG_GLTRACE` |  |
| `gltrace_at` | U | `eagl.c` | `CONFIG_GLTRACE_AT` |  |
| `gltrace_corners` | U | `eagl.c` | `CONFIG_GLTRACE_CORNERS` |  |
| `gltrace_draws` | U | `eagl.c` | `CONFIG_GLTRACE_DRAWS` |  |
| `gltrace_frames` | U | `eagl.c` | `CONFIG_GLTRACE_FRAMES` |  |
| `gltrace_function` | U | `eagl.c` | `CONFIG_GLTRACE_FUNCTION` |  |
| `gltrace_lit` | U | `eagl.c` | `CONFIG_GLTRACE_LIT` |  |
| `gltrace_look` | U | `eagl.c` | `CONFIG_GLTRACE_LOOK` |  |
| `gltrace_place` | U | `eagl.c` | `CONFIG_GLTRACE_PLACE` |  |
| `gltrace_shaders` | U | `eagl.c` | `CONFIG_GLTRACE_SHADERS` |  |
| `gltrace_uniforms` | U | `eagl.c` | `CONFIG_GLTRACE_UNIFORMS` |  |
| `glwait` | U | `eagl.c` | `CONFIG_GLWAIT` |  |
| `gpu` | K | `hakios.adreno.c` | `CONFIG_GPU` |  |
| `gpu_cached` | K | `hakios.gpu.cpp` | `CONFIG_GPU_CACHED` |  |
| `gpu_exact` | K | `hakios.adreno.c` | `CONFIG_GPU_EXACT` |  |
| `gpu_keep` | K | `hakios.gpu.cpp` | `CONFIG_GPU_KEEP` |  |
| `gpu_log` | K | `hakios.adreno.c` | `CONFIG_GPU_LOG` |  |
| `gpu_recover` | K | `hakios.adreno.c` | `CONFIG_GPU_RECOVER` |  |
| `gpu_va` | K | `hakios.adreno.c` | `CONFIG_GPU_VA` |  |
| `mali` | K | `hakios.mali.c` | `CONFIG_MALI` |  |
| `mali_acpm` | K | `hakios.mali.c` | `CONFIG_MALI_ACPM` |  |
| `mali_fastmap` | K | `hakios.mali.c` | `CONFIG_MALI_FASTMAP` |  |
| `mali_flush` | K | `hakios.mali.cpp` | `CONFIG_MALI_FLUSH` |  |
| `mali_gov` | K | `hakios.mali.c` | `CONFIG_MALI_GOV` |  |
| `mali_gov_frame` | K | `hakios.mali.c` | `CONFIG_MALI_GOV_FRAME` |  |
| `mali_mhz` | K | `hakios.mali.c` | `CONFIG_MALI_MHZ` |  |
| `mali_nap_us` | K | `hakios.mali.c` | `CONFIG_MALI_NAP_US` |  |
| `mali_pll` | K | `hakios.mali.c` | `CONFIG_MALI_PLL` |  |
| `mali_trace` | K | `hakios.mali.c` | `CONFIG_MALI_TRACE` |  |
| `mali_uv` | K | `hakios.mali.c` | `CONFIG_MALI_UV` |  |
| `metal` | KU | `MiniIOMobileFramebuffer.cpp`, `hakios_metal_shown.c` | `CONFIG_METAL` |  |
| `panel` | K | `hakios.s6e3ha8.c` | `CONFIG_PANEL` |  |
| `panel_brightness` | K | `hakios.s6e3ha8.c` | `CONFIG_PANEL_BRIGHTNESS` |  |
| `panel_gamma` | K | `hakios.s6e3ha8.c` | `CONFIG_PANEL_GAMMA` |  |
| `panel_gamma_log` | K | `hakios.s6e3ha8.c` | `CONFIG_PANEL_GAMMA_LOG` |  |
| `rec_mb` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_REC_MB` |  |
| `rec_ring` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_REC_RING` |  |
| `rec_secs` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_REC_SECS` |  |
| `rec_start` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_REC_START` |  |
| `rec_step` | K | `MiniIOMobileFramebuffer.cpp` | `CONFIG_REC_STEP` |  |
| `surf_clear` | K | `MiniIOSurface.cpp` | `CONFIG_SURF_CLEAR` |  |
| `surf_wrap` | K | `MiniIOSurface.cpp` | `CONFIG_SURF_WRAP` |  |
| `surftrace` | K | `MiniIOSurface.cpp` | `CONFIG_SURFTRACE` |  |
| `surftrace_of` | K | `MiniIOSurface.cpp` | `CONFIG_SURFTRACE_OF` |  |
| `touch_lat` | KU | `MiniIOMobileFramebuffer.cpp`, `eagl.c`, `hakios.goodix.c`, `hakios.gpu.cpp`, `hakios.input.c`, `inject.c`, `inject_sb.c` | `CONFIG_TOUCH_LAT` |  |
| `vblur` | U | `eagl.c` | `CONFIG_VBLUR` |  |

## Security stand-ins (1)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `privs` | K | `kern_minimac.c` | `CONFIG_PRIVS` |  |

## Wi-Fi (49)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `brcmfmac` | K | `hakios.bcm4361_bt.c`, `hakios.brcmfmac.c`, `lx_brcmf.c` | `CONFIG_BRCMFMAC` |  |
| `brcmfmac_debug` | K | `lx_brcmf.c` | `CONFIG_BRCMFMAC_DEBUG` |  |
| `brcmfmac_poll` | K | `lx_pci.c` | `CONFIG_BRCMFMAC_POLL` |  |
| `brcmfmac_poll_ms` | K | `lx_pci.c` | `CONFIG_BRCMFMAC_POLL_MS` |  |
| `brcmfmac_tcm_idx` | K | `lx_brcmf.c` | `CONFIG_BRCMFMAC_TCM_IDX` |  |
| `wifi` | KU | `hakios.wifi.c`, `net_test.m` | `CONFIG_WIFI` |  |
| `wifi_again` | K | `hakios.wifi.c` | `CONFIG_WIFI_AGAIN` |  |
| `wifi_ask` | K | `hakios.wifi.c` | `CONFIG_WIFI_ASK` |  |
| `wifi_ba` | K | `lx_80211.c` | `CONFIG_WIFI_BA` | implied by wifi=1 |
| `wifi_cal` | K | `hakios.wlfw.c` | `CONFIG_WIFI_CAL` |  |
| `wifi_dirty` | K | `hakios.wifi.c` | `CONFIG_WIFI_DIRTY` |  |
| `wifi_dma_at` | K | `hakios.lx.c` | `CONFIG_WIFI_DMA_AT` |  |
| `wifi_file` | K | `hakios.wifi.c` | `CONFIG_WIFI_FILE` |  |
| `wifi_fine` | K | `hakios.wifi.c` | `CONFIG_WIFI_FINE` |  |
| `wifi_fw` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_FW` |  |
| `wifi_hold` | K | `lx_bus.c` | `CONFIG_WIFI_HOLD` |  |
| `wifi_irq` | K | `lx_bus.c` | `CONFIG_WIFI_IRQ` | implied by wifi=1 |
| `wifi_join` | K | `lx_bus.c` | `CONFIG_WIFI_JOIN` |  |
| `wifi_keep` | K | `hakios.wifi.c` | `CONFIG_WIFI_KEEP` | implied by wifi=1 |
| `wifi_listen` | K | `hakios.wifi.c` | `CONFIG_WIFI_LISTEN` |  |
| `wifi_loud` | K | `hakios.wifi.c` | `CONFIG_WIFI_LOUD` |  |
| `wifi_lx` | K | `hakios.wifi.c`, `lx_bus.c` | `CONFIG_WIFI_LX` | implied by wifi=1 (as 2) |
| `wifi_lx_step` | K | `lx_base.c` | `CONFIG_WIFI_LX_STEP` |  |
| `wifi_mac` | K | `hakios.brcmfmac.c`, `hakios.wifi.c` | `CONFIG_WIFI_MAC` |  |
| `wifi_mission` | K | `hakios.wlfw.c` | `CONFIG_WIFI_MISSION` |  |
| `wifi_new` | K | `hakios.wifi.c` | `CONFIG_WIFI_NEW` |  |
| `wifi_nvram` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_NVRAM` |  |
| `wifi_pause` | K | `hakios.wifi.c` | `CONFIG_WIFI_PAUSE` |  |
| `wifi_pcie` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE` |  |
| `wifi_pcie_coherent` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE_COHERENT` |  |
| `wifi_pcie_mmu_bypass` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE_MMU_BYPASS` |  |
| `wifi_pcie_mmu_probe` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE_MMU_PROBE` |  |
| `wifi_pcie_tries` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE_TRIES` |  |
| `wifi_pcie_ungate` | K | `hakios.exynos_pcie.c` | `CONFIG_WIFI_PCIE_UNGATE` |  |
| `wifi_psk` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_PSK` |  |
| `wifi_quiet` | K | `lx_bus.c` | `CONFIG_WIFI_QUIET` |  |
| `wifi_radio_read` | K | `hakios.wifi.c` | `CONFIG_WIFI_RADIO_READ` |  |
| `wifi_ready` | K | `hakios.wlfw.c` | `CONFIG_WIFI_READY` |  |
| `wifi_return` | K | `hakios.wifi.c` | `CONFIG_WIFI_RETURN` |  |
| `wifi_scan` | K | `lx_bus.c` | `CONFIG_WIFI_SCAN` |  |
| `wifi_smmu` | K | `hakios.wifi.c` | `CONFIG_WIFI_SMMU` | implied by wifi=1 (as 2) |
| `wifi_ssid` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_SSID` |  |
| `wifi_station` | K | `lx_bus.c` | `CONFIG_WIFI_STATION` | implied by wifi=1 |
| `wifi_step` | K | `hakios.wifi.c` | `CONFIG_WIFI_STEP` |  |
| `wifi_vendor` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_VENDOR` |  |
| `wifi_vendor_part` | K | `hakios.brcmfmac.c` | `CONFIG_WIFI_VENDOR_PART` |  |
| `wifi_wait` | K | `hakios.wifi.c` | `CONFIG_WIFI_WAIT` |  |
| `wifi_watch` | K | `hakios.wifi.c` | `CONFIG_WIFI_WATCH` |  |
| `wifi_wide` | K | `lx_80211.c` | `CONFIG_WIFI_WIDE` | implied by wifi=1 |

## Cellular (53)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `modem` | K | `hakios.mpss.c` | `CONFIG_MODEM` |  |
| `modem_again` | K | `hakios.mpss.c` | `CONFIG_MODEM_AGAIN` |  |
| `modem_apn` | K | `hakios.mpss.c` | `CONFIG_MODEM_APN` |  |
| `modem_buffer` | K | `hakios.mpss.c` | `CONFIG_MODEM_BUFFER` |  |
| `modem_data_nobind` | K | `hakios.mpss.c` | `CONFIG_MODEM_DATA_NOBIND` |  |
| `modem_data_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_DATA_OFF` |  |
| `modem_data_wait` | K | `hakios.mpss.c` | `CONFIG_MODEM_DATA_WAIT` |  |
| `modem_dpm_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_DPM_OFF` |  |
| `modem_ipa` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA` |  |
| `modem_ipa_bus_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_BUS_OFF` |  |
| `modem_ipa_data_off` | K | `hakios.ipa.c` | `CONFIG_MODEM_IPA_DATA_OFF` |  |
| `modem_ipa_data_trace` | K | `hakios.cell.c`, `hakios.ipa.c` | `CONFIG_MODEM_IPA_DATA_TRACE` |  |
| `modem_ipa_fw` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_FW` |  |
| `modem_ipa_fw_at` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_FW_AT` |  |
| `modem_ipa_gsi_enable` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_GSI_ENABLE` |  |
| `modem_ipa_item` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_ITEM` |  |
| `modem_ipa_no_probe` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_NO_PROBE` |  |
| `modem_ipa_quiet` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_QUIET` |  |
| `modem_ipa_raw` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_RAW` |  |
| `modem_ipa_routes` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_ROUTES` |  |
| `modem_ipa_skip_uc` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_SKIP_UC` |  |
| `modem_ipa_smmu_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_IPA_SMMU_OFF` |  |
| `modem_keep` | K | `hakios.mpss.c` | `CONFIG_MODEM_KEEP` |  |
| `modem_listen` | K | `hakios.mpss.c` | `CONFIG_MODEM_LISTEN` |  |
| `modem_nas_wait` | K | `hakios.mpss.c` | `CONFIG_MODEM_NAS_WAIT` |  |
| `modem_new` | K | `hakios.mpss.c` | `CONFIG_MODEM_NEW` |  |
| `modem_no_ipa_qmi` | K | `hakios.mpss.c` | `CONFIG_MODEM_NO_IPA_QMI` |  |
| `modem_no_nav` | K | `hakios.mpss.c` | `CONFIG_MODEM_NO_NAV` |  |
| `modem_no_tftp` | K | `hakios.mpss.c` | `CONFIG_MODEM_NO_TFTP` |  |
| `modem_online` | K | `hakios.mpss.c` | `CONFIG_MODEM_ONLINE` |  |
| `modem_pause` | K | `hakios.mpss.c` | `CONFIG_MODEM_PAUSE` |  |
| `modem_return` | K | `hakios.mpss.c` | `CONFIG_MODEM_RETURN` |  |
| `modem_rmtfs_delay` | K | `hakios.mpss.c` | `CONFIG_MODEM_RMTFS_DELAY` |  |
| `modem_rmtfs_rw_fake` | K | `hakios.mpss.c` | `CONFIG_MODEM_RMTFS_RW_FAKE` |  |
| `modem_rmtfs_rw_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_RMTFS_RW_OFF` |  |
| `modem_share_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_SHARE_OFF` |  |
| `modem_share_test` | K | `hakios.mpss.c` | `CONFIG_MODEM_SHARE_TEST` |  |
| `modem_sim_probe_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_SIM_PROBE_OFF` |  |
| `modem_sim_switch_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_SIM_SWITCH_OFF` |  |
| `modem_step` | K | `hakios.mpss.c` | `CONFIG_MODEM_STEP` |  |
| `modem_wait` | K | `hakios.mpss.c` | `CONFIG_MODEM_WAIT` |  |
| `modem_wda_off` | K | `hakios.mpss.c` | `CONFIG_MODEM_WDA_OFF` |  |
| `soc_cmd_db` | K | `hakios.mpss.c` | `CONFIG_SOC_CMD_DB` |  |
| `soc_gsi_base` | K | `hakios.ipa.c` | `CONFIG_SOC_GSI_BASE` |  |
| `soc_imem` | K | `hakios.mpss.c` | `CONFIG_SOC_IMEM` |  |
| `soc_ipa_base` | K | `hakios.ipa.c` | `CONFIG_SOC_IPA_BASE` |  |
| `soc_ipa_fw` | K | `hakios.mpss.c` | `CONFIG_SOC_IPA_FW` |  |
| `soc_ipa_stream` | K | `hakios.mpss.c` | `CONFIG_SOC_IPA_STREAM` |  |
| `soc_ipa_stream_uc` | K | `hakios.mpss.c` | `CONFIG_SOC_IPA_STREAM_UC` |  |
| `soc_mpss_mem` | K | `hakios.mpss.c` | `CONFIG_SOC_MPSS_MEM` |  |
| `soc_pas_ipa` | K | `hakios.mpss.c` | `CONFIG_SOC_PAS_IPA` |  |
| `soc_pas_mpss` | K | `hakios.mpss.c` | `CONFIG_SOC_PAS_MPSS` |  |
| `soc_smem` | K | `hakios.mpss.c`, `hakios.socinfo.c` | `CONFIG_SOC_SMEM` |  |

## Audio and battery (61)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `abox` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX` |  |
| `abox_bclk_on` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_BCLK_ON` |  |
| `abox_burst` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_BURST` |  |
| `abox_dvfs` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_DVFS` |  |
| `abox_gain` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_GAIN` |  |
| `abox_mic` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_MIC` |  |
| `abox_mmu_off` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_MMU_OFF` |  |
| `abox_rdma_ctrl0` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_RDMA_CTRL0` |  |
| `abox_rdma_ctrl1` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_RDMA_CTRL1` |  |
| `abox_rdma_fmt` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_RDMA_FMT` |  |
| `abox_ring_pa` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_RING_PA` |  |
| `abox_route` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_ROUTE` |  |
| `abox_sbank` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_SBANK` |  |
| `abox_secs` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_SECS` |  |
| `abox_syspower` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_SYSPOWER` |  |
| `abox_vol` | K | `hakios.exynos_abox.c` | `CONFIG_ABOX_VOL` |  |
| `adsp` | K | `hakios.adsp.c` | `CONFIG_ADSP` |  |
| `adsp_again` | K | `hakios.adsp.c` | `CONFIG_ADSP_AGAIN` |  |
| `adsp_apr_wait` | K | `hakios.adsp.c` | `CONFIG_ADSP_APR_WAIT` |  |
| `adsp_keep` | K | `hakios.adsp.c` | `CONFIG_ADSP_KEEP` |  |
| `adsp_listen` | K | `hakios.adsp.c` | `CONFIG_ADSP_LISTEN` |  |
| `adsp_new` | K | `hakios.adsp.c` | `CONFIG_ADSP_NEW` |  |
| `adsp_pause` | K | `hakios.adsp.c` | `CONFIG_ADSP_PAUSE` |  |
| `adsp_return` | K | `hakios.adsp.c` | `CONFIG_ADSP_RETURN` |  |
| `adsp_step` | K | `hakios.adsp.c` | `CONFIG_ADSP_STEP` |  |
| `adsp_wait` | K | `hakios.adsp.c` | `CONFIG_ADSP_WAIT` |  |
| `battery` | KL | `hakios.adsp.c`, `hakios.battery.c`, `hakios.scfb_battery.c`, `loader.c` | `CONFIG_BATTERY` |  |
| `battery_shared_pa` | K | `hakios.battery.c` |  | the loader (the address of its charger page) |
| `battery_stop` | K | `hakios.battguard.c` | `CONFIG_BATTERY_STOP` |  |
| `battery_stop_poll` | K | `hakios.battguard.c` | `CONFIG_BATTERY_STOP_POLL` |  |
| `battmgr` | K | `hakios.adsp.c` | `CONFIG_BATTMGR` |  |
| `cs47l93` | K | `hakios.cs47l93.c` | `CONFIG_CS47L93` |  |
| `cs47l93_div` | K | `hakios.cs47l93.c` | `CONFIG_CS47L93_DIV` |  |
| `cs47l93_hp` | K | `hakios.cs47l93.c` | `CONFIG_CS47L93_HP` |  |
| `cs47l93_hp_vol` | K | `hakios.cs47l93.c` | `CONFIG_CS47L93_HP_VOL` |  |
| `cs47l93_mic_vol` | K | `hakios.cs47l93.c` | `CONFIG_CS47L93_MIC_VOL` |  |
| `max77705` | K | `hakios.max77705.c` | `CONFIG_MAX77705` |  |
| `max77705_haptic` | K | `hakios.max77705.c` | `CONFIG_MAX77705_HAPTIC` |  |
| `max77705_ipclk` | K | `hakios.max77705.c` | `CONFIG_MAX77705_IPCLK` |  |
| `max77705_steps` | K | `hakios.max77705.c` | `CONFIG_MAX77705_STEPS` |  |
| `max77705_usbsw` | K | `hakios.max77705.c` | `CONFIG_MAX77705_USBSW` |  |
| `max98512` | K | `hakios.max98512.c` | `CONFIG_MAX98512` |  |
| `max98512_gain` | K | `hakios.max98512.c` | `CONFIG_MAX98512_GAIN` |  |
| `mic` | K | `hakios.mic.c` | `CONFIG_MIC` |  |
| `mic_adc` | K | `hakios.mic.c` | `CONFIG_MIC_ADC` |  |
| `mic_input` | K | `hakios.mic.c` | `CONFIG_MIC_INPUT` |  |
| `mic_main` | K | `hakios.mic.c` | `CONFIG_MIC_MAIN` |  |
| `mic_secs` | K | `hakios.mic.c` | `CONFIG_MIC_SECS` |  |
| `mic_tap` | K | `hakios.mic.c` | `CONFIG_MIC_TAP` |  |
| `mic_test` | K | `hakios.mic.c` | `CONFIG_MIC_TEST` |  |
| `rec_sound` | K | `hakios.audio.c` | `CONFIG_REC_SOUND` |  |
| `rec_sound_fed` | K | `hakios.audio.c` | `CONFIG_REC_SOUND_FED` |  |
| `rec_sound_mic` | K | `hakios.audio.c` | `CONFIG_REC_SOUND_MIC` |  |
| `sound` | K | `hakios.sound.c` | `CONFIG_SOUND` |  |
| `sound_below` | K | `hakios.sound.c` | `CONFIG_SOUND_BELOW` |  |
| `sound_gain` | K | `hakios.sound.c` | `CONFIG_SOUND_GAIN` |  |
| `sound_guard_as` | K | `hakios.sound.c` | `CONFIG_SOUND_GUARD_AS` |  |
| `sound_pause` | K | `hakios.sound.c` | `CONFIG_SOUND_PAUSE` |  |
| `sound_secs` | K | `hakios.sound.c` | `CONFIG_SOUND_SECS` |  |
| `speaker_probe` | K | `hakios.speaker.c` | `CONFIG_SPEAKER_PROBE` |  |
| `virtio_sound` | K | `hakios.virtio_sound.c` | `CONFIG_VIRTIO_SOUND` |  |

## Cameras (28)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `cam` | KL | `1400-avf-scion-camera.patch`, `hakios.camera.c`, `loader.c` | `CONFIG_CAM` |  |
| `cam_after` | K | `hakios.camera.c` | `CONFIG_CAM_AFTER` |  |
| `cam_fake` | K | `1400-avf-scion-camera.patch`, `hakios.camera.c` | `CONFIG_CAM_FAKE` |  |
| `cam_fake_fps` | K | `hakios.camera.c` | `CONFIG_CAM_FAKE_FPS` |  |
| `cam_frames` | K | `hakios.camera.c` | `CONFIG_CAM_FRAMES` |  |
| `cam_gain` | K | `hakios.camera.c` | `CONFIG_CAM_GAIN` |  |
| `cam_keep_mb` | K | `hakios.camera.c` | `CONFIG_CAM_KEEP_MB` |  |
| `cam_mode` | K | `hakios.camera.c` | `CONFIG_CAM_MODE` |  |
| `cam_pattern` | K | `hakios.camera.c` | `CONFIG_CAM_PATTERN` |  |
| `cam_s9` | K | `hakios.exynos_camera.c`, `hakios.s2mpb02_torch.c` | `CONFIG_CAM_S9` |  |
| `cam_s9_addr` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_ADDR` |  |
| `cam_s9_ae` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_AE` |  |
| `cam_s9_ae_frames` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_AE_FRAMES` |  |
| `cam_s9_bayer` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_BAYER` |  |
| `cam_s9_dgain` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_DGAIN` |  |
| `cam_s9_exp` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_EXP` |  |
| `cam_s9_gain` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_GAIN` |  |
| `cam_s9_ipclk` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_IPCLK` |  |
| `cam_s9_ispdiv` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_ISPDIV` |  |
| `cam_s9_mmu_probe` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_MMU_PROBE` |  |
| `cam_s9_pack12` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_PACK12` |  |
| `cam_s9_probe` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_PROBE` |  |
| `cam_s9_sat` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_SAT` |  |
| `cam_s9_stride` | K | `hakios.exynos_camera.c` | `CONFIG_CAM_S9_STRIDE` |  |
| `cam_sensor` | K | `hakios.camera.c` | `CONFIG_CAM_SENSOR` |  |
| `cam_settle` | K | `hakios.camera.c` | `CONFIG_CAM_SETTLE` |  |
| `cam_shutter` | K | `hakios.camera.c` | `CONFIG_CAM_SHUTTER` |  |
| `cam_vblank` | K | `hakios.camera.c` | `CONFIG_CAM_VBLANK` |  |

## Sensors (10)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `shub` | K | `hakios.ssp.c` | `CONFIG_SHUB` |  |
| `shub_chub_on` | K | `hakios.ssp.c` | `CONFIG_SHUB_CHUB_ON` |  |
| `shub_div` | K | `hakios.ssp.c` | `CONFIG_SHUB_DIV` |  |
| `shub_hello` | K | `hakios.ssp.c` | `CONFIG_SHUB_HELLO` |  |
| `shub_keep` | K | `hakios.ssp.c` | `CONFIG_SHUB_KEEP` |  |
| `shub_log` | K | `hakios.ssp.c` | `CONFIG_SHUB_LOG` |  |
| `shub_ms` | K | `hakios.ssp.c` | `CONFIG_SHUB_MS` |  |
| `shub_req_len` | K | `hakios.ssp.c` | `CONFIG_SHUB_REQ_LEN` |  |
| `shub_req_rpc` | K | `hakios.ssp.c` | `CONFIG_SHUB_REQ_RPC` |  |
| `shub_sensors` | K | `hakios.ssp.c` | `CONFIG_SHUB_SENSORS` |  |

## Touch and buttons (17)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `buttons` | K | `hakios.buttons.c` | `CONFIG_BUTTONS` |  |
| `buttons_steps` | K | `hakios.buttons.c` | `CONFIG_BUTTONS_STEPS` |  |
| `exynos_keys` | K | `hakios.exynos_keys.c` | `CONFIG_EXYNOS_KEYS` |  |
| `press_power` | K | `hakios.buttons.c` | `CONFIG_PRESS_POWER` |  |
| `press_power_ms` | K | `hakios.buttons.c` | `CONFIG_PRESS_POWER_MS` |  |
| `s6sy761` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761` |  |
| `s6sy761_ipclk` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761_IPCLK` |  |
| `s6sy761_ldo` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761_LDO` |  |
| `s6sy761_pri` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761_PRI` |  |
| `s6sy761_steps` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761_STEPS` |  |
| `s6sy761_trace` | K | `hakios.s6sy761.c` | `CONFIG_S6SY761_TRACE` |  |
| `touch_flip` | K | `hakios.goodix.c` | `CONFIG_TOUCH_FLIP` |  |
| `touch_poll_us` | K | `hakios.goodix.c` | `CONFIG_TOUCH_POLL_US` |  |
| `touch_rate` | K | `hakios.goodix.c` | `CONFIG_TOUCH_RATE` |  |
| `touch_spi_hz` | K | `hakios.goodix.c` | `CONFIG_TOUCH_SPI_HZ` |  |
| `touchscreen` | K | `hakios.buttons.c`, `hakios.goodix.c` | `CONFIG_TOUCHSCREEN` |  |
| `virtio_input` | K | `hakios.virtio_input.c` | `CONFIG_VIRTIO_INPUT` |  |

## Storage and APFS (53)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `apfs_cheap` | K | `apfs_vfsops.c` | `CONFIG_APFS_CHEAP` |  |
| `apfs_check` | K | `hakios.apfs_check.c` | `CONFIG_APFS_CHECK` |  |
| `apfs_chunk` | K | `hakios.apfs_check.c` | `CONFIG_APFS_CHUNK` |  |
| `apfs_contents` | K | `hakios.apfs_check.c` | `CONFIG_APFS_CONTENTS` |  |
| `apfs_depth` | K | `hakios.apfs_check.c` | `CONFIG_APFS_DEPTH` |  |
| `apfs_dev` | K | `hakios.apfs_check.c` | `CONFIG_APFS_DEV` |  |
| `apfs_hash` | K | `hakios.apfs_check.c` | `CONFIG_APFS_HASH` |  |
| `apfs_max` | K | `hakios.apfs_check.c` | `CONFIG_APFS_MAX` |  |
| `apfs_meta_cluster` | K | `apfs_vfsops.c` | `CONFIG_APFS_META_CLUSTER` |  |
| `apfs_mnt` | K | `hakios.apfs_check.c` | `CONFIG_APFS_MNT` |  |
| `apfs_par` | K | `apfs_vfsops.c` | `CONFIG_APFS_PAR` |  |
| `apfs_root_c` | K | `apfs_vfsops.c` | `CONFIG_APFS_ROOT_C` |  |
| `apfs_root_v` | K | `apfs_vfsops.c` | `CONFIG_APFS_ROOT_V` |  |
| `apfs_round` | K | `apfs_vfsops.c` | `CONFIG_APFS_ROUND` |  |
| `apfs_skip` | K | `hakios.apfs_check.c` | `CONFIG_APFS_SKIP` |  |
| `apfs_sparse` | K | `hakios.sparse.c` | `CONFIG_APFS_SPARSE` |  |
| `apfs_store_len` | K | `hakios.apfs_check.c` | `CONFIG_APFS_STORE_LEN` |  |
| `apfs_store_note` | K | `hakios.apfs_check.c` | `CONFIG_APFS_STORE_NOTE` |  |
| `apfs_store_seed` | K | `hakios.apfs_check.c` | `CONFIG_APFS_STORE_SEED` |  |
| `apfs_subtree` | K | `hakios.apfs_check.c` | `CONFIG_APFS_SUBTREE` |  |
| `apfs_threads` | K | `hakios.apfs_check.c` | `CONFIG_APFS_THREADS` |  |
| `apfs_ubc_read` | K | `apfs_vfsops.c` | `CONFIG_APFS_UBC_READ` |  |
| `apfs_volume` | K | `hakios.apfs_check.c` | `CONFIG_APFS_VOLUME` |  |
| `apfs_volume_at` | K | `hakios.apfs_check.c` | `CONFIG_APFS_VOLUME_AT` |  |
| `apfs_walkers` | K | `hakios.apfs_check.c` | `CONFIG_APFS_WALKERS` |  |
| `apfs_write_at` | K | `hakios.apfs_check.c` | `CONFIG_APFS_WRITE_AT` |  |
| `apfs_write_byte` | K | `hakios.apfs_check.c` | `CONFIG_APFS_WRITE_BYTE` |  |
| `apfs_write_len` | K | `hakios.apfs_check.c` | `CONFIG_APFS_WRITE_LEN` |  |
| `apfs_write_path` | K | `hakios.apfs_check.c` | `CONFIG_APFS_WRITE_PATH` |  |
| `ufs` | KL | `hakios.ufs.c`, `loader.c` | `CONFIG_UFS` |  |
| `ufs_bench` | K | `hakios.ufs.c` | `CONFIG_UFS_BENCH` |  |
| `ufs_check` | K | `hakios.ufs.c` | `CONFIG_UFS_CHECK` |  |
| `ufs_native` | KL | `hakios.ufs.c`, `hakios.ufshc.c`, `loader.c` | `CONFIG_UFS_NATIVE` |  |
| `ufs_native_bench` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_BENCH` |  |
| `ufs_native_buffer` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_BUFFER` |  |
| `ufs_native_check` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_CHECK` |  |
| `ufs_native_hang` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_HANG` |  |
| `ufs_native_leave` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_LEAVE` |  |
| `ufs_native_log` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_LOG` |  |
| `ufs_native_ro` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_RO` |  |
| `ufs_native_slots` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_SLOTS` |  |
| `ufs_native_soak` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_SOAK` |  |
| `ufs_native_soc` | K | `hakios.ufs.c`, `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_SOC` |  |
| `ufs_native_spin` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_SPIN` |  |
| `ufs_native_step` | K | `hakios.ufshc.c` | `CONFIG_UFS_NATIVE_STEP` |  |
| `ufs_native_test` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_TEST` |  |
| `ufs_native_verify` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_VERIFY` |  |
| `ufs_native_write` | K | `hakios.ufs.c` | `CONFIG_UFS_NATIVE_WRITE` |  |
| `ufs_shared_pa` | K | `hakios.ufs.c` |  | the loader (the address of its block-service page) |
| `ufs_write` | KL | `hakios.ufs.c`, `loader.c` | `CONFIG_UFS_WRITE` |  |
| `var_save_ms` | K | `hakios.ufs.c` | `CONFIG_VAR_SAVE_MS` |  |
| `virtio_blk` | K | `hakios.virtio_blk.c` | `CONFIG_VIRTIO_BLK` |  |
| `virtio_blk_rw` | K | `hakios.virtio_blk.c` | `CONFIG_VIRTIO_BLK_RW` |  |

## Network (5)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `net_names` | K | `hakios.net.c` | `CONFIG_NET_NAMES` |  |
| `net_trace` | K | `hakios.net.c` | `CONFIG_NET_TRACE` |  |
| `virtio_net` | K | `hakios.virtio_net.c` | `CONFIG_VIRTIO_NET` |  |
| `virtio_net_ssid` | K | `hakios.virtio_net.c` | `CONFIG_VIRTIO_NET_SSID` |  |
| `virtio_net_wifi` | K | `hakios.virtio_net.c` | `CONFIG_VIRTIO_NET_WIFI` |  |

## USB (9)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `usb_gadget` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET` |  |
| `usb_gadget_clk` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_CLK` |  |
| `usb_gadget_crlf` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_CRLF` |  |
| `usb_gadget_drain` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_DRAIN` |  |
| `usb_gadget_fs` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_FS` |  |
| `usb_gadget_ldo` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_LDO` |  |
| `usb_gadget_poll` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_POLL` |  |
| `usb_gadget_soc` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_SOC` |  |
| `usb_gadget_susphy` | K | `hakios.dwc3.c` | `CONFIG_USB_GADGET_SUSPHY` |  |

## Haptics and torch (5)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `haptic` | K | `hakios.haptic.c` | `CONFIG_HAPTIC` |  |
| `haptic_effect` | K | `hakios.haptic.c` | `CONFIG_HAPTIC_EFFECT` |  |
| `torch` | KL | `hakios.s2mpb02_torch.c`, `hakios.sccam_torch.c`, `hakios.torch.c`, `hakios.sm5713.c`, `loader.c` | `CONFIG_TORCH` |  |
| `torch_max_s` | K | `hakios.s2mpb02_torch.c`, `hakios.torch.c`, `hakios.sm5713.c` | `CONFIG_TORCH_MAX_S` |  |
| `torch_secs` | K | `hakios.s2mpb02_torch.c`, `hakios.torch.c` | `CONFIG_TORCH_SECS` |  |

## Chip, thermal and PMIC (19)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `halt_off` | K | `hakios.exynos_poweroff.c` | `CONFIG_HALT_OFF` |  |
| `pmic` | K | `hakios.pmic.c` | `CONFIG_PMIC` |  |
| `pmic_list` | K | `hakios.pmic.c` | `CONFIG_PMIC_LIST` |  |
| `soc_smem_size` | K | `hakios.socinfo.c` | `CONFIG_SOC_SMEM_SIZE` |  |
| `socinfo` | K | `hakios.socinfo.c` | `CONFIG_SOCINFO` |  |
| `thermal` | K | `hakios.thermal.c` | `CONFIG_THERMAL` |  |
| `thermal_log` | K | `hakios.thermal.c` | `CONFIG_THERMAL_LOG` |  |
| `thermal_poll` | K | `hakios.thermal.c` | `CONFIG_THERMAL_POLL` |  |
| `thermal_stop` | K | `hakios.thermal.c` | `CONFIG_THERMAL_STOP` |  |
| `thermal_tm0` | K | `hakios.thermal.c` | `CONFIG_THERMAL_TM0` |  |
| `thermal_tm1` | K | `hakios.thermal.c` | `CONFIG_THERMAL_TM1` |  |
| `tmu` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU` |  |
| `tmu_batt_stop` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_BATT_STOP` |  |
| `tmu_enable` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_ENABLE` |  |
| `tmu_log` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_LOG` |  |
| `tmu_off` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_OFF` |  |
| `tmu_poll` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_POLL` |  |
| `tmu_stop` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_STOP` |  |
| `tmu_ungate` | K | `hakios.exynos_tmu.c` | `CONFIG_TMU_UNGATE` |  |

## Version (1)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `hakios_kver` | K | `hakios.version.c` |  | the loader (from the payload's kernel.ver) |

## CPU, memory and boot (23)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `applog` | K | `(patch)` | `CONFIG_APPLOG` |  |
| `applog_name` | K | `(patch)` | `CONFIG_APPLOG_NAME` |  |
| `bootcpu` | KL | `loader.c`, `loader_return.c` | `CONFIG_BOOTCPU` |  |
| `cpufreq` | K | `hakios.cpufreq.c` | `CONFIG_CPUFREQ` |  |
| `cpuload` | K | `hakios.cpufreq.c` | `CONFIG_CPULOAD` |  |
| `ddr` | K | `hakios.cpufreq.c` | `CONFIG_DDR` |  |
| `ddr_rest` | K | `hakios.cpufreq.c` | `CONFIG_DDR_REST` |  |
| `ddr_vote` | K | `hakios.cpufreq.c` | `CONFIG_DDR_VOTE` |  |
| `epoch` | K | `GenericARMPE.cpp` |  | xl (the host's clock) |
| `hv_wdt_pet` | K | `loader_return.c` |  | the loader, when it has armed the watchdog (the runner also passes it) |
| `loader_return_after` | KL | `loader.c`, `loader_return.c` |  | xl (XL_SECS) |
| `machine_name` | K | `GenericARMPE.cpp` | `CONFIG_MACHINE_NAME` |  |
| `membench` | K | `hakios.cpufreq.c` | `CONFIG_MEMBENCH` |  |
| `rd` | KL | `(patch)`, `1100-virtio-pci-transport.patch`, `loader.c` | `CONFIG_RD` |  |
| `regtrace` | K | `(patch)` | `CONFIG_REGTRACE` |  |
| `return_cpu` | K | `loader_return.c` | `CONFIG_RETURN_CPU` |  |
| `return_panic` | K | `loader_return.c` | `CONFIG_RETURN_PANIC` |  |
| `small_cores` | K | `(patch)` | `CONFIG_SMALL_CORES` |  |
| `small_cores_after` | K | `GenericARMPE.cpp` | `CONFIG_SMALL_CORES_AFTER` |  |
| `syssleep` | K | `(patch)` | `CONFIG_SYSSLEEP` |  |
| `systrace` | K | `(patch)`, `user_exception_trace.c` | `CONFIG_SYSTRACE` |  |
| `systrace_name` | K | `(patch)` | `CONFIG_SYSTRACE_NAME` |  |
| `xpcproxy_idversion` | K | `(patch)` | `CONFIG_XPCPROXY_IDVERSION` |  |

## Debugging (8)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `ksample` | K | `user_exception_trace.c` | `CONFIG_KSAMPLE` |  |
| `ksample_us` | K | `user_exception_trace.c` | `CONFIG_KSAMPLE_US` |  |
| `ustacks` | K | `user_exception_trace.c` | `CONFIG_USTACKS` |  |
| `ustacks_at` | K | `user_exception_trace.c` | `CONFIG_USTACKS_AT` |  |
| `ustacks_count` | K | `user_exception_trace.c` | `CONFIG_USTACKS_COUNT` |  |
| `ustacks_ms` | K | `user_exception_trace.c` | `CONFIG_USTACKS_MS` |  |
| `ustacks_threads` | K | `user_exception_trace.c` | `CONFIG_USTACKS_THREADS` |  |
| `ustacks_tname` | K | `user_exception_trace.c` | `CONFIG_USTACKS_TNAME` |  |

## Loader (8)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `cpumask` | L | `loader.c` | `CONFIG_CPUMASK` |  |
| `cpus` | L | `loader.c` | `CONFIG_CPUS` |  |
| `gamut` | L | `loader.c` | `CONFIG_GAMUT` |  |
| `loader_platform` | L | `loader.c` | `CONFIG_LOADER_PLATFORM` |  |
| `recovery` | L | `loader.c` | `CONFIG_RECOVERY` |  |
| `serial` | L | `loader.c` | `CONFIG_SERIAL` |  |
| `ufs_probe` | L | `loader.c` | `CONFIG_UFS_PROBE` |  |
| `wdt` | L | `loader.c` | `CONFIG_WDT` |  |

## User space (84)

| Argument | Layer | Read by | Configuration | Set by |
|---|---|---|---|---|
| `autounlock` | U | `inject_sb.c` | `CONFIG_AUTOUNLOCK` |  |
| `avf_exit` | K | `1001-avf-psci-system-off.patch` | `CONFIG_AVF_EXIT` |  |
| `brightness_bridge` | U | `inject_sb.c` | `CONFIG_BRIGHTNESS_BRIDGE` |  |
| `bt` | K | `hakios.bcm4361_bt.c`, `hakios.bt.c` | `CONFIG_BT` |  |
| `bt_again` | K | `hakios.bt.c` | `CONFIG_BT_AGAIN` |  |
| `bt_baud` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_BAUD` |  |
| `bt_expander_reset` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_EXPANDER_RESET` |  |
| `bt_hcd` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_HCD` |  |
| `bt_keep` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_KEEP` |  |
| `bt_module` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_MODULE` |  |
| `bt_scan` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_SCAN` |  |
| `bt_tty` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_TTY` |  |
| `bt_uart_mhz` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_UART_MHZ` |  |
| `bt_vendor` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_VENDOR` |  |
| `bt_vendor_part` | K | `hakios.bcm4361_bt.c` | `CONFIG_BT_VENDOR_PART` |  |
| `ca_vbl` | U | `inject.c` | `CONFIG_CA_VBL` |  |
| `caenv` | U | `inject.c`, `inject_sb.c` | `CONFIG_CAENV` |  |
| `camtl` | U | `inject.c` | `CONFIG_CAMTL` |  |
| `cc_fade` | U | `inject_sb.c` | `CONFIG_CC_FADE` |  |
| `cohtest` | K | `hakios.exynos_m3.c` | `CONFIG_COHTEST` |  |
| `cohtest_delay` | K | `hakios.exynos_m3.c` | `CONFIG_COHTEST_DELAY` |  |
| `console_quiet` | K | `1003-avf-console-quiet.patch` | `CONFIG_CONSOLE_QUIET` |  |
| `cpufreq_gov` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV` |  |
| `cpufreq_gov_big_max` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_BIG_MAX` |  |
| `cpufreq_gov_big_min` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_BIG_MIN` |  |
| `cpufreq_gov_lit_max` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_LIT_MAX` |  |
| `cpufreq_gov_lit_min` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_LIT_MIN` |  |
| `cpufreq_gov_log` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_LOG` |  |
| `cpufreq_gov_ms` | K | `hakios.exynos_cpufreq.c` | `CONFIG_CPUFREQ_GOV_MS` |  |
| `cs_relax_platform_task_ports` | K | `1002-avf-security-level.patch` | `CONFIG_CS_RELAX_PLATFORM_TASK_PORTS` |  |
| `early_log` | K | `1503-early-log-and-panic.patch` | `CONFIG_EARLY_LOG` |  |
| `esslog` | K | `1507-esslog-exynos-snapshot.patch`, `1509-m3-diag-and-pan-off.patch` | `CONFIG_ESSLOG` |  |
| `gl` | U | `inject.c`, `inject_sb.c` | `CONFIG_GL` |  |
| `glcompile` | U | `inject.c` | `CONFIG_GLCOMPILE` |  |
| `glno` | U | `inject.c` | `CONFIG_GLNO` |  |
| `glsb3` | U | `inject.c` | `CONFIG_GLSB3` |  |
| `glyes` | U | `inject.c` | `CONFIG_GLYES` |  |
| `gov_boost_big` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_BOOST_BIG` |  |
| `gov_boost_lit` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_BOOST_LIT` |  |
| `gov_boost_ms` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_BOOST_MS` |  |
| `gov_down` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_DOWN` |  |
| `gov_frame_big` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_FRAME_BIG` |  |
| `gov_frame_ms` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_FRAME_MS` |  |
| `gov_target` | K | `hakios.exynos_cpufreq.c` | `CONFIG_GOV_TARGET` |  |
| `gpu3d` | K | `hakios.vgpu3d_xnu.c` | `CONFIG_GPU3D` |  |
| `gpu3d_size` | K | `hakios.vgpu3d_xnu.c` | `CONFIG_GPU3D_SIZE` |  |
| `immediate_NMI` | K | `1509-m3-diag-and-pan-off.patch` | `CONFIG_IMMEDIATE_NMI` |  |
| `ioaes` | K | `MiniIOAESAccelerator.cpp` | `CONFIG_IOAES` |  |
| `isa_floor` | K | `1510-isa-floor.patch` | `CONFIG_ISA_FLOOR` |  |
| `m3_diag` | K | `1509-m3-diag-and-pan-off.patch` | `CONFIG_M3_DIAG` |  |
| `m3_flush` | K | `1509-m3-diag-and-pan-off.patch` | `CONFIG_M3_FLUSH` |  |
| `m3_ids` | K | `1509-m3-diag-and-pan-off.patch` | `CONFIG_M3_IDS` |  |
| `m3_prep` | K | `1510-isa-floor.patch`, `1511-m3-user-cache-pou.patch`, `hakios.exynos_m3.c` | `CONFIG_M3_PREP` |  |
| `md0` | K | `1101-virtio-disk-as-md0.patch` | `CONFIG_MD0` |  |
| `metal_profile` | U | `hakiosmetal_air.m` | `CONFIG_METAL_PROFILE` |  |
| `pan` | K | `1509-m3-diag-and-pan-off.patch` | `CONFIG_PAN` |  |
| `poster_retry` | U | `poster_retry.c` | `CONFIG_POSTER_RETRY` |  |
| `root_apfs` | U | `init.c` | `CONFIG_ROOT_APFS` |  |
| `rtc_pmic` | K | `hakios.s2mps18_rtc.c` | `CONFIG_RTC_PMIC` |  |
| `sb_delay` | U | `inject_sb.c` |  | xl (XL_SB_DELAY) |
| `sb_fps` | U | `inject_sb.c` | `CONFIG_SB_FPS` |  |
| `sccam.addr` | K | `1400-avf-scion-camera.patch`, `hakios.sccam_torch.c` | `CONFIG_SCCAM_ADDR` |  |
| `sccam.h` | K | `1400-avf-scion-camera.patch` | `CONFIG_SCCAM_H` |  |
| `sccam.size` | K | `1400-avf-scion-camera.patch` | `CONFIG_SCCAM_SIZE` |  |
| `sccam.turn` | K | `1400-avf-scion-camera.patch` | `CONFIG_SCCAM_TURN` |  |
| `sccam.w` | K | `1400-avf-scion-camera.patch` | `CONFIG_SCCAM_W` |  |
| `scfb.addr` | K | `1200-avf-scfb-display.patch`, `1202-virtio-gpu-2d.patch`, `hakios.scfb_battery.c` | `CONFIG_SCFB_ADDR` |  |
| `scgpu.addr` | K | `hakios.vgpu3d_xnu.c` | `CONFIG_SCGPU_ADDR` |  |
| `scgpu.size` | K | `hakios.vgpu3d_xnu.c` | `CONFIG_SCGPU_SIZE` |  |
| `scsnd.addr` | K | `hakios.scsnd.c` | `CONFIG_SCSND_ADDR` |  |
| `scsnd.size` | K | `hakios.scsnd.c` | `CONFIG_SCSND_SIZE` |  |
| `security` | K | `1002-avf-security-level.patch` | `CONFIG_SECURITY` |  |
| `switcher_wallpaper` | U | `inject_sb.c` | `CONFIG_SWITCHER_WALLPAPER` |  |
| `system_apfs` | U | `init.c` | `CONFIG_SYSTEM_APFS` |  |
| `touch_fault` | U | `inject.c` | `CONFIG_TOUCH_FAULT` |  |
| `ucache_trap` | K | `1511-m3-user-cache-pou.patch` | `CONFIG_UCACHE_TRAP` |  |
| `var_hfs` | U | `init.c` | `CONFIG_VAR_HFS` |  |
| `var_seed` | U | `init.c` | `CONFIG_VAR_SEED` |  |
| `var_store` | U | `init.c` | `CONFIG_VAR_STORE` |  |
| `vgpu_height` | K | `hakios.vgpu.c` | `CONFIG_VGPU_HEIGHT` |  |
| `vgpu_width` | K | `hakios.vgpu.c` | `CONFIG_VGPU_WIDTH` |  |
| `virtio_console` | K | `hakios.virtio_console.c` | `CONFIG_VIRTIO_CONSOLE` |  |
| `virtio_mmio` | K | `hakios.virtio.c` | `CONFIG_VIRTIO_MMIO` |  |
| `virtio_poll` | K | `hakios.virtio.c` | `CONFIG_VIRTIO_POLL` |  |
