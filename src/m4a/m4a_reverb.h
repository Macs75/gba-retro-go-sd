/* m4a_reverb.h -- M4A's reverb loop run natively (m4a_reverb.c) */
#ifndef M4A_REVERB_H
#define M4A_REVERB_H
#ifdef __cplusplus
extern "C" {
#endif
/* find the loop in IWRAM and take the native-routine hook if it is free */
void m4a_reverb_find(void);
/* drop the hook (a new game, a reset) */
void m4a_reverb_reset(void);
#ifdef __cplusplus
}
#endif
#endif
