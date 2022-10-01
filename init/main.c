/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline fork(void) __attribute__((always_inline));
static inline pause(void) __attribute__((always_inline));
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)		// 从1M开始拓展的内存(KB)
#define DRIVE_INFO (*(struct drive_info *)0x90080) 	// 硬盘参数表
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)	// 根设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

// 老哥说：这是真的void，毛问题！
void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * 老哥说：中断已经关了，要做一些必要的初始化并启用这些中断
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
 	ROOT_DEV = ORIG_ROOT_DEV; // 初始化根设备和硬盘
 	drive_info = DRIVE_INFO;
	// 1MB = 1024*1024B = 2^10*2^10 = 2^20 = 1<<20
	memory_end = (1<<20) + (EXT_MEM_K<<10); // 求出了整个拓展内存的大小(B)
	memory_end &= 0xfffff000;				// 不需要小于4KB(2^12)的内存
	if (memory_end > 16*1024*1024)			// 限制最大内存为16MB
		memory_end = 16*1024*1024;			
	if (memory_end > 12*1024*1024) 			// 如果内存大于12MB，则划分0~4MB为缓冲区
		buffer_memory_end = 4*1024*1024;	
	else if (memory_end > 6*1024*1024)		// 划分0~2MB作为缓冲区
		buffer_memory_end = 2*1024*1024;
	else									// 缓冲区还是不能低于1MB
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;	// 缓冲区之后的内存作为主存
#ifdef RAMDISK								// 这个我们先不管，因为在搞闪存
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	mem_init(main_memory_start,memory_end);	// 初始化内存映射
	trap_init();							// 初始化中断捕获
	blk_dev_init();							// 块设备初始化
	chr_dev_init();							// 字符设备初始化
	tty_init();								// 终端初始化
	time_init();							// 时间初始化
	sched_init();							// 初始化0进程
	buffer_init(buffer_memory_end);			// 缓冲区初始化
	hd_init();								// 初始化硬盘
	floppy_init();							// 初始化软盘
	sti();									// 开启全局中断
	move_to_user_mode();					// 将进程0特权调到3级
	if (!fork()) {		/* we count on this going ok */
		init();								// 子进程进行初始化
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();						// 父进程(翻转0特权)死循环接收中断
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;								// pid用于fork

	setup((void *) &drive_info);			// 配置系统，包括磁盘、文件系统
	(void) open("/dev/tty0",O_RDWR,0);		// 打开tty文件，此时是标准输入设备文件
	(void) dup(0);							// 从tty复制句柄，打开标准输出设备文件
	(void) dup(0);							// 继续复制句柄，打开标准错误输出设备
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {					// 到这里就要启动进程2了
		close(0);							
		if (open("/etc/rc",O_RDONLY,0))		
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
