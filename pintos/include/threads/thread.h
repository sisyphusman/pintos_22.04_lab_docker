#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드의 생애주기 상태들. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중은 아니지만 실행할 준비가 된 스레드. */
	THREAD_BLOCKED,     /* 어떤 이벤트가 발생하길 기다리는 스레드. */
	THREAD_DYING        /* 곧 파괴될 스레드. */
};

/* 스레드 식별자 타입.
   원한다면 다른 타입으로 재정의할 수 있다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t에서의 오류 값. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* 최소 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 최대 우선순위. */

/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 자신만의 4 kB 페이지에 저장된다.
 * 스레드 구조체 자체는 페이지의 가장 아래(오프셋 0)에 위치한다.
 * 페이지의 나머지 공간은 스레드의 커널 스택을 위해 예약되며,
 * 커널 스택은 페이지의 꼭대기(오프셋 4 kB)에서 아래로 자란다.
 * 그림은 다음과 같다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이로부터 두 가지 중요한 점이 나온다:
 *
 *    1. 첫째, `struct thread`가 너무 커지면 안 된다.
 *       커지면 커널 스택에 충분한 공간이 남지 않는다.
 *       기본 `struct thread`는 몇 바이트밖에 되지 않는다.
 *       가능하면 1 kB보다 훨씬 작게 유지되어야 한다.
 *
 *    2. 둘째, 커널 스택이 너무 커지면 안 된다.
 *       스택이 넘치면 스레드 상태를 망가뜨린다.
 *       따라서 커널 함수에서는 큰 구조체나 배열을
 *       정적이 아닌 지역 변수로 할당하지 말 것.
 *       대신 malloc()이나 palloc_get_page() 같은 동적 할당을 사용하라.
 *
 * 이 둘 중 어느 문제가 발생하더라도 보통 첫 증상은
 * thread_current()에서의 어설션 실패다. 이 함수는
 * 실행 중인 스레드의 `struct thread` 안 `magic` 멤버가
 * THREAD_MAGIC으로 설정되어 있는지 검사한다.
 * 스택 오버플로우는 보통 이 값을 바꿔서 어설션을 유발한다. */
/* `elem` 멤버는 이중 용도를 가진다.
 * 실행 큐(run queue, thread.c)의 원소가 될 수도 있고,
 * 세마포어 대기 리스트(synch.c)의 원소가 될 수도 있다.
 * 두 경우 모두에서 사용할 수 있는 이유는 상호 배타적이기 때문이다:
 * 준비(ready) 상태의 스레드만 실행 큐에 있고,
 * 블록(blocked) 상태의 스레드만 세마포어 대기 리스트에 있다. */

struct thread {
	/* thread.c에서 소유. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름(디버깅용). */
	int priority;                       /* 우선순위. */
	int64_t wakeup_tick;				// 이 스레드가 깨워져야 할 시간(tick 단위)

	/* thread.c와 synch.c 사이에서 공유. */
	struct list_elem elem;              /* 리스트 원소. */

#ifdef USERPROG
	/* userprog/process.c에서 소유. */
	uint64_t *pml4;                     /* 4단계 페이지 맵 (PML4). */
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c에서 소유. */
	struct intr_frame tf;               /* 스위칭을 위한 정보. */
	unsigned magic;                     /* 스택 오버플로우 감지. */
};

/* false(기본값)이면 라운드 로빈 스케줄러 사용.
   true이면 다단계 피드백 큐 스케줄러 사용.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어된다. */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

// Alarm Clock
bool thread_wakeup_cmp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void thread_sleep (int64_t ticks);
void thread_awake (int64_t now_tick);

// Priority
bool thread_prio_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void max_priority();

#endif /* threads/thread.h */