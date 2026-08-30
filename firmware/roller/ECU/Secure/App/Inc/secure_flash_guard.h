#ifndef SECURE_FLASH_GUARD_H
#define SECURE_FLASH_GUARD_H

/* A Flash controller that cannot be re-locked is not a recoverable runtime
   condition. Reset immediately; reset restores the hardware lock state. */
void SecureFlash_LockSecureOrReset(void);
void SecureFlash_LockNonSecureOrReset(void);

#endif /* SECURE_FLASH_GUARD_H */
