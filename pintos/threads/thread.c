#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic` 멤버를 위한 임의의 값.
   스택 오버플로우를 감지하는 데 사용된다. 자세한 내용은 thread.h 상단의
   큰 주석을 참고하라. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 임의의 값
   이 값은 수정하지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스들의 리스트,
   즉 실행 준비는 되었지만 실제로 실행 중은 아닌 프로세스들의 리스트. */
static struct list ready_list;

// 타이머 슬립을 구현할 때 잠든 스레드들을 보관하는 리스트
static struct list sleep_list;

/* idle 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용되는 락. */
static struct lock tid_lock;

/* 스레드 파괴 요청 목록 */
static struct list destruction_req;

/* 통계 정보. */
static long long idle_ticks;    /* idle로 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서의 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서의 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 할당되는 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 양보(yield) 이후 경과한 타이머 틱 수. */

/* false(기본값)면 라운드 로빈 스케줄러 사용.
   true이면 다단계 피드백 큐(MLFQ) 스케줄러 사용.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어된다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환.
 * CPU의 스택 포인터 `rsp`를 읽고, 그 값을 페이지의 시작 주소로
 * 내림(round down)한다. `struct thread`는 항상 페이지의 시작에 위치하고
 * 스택 포인터는 그 중간 어딘가를 가리키므로, 이를 통해 현재 스레드를 찾는다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 디스크립터 테이블(GDT).
// gdt는 thread_init 이후에 설정되기 때문에,
// 먼저 임시 gdt를 설정해 두어야 한다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화한다.
   이는 일반적으로는 불가능하지만, 이번 경우에는 loader.S가
   스택의 바닥을 페이지 경계에 놓도록 주의했기 때문에 가능하다.

   또한 실행 큐(run queue)와 tid 락을 초기화한다.

   이 함수를 호출한 뒤에는, 페이지 할당자를 초기화한 다음에
   thread_create()로 스레드를 생성해야 한다.

   thread_init()이 끝나기 전까지는 thread_current()를 호출하는 것이
   안전하지 않다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 gdt를 다시 로드한다.
	 * 이 gdt에는 사용자 컨텍스트가 포함되어 있지 않다.
	 * 커널은 gdt_init()에서 사용자 컨텍스트를 포함한 gdt를 다시 구성한다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);	
	
	// Alarm Clock
	list_init (&sleep_list);						// sleep_list 초기화

	/* 실행 중인 스레드를 위한 스레드 구조체를 설정. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작한다.
   또한 idle 스레드를 생성한다. */
void
thread_start (void) {
	/* idle 스레드를 생성. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링 시작. */
	intr_enable ();

	/* idle 스레드가 idle_thread를 초기화할 때까지 대기. */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러가 각 타이머 틱마다 호출한다.
   즉, 이 함수는 외부 인터럽트 컨텍스트에서 실행된다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계 업데이트. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점 강제. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계(통계치)를 출력. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* NAME이라는 이름으로, 초기 PRIORITY를 가지고,
   AUX를 인자로 하여 FUNCTION을 실행하는 새로운 커널 스레드를 생성하고,
   이를 레디 큐에 추가한다. 새 스레드의 식별자(tid)를 반환하며,
   실패하면 TID_ERROR를 반환한다.

   thread_start()가 호출된 상태라면, 새 스레드는 thread_create()가
   반환되기 전에 스케줄될 수도 있다. 심지어 thread_create()가 반환되기 전에
   종료될 수도 있다. 반대로, 원래 스레드는 새 스레드가 스케줄되기 전까지
   얼마든지 실행될 수 있다. 순서를 보장해야 한다면 세마포어나
   다른 동기화 방식을 사용하라.

   제공된 코드는 새 스레드의 `priority` 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되어 있지 않다.
   우선순위 스케줄링은 과제 1-3의 목표이다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 할당. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드 초기화. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄되면 kernel_thread를 호출하도록 설정.
	 * 주의) rdi는 첫 번째 인자, rsi는 두 번째 인자 레지스터. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가. */
	thread_unblock (t);

	if (t->priority > thread_current()->priority) 
	{
		thread_yield();
	}

	return tid;
}

/* 현재 스레드를 수면 상태로 전환한다. thread_unblock()으로
   다시 깨울 때까지 스케줄되지 않는다.

   이 함수는 반드시 인터럽트가 꺼진 상태에서 호출되어야 한다.
   보통은 synch.h에 있는 동기화 원시(프리미티브)를 사용하는 것이 더 좋다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 블록된 스레드 T를 준비(ready)-실행 상태로 전환한다.
   T가 블록된 상태가 아니라면 오류이다. (실행 중인 스레드를 준비 상태로 만들려면
   thread_yield()를 사용하라)

   이 함수는 실행 중인 스레드를 선점하지 않는다. 이는 중요하다:
   호출자가 인터럽트를 직접 비활성화한 경우, 원자적으로 스레드를 깨우고
   다른 데이터를 갱신할 수 있다고 기대할 수 있기 때문이다. */
void
thread_unblock (struct thread *t) {

	// 아래 부분은 이 함수에서 스레드를 선점하면 안되는데 선점을 했다
	// 여기서는 깨우기만 하고 실제 선점 여부는 호출자(혹은 반환 후)에서 결정하는 구조가 맞다
	// enum intr_level old_level;

	// ASSERT (is_thread (t));

	// old_level = intr_disable ();
	// ASSERT (t->status == THREAD_BLOCKED);

	// //Priority
	// list_insert_ordered(&ready_list, &t->elem, thread_prio_cmp, NULL);
	// t->status = THREAD_READY;

	// bool need_preempt = (t->priority > thread_current()->priority);
	// intr_set_level (old_level);

	// if (need_preempt)
	// {
	// 	if (intr_context())
	// 	{
	// 		intr_yield_on_return();			// 인터럽트 리턴 시점에 양보
	// 	}
	// 	else
	// 	{
	// 		thread_yield();					// 일반 컨텍스트														// 더 높은 애를 방금 깨웠다면 바로 양보
	// 	}
	// }

	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);

	/** project1-Priority Scheduling */
	list_insert_ordered(&ready_list, &t->elem, thread_prio_cmp, NULL);
	//list_push_back (&ready_list, &t->elem);

	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* 실행 중인 스레드의 이름을 반환. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환.
   running_thread()에 몇 가지 무결성 검사를 추가한 버전.
   자세한 내용은 thread.h 상단의 큰 주석을 보라. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말 스레드인지 확인.
	   아래의 어느 어설션이든 실패한다면, 스레드가 스택 오버플로우를
	   일으켰을 가능성이 있다. 각 스레드에는 4 kB 미만의 스택만 있고,
	   몇 개의 큰 자동 배열이나 적당한 깊이의 재귀만으로도
	   스택 오버플로우가 발생할 수 있다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 디스케줄하고 파괴한다. 호출자에게는 절대
   반환되지 않는다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 dying으로 설정하고 다른 프로세스를 스케줄한다.
	   실제 파괴는 schedule_tail() 호출 중에 수행된다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보(yield)한다. 현재 스레드는 잠들지 않으며,
   스케줄러의 선택에 따라 즉시 다시 스케줄될 수도 있다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
	{
		// list_push_back (&ready_list, &curr->elem);
		list_insert_ordered(&ready_list, &curr->elem, thread_prio_cmp, NULL);
	}

	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
	max_priority ();
}

/* 현재 스레드의 우선순위를 반환. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 여기에 구현하시오 */
}

/* 현재 스레드의 nice 값을 반환. */
int
thread_get_nice (void) {
	/* TODO: 여기에 구현하시오 */
	return 0;
}

/* 시스템 load average를 100배 한 값을 반환. */
int
thread_get_load_avg (void) {
	/* TODO: 여기에 구현하시오 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값을 100배 한 값을 반환. */
int
thread_get_recent_cpu (void) {
	/* TODO: 여기에 구현하시오 */
	return 0;
}

/* Idle 스레드. 실행할 다른 스레드가 없을 때 실행된다.

   idle 스레드는 thread_start()에 의해 처음 레디 리스트에 들어간다.
   초기 한 번 스케줄되면, idle_thread를 초기화하고,
   thread_start()가 계속 진행할 수 있도록 전달받은 세마포어를 up한 뒤,
   즉시 블록된다. 그 이후로 idle 스레드는 레디 리스트에 나타나지 않는다.
   레디 리스트가 비어 있을 때 특수 케이스로 next_thread_to_run()이
   idle_thread를 반환한다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행하도록 양보. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 재활성화하고 다음 인터럽트를 기다린다.

		   `sti` 명령은 다음 명령이 완료될 때까지 인터럽트를 비활성화하므로,
		   아래 두 명령은 원자적으로 실행된다. 이 원자성은 중요하다.
		   그렇지 않으면, 인터럽트를 재활성화하고 다음 인터럽트를 기다리는 사이에
		   인터럽트가 처리되어 최대 한 클록 틱만큼의 시간을 낭비할 수 있다.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", [IA32-v3a] 7.11.1 "HLT Instruction" 참고. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반이 되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트를 끈 상태로 동작한다. */
	function (aux);       /* 스레드 함수 실행. */
	thread_exit ();       /* function()이 반환하면 스레드를 종료. */
}


/* T를 NAME이라는 이름의 블록된 스레드로 기본 초기화 수행. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 다음에 스케줄될 스레드를 선택하고 반환.
   실행 큐에서 스레드를 반환해야 하며, 실행 큐가 비어 있다면
   (현재 실행 중인 스레드가 계속 실행 가능하다면, 실행 큐에 있을 것이다)
   idle_thread를 반환한다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용해 스레드를 실행(런치)한다 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환,
   그리고 이전 스레드가 죽는 중이라면 파괴한다.

   이 함수가 호출될 시점에는 방금 thread PREV에서 전환되었고,
   새 스레드는 이미 실행 중이며, 인터럽트는 여전히 비활성화 상태다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않다.
   실제로는 함수 끝부분에서만 printf()를 추가해야 한다는 뜻이다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 주요 전환 로직.
	 * 먼저 전체 실행 컨텍스트를 intr_frame에 저장(복원)한 뒤
	 * do_iret를 호출하여 다음 스레드로 전환한다.
	 * 주의: 전환이 완료될 때까지 여기서 스택을 사용해서는 안 된다. */
	__asm __volatile (
			/* 사용할 레지스터 저장. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번만 가져온다. */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip를 읽는다.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄한다. 진입 시 인터럽트는 꺼져 있어야 한다.
 * 이 함수는 현재 스레드의 상태를 status로 변경한 후
 * 실행할 다른 스레드를 찾아 그 스레드로 전환한다.
 * schedule() 안에서는 printf()를 호출하는 것이 안전하지 않다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* next를 실행 중으로 표시. */
	next->status = THREAD_RUNNING;

	/* 새로운 타임 슬라이스 시작. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새로운 주소 공간을 활성화. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환한 이전 스레드가 죽는 중이라면, 그 struct thread를 파괴.
		   이는 thread_exit()가 자신 밑에서 카펫을 빼버리지 않도록
		   (즉, 자기 자신을 당장 해제하지 않도록) 늦게 수행되어야 한다.
		   현재 스택이 이 페이지를 사용 중이므로 여기서는 페이지 해제 요청만
		   큐잉한다.
		   실제 파괴 로직은 schedule()의 시작 부분에서 호출된다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에 현재 실행 중인 정보부터 저장. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

// 앞이 뒤보다 작으면 true
bool thread_wakeup_cmp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *x = list_entry(a, struct thread, elem);
	const struct thread *y = list_entry(b, struct thread, elem);
	
	//깨울 시각이 더 이른 스레드가 "작다" => 리스트 앞쪽으로
	if (x->wakeup_tick != y ->wakeup_tick)
	{
		return x->wakeup_tick < y->wakeup_tick;
	}
 
	// 똑같을때 priority가 큰 것이 앞으로 이동
	return x->priority > y->priority;
}

// Priority
bool thread_prio_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *x = list_entry (a, struct thread, elem);
	const struct thread *y = list_entry (b, struct thread, elem);

	if (x == NULL || y == NULL)
		return false;

	return x->priority > y->priority; 			// 높은 priority가 리스트 앞
}

// Alarm Clock
// wakeup_tick 기록 + sleep_list 삽입 + block 처리
void thread_sleep (int64_t wakeup_tick)
{
	struct thread* cur = thread_current();		    							// 현재 스레드를 가져온다
	if (cur == idle_thread) 													// idle 가드
	{
		return;
	}
	enum intr_level old_level = intr_disable();									// 인터럽트 비활성화
	cur->wakeup_tick = wakeup_tick;												// 계산된 틱을 저장한다

	ASSERT(!intr_context());

	list_insert_ordered(&sleep_list, &cur->elem, thread_wakeup_cmp, NULL);	// sleep 리스트에 비교 함수에 따라 새로운 원소를 넣는다

	thread_block();											

	intr_set_level(old_level);													// 인터럽트 활성화
}

void thread_awake (int64_t now_tick)
{
	enum intr_level old = intr_disable();
    bool preempt = false;

	while (!list_empty(&sleep_list))												// sleep_list가 빌 때까지(즉, 재울 스레드가 없을 때까지) 맨 앞 원소를 확인
	{
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);

		if (t->wakeup_tick <= now_tick)
		{
			list_pop_front(&sleep_list);
			thread_unblock(t);														// READY로
					
			if (t->priority > thread_current()->priority) 
			{ 
				preempt = true;														// 깬 애가 더 높으면 선점 플래그
			}
		}
		else
		{
			break;																	// 아직 안 깰 시간이면 즉시 종료
		}
	}

  	if (preempt) 
	{
		intr_yield_on_return();   													// 인터럽트 리턴 시 선점
	}

	intr_set_level(old);
}

void max_priority()
{
	if (list_empty(&ready_list))
	{
		return;
	}

	struct thread *th = list_entry(list_front(&ready_list), struct thread, elem);

	if (thread_get_priority() < th->priority)
	{
		thread_yield();
	}
}