
#ifndef BOOTLOADER_HOOKS_H
#define BOOTLOADER_HOOKS_H
/**
 * @brief Function executed *before* the second stage bootloader initialization,
 * if provided.
 */
void __attribute__((weak)) bootloader_before_init(void);

/**
 * @brief Function executed *after* the second stage bootloader initialization,
 * if provided.
 */
void __attribute__((weak)) bootloader_after_init(void);

#endif // BOOTLOADER_HOOKS_H