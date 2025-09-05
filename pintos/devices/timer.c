#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS가 부팅된 이후의 타이머 틱 수 */
static int64_t ticks;

/* 한 타이머 틱 당 반복 루프 횟수 (timer_calibrate()에서 초기화됨) */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
   1초에 PIT_FREQ번 인터럽트를 발생시키고,
   해당 인터럽트를 등록한다. */
void
timer_init (void) {
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* 카운터 0, LSB → MSB, 모드 2, 이진 */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* 짧은 지연을 구현하기 위해 사용되는 loops_per_tick을 보정한다. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* 한 틱보다 작은 가장 큰 2의 제곱수로 설정 */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* loops_per_tick의 다음 8비트를 보정 */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS 부팅 이후의 타이머 틱 수를 반환한다. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* timer_ticks()가 반환한 시점 이후 경과한 틱 수를 반환한다. */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* 대략 ticks 틱 동안 실행을 중단한다. */
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();	/* 현재 시간(틱 수)을 저장 */

	ASSERT (intr_get_level () == INTR_ON);	/* 인터럽트가 켜져 있는지 확인 */
	while (timer_elapsed (start) < ticks)
		thread_yield ();	/* 원하는 시간이 지날 때까지 CPU를 양보 */
}

/* 대략 ms 밀리초 동안 실행을 중단한다. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* 대략 us 마이크로초 동안 실행을 중단한다. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* 대략 ns 나노초 동안 실행을 중단한다. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 출력한다. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* 타이머 인터럽트 핸들러 */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();
}

/* LOOPS 반복이 한 틱 이상 걸리면 true를 반환한다. */
static bool
too_many_loops (unsigned loops) {
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	start = ticks;
	busy_wait (loops);

	barrier ();
	return start != ticks;
}

/* 짧은 지연을 위해 LOOPS번 반복 실행한다.
   (NO_INLINE: 코드 정렬에 따른 타이밍 영향을 줄이기 위함) */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* 대략 num/denom 초 동안 잠들게 한다. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* 한 틱 이상이면 CPU를 양보하며 sleep */
		timer_sleep (ticks);
	} else {
		/* 그보다 짧으면 busy-wait 사용 (오버플로우 방지를 위해 1000으로 스케일링) */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}