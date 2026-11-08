/*
 * PLACEHOLDER. Samsung's ACPM "fvp" plugin for the Exynos 9610 is Samsung's binary and is not in this repository.
 * Make the real header from the phone's own stock BOOT image (it checks the plugin's SHA-256):
 *
 *     tools/a50-fvp.py boot.img        (boot.img: from the stock AP tarball, or read from the phone in TWRP)
 *     git update-index --skip-worktree drivers/core/hakios.exynos9610_fvp.h
 *
 * With this placeholder the ACPM driver leaves the plugin alone (acpm9610=6 says so and does nothing).
 */
#define HAKIOS_FVP_9610_SIZE 0
static const uint8_t hakios_fvp_9610[1] = { 0 };
